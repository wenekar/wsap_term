# Inline graphics

`wsap_term` displays SIXEL and direct Kitty graphics in the terminal grid on the CPU and GPU renderers.

| Protocol | Example |
|---|---|
| SIXEL | `chafa -f sixels image.png` |
| Kitty | `kitten icat --transfer-mode=stream image.png` |
| Kitty with Chafa | `chafa -f kitty image.png` |

SIXEL supports RGB and HLS palettes, transparency, repeats, pixel aspect ratios, and the xterm display and cursor modes. Kitty accepts inline RGB, RGBA, and PNG images, including chunked and zlib-compressed transfers. It supports storing images by ID or number, placing, replacing, and deleting them, and playing frame animations that the client transmits.

| Kitty feature | Limit |
|---|---|
| Frames per image | 64 |
| Frame data | Full frames only; composition modes are rejected |
| Local file or shared-memory transfer | Not supported |
| Unicode placeholders | Not supported |

Images move with their rows into scrollback and appear when you scroll back. Resize preserves placements on rows that keep their boundaries; reflow can discard bands on rows it splits or joins. Erasing text cells also erases the corresponding part of an image. The alternate screen keeps a separate set of images. Tab thumbnails omit inline images.

| Kitty z-index | Draws |
|---|---|
| Zero or greater | Above text |
| Negative | Behind text but above cell backgrounds |
| Less than −1,073,741,824 | Behind text and non-default cell backgrounds |

| Storage limit per terminal | Value |
|---|---|
| Decoded image data | 64 MiB |
| Stored images | 256 |
| Placements | 256 |

When adding an image would exceed the storage quota, the terminal discards older images, starting with images that have no placement. See the [core API](../subprojects/term/docs/api.md#graphics) for individual image and transfer limits.
