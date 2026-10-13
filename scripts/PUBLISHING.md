# Publishing image size

The configured post-build hook removes development-only material from the
disposable publishing filesystem, after both artifacts have built.
It must not be run with `--apply` in the development workspace.

The exclusions cover research outputs (including private archives), Git
history, source uploads, temporary native build trees, migration backups,
compiler caches and browser-test profiles. Runtime code does not reference
these directories. Built frontend files, demo media, decoder binaries, API
output, Node dependencies and artifact configuration remain included.

Cache cleanup uses an explicit allowlist. Preserve `.cache/replit` and all
unknown cache directories: the publishing environment can actively recreate
its environment-cache files during the hook. Removing the entire `.cache`
directory races those writes and can fail publishing with `ENOTEMPTY`.

Preview the exclusions safely with:

```sh
node scripts/prune-publish-image.mjs
```

This does not remove Nix system dependencies. Confirm total image size on the
next publish attempt; if it still exceeds the platform limit, investigate the
system dependency layer rather than deleting runtime assets.
