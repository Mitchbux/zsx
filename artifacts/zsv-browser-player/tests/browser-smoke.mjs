import { spawn } from "node:child_process";
import { writeFileSync } from "node:fs";

const targetUrl = process.env.ZSV_TEST_URL ?? "http://localhost:80/";
const port = 9223;
const chrome = spawn(
  process.env.CHROMIUM ?? "chromium",
  [
    "--headless=new",
    "--no-sandbox",
    "--disable-gpu",
    "--autoplay-policy=no-user-gesture-required",
    `--remote-debugging-port=${port}`,
    targetUrl,
  ],
  { stdio: ["ignore", "ignore", "pipe"] },
);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let socket;

try {
  let page;
  for (let attempt = 0; attempt < 100; attempt++) {
    try {
      const pages = await fetch(`http://127.0.0.1:${port}/json/list`).then((response) =>
        response.json(),
      );
      page = pages.find((entry) => entry.type === "page");
      if (page) break;
    } catch {
      // Chromium is still starting.
    }
    await sleep(100);
  }
  if (!page) throw new Error("Chromium debugging target did not start");

  socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    socket.addEventListener("open", resolve, { once: true });
    socket.addEventListener("error", reject, { once: true });
  });

  let nextId = 1;
  const pending = new Map();
  const runtimeErrors = [];
  socket.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (message.method === "Runtime.exceptionThrown") {
      runtimeErrors.push(message.params.exceptionDetails.exception?.description ?? "runtime exception");
    }
    if (!message.id) return;
    const request = pending.get(message.id);
    if (!request) return;
    pending.delete(message.id);
    if (message.error) request.reject(new Error(message.error.message));
    else request.resolve(message.result);
  });
  const command = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      socket.send(JSON.stringify({ id, method, params }));
    });
  const evaluate = async (expression, awaitPromise = false) => {
    const result = await command("Runtime.evaluate", {
      expression,
      awaitPromise,
      returnByValue: true,
    });
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.exception?.description ?? "browser evaluation failed");
    }
    return result.result.value;
  };
  await command("Runtime.enable");
  const waitFor = async (expression, description, timeoutMs = 120000) => {
    const started = Date.now();
    while (Date.now() - started < timeoutMs) {
      if (await evaluate(expression)) return;
      await sleep(100);
    }
    throw new Error(`Timed out waiting for ${description}`);
  };

  await waitFor("document.querySelector('#zsv-sample') !== null", "player mount");
  await command("Runtime.evaluate", { expression: "document.querySelector('#zsv-sample').click()", userGesture: true });
  await waitFor(
    "document.querySelector('#zsv-drop-zone').dataset.status === 'ready'",
    "sample decode",
  );
  const decoded = await evaluate(`({
    fps: document.querySelector('#zsv-drop-zone').dataset.fps,
    duration: document.querySelector('#zsv-duration').textContent,
    width: document.querySelector('#zsv-canvas').width,
    height: document.querySelector('#zsv-canvas').height
  })`);
  if (
    decoded.duration !== "04:31" ||
    decoded.width !== 270 ||
    decoded.height !== 480 ||
    decoded.fps !== "25.00"
  ) {
    throw new Error(`unexpected sample metadata: ${JSON.stringify(decoded)}`);
  }
  await waitFor("Number(document.querySelector('#zsv-seek').value) > 0.1", "automatic playback after buffering");
  await evaluate("document.querySelector('#zsv-play').click()");
  await evaluate("document.querySelector('#zsv-video').dispatchEvent(new PointerEvent('pointermove'))");
  await waitFor("document.querySelector('.zsv-overlay').dataset.visible === 'true'", "overlay wake");
  await sleep(2200);
  if (await evaluate("document.querySelector('.zsv-overlay').dataset.visible !== 'false'")) throw Error("overlay did not hide after two seconds");
  await evaluate("document.querySelector('#zsv-video').dispatchEvent(new PointerEvent('pointermove'))");
  await command("Runtime.evaluate", {
    expression: "document.querySelector('[data-testid=\"button-fullscreen\"]').click()",
    userGesture: true,
  });
  await waitFor("document.fullscreenElement?.id === 'zsv-player'", "enter fullscreen");
  await waitFor("document.querySelector('[data-testid=\"button-fullscreen\"]').getAttribute('aria-label') === 'Exit fullscreen'", "fullscreen button state");
  await command("Runtime.evaluate", {
    expression: "document.querySelector('[data-testid=\"button-fullscreen\"]').click()",
    userGesture: true,
  });
  await waitFor("document.fullscreenElement === null", "exit fullscreen");
  const capture = await command("Page.captureScreenshot", {
    format: "png",
    captureBeyondViewport: false,
  });
  writeFileSync(
    new URL("../../../screenshots/zsv-browser-player-loaded.png", import.meta.url),
    Buffer.from(capture.data, "base64"),
  );

  await evaluate(`(() => {
    const seek = document.querySelector('#zsv-seek');
    seek.value = '10';
    seek.dispatchEvent(new Event('input', { bubbles: true }));
  })()`);
  if ((await evaluate("document.querySelector('#zsv-elapsed').textContent")) !== "00:10") {
    throw new Error("seek did not update elapsed time");
  }

  await waitFor("document.querySelector('#zsv-drop-zone').dataset.status === 'ready'", "seek replay");
  await evaluate("document.querySelector('#zsv-play').click()");
  await waitFor("Number(document.querySelector('#zsv-seek').value) >= 12", "playback across multiple batches", 10000);
  const streamed = await evaluate(`({
    elapsed: Number(document.querySelector('#zsv-seek').value),
    bytes: Number(document.querySelector('#zsv-drop-zone').dataset.queuedBytes),
    items: Number(document.querySelector('#zsv-drop-zone').dataset.queueItems)
  })`);
  if (streamed.elapsed < 12 || streamed.bytes > 192 * 1024 * 1024 || streamed.items > 8192) {
    throw new Error("streaming failed to cross queue boundaries within its bounds: " + JSON.stringify(streamed));
  }
  await evaluate(`(() => {
    const volume = document.querySelector('#zsv-volume');
    volume.value = '25';
    volume.dispatchEvent(new Event('input', { bubbles: true }));
    document.querySelector('#zsv-mute').click();
  })()`);
  const audioControls = await evaluate(`({
    volume: document.querySelector('#zsv-drop-zone').dataset.volume,
    muted: document.querySelector('#zsv-drop-zone').dataset.muted
  })`);
  if (audioControls.volume !== "0.25" || audioControls.muted !== "true") {
    throw new Error(`volume/mute controls did not update Haxe state: ${JSON.stringify(audioControls)}`);
  }
  await evaluate("document.querySelector('#zsv-mute').click()");
  await evaluate("document.querySelector('#zsv-play').click()");
  const pausedAt = await evaluate("document.querySelector('#zsv-elapsed').textContent");
  await sleep(1100);
  if ((await evaluate("document.querySelector('#zsv-elapsed').textContent")) !== pausedAt) {
    throw new Error("pause did not stop the playback clock");
  }
  await evaluate(`(() => {
    const seek = document.querySelector('#zsv-seek');
    for (const value of ['25', '2', '17']) {
      seek.value = value;
      seek.dispatchEvent(new Event('input', { bubbles: true }));
    }
  })()`);
  await waitFor("document.querySelector('#zsv-drop-zone').dataset.status === 'ready'", "latest seek");
  if ((await evaluate("document.querySelector('#zsv-elapsed').textContent")) !== "00:17") {
    throw new Error("a stale seek replaced the latest seek");
  }

  await evaluate(`(() => {
    const seek = document.querySelector('#zsv-seek');
    seek.value = '271.4';
    seek.dispatchEvent(new Event('input', { bubbles: true }));
  })()`);
  await waitFor("document.querySelector('#zsv-drop-zone').dataset.status === 'ready'", "end seek replay");
  await evaluate("document.querySelector('#zsv-play').click()");
  await waitFor(
    "document.querySelector('#zsv-drop-zone').dataset.status === 'ended'",
    "end of file",
    5000,
  );

  await evaluate(`(() => {
    const transfer = new DataTransfer();
    transfer.items.add(new File([new Uint8Array(32).fill(165)], 'malformed.zsv'));
    document.querySelector('#zsv-drop-zone').dispatchEvent(
      new DragEvent('drop', { dataTransfer: transfer, bubbles: true, cancelable: true })
    );
  })()`);
  await waitFor(
    "document.querySelector('#zsv-drop-zone').dataset.status === 'error'",
    "malformed-file error",
  );

  await evaluate(`fetch(document.querySelector('#zsv-worker-url').dataset.sampleUrl)
    .then(response => response.arrayBuffer())
    .then(bytes => {
      const transfer = new DataTransfer();
      transfer.items.add(new File([bytes], 'local-valid.zsv'));
      document.querySelector('#zsv-drop-zone').dispatchEvent(
        new DragEvent('drop', { dataTransfer: transfer, bubbles: true, cancelable: true })
      );
    })`, true);
  await waitFor(
    "document.querySelector('#zsv-drop-zone').dataset.status === 'ready'",
    "valid local-file replacement",
  );
  await evaluate(`(async () => {
    const sampleUrl = document.querySelector('#zsv-worker-url').dataset.sampleUrl;
    const originalFetch = window.fetch;
    const validBytes = await originalFetch(sampleUrl).then(response => response.arrayBuffer());
    window.fetch = () => new Promise(resolve =>
      setTimeout(() => resolve(new Response(validBytes)), 600)
    );
    document.querySelector('#zsv-sample').click();
    const transfer = new DataTransfer();
    transfer.items.add(new File([validBytes], 'newer-local-choice.zsv'));
    document.querySelector('#zsv-drop-zone').dispatchEvent(
      new DragEvent('drop', { dataTransfer: transfer, bubbles: true, cancelable: true })
    );
    setTimeout(() => { window.fetch = originalFetch; }, 800);
  })()`, true);
  await waitFor(
    "document.querySelector('#zsv-drop-zone').dataset.status === 'ready'",
    "overlapping local replacement",
  );
  await sleep(900);
  if ((await evaluate("document.querySelector('#zsv-drop-zone').dataset.mediaName"))
    !== "newer-local-choice.zsv") {
    throw new Error("a stale sample request replaced the newer local file");
  }
  if (
    runtimeErrors.length ||
    (await evaluate("document.querySelector('replit-runtime-error-modal') !== null"))
  ) {
    throw new Error(`browser runtime error: ${runtimeErrors.join("; ") || "error overlay present"}`);
  }

  console.log(
    `Browser smoke passed: sample decode, seek, play/pause, EOF, malformed local file, replacement (${decoded.width}x${decoded.height}, ${decoded.duration})`,
  );
} finally {
  if (socket?.readyState === WebSocket.OPEN) socket.close();
  chrome.kill("SIGTERM");
}