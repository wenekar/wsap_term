# Third-party notices

The WSAP framework ships the components below. Each component holds its own
license. The license texts are in this directory.

## FiraCode Nerd Font Mono

`assets/fonts/FiraCodeNerdFontMono-Regular.ttf` and
`assets/fonts/FiraCodeNerdFontMono-Bold.ttf` are Fira Code, patched with the
Nerd Fonts glyph set.

- Fira Code is Copyright 2014-2021, The Fira Code Project Authors
  (https://github.com/tonsky/FiraCode). It is licensed under the SIL Open
  Font License 1.1. See `assets/fonts/LICENSE` and `OFL-1.1.txt`.
- The Nerd Fonts patcher is Copyright 2014, Ryan L McIntyre. It is licensed
  under the MIT License. See https://github.com/ryanoasis/nerd-fonts.

## Adwaita Mono subset

`src/font/backends/freetype/cards_font.inc` holds a subset of Adwaita Mono
covering U+0020 and U+1F0A0-U+1F0FF. The font family name is "wsap Playing
Cards".

- Adwaita Mono is Copyright 2015-2026, Renzhi Li (aka. Belleve Invis), and
  is derived from Iosevka. It is licensed under the SIL Open Font License
  1.1. The font name table holds only a summary of the license.
  `OFL-1.1.txt` holds the full text.
- Run `tools/generate_cards_font.py` to make the subset file again.

## font5x7.h

`src/font/font5x7.h` is the 5x7 dot-matrix glyph table for the 95 printable
ASCII characters. It comes from the public-domain character generator tables
that many terminal and embedded projects use. It has no copyright notice and
no conditions of use.

## accesskit-c

The `accesskit-c` subproject provides accessibility support.

- The library is licensed under MIT OR Apache-2.0. See
  `accesskit-c-MIT.txt` and `accesskit-c-APACHE-2.0.txt`.
- `LICENSE.chromium` is the BSD-3-Clause license of the Chromium code that
  the crate uses. See `accesskit-c-BSD-3-Clause-chromium.txt`.
- The build scripts are licensed under LGPL-2.1. See
  `accesskit-c-LGPL-2.1.txt`.

## yasl

The `yasl` subproject provides the desktop surfaces and the input events. It
is part of this repository and uses the MIT license of `LICENSE`.

## Unicode data

`src/util/unicode/unicode_data.inc` holds tables from the Unicode Character
Database 18.0.0. The Unicode data files are Copyright 2004-2023, Unicode,
Inc., and are licensed under the Unicode License V3. See
`Unicode-3.0.txt`.
