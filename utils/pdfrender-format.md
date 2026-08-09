# pdfrender archive format, version 1

`pdfrender input.pdf output.zip` produces an immutable render archive. A page
is HTML positioned text over a Cairo SVG background; PNG/JPEG assets are
converted to WebP. Ordinary PDF text is excluded from the SVG and represented
as selectable HTML. Type 3 glyph programs remain in SVG.

The container is a standard ZIP64 archive. Every entry uses ZIP method 0
(STORED): binary objects are already compressed formats such as WebP, while
text objects are independently compressed Zstandard frames. Keeping ZIP out of
the object compression layer makes each payload directly range-readable.

Applications should use a standard ZIP library such as `fflate` to validate
and import the archive. The ClassApp client does not parse this format; its
server exposes a separate streamable Bundle protocol.

## Entries

- `manifest.json`: uncompressed UTF-8 archive index, always present.
- `dictionary.zdict`: optional standard Zstandard dictionary.
- `objects/<sha256>`: identity-encoded binary resource.
- `objects/<sha256>.zst`: Zstandard-compressed text resource.

`<sha256>` is the lowercase SHA-256 of the uncompressed resource. ZIP entry
names are UTF-8. Implementations must reject duplicate, absolute, parent (`..`)
or otherwise unexpected paths.

## Manifest

The manifest has `format: "classapp-render-archive"` and `version: 1`.

- `dictionary` is null or contains `contentId`, `path`, `size`, and
  `storedOffset`.
- `resources[]` contains `contentId`, `mime`, `encoding`, `rawSize`,
  `storedSize`, `storedOffset`, and `path`.
- `files[]` maps semantic archive paths to content IDs.
- `document` describes the renderer-neutral fixed-layout document.

Resource `encoding` is one of `identity`, `zstd`, or `zstd-dictionary`.
`storedOffset` is the absolute byte position of the STORED payload inside the
ZIP file; consumers must validate it against the ZIP directory and file size
before using it for range reads.

The fixed-layout `document` contains `sourceMime`, `shared`, and ordered
`items`. Each item has a stable `id`, zero-based `ordinal`, intrinsic `width`
and `height`, one HTML `document` content ID, and its `dependencies`.

HTML external references use `data-bundle-ref="<contentId>"` alongside a
`src` or `href`. A consumer resolves these references to local files or Blob
URLs. Rendering should occur in a sandboxed iframe with a restrictive CSP.

Text resources use Zstandard level 8. When enough samples exist, pdfrender
trains a standard dictionary capped at 64 KiB and all text frames use it.
Every frame remains independent. Binary resources are identity encoded.

The renderer spools page resources to temporary files as workers finish, so
peak memory is bounded by active pages, dictionary samples, and the largest
single output object instead of total document size.

To validate and scaffold an archive with the example parser:

```sh
node examples/pdfrender-bundle-parser.mjs output.zip webroot
```
