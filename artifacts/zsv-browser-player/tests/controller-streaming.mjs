import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { Script, createContext } from "node:vm";

/*
 * These tests intentionally do not use a browser (or a test agent).  Player is
 * a static Haxe controller, so a small DOM, worker, AudioContext, and clock
 * are enough to exercise its stream state machine.  The generated file calls
 * Player.main() as a side effect; replace that call with an export while
 * loading it into the VM.
 */
const source = readFileSync(new URL("../src/generated/player.js", import.meta.url), "utf8");

function element(id) {
  const classes = new Set();
  return {
    id,
    dataset: {},
    textContent: "",
    value: 0,
    max: 0,
    files: [],
    onclick: null,
    onchange: null,
    oninput: null,
    ondragover: null,
    ondrop: null,
    click() {
      this.clicked = true;
    },
    setAttribute(name, value) {
      this[name] = value;
    },
    classList: {
      add(name) {
        classes.add(name);
      },
      remove(name) {
        classes.delete(name);
      },
      contains(name) {
        return classes.has(name);
      },
    },
    getContext(kind) {
      assert.equal(kind, "2d");
      return {
        fillStyle: "",
        fillRect() {},
        createImageData(width, height) {
          return { data: new Uint8ClampedArray(width * height * 4) };
        },
        putImageData() {},
      };
    },
  };
}

function makeRuntime() {
  const ids = [
    "zsv-drop-zone",
    "zsv-status-text",
    "zsv-play",
    "zsv-mute",
    "zsv-volume",
    "zsv-seek",
    "zsv-empty",
    "zsv-video",
    "zsv-controls",
    "zsv-canvas",
    "zsv-duration",
    "zsv-elapsed",
    "zsv-open",
    "zsv-file",
    "zsv-sample",
    "zsv-worker-url",
  ];
  const elements = new Map(ids.map((id) => [id, element(id)]));
  elements.get("zsv-worker-url").dataset.workerUrl = "decoder-worker.js";
  elements.get("zsv-worker-url").dataset.sampleUrl = "sample.zsv";
  elements.get("zsv-volume").value = "80";
  elements.get("zsv-seek").value = 0;

  const intervals = new Map();
  const timeouts = new Map();
  let nextTimer = 1;
  const workers = [];
  const audioContexts = [];

  class FakeWorker {
    constructor(url, options) {
      this.url = url;
      this.options = options;
      this.messages = [];
      this.terminated = false;
      workers.push(this);
    }

    postMessage(message, transfer) {
      this.messages.push({ message, transfer });
    }

    terminate() {
      this.terminated = true;
    }

    emit(data) {
      this.onmessage?.({ data });
    }

    emitError(error = new Error("worker error")) {
      this.onerror?.(error);
    }
  }

  class FakeAudioContext {
    constructor() {
      this.currentTime = 0;
      this.destination = { connected: true };
      this.sources = [];
      this.buffers = [];
      this.closed = false;
      audioContexts.push(this);
    }

    createGain() {
      return {
        gain: { value: 0 },
        connect() {},
      };
    }

    createBuffer(channels, length, rate) {
      const buffer = {
        numberOfChannels: channels,
        length,
        sampleRate: rate,
        channels: Array.from({ length: channels }, () => new Float32Array(length)),
        getChannelData(channel) {
          return this.channels[channel];
        },
        copyToChannel(values, channel) {
          this.channels[channel].set(values);
        },
      };
      this.buffers.push(buffer);
      return buffer;
    }

    createBufferSource() {
      const context = this;
      const source = {
        buffer: null,
        onended: null,
        connected: false,
        starts: [],
        stopped: false,
        connect() {
          this.connected = true;
        },
        start(...args) {
          this.starts.push(args);
        },
        stop() {
          this.stopped = true;
        },
        disconnect() {
          this.connected = false;
        },
      };
      context.sources.push(source);
      return source;
    }

    resume() {
      return Promise.resolve();
    }

    close() {
      this.closed = true;
      return Promise.resolve();
    }
  }

  const context = {
    console,
    Uint8Array,
    Uint8ClampedArray,
    Float32Array,
    ArrayBuffer,
    Promise,
    Math,
    Date,
    performance: { now: () => 0 },
    document: {
      getElementById(id) {
        return elements.get(id) ?? null;
      },
    },
    Worker: FakeWorker,
    AudioContext: FakeAudioContext,
    webkitAudioContext: FakeAudioContext,
    setInterval(callback, delay) {
      const id = nextTimer++;
      intervals.set(id, { callback, delay });
      return id;
    },
    clearInterval(id) {
      intervals.delete(id);
    },
    setTimeout(callback, delay) {
      const id = nextTimer++;
      timeouts.set(id, { callback, delay });
      return id;
    },
    clearTimeout(id) {
      timeouts.delete(id);
    },
    requestAnimationFrame(callback) {
      const id = nextTimer++;
      timeouts.set(id, { callback, delay: 16 });
      return id;
    },
    cancelAnimationFrame(id) {
      timeouts.delete(id);
    },
  };
  context.window = context;
  context.globalThis = context;

  const vmContext = createContext(context);
  // The source has an IIFE, so exporting Player before invoking main is
  // deterministic and does not modify the production generated artifact.
  const instrumented = source.replace("Player.main();", "globalThis.__Player = Player;");
  new Script(instrumented, { filename: "src/generated/player.js" }).runInContext(vmContext);
  const Player = context.__Player;
  assert.ok(Player, "generated player must expose Player after VM instrumentation");
  Player.main();

  return {
    context,
    Player,
    elements,
    workers,
    audioContexts,
    runInterval(id) {
      intervals.get(id)?.callback(0);
    },
    runAllIntervals() {
      for (const { callback } of intervals.values()) callback(0);
    },
    runTimeout(id) {
      const timer = timeouts.get(id);
      if (!timer) return;
      timeouts.delete(id);
      timer.callback();
    },
    async flush() {
      await Promise.resolve();
      await Promise.resolve();
    },
  };
}

function frame(time) {
  // 2x2 YUV420: four luma samples, one U, and one V sample.
  return {
    time,
    width: 2,
    height: 2,
    data: new Uint8Array([80, 80, 80, 80, 128, 128]),
  };
}

function audio(time, duration = 1, rate = 10, channels = 2) {
  const samples = Math.round(duration * rate);
  return {
    time,
    samples,
    rate,
    channels,
    data: new Float32Array(samples * channels),
  };
}

const metadata = {
  type: "metadata",
  duration: 6,
  name: "fixture.zsv",
  bytes: 4096,
  width: 2,
  height: 2,
  fps: 5,
  rate: 10,
  channels: 2,
};

async function loadMetadata(runtime, name = "fixture.zsv", extra = {}) {
  const file = {
    name,
    size: 1,
    arrayBuffer: () => Promise.resolve(new Uint8Array([1, 2, 3])),
  };
  runtime.Player.loadFile(file);
  await runtime.flush();
  assert.equal(runtime.workers.length, 1);
  runtime.workers[0].emit({ ...metadata, ...extra });
  assert.equal(runtime.workers[0].messages.at(-1).message.type, "seek");
  return runtime.workers[0];
}

function sendBatch(worker, runtime, {
  videoThrough,
  audioThrough,
  frames = [frame(0)],
  audio: audioPackets = [audio(0)],
  eof = false,
  epoch = runtime.Player.epoch,
} = {}) {
  worker.emit({
    type: "batch",
    epoch,
    frames,
    audio: audioPackets,
    videoThrough,
    audioThrough,
    eof,
  });
}

async function testCreditBoundsAndRelease() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime);

  // A paused player may request more data while coverage is short, but the
  // entire next batch must fit under the rolling queue credit.
  sendBatch(worker, runtime, {
    videoThrough: 0.25,
    audioThrough: 0.25,
    frames: [frame(0)],
    audio: [audio(0, 0.2)],
  });
  const pullsAfterShortCoverage = worker.messages.filter((x) => x.message.type === "pull").length;
  assert.equal(pullsAfterShortCoverage, 1);

  Player.pending = false;
  Player.queuedBytes = Player.queueLimit - Player.batchLimit + 1;
  Player.fill();
  assert.equal(
    worker.messages.filter((x) => x.message.type === "pull").length,
    pullsAfterShortCoverage,
    "paused fill must not grant credit when a complete batch cannot fit",
  );

  Player.queuedBytes = 0;
  Player.frames = new Array(8001).fill(frame(0));
  Player.audio = [];
  Player.fill();
  assert.equal(
    worker.messages.filter((x) => x.message.type === "pull").length,
    pullsAfterShortCoverage,
    "paused fill must cap decoded item count before requesting another batch",
  );

  // Consumption releases both video and audio bytes.  draw() retains the
  // latest frame, while prune() releases frames and packets wholly behind p.
  Player.frames = [frame(0), frame(0.2)];
  const consumedAudio = audio(0, 0.2);
  Player.audio = [consumedAudio];
  Player.queuedBytes = 6 + 6 + consumedAudio.data.byteLength;
  Player.prune(0.3);
  assert.equal(Player.frames.length, 1);
  assert.equal(Player.audio.length, 0);
  assert.equal(Player.queuedBytes, 6);
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.queueItems, "1");
}

async function testAudioSchedulingAndPause() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime);
  Player.seek(1);
  const seekEpoch = Player.epoch;
  sendBatch(worker, runtime, {
    epoch: seekEpoch,
    videoThrough: 6,
    audioThrough: 6,
    frames: [frame(1)],
    audio: [audio(0, 2)],
  });
  // AudioContext is created only by play, and scheduling clips a packet to
  // the current seek point.  Set currentTime before the resume continuation
  // runs so the source timestamp is deterministic.
  Player.play();
  const audioContext = runtime.audioContexts[0];
  assert.ok(audioContext);
  audioContext.currentTime = 4.25;
  await runtime.flush();
  assert.equal(audioContext.sources.length, 1);
  assert.deepEqual(audioContext.sources.at(-1).starts[0], [4.28]);
  assert.equal(audioContext.buffers.at(-1).length, 10);

  audioContext.currentTime = 4.75;
  runtime.runAllIntervals();
  assert.equal(runtime.elements.get("zsv-elapsed").textContent, "00:01");
  const pausedAt = Player.position();
  Player.pause();
  audioContext.currentTime = 9;
  runtime.runAllIntervals();
  assert.equal(Player.position(), pausedAt, "pause must stop the playback clock");
}

async function testThirtySecondStartupBuffer() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime, "stream.zsv", { streaming: true, duration: 60, durationKnown: true });
  sendBatch(worker, runtime, { videoThrough: 10, audioThrough: 10, audio: [audio(0, 10)] });
  Player.play();
  await runtime.flush();
  assert.equal(Player.running, false, "must not start with only ten seconds");
  sendBatch(worker, runtime, { videoThrough: 30, audioThrough: 30, audio: [audio(10, 20)] });
  assert.equal(Player.running, true, "starts with thirty seconds");
  const ctx = runtime.audioContexts[0];
  Player.videoThrough = 0.2;
  ctx.currentTime = 5;
  runtime.runAllIntervals();
  assert.equal(Player.running, true, "audio must continue despite a video stall");
  assert.ok(ctx.sources.every(s => !s.stopped));
  Player.pause();
  assert.ok(ctx.sources.every(s => s.stopped));
}

async function testIndependentAudioSurvivesVideoStall() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime, "fixture.zsv", { audioTrack: [audio(0, 6)] });
  sendBatch(worker, runtime, { videoThrough: 0.2, audioThrough: 0.2, audio: [] });
  Player.play();
  await runtime.flush();
  const context = runtime.audioContexts[0];
  assert.equal(context.buffers[0].length, 60, "entire audio track scheduled before video advances");
  const source = context.sources[0];
  // No more worker replies: video stays stuck at 0.2 seconds.
  context.currentTime = 3;
  runtime.runAllIntervals();
  assert.equal(Player.running, true);
  assert.equal(source.stopped, false, "video starvation must not stop audio");
  assert.ok(Player.position() > 2.9, "audio remains the playback clock");
  const cached = Player.audioTrack;
  Player.pause();
  assert.equal(source.stopped, true, "explicit pause still stops audio");
  Player.seek(4);
  assert.equal(Player.audioTrack, cached, "seeking retains the validated audio");
  sendBatch(worker, runtime, { videoThrough: 4.2, audioThrough: 4.2, audio: [], frames: [frame(4)] });
  Player.play();
  await runtime.flush();
  assert.equal(context.buffers.at(-1).length, 20, "seek schedules only the remaining audio");
  Player.cleanup();
  assert.equal(context.sources.at(-1).stopped, true, "replacement stops old audio");
  assert.equal(Player.audioTrack.length, 0);
}

async function testSeekCancellationAndReplacement() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime);
  sendBatch(worker, runtime, {
    videoThrough: 2,
    audioThrough: 2,
    frames: [frame(0)],
    audio: [audio(0, 2)],
  });
  Player.play();
  await runtime.flush();
  const oldEpoch = Player.epoch;
  const oldSource = runtime.audioContexts[0].sources.at(-1);

  Player.seek(3);
  assert.equal(oldSource.stopped, true);
  assert.equal(Player.frames.length, 0);
  assert.equal(Player.audio.length, 0);
  assert.equal(Player.epoch, oldEpoch + 1);

  // Late packets from the previous seek are ignored by the worker callback.
  sendBatch(worker, runtime, {
    epoch: oldEpoch,
    videoThrough: 6,
    audioThrough: 6,
    frames: [frame(0)],
    audio: [audio(0, 1)],
  });
  assert.equal(Player.frames.length, 0);
  assert.equal(Player.audio.length, 0);

  sendBatch(worker, runtime, {
    epoch: Player.epoch,
    videoThrough: 5,
    audioThrough: 5,
    frames: [frame(3)],
    audio: [audio(3, 2)],
  });
  assert.equal(Player.frames.length, 1);
  assert.ok(runtime.audioContexts[0].sources.length > 1, "replacement seek should schedule fresh audio");

  // A newer load invalidates both an unresolved old file and messages from
  // its worker, even if those callbacks are invoked after replacement.
  const oldFile = {};
  const newFile = {};
  oldFile.name = "old.zsv";
  newFile.name = "new.zsv";
  oldFile.size = newFile.size = 1;
  let resolveOld;
  let resolveNew;
  oldFile.arrayBuffer = () => new Promise((resolve) => { resolveOld = resolve; });
  newFile.arrayBuffer = () => new Promise((resolve) => { resolveNew = resolve; });
  Player.loadFile(oldFile);
  Player.loadFile(newFile);
  resolveOld(new Uint8Array([1]));
  resolveNew(new Uint8Array([2]));
  await runtime.flush();
  assert.equal(runtime.workers.length, 2);
  const replacedWorker = runtime.workers.at(-1);
  assert.equal(replacedWorker.messages[0].message.name, "new.zsv");
  replacedWorker.emit({ ...metadata, name: "new.zsv" });
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.mediaName, "new.zsv");
}

async function testBufferingAndEofReplay() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime);
  sendBatch(worker, runtime, {
    videoThrough: 1,
    audioThrough: 1,
    frames: [frame(0)],
    audio: [audio(0, 1)],
  });
  Player.play();
  await runtime.flush();
  const ctx = runtime.audioContexts[0];
  ctx.currentTime = 2;
  runtime.runAllIntervals();
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.status, "buffering");
  assert.equal(Player.running, false);
  assert.equal(Player.position(), 1, "buffering must freeze the playback clock at coverage");
  assert.equal(ctx.sources.at(-1).stopped, true);

  sendBatch(worker, runtime, {
    videoThrough: 3,
    audioThrough: 3,
    frames: [frame(1)],
    audio: [audio(1, 2)],
  });
  assert.equal(Player.running, true, "coverage extension should restart a buffered play request");
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.status, "ready");

  // An EOF batch ends once, then play from EOF requests a seek/replay rather
  // than leaving the controller permanently ended.
  Player.pause();
  Player.seek(0);
  sendBatch(worker, runtime, {
    epoch: Player.epoch,
    videoThrough: 6,
    audioThrough: 6,
    frames: [frame(0)],
    audio: [audio(0, 6)],
    eof: true,
  });
  Player.play();
  await runtime.flush();
  ctx.currentTime = 9;
  runtime.runAllIntervals();
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.status, "ended");
  assert.equal(Player.offset, 6);
  assert.equal(Player.playing, false);

  Player.play();
  await runtime.flush();
  assert.equal(Player.playing, true);
  assert.equal(Player.epoch > 1, true);
  sendBatch(worker, runtime, {
    epoch: Player.epoch,
    videoThrough: 6,
    audioThrough: 6,
    frames: [frame(0)],
    audio: [audio(0, 6)],
    eof: true,
  });
  assert.equal(Player.offset, 0);
  assert.equal(Player.running, true);
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.status, "ready");
}

async function testQueueAllocationLimits() {
  const runtime = makeRuntime();
  const { Player } = runtime;
  const worker = await loadMetadata(runtime);

  Player.queuedBytes = Player.queueLimit;
  Player.pending = false;
  Player.videoThrough = 1;
  Player.audioThrough = 1;
  sendBatch(worker, runtime, {
    videoThrough: 1,
    audioThrough: 1,
    frames: [frame(0)],
    audio: [],
  });
  assert.equal(runtime.elements.get("zsv-drop-zone").dataset.status, "error");
  assert.match(runtime.elements.get("zsv-status-text").textContent, /bounded playback queue/i);

  // The receive path must remain an ordinary bounded-queue error even when
  // the old coverage is still zero.  In particular, fill() can reject the
  // queue during receive's initial tick; that must not leave receive()
  // dereferencing media after cleanup.
  const lowCoverage = makeRuntime();
  const lowCoverageWorker = await loadMetadata(lowCoverage);
  lowCoverage.Player.queuedBytes = lowCoverage.Player.queueLimit;
  lowCoverage.Player.pending = false;
  assert.doesNotThrow(() => {
    sendBatch(lowCoverageWorker, lowCoverage, {
      videoThrough: 1,
      audioThrough: 1,
      frames: [frame(0)],
      audio: [],
    });
  });
  assert.equal(lowCoverage.elements.get("zsv-drop-zone").dataset.status, "error");

  const second = makeRuntime();
  const secondWorker = await loadMetadata(second);
  second.Player.frames = new Array(8192).fill(frame(0));
  second.Player.audio = [];
  second.Player.queuedBytes = 0;
  sendBatch(secondWorker, second, {
    videoThrough: 1,
    audioThrough: 1,
    frames: [frame(0)],
    audio: [],
  });
  assert.equal(second.elements.get("zsv-drop-zone").dataset.status, "error");
  assert.match(second.elements.get("zsv-status-text").textContent, /bounded playback queue/i);
}

const tests = [
  ["credit bounds and consumption release", testCreditBoundsAndRelease],
  ["audio scheduling timestamps and pause", testAudioSchedulingAndPause],
  ["independent audio survives video stall", testIndependentAudioSurvivesVideoStall],
  ["thirty-second startup buffer", testThirtySecondStartupBuffer],
  ["seek cancellation and replacement", testSeekCancellationAndReplacement],
  ["buffering coverage and EOF replay", testBufferingAndEofReplay],
  ["bounded queue allocations", testQueueAllocationLimits],
];

for (const [name, test] of tests) {
  try {
    await test();
    console.log(`ok - ${name}`);
  } catch (error) {
    console.error(`not ok - ${name}`);
    throw error;
  }
}

console.log(`Controller streaming tests passed (${tests.length} cases)`);