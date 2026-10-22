# Live Rice ZIP texture packs

Hosts can call `TextureCache::loadTexturePack(zipPath, romTitle, rdramByteCount, logLoads)`
while texture uploads and graphics submissions are paused, with streaming
workers drained, as for `loadReplacementDirectories`. Supply the trimmed N64
header title and the actual accessible RDRAM size. The host needs no scanner,
capture callback, or periodic pack reload. `TextureManager` captures bounded
immutable load data during draws; RT64 handles matching and streaming.

Set `logLoads` to print each streamed entry, dimensions, and alpha-mask path.
The original ZIP remains read-only. Its sibling `<zip filename>.rt64.json`
contains standard RT64 replacement records with exact archive-entry paths and
source identity metadata. Delete the sidecar to rebuild the cache. Changed ZIP
size/mtime, ROM title, and hash version invalidate it. PNG Rice filenames,
CI wildcards, RGB/alpha pairs, alternates, and ambiguous-image rejection retain
the previous scanner rules. The JSON writer needs a writable ZIP parent folder.
Use one running writer per ZIP sidecar.

The capture queue is bounded to 64 entries / 8 MiB with per-frame admission
limits. The graphics thread retries queue pressure on subsequent uses. Rice
checksums, candidate decoding and JSON writes run on a matching worker; ZIP
reads, decoding and RGB/alpha composition run on streaming workers. Mappings
are queued to the texture uploader and do not trigger a global reload. A missing
low-mip cache is supported: the original stays active until streaming finishes.

The checksum implementation and live scanner are GPL-2.0-only, derived from
GLideN64/RiceVideo. See `src/tools/texture_hasher/GLIDEN64-LICENSE`.
