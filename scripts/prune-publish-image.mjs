// Run only against the disposable publishing filesystem after artifact builds.
// Without --apply this is a dry run; it never deletes workspace files by default.
import { lstat, readdir, rm } from "node:fs/promises";
import { resolve, join } from "node:path";
import { pathToFileURL } from "node:url";

export const excluded = [
  "outputs", ".git", "temporarybuildtree", ".migration-backup",
  // Never delete .cache itself: Replit actively maintains .cache/replit/env.
  ".cache/zig", ".cache/uv", ".cache/typescript", ".cache/chromium-headless",
  ".config/chromium-headless", "attached_assets",
];

async function bytes(path) {
  const stat = await lstat(path).catch(error => {
    if (error.code === "ENOENT") return null;
    throw error;
  });
  if (!stat) return 0;
  if (!stat.isDirectory() || stat.isSymbolicLink()) return stat.size;
  let total = 0;
  for (const name of await readdir(path)) total += await bytes(join(path, name));
  return total;
}

export async function prune(root, apply = false) {
  let total = 0;
  for (const entry of excluded) {
    const path = resolve(root, entry);
    const size = await bytes(path);
    total += size;
    console.log(`${apply ? "Exclude" : "Would exclude"} ${entry}: ${(size / 1024 ** 3).toFixed(3)} GiB`);
    if (apply) await rm(path, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 });
  }
  console.log(`${apply ? "Excluded" : "Dry-run total"}: ${(total / 1024 ** 3).toFixed(3)} GiB`);
  return total;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  await prune(process.cwd(), process.argv.includes("--apply"));
}
