# Inline graphics

`wsap_term` displays SIXEL and direct Kitty graphics in the terminal grid on the CPU and GPU renderers.

| Protocol | Example |
|---|---|
| SIXEL | `chafa -f sixels image.png` |
| Kitty | `kitten icat --transfer-mode=stream image.png` |
| Kitty with Chafa | `chafa -f kitty image.png` |

SIXEL supports RGB and HLS palettes, transparency, repeats, and pixel aspect ratios. Kitty accepts inline RGB, RGBA, and PNG images, including chunked transfers. It supports storing images by ID and placing, replacing, and deleting them. Local file and shared-memory transfers, extra zlib compression, animation, and Unicode placeholders are not supported.

Images move with their rows on the active screen. The alternate screen keeps a separate set of images. Images do not enter scrollback, and reflow can discard their placements. Kitty z-index orders images relative to each other, but even negative z-index images draw over text. Inline images are not included in tab thumbnails.

| Storage limit per terminal | Value |
|---|---|
| Decoded image data | 64 MiB |
| Stored images | 256 |
| Placements | 256 |

When adding an image would exceed the storage quota, the terminal discards older images, starting with images that have no placement. See the [core API](../subprojects/term/docs/api.md#graphics) for individual image and transfer limits.
