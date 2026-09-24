# AGENTS.md — Teletext

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

A picture sent as Level 1 teletext mosaic graphics, as an FFGL 2.1 effect (`TX01`,
shown as `SW Teletext`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake,
universal macOS `.bundle` and a Windows `.dll`. MIT; intended home
`github.com/stoatworks-labs/teletext`, which does not exist yet.

Built 2026-09-24 in one session from `specs/SPEC-teletext.md` and the fleet's
templates: rebate for the effect skeleton, the harness, `--pipe` and the negative
controls; copperlist for the field arithmetic and the two-raster discipline; tinsel
for `PassBuffer`, the sweep and CI; graticule for the 5x7 font (by way of rebate).
Tranche four, Allan's own pick.

---

## The one idea

**Colour is not stored per cell.** A teletext row is 40 bytes; a byte below 0x20 is a
spacing attribute that sets the colour (or the background, or Hold) from that cell
on, and it *occupies the cell*, which shows as background unless Hold Mosaics is on.
Every row starts alphanumeric, white on black. So encoding a picture is an
optimisation under a hard serial constraint, and the look is what the optimum looks
like:

| the constraint | what comes out |
| --- | --- |
| a colour code costs a cell that shows background | **the gap**: a black column at every foreground colour boundary; column 0 of every row lost to the code that switches to mosaics |
| a new background costs two codes | colours rationed; where the background may move, a solid boundary costs three code cells and **no black at all** (New Background shows the old colour, the colour code shows it too, New Background again shows the new one) |
| eight colours, 2x3 sixels | 80 x 72 sixels and nothing between |
| Hold Mosaics: a code cell shows the last mosaic received | the gaps filled, and the held shape bleeding in whatever colours are in effect |
| rows arrive a few per field, in order, cycling | a moving picture tears between rows |
| odd parity per byte, Hamming on the row address | a single bit blanks a cell (a broken colour code loses the colour for the rest of the row); two bits pass as the wrong mosaic; a row never moves |

The pipeline, in order:

1. **Cells** (GPU, 80 x 72 RGBA32F). Each sixel's mean over the source pixels whose
   centres fall in its rectangle on the output, in linear light. `Layout` decides
   the rectangle: the 4:3 safe area or the whole frame, scaled by whole pixels where
   both dimensions allow at least one pixel a dot, else by a fraction.
2. **Read-back** (92 KB) to the CPU.
3. **Encode** (CPU, `Encoder.cpp`). Per row a Viterbi programme over 40 cells with
   state (mode, fg, bg, hold): 224 states. Transitions: the best mosaic for the
   state's colours (each sixel the nearer of the two, ties to the background) or a
   space in alphanumerics; a mosaic colour code (set-after); New Background and Black
   Background (set-at); Hold Mosaics (set-at). Costs are squared errors on the sRGB
   encoding of the linear mean, RGB or luma weighted, quantised to 1/65536 and summed
   in 64 bits.
4. **Transmit** (CPU, `Transmission.cpp`). Fields at 50 Hz from the host clock; each
   field carries Rows per Field rows (and the header as row 0 when on) into a 24 x 40
   byte page memory, through a channel that flips bits by an integer hash of
   (field, row, column).
5. **Decode** (CPU, `Decoder.cpp`) every row to what each cell shows, and upload a
   40 x 24 RGBA8 cell texture: fg, bg, code, flags.
6. **Render** (GPU). The SAA5050's 6 x 10 cell per output pixel; Show Codes tints;
   Mix.

### The decoder is the reference

`Decoder::Step` is the receiver's half of ETS 300 706, Level 1: set-at and
set-after, parity, the held character. Three things are judged by it and share
nothing else: the encoder's realised cost (`RealisedCost` decodes the bytes and costs
what they show), the harness's exhaustive search (which drives `Step` byte by byte),
and the render pass (which draws the cells `Step` produces). A wrong decoder would
make all three agree with each other and disagree with a real television; the checks
that would notice are `--gap` and `--parity`, which assert the *standard's* structure
(where the codes are, what a parity failure shows) rather than the decoder's.

### Hold Mosaics is approximate, and bounded

Under Hold a code cell shows the last mosaic *received*, which depends on the path,
not on the state. The programme costs a held cell as if the previous cell were a
mosaic placed under the state's own colours — exact when it was (a colour change
between two runs), an assumption when a run of codes follows. So the encoder runs
both programmes, with and without Hold, realises both through the decoder and keeps
the cheaper. **The realised cost with Hold on is therefore never worse than the exact
optimum without it**, by construction; `--optimal` measures it anyway, and also
reports the gap to an exhaustive search *with* Hold in the alphabet — which, because
every mosaic cell takes the best mosaic for its colours, is a search over control
placements and not the true optimum. On the 24 random rows the gap is 0 and Hold wins
on none of them; on the test card Hold visibly fills gaps (the sweep sees 327
subpixels change at 320 x 180). Random sixels rarely reward Hold; adjacent runs of the
same shape do.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Codes.h` | The standard's numbers: the codes, parity, the mosaic bit layout, the 6x10 cell and its separated gutter, the `Perturb` hook bits. |
| `source/Decoder.{h,cpp}` | Received bytes to cells. The reference. |
| `source/Encoder.{h,cpp}` | Cost table, best mosaic, the Viterbi programme (and the greedy one for the negative control), realised cost. |
| `source/Transmission.{h,cpp}` | Field index, rows per field, the page memory, the channel's bit errors, the forced error hook. |
| `source/Header.{h,cpp}` | Row 0: page number, service name, running clock. |
| `source/Layout.{h,cpp}` | Grid Fit and the dot scale; sixel rectangles. Shared with the harness. |
| `source/Controls.{h,cpp}` | Option names and the two integer ranges. |
| `source/Font.{h,cpp}` | graticule's 5x7 font, unchanged. |
| `source/Shaders.{h,cpp}` | The cells pass and the render pass. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Teletext.{h,cpp}` | The plugin: parameters, the clock, the two passes, the read-back, the CPU stages. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/txtest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

---

## Traps

Roughly in the order they will bite.

### The encoder is cleverer than the spec's claims

The spec's `--gap` says a red-then-blue row "produces exactly one background cell at
the boundary". With backgrounds allowed it produces none: the programme found New
Background (shows red), blue (shows the red background), New Background (shows blue)
— three code cells, every one the right colour, cost zero. And the spec's
`--separated` wants full white mosaics on a white frame; with backgrounds allowed the
programme writes New Background in alphanumerics at column 1 and 38 spaces, and draws
no mosaic at all. Both are the optimum, both are what a teletext artist would do, and
both were first seen as check failures. `--gap` A and `--separated` run with Allow
Background off, where the spec's claims are the truth; `--gap` C states the free
boundary and measures it.

### Linear light rounds a mid-tone to black

The first card came out as blue sky, black ground, black colour patches: a (0.2, 0.6,
0.2) green is 0.32 green in linear light, nearer black than green. The mean stays in
linear light (an area average has to be), and the error is measured on its sRGB
encoding. Stated in `BuildCosts`.

### The separated palette check treated a sixel as uniform

In separated mode a sixel is two classes of dot, body and gutter, each uniform.
27,360 pixels "in a non-uniform sixel" were the gutters. The check groups by
(sixel, gutter class) now.

### A fractional cell boundary belongs to whichever side the shader says

`--parity`'s "no pixel changed outside the cell" first failed at 320 x 180 with 8
pixels: the pixels within 1e-3 dot of the cell's edge, which the strict pixel set
leaves out and which the shader (in float) put inside the cell. "Outside" is judged
against a rectangle a pixel wider than the cell; the inside assertions use the strict
set.

### A negative control that only bites at random

`kPerturbErrorsMoveRows` first moved a row only when the random channel drew an error,
which at Signal Quality 1 it never does, so the forced-error render was unmoved and
the negative "passed" at 1280 x 720 (and "failed" at 320 x 180 for the unrelated
reason above). The perturbation now moves a forced error's row too.

### The DP was 5 ms a frame at 24 rows a field

The first programme rebuilt each state's best mosaic from the cost table 224 times a
cell and pushed transitions into a `std::vector` each time. Once per cell now
(`CellCache`), and the transitions go through a callback. About 0.05 ms a row for
both programmes.

### The bench was timing the upload

Uploading a 4K card per frame put the 4K figure at 8 ms. A host hands over a texture
it already has, so the bench uploads eight cards once and cycles them by handle.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
put back before the render pass); every `ffglex::Scoped*` clears to 0 on exit, so the
cells buffer is ensured and the cell texture uploaded before anything binds a texture;
`FFGLFBO::Release()` leaks the colour texture, which is why `PassBuffer::Destroy()`
deletes it first; `SetParamInfo` clamps a STANDARD default into 0..1 before
`SetParamRange` can widen it (Rows per Field and Page Number are `FF_TYPE_INTEGER`);
the core is an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS` for the
About block; the harness drives a synthetic clock; `nm | grep -q` fails under pipefail
when grep succeeds; an option's range reads back 0..1 whatever its element count;
Resolume's clock overflows a float, so the field index and the header clock are
reduced in double; 1/60 is not a double, so the field is `floor( t x 50 + 1e-6 )`
(copperlist); the page memory is CPU-side and never reallocated, so a resize cannot
clear it (photofinish's trap, avoided by construction and checked by `--carriage`).

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every check ran at 320 x 180 and 1280 x 720 in `verify.sh`.
The plugin's output is written as exactly 0.0 or 1.0 per channel into an RGBA8
target, and unorm conversion of those is exact under GL 4.1 §2.1.6.1, so a palette
pixel is byte-exact on any conforming rasteriser; nothing in these checks reads a
filtered or interpolated value.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--optimal` | the programme's realised cost against an exhaustive search over control placements | **exact equality of 64-bit integers**; the costs are quantised to 1/65536 of a squared error and both sides sum the same integers | none: no GL, no raster |
| `--gap` A, B | which cells are control cells, and what the mosaics are, in the decoded page; the boundary cell's pixels black, its neighbours red and blue | **exact**: the page is bytes; the pixels are palette bytes | the source is painted in sixel space by the plugin's own `Layout`, so the sixel means are pure colours at any raster; the pixel test uses only pixels ≥ 1e-3 dot from a boundary at a fractional scale |
| `--gap` C | every pixel of the row the target colour | **exact** palette bytes | as A |
| `--palette` | every pixel one of eight colours; every sixel (or sixel body / gutter) one colour | **exact** on the colour; the sixel a pixel belongs to is computed in double from the same `Layout`, and at a fractional scale pixels within **1e-3 dot** of a dot boundary are left out — the shader computes `( p + 0.5 - origin ) / dot` in float32, a few ULP at magnitudes ≤ 240, which is under 1e-4 dot; at a whole-pixel scale nothing is left out because dot edges are pixel edges | holds at any raster; the margin only ever removes boundary pixels |
| `--separated` | each dot of a full white cell white iff the stated gutter rule lights it; at a whole-pixel scale, exactly 28 kx ky white pixels a cell | **exact**, same margin argument | at 320 x 180 only dots that own a pixel centre are seen (a dot is 1.33 x 0.75 px in Fill); the count assertion is made only at a whole-pixel scale, and says so |
| `--carriage` | the palette colour at cell (r, 20) of every row against the harness's own field arithmetic | **exact** colour; the field index is `floor( 50 k / f + 1e-6 )` in double on both sides — the allowance is copperlist's, for the frames that sit on a field boundary | none: a flat frame is one colour at any raster; the resize case reads the resized raster |
| `--parity` | the broken cell's pixels all background (single), some changed and all on the palette (double); no pixel changed outside a rectangle one pixel larger than the cell | **exact** bytes; the one-pixel rectangle is the fractional-boundary allowance | the cell is found on the decoded page at whatever raster; at 320 x 180 a cell is 8 x 7.5 px and the strict set is 40 pixels |
| sweep | any subpixel differs | ≥ 1 | 320 x 180 and 480 x 270 |

Deliberately NOT relied on: `pow( 1, x ) == 1` (no check reads a filtered colour);
exact cancellation anywhere; the shader's float dot index at a boundary (the margin
above). What might still differ on another rasteriser: the cells pass's mean (a sum
of up to 48 x 48 `pow` decodes) will round differently, so the *encoder's choices* on
a real picture may differ by a sixel here and there between GPUs — no check asserts
the card's encoding, only structure on pure-colour sources and invariants (palette,
uniformity, parity) on the card. The DP itself is integer and must agree bit for bit
given the same means.

### The negative controls

`txtest --negative` runs nine, and `--perturb BITS` runs any check verbosely against
one. Each perturbs the *plugin's* model — a `Perturb` bit the shipped plugin carries
at zero, or a real control — never the harness's expectation.

| perturbation | what fails, measured at 320 x 180 |
| --- | --- |
| one-cell-lookahead greedy in place of the programme | `--optimal`: worse than the optimum on 12 of 24 rows |
| control codes take no cell (the plan runs past 40 bytes and is truncated) | `--gap` B: control cells at 0, 20, 21 and cell 20 not a mosaic (A survives it by the tie: a boundary code at 20 is also optimal) |
| the sixel's mean colour instead of the palette colour | `--palette`: pixels off the palette |
| Show Codes on (a real control) | `--palette`: the tinted code cells are off the palette |
| the gutter on the right and the top | `--separated`: every gutter pixel wrong |
| every row every field | `--carriage`: rows that should show an earlier frame show the last |
| a decoder that ignores parity | `--parity`: the single-bit cell is not blanked |
| a double error that flips one bit twice | `--parity`: the mosaic does not change (and the error is now a single, which fails parity) |
| errors that land the row one down | `--parity`: pixels change outside the cell |

### The mutation

One character of the shipped GLSL, on a clean committed tree (571369f): in the render
pass, `int sy = line < 3 ? 0 : ( line < 7 ? 1 : 2 )` became `( line < 6 ? 1 : 2 )` —
the middle sixel row one dot line short. Caught by **`--palette`** at both rasters
(207 pixels in a non-uniform sixel at 320 x 180). Correctly not caught by `--gap`
(solid rows), `--separated` (full mosaics: which sixel a lit dot belongs to does not
change its colour), `--carriage` (flat frames) or `--parity` (which compares the
mutated shader with itself). Reverted with `git checkout source/Shaders.cpp` and a
`touch` against the same-second make trap; the tree was clean before and after, and
the rebuilt `--palette` passes.

---

## Decisions taken without asking

- **The encoder runs on the CPU, from a read-back.** No sibling reads back; this one
  does, 92 KB a frame, because a Viterbi programme is serial and the claim of
  exactness against an exhaustive search wants integers. The read-back is the one
  stall in the plugin and is in the bench figure.
- **Row 0 is the header, over the picture.** A real page's row 0 is the header. The
  picture is always encoded as 24 rows and the header, when on, replaces row 0 of it
  (transmitted every field, like the real one); turning the header off does not
  re-fit the grid.
- **The header clock is the composition's running time**, not the wall clock: two
  renders of the same frame must be the same picture, and the sweep would otherwise
  see every control move. Format `HH:MM/SS`, in the row's own cyan.
- **"TELETEXT" is the service name.** The medium's own word; no broadcaster's.
- **Separated costs a cell** (0x1A at column 0), as it does on air.
- **Column 0 is always a code** in any row with mosaics, because a row starts
  alphanumeric and 0x11..0x17 is the only way in. Real teletext art is 39 columns
  wide for the same reason.
- **Release Mosaics is not in the programme's alphabet** (it is in the exhaustive
  search's); once on, Hold stays on for the row. Releasing only helps where a space
  would beat the held shape at a later code, and the second programme without Hold
  covers most of that.
- **Errors**: byte error rate 0.3 (1 − q)²; a quarter of errored bytes carry two
  flipped bits; packets dropped for an unrepairable address at a quarter of the byte
  rate. Stated constants, not measured from any transmitter. Errors are redrawn on
  every transmission of a row, so they flicker at the page cycle as real ones did.
- **The separated gutter is the left column and the bottom line of each block**,
  following jsbeeb's SAA5050 emulation (`setGraphicsBlock`: `xx === 0 || yy === h -
  1` cleared) and Wikipedia's "two fewer in each direction"; the datasheet's figure
  was not read. A one-line table in `Codes.h` if it is the other corner.
- **The font is graticule's 5x7**, at dot columns 0-4, lines 1-7 of the 6x10 cell.
  Not a copy of the SAA5050's 5x9 ROM.
- **Grid Fit 4:3 draws a whole-pixel integer scale where it can** (960 x 720 at
  1280 x 720: kx 4, ky 3; 1440 x 960 letterboxed at 1920 x 1080) and a fraction only
  where a dot would be under a pixel (320 x 180). The dot aspect follows: 240 x 240
  dots on a 4:3 area is a 4:3 dot, roughly the SAA5050's on a 625-line display.
- **Output alpha is 1**: a television is opaque. Mix blends the whole RGBA.
- **No presets, no OpenFX port, no browser demo, no user guide.** None required for
  0.1.0. `StoatworksAbout.h` (`guide=""`) and `ATTRIBUTIONS.md` are provisional hand
  copies; the sync will overwrite them.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-24)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320 x 180 and 1280 x 720.

- **Optimal.** 24 random rows, both error spaces: programme = exhaustive, exactly;
  Hold bounded both sides, gap 0; greedy worse on 12.
- **Gap.** A: {0, 20}; B: {0, 18, 19}; C: 0 pixels off target (the free boundary).
- **Palette.** 0 pixels off the palette, 0 in a non-uniform sixel, in 4 renders at
  each raster; 8 colours seen.
- **Separated.** 912 cells a render, 0 pixels wrong, exactly 28 kx ky white per cell
  at 1280 x 720.
- **Carriage.** 24 of 24 rows right at 60 fps, through a resize, and at 24 fps.
- **Parity.** Single: 40/40 (900/900) cell pixels background, 0 outside changed.
  Double: 0x60 → 0x63 (0x70 → 0x73 at 320 x 180), 0 outside changed. Noisy: 124 of
  960 cells fail parity, 0 pixels off the palette.
- **Negative controls.** All nine fail their check.
- **Mutation.** Caught by `--palette` at both rasters.
- **No dead controls**, all 12, at 320 x 180 and 480 x 270.
- **Every shader compiles** through `glslc`, as the plugin assembles it.
- **`--pipe`**: 2.5 frames in, 2 out; unknown cue exit 2; closed stdout exit 1.
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.teletext`, ad-hoc signs; `oxbow` reports `SW Teletext` / `TX01`
  / `effect` and renders 120 frames through `plugMain`.
- **Render cost** at the defaults, best of three runs of 60 frames after a warm-up,
  `glFinish` both sides, on a shared machine:

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280x720 | 0.67 | 4.0% |
  | 1920x1080 | 0.86 | 5.2% |
  | 2560x1440 | 1.42 | 8.5% |
  | 3840x2160 | 2.98 | 17.9% |

  With Rows per Field 24 (the whole page every field) a separate, noisier run gave
  1.8–4.9 ms. The 4K figure is the cells pass's up to 48 x 48 `pow` decodes a sixel
  plus the read-back stall.

### Assumed, or not done

- **Never loaded into Resolume.** Everything numeric was compiled, rendered and
  measured offline against the real plugin class in a headless CGL context, plus an
  `oxbow` load. No Windows build exists (CI cannot run yet). How the read-back stall
  behaves inside a busy host, and what the host's clock does over hours, is untested.
- **Never seen on footage.** The synthetic card only.
- **The gutter geometry is a reading**, see the decision above.
- **The error constants are invented.**
- **The exhaustive search with Hold is over control placements**, not over every
  mosaic, so "bounded with Hold" is a bound against a restricted optimum plus the
  provable one against the hold-off optimum — not a proof of optimality with Hold.
- **The 4K cost** is measured, not optimised: a two-stage reduction would cut the
  cells pass, and was not needed for 60 fps.
- **Nothing has been through a show.**

---

## Open questions

- **Should Hold be exact?** State would need the held character (64x the states, or a
  lazier trick that only tracks it while a run of codes is open). The realised-cost
  selection makes the approximation safe; whether the gap is ever visible on real
  footage is unmeasured.
- **Should the header be re-fitted out of the picture** (23 picture rows under the
  header) rather than covering row 0? A real page's picture never used row 0 either.
- **Release Mosaics in the programme's alphabet** — cheap to add; would it ever win?
- **A perceptual error space** (a real ΔE) instead of encoded RGB and luma weights.
- **Should errors be a rate per bit** rather than per byte, so the single/double split
  follows from independence rather than a stated quarter?
- **A wall-clock header** as an option, for a live show that wants the real time.

---

## Siblings

- **rebate** — the effect skeleton, the harness, `--pipe` (SIGPIPE ignored), the
  negative-control pattern, the clock-unit voting.
- **copperlist** — the field arithmetic and its allowance; every check at two rasters.
- **tinsel** — `PassBuffer`, `sweep.py`, CI, and the fleet's trap list.
- **graticule** — the 5x7 font.
- **nesolume**, **asciify** — attribute constraints and character cells, read for the
  shape; nothing copied.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
