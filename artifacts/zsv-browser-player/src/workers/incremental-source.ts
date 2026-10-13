/** Bounded encoded-byte cache: consume the network lazily; replay for seeking. */
export class IncrementalSource {
  private chunks: Uint8Array[] = [];
  private index = 0;
  private offset = 0;
  private ended = false;
  bytes = 0;

  constructor(private reader: ReadableStreamDefaultReader<Uint8Array>, private limit: number) {}

  rewind() { this.index = 0; this.offset = 0; }

  async read(size: number): Promise<Uint8Array> {
    if (!Number.isInteger(size) || size <= 0) throw Error("Invalid stream read size.");
    while (this.index === this.chunks.length && !this.ended) {
      let timeout: ReturnType<typeof setTimeout> | undefined;
      try {
        const next = await Promise.race([
          this.reader.read(),
          new Promise<never>((_, reject) => {
            timeout = setTimeout(() => reject(Error("Media download timed out.")), 30000);
          }),
        ]);
        if (next.done) { this.ended = true; break; }
        if (!next.value.length) continue;
        this.bytes += next.value.length;
        if (this.bytes > this.limit) throw Error("File exceeds the 64 MiB encoded input limit.");
        this.chunks.push(next.value);
      } finally {
        if (timeout !== undefined) clearTimeout(timeout);
      }
    }
    if (this.index === this.chunks.length) return new Uint8Array(0);
    const chunk = this.chunks[this.index];
    const end = Math.min(chunk.length, this.offset + size);
    const output = chunk.subarray(this.offset, end);
    this.offset = end;
    if (end === chunk.length) { this.index++; this.offset = 0; }
    return output;
  }

  dispose() { void this.reader.cancel().catch(() => undefined); this.chunks = []; }
}
