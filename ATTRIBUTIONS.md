# Attributions

Teletext is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is a PROVISIONAL hand copy (2026-09-24). The fleet's copies are generated — the
master lists live in the `stoatworks-backend` repo and are pushed out by
`scripts/sync-attributions.py` — and the first sync will overwrite this file; the
entries below are what it needs to carry.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### 5x7 bitmap font — Stoatworks graticule

<https://github.com/stoatworks-labs/graticule>  
Licence: MIT  
Copyright: Stoatworks Labs

The 5x7 bitmap font the header row is drawn from is graticule's, copied unchanged by
way of rebate. It is Stoatworks' own drawing, not a copy of the SAA5050's character
ROM or of any other character generator's, and it is placed in the chip's 6x10 cell
at dot columns 0-4, lines 1-7.

### Harness shape, --pipe contract and verify — Stoatworks rebate

<https://github.com/stoatworks-labs/rebate>  
Licence: MIT  
Copyright: Stoatworks Labs

The harness shape, the --pipe contract (SIGPIPE ignored, a closed stdout exits 1),
the verify script and the negative-control pattern are rebate's, which had them from
pitch; the host clock-unit voting is readout's by way of rebate.

### PassBuffer — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer is tinsel's.

### Field arithmetic — Stoatworks copperlist

<https://github.com/stoatworks-labs/copperlist>  
Licence: MIT  
Copyright: Stoatworks Labs

The field index from the host clock, `floor( t x rate + 1e-6 )` in double with up to
four fields a frame and a longer gap treated as a jump, is copperlist's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Teletext, and the people who drew pictures in it

Built from the published standard and from the folk knowledge of teletext artists:
the serial attributes that cost a cell, Hold Mosaics as the way round them, the black
column at every colour boundary, separated mosaics, and a page that arrives a few rows
a field. No broadcaster's service name, page or artwork appears in the plugin or its
header row; "TELETEXT" is the medium's own name.

## Standards and published specifications

What the implementation is measured against.

- **ETSI EN 300 706, "Enhanced Teletext specification"** — the Level 1 spacing
  attributes and their set-at / set-after timing, the 2x3 mosaic character set and
  its bit layout, odd parity on character bytes and Hamming protection on the row
  address, and 24 rows of 40 characters with row 0 as the header. Implemented from
  the standard's description; no reference decoder's code was consulted.
- **Mullard/Philips SAA5050 teletext character generator** — the 6x10 dot character
  cell and the 2x3 block layout of 3x3, 3x4 and 3x3 dots. The separated-mode gutter
  (each block losing its left column and bottom line) follows jsbeeb's SAA5050
  emulation (`teletext.js`, read for the geometry only) and Wikipedia's description;
  the datasheet figure itself was not consulted — see AGENTS.md.
- **ITU-R BT.709** — the luma coefficients 0.2126, 0.7152, 0.0722 behind the Luma
  Weighted error space.
- **Melissa E. O'Neill, "PCG: A Family of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation" (Harvey Mudd College, 2014)** — the pcg_hash output mix used for the channel's bit errors, written out rather than copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
