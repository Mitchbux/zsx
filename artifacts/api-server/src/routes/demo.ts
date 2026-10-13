import { Router } from "express";
import { logger } from "../lib/logger";
// @ts-ignore shared with the offline asset uploader
import { demoObject } from "../demo-storage.mjs";
const router = Router();
router.get("/demo/snowboard-full.zsv", async (_req, res) => {
  try {
    const object = demoObject();
    await object.getMetadata();
    res.setHeader("Content-Type", "application/octet-stream");
    // Keep this chunked: the full demo exceeds buffered-response limits
    // on the publishing proxy. Do not add Content-Length here.
    res.setHeader("Cache-Control", "no-store, no-transform");
    res.setHeader("X-Accel-Buffering", "no");
    res.flushHeaders();
    const stream = object.createReadStream();
    stream.on("error", (error: Error & { code?: string | number }) => {
      logger.error({ code: error.code, errorType: error.name }, "Demo storage stream failed");
      res.destroy();
    });
    res.on("close", () => stream.destroy());
    stream.pipe(res);
  } catch (error) {
    logger.error({ errorType: error instanceof Error ? error.name : "UnknownError" }, "Demo storage lookup failed");
    if (res.headersSent) res.destroy();
    else res.status(503).json({ error: "Demo unavailable." });
  }
});
export default router;
