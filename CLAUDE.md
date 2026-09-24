# teletext

A picture sent as Level 1 teletext mosaic graphics, as an FFGL **effect** for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.
v0.1.0 released 2026-09-24; never loaded into Resolume on macOS (Arena on Windows, software rendering, 8/9 on the fleet gate).

Read `AGENTS.md` before changing the encoder, the decoder, the transmission or the cell
geometry.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/txtest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Separated=1" --set "Rows per Field=2" --set "Grid Fit=1"`
  (0..1 for sliders and booleans, the real integer for integers, the element index for
  options: Error Space 0 RGB, 1 Luma Weighted; Grid Fit 0 4:3, 1 Fill)
- List parameters, kinds, defaults and ranges: `./build/txtest --list`
- Other sources: `--source flat --level 4` (a palette colour), `--source white`
- The exact GLSL the plugin compiles: `./build/txtest --dump-shaders DIR`
- Footage through the real plugin — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues, linearly interpolated between cues;
  a cue naming no parameter is refused with exit 2, a partial frame at the end of stdin
  ends the stream cleanly, a failed render or a closed stdout exits 1 (SIGPIPE is
  ignored, so `| head -c 1` gives 1, not 141):
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/txtest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + every check at 320x180
  AND 1280x720 + --pipe + two sweeps + the bundle + oxbow, ~2 min)
- The row programme equals an exhaustive search, exactly (no GL): `./build/txtest --optimal`
- One background cell per colour change, two per background change: `./build/txtest --gap`
- Eight colours, every sixel uniform: `./build/txtest --palette`
- The separated gutter is the SAA5050's: `./build/txtest --separated`
- Row r shows the frame of the field that carried it, through a resize: `./build/txtest --carriage`
- One bit blanks a cell, two bits change it, none moves a row: `./build/txtest --parity`
- The checks can fail: `./build/txtest --negative`; one perturbation verbosely:
  `./build/txtest --perturb BITS --gap` (bits in `Codes.h`)
- Every check takes `--size WxH`; CI runs them at 320x180
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/txtest --bench` (best of three; the GPU here is shared)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Teletext.bundle`
- The browser demo's shaders are the plugin's, character for character: `python3 demo/tools/check_shaders.py`
  (in verify.sh). The demo's CPU half (`demo/plugin.js`) is a hand port; only a reader checks it.
- Deploy the demo: `cf-run npx wrangler deploy` from the repo root (a push to main also deploys it);
  verify by content: `curl -s 'https://teletext-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`

## Notes
- **The decoder is the reference.** `Decoder.cpp` is the receiver's half of the
  standard; the encoder is judged by what the decoder makes of its bytes
  (`encoder::RealisedCost`), the harness's exhaustive search drives the decoder step by
  step, and the render pass draws exactly the cells the decoder produces.
- **The encoder runs on the CPU**, from an 80x72 read-back of the cells pass. The
  Viterbi programme is exact integer arithmetic and the harness proves it against an
  exhaustive search; that is not a shader. Cost: about 0.05 ms a row.
- **Every row starts alphanumeric, white on black**, so column 0 of any row with a
  mosaic in it is a control code. That is the standard, not a bug.
- **With backgrounds allowed the encoder is cleverer than the spec's claims**: a solid
  colour boundary costs no black cell (New Background, colour, New Background), and a
  white frame is spaces on a white background. `--gap` A and `--separated` run with
  Allow Background off for that reason; `--gap` C measures the free boundary.
- **Errors are measured on the sRGB encoding of the linear mean**, in RGB or luma
  weighted. In linear light a mid green rounds to black.
- **Hold Mosaics is approximate and bounded**: the programme runs with and without it,
  both are realised through the decoder, the cheaper is kept.
- **Nothing absolute crosses into GLSL.** The field index is `floor( t x 50 + 1e-6 )`
  in double; the header clock is the same seconds.
- **The page memory is 24 x 40 bytes on the CPU** and is never reallocated, so a resize
  cannot clear it; `--carriage` resizes halfway.
- **`Perturb` bits are test hooks**, always 0 in the plugin; they exist so `--negative`
  can prove the checks fail.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so Rows per Field and Page Number are `FF_TYPE_INTEGER`, which is exempt. Options
  are mapped by index in `Controls.cpp`; an option's range reads back 0..1.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `teletext_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `TX01`, display name `SW Teletext`.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything is measured offline on macOS, plus an
  `oxbow` load. On Windows the v0.1.0 DLL ran in Arena 7.27.1 on win-lab (software
  rendering): 8/9 on the fleet gate, Rows per Field and Freeze unprovable on a still.
- Seen on the synthetic card and, through `--pipe`, on Resolume's demo clips (the video).
- No OpenFX port, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are GENERATED by the backend's sync scripts;
  do not hand-edit them.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/teletext/teletext.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\teletext\logs\teletext.YYYY-MM-DD.log   (Windows)
