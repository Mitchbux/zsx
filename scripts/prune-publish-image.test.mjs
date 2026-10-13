import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, mkdir, writeFile, readFile, rm, access } from "node:fs/promises";
import { join, dirname } from "node:path";
import { tmpdir } from "node:os";
import { prune, excluded } from "./prune-publish-image.mjs";

test("dry run preserves everything; cleanup preserves platform and runtime files", async () => {
  const root = await mkdtemp(join(tmpdir(), "publish-cleanup-"));
  const kept = [
    ".cache/replit/env/current", ".cache/unknown-package/state",
    "artifacts/zsv-browser-player/dist/public/sample/demo.zsv",
    "artifacts/zsv-browser-player/dist/public/decoder/decoder.wasm",
    "artifacts/api-server/dist/index.mjs", "node_modules/package/index.js",
  ];
  try {
    for (const path of [...kept, ...excluded.map(path => `${path}/fixture`)]) {
      await mkdir(dirname(join(root, path)), { recursive: true });
      await writeFile(join(root, path), "fixture");
    }
    await prune(root);
    for (const path of excluded) await access(join(root, path, "fixture"));
    await prune(root, true);
    for (const path of excluded) await assert.rejects(access(join(root, path)), { code: "ENOENT" });
    for (const path of kept) assert.equal(await readFile(join(root, path), "utf8"), "fixture");
    await prune(root, true); // Already absent excluded directories are safe.
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
