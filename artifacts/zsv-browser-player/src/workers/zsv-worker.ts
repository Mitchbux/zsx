/* The worker owns the decoder and is the only place that touches WASM. */
import { IncrementalSource } from "./incremental-source";

const ENCODED_LIMIT = 64 * 1024 * 1024;
const BATCH_LIMIT = 4 * 1024 * 1024;
const MAX_BATCH_ITEMS = 128;
const BATCH_SECONDS = 1;

type DecoderModule = {
  ccall?(name: string, result: string, types: string[], args: number[], options: { async: boolean }): Promise<number>;
  zsvRead?: (size: number) => Promise<Uint8Array>;
  streamError?: string;
  _malloc(n: number): number;
  _free(p: number): void;
  _zsv_open(p: number, n: number): number;
  _zsv_close(h: number): void;
  _zsv_next(...a: number[]): number;
  _zsv_free(p: number): void;
  _zsv_error(h: number): number;
  UTF8ToString(p: number): string;
  HEAPU8: Uint8Array;
  HEAP32: Int32Array;
};

type Scratch = {
  kind: number; width: number; height: number; channels: number; rate: number;
  samples: number; pts: number; out: number; outSize: number;
};

type Metadata = {
  name: string; bytes: number; width: number; height: number; fps: number;
  duration: number; audioStart: number; rate: number; channels: number;
};

type Frame = { data: ArrayBuffer; width: number; height: number; time: number; grayscale?: boolean };
type Audio = { data: ArrayBuffer; time: number; samples: number; rate: number; channels: number };
type Output = ({ kind: "video" } & Frame) | ({ kind: "audio" } & Audio);

type Validation = {
  width: number; height: number; rate: number; channels: number;
  lastVideo: number; expectedAudio: number; audioStart: number;
  audioSamples: number; videoCount: number; firstVideo: number;
  penultimateVideo: number; lastVideoForDuration: number;
};

type RawRecord = {
  kind: "video" | "audio"; time: number; width: number; height: number;
  rate: number; channels: number; samples: number; size: number;
  output(): Output;
};

type Session = { handle: number; scratch?: Scratch; validation: Validation };
type PlayerState = {
  m: DecoderModule; input: number; inputLength: number; metadata: Metadata;
  session: Session; epoch: number; pending?: Output; eof: boolean;
  videoThrough: number; audioThrough: number;
  finalVideo: number; audioEnd: number; videoFinished: boolean; audioFinished: boolean;
  source?: IncrementalSource; seed: Output[]; initial: boolean;
};

let modulePromise: Promise<DecoderModule> | undefined;
const module = () => modulePromise ||= (async () => {
  const decoderUrl = new URL(`${import.meta.env.BASE_URL}decoder/zsv_decoder.js`, self.location.origin).href;
  const { default: moduleFactory } = await import(/* @vite-ignore */ decoderUrl);
  return moduleFactory({
    locateFile: (file: string) => new URL(`${import.meta.env.BASE_URL}decoder/${file}`, self.location.origin).href,
  }) as Promise<DecoderModule>;
})();

let state: PlayerState | undefined;
let revision = 0;
let serial: Promise<void> = Promise.resolve();

const send = (message: unknown, transfer?: Transferable[]) => {
  if (transfer?.length) {
    (self as unknown as { postMessage(message: unknown, transfer: Transferable[]): void }).postMessage(message, transfer);
  } else {
    postMessage(message);
  }
};

const errorMessage = (error: unknown) =>
  error instanceof Error && error.message ? error.message : "Unable to decode ZSV2 media.";

const yieldToMessages = () => new Promise<void>(resolve => setTimeout(resolve, 0));
const active = (operation: number) => operation === revision;

function decoderError(m: DecoderModule, handle: number, fallback: string) {
  const detail = handle ? m.UTF8ToString(m._zsv_error(handle)) : "";
  return detail || fallback;
}

function allocate(m: DecoderModule, bytes: number, label: string) {
  const pointer = m._malloc(bytes);
  if (!pointer) throw Error(`Decoder could not allocate ${label}.`);
  return pointer;
}

function makeScratch(m: DecoderModule): Scratch {
  const pointers: number[] = [];
  try {
    const pointer = (label: string) => {
      const value = allocate(m, 4, label);
      pointers.push(value);
      return value;
    };
    return {
      kind: pointer("record kind"), width: pointer("video width"), height: pointer("video height"),
      channels: pointer("audio channels"), rate: pointer("audio rate"), samples: pointer("audio samples"),
      pts: pointer("timestamp"), out: pointer("decoded output pointer"), outSize: pointer("decoded output size"),
    };
  } catch (error) {
    for (const pointer of pointers) m._free(pointer);
    throw error;
  }
}

function freeScratch(m: DecoderModule, scratch: Scratch | undefined) {
  if (!scratch) return;
  for (const pointer of Object.values(scratch)) if (pointer) m._free(pointer);
}

function closeSession(m: DecoderModule, session: Session | undefined) {
  if (!session) return;
  const handle = session.handle;
  const scratch = session.scratch;
  // A failed reopen can leave this old Session in PlayerState.  Clear its
  // ownership before freeing so releaseState() can safely run afterwards.
  session.handle = 0;
  session.scratch = undefined;
  try {
    if (handle) m._zsv_close(handle);
  } finally {
    freeScratch(m, scratch);
  }
}

function releaseState() {
  const previous = state;
  state = undefined;
  if (!previous) return;
  try {
    closeSession(previous.m, previous.session);
  } finally {
    if (previous.input) previous.m._free(previous.input);
    previous.source?.dispose();
    previous.m.zsvRead = undefined;
  }
}

function newValidation(): Validation {
  return {
    width: 0, height: 0, rate: 0, channels: 0, lastVideo: -1, expectedAudio: -1,
    audioStart: -1, audioSamples: 0, videoCount: 0, firstVideo: -1,
    penultimateVideo: -1, lastVideoForDuration: -1,
  };
}

function readInt(m: DecoderModule, pointer: number) {
  return m.HEAP32[pointer >> 2];
}

/*
 * The callback is deliberately synchronous.  The native output pointer is
 * always released before this function returns, including validation failures.
 */
async function readRecord(m: DecoderModule, session: Session, use: (record: RawRecord) => void): Promise<boolean> {
  const s = session.scratch;
  if (!session.handle || !s) throw Error("Decoder session is closed.");
  const args = [session.handle, s.kind, s.width, s.height, s.channels, s.rate, s.samples, s.pts, s.out, s.outSize];
  const result = m.ccall
    ? await m.ccall("zsv_next", "number", args.map(() => "number"), args, { async: true })
    : m._zsv_next(...args);
  if (result === 0) return false;
  if (result < 0) throw Error(decoderError(m, session.handle, "Decoder reported a media error."));

  let nativeOutput = 0;
  try {
    nativeOutput = readInt(m, s.out);
    const size = readInt(m, s.outSize);
    const rawKind = readInt(m, s.kind);
    const timestamp = readInt(m, s.pts) / 1000;
    if (!Number.isFinite(timestamp) || timestamp < 0) throw Error("ZSV2 stream is missing required timestamps.");
    if (!Number.isSafeInteger(nativeOutput) || nativeOutput <= 0 || !Number.isSafeInteger(size) || size <= 0 ||
        nativeOutput + size > m.HEAPU8.byteLength) {
      throw Error("Decoder returned an invalid decoded media buffer.");
    }

    const validation = session.validation;
    const video = rawKind === 1;
    const audio = rawKind === 2;
    if (!video && !audio) throw Error("ZSV2 stream contains an unsupported decoded record type.");

    const width = readInt(m, s.width);
    const height = readInt(m, s.height);
    const rate = readInt(m, s.rate);
    const channels = readInt(m, s.channels);
    const samples = readInt(m, s.samples);

    let mediaTime = timestamp;
    if (video) {
      if (size > BATCH_LIMIT) throw Error("Decoded video frame exceeds the 4 MiB streaming batch limit.");
      if (!Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width <= 0 || height <= 0 ||
          width * height * 3 / 2 !== size) throw Error("ZSV2 video frame has an invalid decoded size.");
      if ((validation.width && validation.width !== width) || (validation.height && validation.height !== height)) {
        throw Error("Changing video dimensions are unsupported.");
      }
      if (validation.lastVideo >= 0 && timestamp <= validation.lastVideo) {
        throw Error("Video timestamps are not strictly increasing.");
      }
      validation.width = width;
      validation.height = height;
      validation.penultimateVideo = validation.lastVideoForDuration;
      validation.lastVideoForDuration = timestamp;
      validation.lastVideo = timestamp;
      validation.videoCount++;
      if (validation.firstVideo < 0) validation.firstVideo = timestamp;
    } else {
      if (size > BATCH_LIMIT) throw Error("Decoded audio packet exceeds the 4 MiB streaming batch limit.");
      if (!Number.isSafeInteger(rate) || !Number.isSafeInteger(channels) || !Number.isSafeInteger(samples) ||
          rate <= 0 || channels <= 0 || samples <= 0 || samples * channels * 4 !== size) {
        throw Error("ZSV2 audio packet has an invalid decoded format.");
      }
      if ((validation.rate && validation.rate !== rate) || (validation.channels && validation.channels !== channels)) {
        throw Error("Changing audio format is unsupported.");
      }
      if (validation.audioStart < 0) {
        validation.audioStart = timestamp;
        validation.expectedAudio = timestamp;
      }
      if (Math.abs(timestamp - validation.expectedAudio) > 0.002) {
        throw Error("Audio gaps or overlapping packets are unsupported.");
      }
      /*
       * NUT timestamps are millisecond values while PCM packet duration is
       * usually fractional milliseconds.  Once that bounded rounding error is
       * validated, expose the exact cumulative clock so adjacent WebAudio
       * buffers neither leave a gap nor overlap.
       */
      mediaTime = validation.expectedAudio;
      validation.rate = rate;
      validation.channels = channels;
      validation.audioSamples += samples;
      validation.expectedAudio += samples / rate;
    }

    let copied = false;
    const output = (): Output => {
      if (copied) throw Error("Internal decoder output was retained twice.");
      copied = true;
      const grayscale = video && m.HEAPU8.subarray(nativeOutput + width * height, nativeOutput + size).every(v => v === 128);
      const data = m.HEAPU8.slice(nativeOutput, nativeOutput + (grayscale ? width * height : size)).buffer;
      return video
        ? { kind: "video", data, width, height, time: mediaTime, grayscale }
        : { kind: "audio", data, time: mediaTime, samples, rate, channels };
    };
    use({ kind: video ? "video" : "audio", time: mediaTime, width, height, rate, channels, samples, size, output });
  } finally {
    if (nativeOutput) m._zsv_free(nativeOutput);
  }
  return true;
}

async function openSession(m: DecoderModule, input: number, inputLength: number, source?: IncrementalSource) {
  let handle = 0;
  let scratch: Scratch | undefined;
  try {
    if (source) {
      if (!m.ccall) throw Error("Streaming decoder is unavailable. Reload the page.");
      m.streamError = "";
      m.zsvRead = n => source.read(n);
      handle = await m.ccall("zsv_open_stream", "number", [], [], { async: true });
    } else handle = m._zsv_open(input, inputLength);
    if (!handle) throw Error("Invalid ZSV2 file or unsupported stream.");
    scratch = makeScratch(m);
    return { handle, scratch, validation: newValidation() };
  } catch (error) {
    if (handle) m._zsv_close(handle);
    freeScratch(m, scratch);
    throw error;
  }
}

function durationOf(v: Validation) {
  if (!v.videoCount || !v.audioSamples) throw Error("Stream requires video and audio.");
  return Math.max(v.audioStart + v.audioSamples / v.rate,
    v.lastVideoForDuration + (v.videoCount > 1 ? v.lastVideoForDuration - v.penultimateVideo : 1 / 30));
}

function outputBytes(output: Output) {
  return output.data.byteLength;
}

function assertOutputFits(output: Output) {
  if (outputBytes(output) > BATCH_LIMIT) {
    throw Error(output.kind === "video"
      ? "Decoded video frame exceeds the 4 MiB streaming batch limit."
      : "Decoded audio packet exceeds the 4 MiB streaming batch limit.");
  }
}

type WorkingBatch = {
  frames: Frame[]; audio: Audio[]; byteLength: number; start: number;
};

function makeBatch(): WorkingBatch {
  return { frames: [], audio: [], byteLength: 0, start: -1 };
}

function itemCount(batch: WorkingBatch) {
  return batch.frames.length + batch.audio.length;
}

function addOutput(player: PlayerState, batch: WorkingBatch, output: Output) {
  assertOutputFits(output);
  if (itemCount(batch) >= MAX_BATCH_ITEMS || batch.byteLength + outputBytes(output) > BATCH_LIMIT) return false;
  if (batch.start < 0) batch.start = output.time;
  batch.byteLength += outputBytes(output);
  if (output.kind === "video") {
    batch.frames.push(output);
    player.videoThrough = output.time;
    if (output.time === player.finalVideo) player.videoFinished = true;
  } else {
    batch.audio.push(output);
    player.audioThrough = output.time + output.samples / output.rate;
    if (Math.abs(player.audioThrough - player.audioEnd) < 1e-9) player.audioFinished = true;
  }
  return true;
}

function targetCovered(player: PlayerState, batch: WorkingBatch) {
  return batch.start >= 0 && Math.max(player.videoThrough, player.audioThrough) - batch.start >= BATCH_SECONDS;
}

function messageForBatch(player: PlayerState, epoch: number, batch: WorkingBatch) {
  const eof = player.eof;
  return {
    type: "batch", epoch, frames: batch.frames, audio: batch.audio,
    duration: eof ? player.metadata.duration : Math.max(player.videoThrough, player.audioThrough),
    durationKnown: eof,
    // Once the terminal item has actually been sent, that stream safely
    // covers the remainder of the media even when its sibling is longer.
    // Keep raw timestamps for batch sizing (targetCovered) instead.
    videoThrough: eof || player.videoFinished ? player.metadata.duration : player.videoThrough,
    audioThrough: eof || player.audioFinished ? player.metadata.duration : player.audioThrough,
    eof, byteLength: batch.byteLength,
  };
}

async function generateBatch(player: PlayerState, epoch: number, operation: number, seekTime?: number) {
  const batch = makeBatch();
  let records = 0;
  let candidate: Output | undefined;
  let passedVideoTarget = seekTime === undefined;

  const addOrPend = (output: Output) => {
    if (addOutput(player, batch, output)) return true;
    player.pending = output; // The sole output retained between pulls.
    return false;
  };
  const flushCandidate = () => {
    if (!candidate) return true;
    if (!addOutput(player, batch, candidate)) {
      throw Error("Seek replay exceeded streaming batch bounds before its video position was established; unsupported multiplex skew.");
    }
    candidate = undefined;
    return true;
  };

  if (player.eof) return messageForBatch(player, epoch, batch);
  if (seekTime === undefined && player.pending) {
    const pending = player.pending;
    player.pending = undefined;
    if (!addOutput(player, batch, pending)) throw Error("Pending decoded output exceeds streaming batch bounds.");
    if (targetCovered(player, batch)) return messageForBatch(player, epoch, batch);
  }

  for (;;) {
    if (!active(operation)) return undefined;
    if (player.seed.length) {
      if (!addOrPend(player.seed.shift()!)) return messageForBatch(player, epoch, batch);
      continue;
    }
    let stopped = false;
    const hasRecord = await readRecord(player.m, player.session, raw => {
      if (seekTime !== undefined && !passedVideoTarget) {
        if (raw.kind === "video") {
          if (raw.time <= seekTime) {
            candidate = raw.output(); // Replace, rather than accumulate, the seek frame.
            return;
          }
          passedVideoTarget = true;
          flushCandidate();
          if (!addOrPend(raw.output())) stopped = true;
          return;
        }
        const end = raw.time + raw.samples / raw.rate;
        if (end <= seekTime) return;
        /*
         * Audio may precede the first post-seek video record in a mux.  It is
         * placed in this in-flight batch while the candidate remains singular.
         * If that batch fills before the video boundary, reject the pathological
         * stream rather than retaining unbounded replay output.
         */
        if (!addOutput(player, batch, raw.output())) {
          throw Error("Seek replay exceeded streaming batch bounds before its video position was established; unsupported multiplex skew.");
        }
        return;
      }

      if (seekTime !== undefined && raw.kind === "audio" && raw.time + raw.samples / raw.rate <= seekTime) return;
      if (!addOrPend(raw.output())) stopped = true;
    });
    if (!hasRecord) {
      if (seekTime !== undefined && !passedVideoTarget) flushCandidate();
      player.eof = true;
      player.metadata.duration = durationOf(player.session.validation);
      return messageForBatch(player, epoch, batch);
    }
    records++;
    if (stopped || (passedVideoTarget && targetCovered(player, batch))) {
      return messageForBatch(player, epoch, batch);
    }
    if (records % 20 === 0) {
      await yieldToMessages();
      if (!active(operation)) return undefined;
    }
  }
}

async function decode(bytes: ArrayBuffer, name: string, operation: number, source?: IncrementalSource) {
  releaseState();
  let m: DecoderModule | undefined;
  let input = 0;
  const inputLength = bytes.byteLength;
  try {
    m = await module();
    if (!active(operation)) return;
    if (!source) {
      input = allocate(m, inputLength, "input memory");
      m.HEAPU8.set(new Uint8Array(bytes), input);
    }
    bytes = new ArrayBuffer(0);
    const session = await openSession(m, input, inputLength, source);
    state = {
      m, input, inputLength, metadata: {} as Metadata, session, epoch: -1,
      eof: false, videoThrough: -1, audioThrough: -1,
      finalVideo: Infinity, audioEnd: Infinity, source, seed: [], initial: true,
      videoFinished: false, audioFinished: false,
    };
    input = 0; // Ownership transferred to state.
    const v = session.validation;
    let seedBytes = 0;
    while (v.videoCount < 2 || !v.audioSamples) {
      if (!await readRecord(m, session, record => {
        const output = record.output(); seedBytes += output.data.byteLength;
        if (seedBytes > BATCH_LIMIT || state!.seed.length >= MAX_BATCH_ITEMS) throw Error("Stream headers are too widely interleaved.");
        state!.seed.push(output);
      })) { state.eof = true; break; }
      if (!active(operation)) return;
    }
    if (!v.videoCount || !v.audioSamples) throw Error("Stream requires video and audio.");
    state.metadata = { name, bytes: inputLength, width: v.width, height: v.height,
      fps: v.videoCount > 1 ? (v.videoCount - 1) / (v.lastVideoForDuration - v.firstVideo) : 0,
      duration: state.eof ? durationOf(v) : Infinity, audioStart: v.audioStart, rate: v.rate, channels: v.channels };
    send({ type: "metadata", ...state.metadata, streaming: true, durationKnown: state.eof });
  } catch (error) {
    releaseState();
    source?.dispose();
    if (active(operation)) send({ type: "error", message: errorMessage(error) });
  } finally {
    if (input && m) m._free(input);
  }
}

async function seek(epoch: number, time: number, operation: number) {
  const player = state;
  if (!player || !active(operation)) return;
  if (!Number.isSafeInteger(epoch) || epoch < 0) throw Error("Seek epoch must be a non-negative integer.");
  if (!Number.isFinite(time) || time < 0 || time > player.metadata.duration) {
    throw Error("Seek time must be within the decoded media duration.");
  }
  const first = player.initial && time === 0;
  player.initial = false;
  if (!first) {
    closeSession(player.m, player.session);
    player.source?.rewind();
    player.session = await openSession(player.m, player.input, player.inputLength, player.source);
    player.seed = [];
  }
  player.epoch = epoch;
  player.pending = undefined;
  player.eof = false;
  player.videoThrough = -1;
  player.audioThrough = -1;
  player.videoFinished = false;
  player.audioFinished = false;
  const batch = await generateBatch(player, epoch, operation, first ? undefined : time);
  if (batch && active(operation) && state === player) {
    const transfers = [...batch.frames.map(frame => frame.data), ...batch.audio.map(packet => packet.data)];
    send(batch, transfers);
  }
}

async function pull(epoch: number, operation: number) {
  const player = state;
  if (!player || !active(operation) || player.epoch !== epoch) return;
  const batch = await generateBatch(player, epoch, operation);
  if (batch && active(operation) && state === player) {
    const transfers = [...batch.frames.map(frame => frame.data), ...batch.audio.map(packet => packet.data)];
    send(batch, transfers);
  }
}

function schedule(operation: (id: number) => Promise<void>) {
  const id = ++revision; // Increment synchronously so a new seek cancels replay at its next yield.
  serial = serial.catch(() => undefined).then(async () => {
    try {
      await operation(id);
    } catch (error) {
      if (active(id)) {
        releaseState(); // A decoder failure is fatal; always close it before reporting the error.
        send({ type: "error", message: errorMessage(error) });
      }
    }
  });
}

self.onmessage = (event: MessageEvent) => {
  if (event.data.type === "stream") {
    const { url, file, name } = event.data;
    schedule(async id => {
      let stream: ReadableStream<Uint8Array>;
      if (file instanceof Blob) {
        if (file.size > ENCODED_LIMIT) throw Error("File exceeds the 64 MiB encoded input limit.");
        stream = file.stream();
      } else {
        const target = new URL(url, self.location.origin);
        if (target.origin !== self.location.origin) throw Error("Media URL must be same-origin.");
        const response = await fetch(target);
        if (!response.ok || !response.body) throw Error("Sample is unavailable.");
        stream = response.body;
      }
      await decode(new ArrayBuffer(0), String(name || "stream.zsv"), id, new IncrementalSource(stream.getReader(), ENCODED_LIMIT));
    });
    return;
  }
  const data = event.data as { type?: string; bytes?: unknown; name?: unknown; epoch?: unknown; time?: unknown };
  if (data.type === "decode") {
    if (!(data.bytes instanceof ArrayBuffer)) return send({ type: "error", message: "Decode request must contain an ArrayBuffer." });
    if (data.bytes.byteLength > ENCODED_LIMIT) return send({ type: "error", message: "File exceeds the 64 MiB encoded input limit." });
    if (data.bytes.byteLength === 0) return send({ type: "error", message: "ZSV2 input file is empty." });
    if (typeof data.name !== "string") return send({ type: "error", message: "Decode request must contain a file name." });
    /*
     * Clear the closure's reference as soon as the operation starts.  The WASM
     * input is then the sole encoded-media copy during asynchronous preflight.
     */
    let requestedBytes: ArrayBuffer | undefined = data.bytes;
    const requestedName = data.name;
    schedule(id => {
      const bytes = requestedBytes;
      requestedBytes = undefined;
      return decode(bytes as ArrayBuffer, requestedName as string, id);
    });
  } else if (data.type === "seek") {
    if (typeof data.epoch !== "number" || typeof data.time !== "number") {
      return send({ type: "error", message: "Seek request must contain numeric epoch and time values." });
    }
    schedule(id => seek(data.epoch as number, data.time as number, id));
  } else if (data.type === "pull") {
    if (typeof data.epoch !== "number") return send({ type: "error", message: "Pull request must contain a numeric epoch." });
    schedule(id => pull(data.epoch as number, id));
  }
};