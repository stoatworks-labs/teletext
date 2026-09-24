/**
 * Teletext — browser demo.
 *
 * A picture sent as Level 1 teletext mosaic graphics. The one idea, from
 * `source/Encoder.h`: colour is not stored per cell. A teletext row is 40
 * bytes, a byte below 0x20 is a control code that sets the colour from that
 * cell on, and the code itself occupies the cell — so encoding a picture is an
 * optimisation under a hard serial constraint, and the black column at every
 * colour boundary, the rationed colour and the Hold bleed are what the optimum
 * looks like.
 *
 * Like clamp and galvo, this plugin is **not only a shader**, and the two halves
 * of the page are not equally faithful:
 *
 *   The shaders are the plugin's. `VERTEX_BODY`, `CELLS_BODY` and `RENDER_BODY`
 *   below are `kVertexBody`, `kCellsBody` and `kRenderBody` from
 *   `source/Shaders.cpp`, copied across unedited, and assembled with the same
 *   `#version 410 core` line the plugin's `shaders::assemble` prepends.
 *   `demo/tools/check_shaders.py` compares them character for character and
 *   `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT — of `Codes.h`, `Decoder.cpp`, `Encoder.cpp`,
 *   `Transmission.cpp`, `Header.cpp`, `Layout.cpp`, `Controls.cpp`, the font
 *   table in `Font.cpp`, and the frame sequence in `Teletext::ProcessOpenGL` —
 *   function for function. Nothing checks a port but a reader. `txtest
 *   --optimal`, `--gap`, `--parity` and the rest check the C++ originals and
 *   have no idea this page exists. What is deliberately NOT ported: the
 *   `Perturb` test hooks (always 0 in the plugin), the greedy encoder (a
 *   negative control for the harness) and the forced-error hook, because none
 *   of them is part of what the plugin does in a host.
 *
 * ------------------------------------------------------ the readback
 *
 * The plugin reads the 80 x 72 sixel means back with `glGetTexImage( …,
 * GL_FLOAT )` every frame, runs the encoder on the CPU, and uploads a 40 x 24
 * RGBA8 cell texture. The page does exactly that: one `readPixels` of an
 * RGBA32F target as FLOAT (EXT_color_buffer_float), the encoder in JavaScript
 * — whose numbers are IEEE doubles, and whose cost sums stay well inside the
 * range where a double is exact, so the programme's integers are the same
 * integers — and a 40 x 24 RGBA8 upload. It stalls the pipeline where the
 * plugin's does.
 *
 * ------------------------------------------------------- the clock
 *
 * Fields run at 50 Hz on the page's clock — the kit's `time`, seconds since
 * the page started, paused by Pause and stepped by Step — through the same
 * `floor( t x 50 + 1e-6 )` the plugin uses. The unit vote the plugin runs
 * against Resolume's millisecond clock never runs here, because the page
 * declares seconds, as the harness does. Restart sends the clock to 0; the
 * page memory keeps what it had until fields bring the new frame in, exactly
 * as the plugin's does across a scrub.
 *
 * ------------------------------------------------------- what is missing
 *
 * **Nothing audio.** Teletext has no audio path, so there is no caveat to make.
 * **The About block is absent**, as on every page in this suite. **The two
 * integer controls are dropdowns**: the kit has no integer control, so Rows per
 * Field and Page Number are dropdowns of every value in the plugin's range, as
 * copperlist's and galvo's are.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here. The plugin
// assembles each as kVersion + body; so does `assemble` below.
//---------------------------------------------------------------------------

const VERSION = '#version 410 core\n';

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const CELLS_BODY = `
uniform sampler2D InputTexture;
uniform ivec2 InputSize;  //the picture's size in texels (not the hardware size)
uniform vec2 DrawOrigin;  //top-left of the dot grid on the output, pixels, y down
uniform vec2 DotSize;     //a dot in pixels

in vec2 uv;
out vec4 fragColor;

//The three sixel rows of a cell start at dot lines 0, 3, 7 and end at 10.
const int LineStart[ 4 ] = int[ 4 ]( 0, 3, 7, 10 );

float toLinear( float v )
{
	return v <= 0.04045 ? v / 12.92 : pow( ( v + 0.055 ) / 1.055, 2.4 );
}

void main()
{
	ivec2 sixel = ivec2( floor( gl_FragCoord.xy ) );   //x 0..79, y 0..71 (top-down)
	int cellRow = sixel.y / 3;
	int band    = sixel.y - cellRow * 3;

	//The sixel's rectangle on the output, y down.
	float x0 = DrawOrigin.x + float( sixel.x * 3 ) * DotSize.x;
	float x1 = DrawOrigin.x + float( sixel.x * 3 + 3 ) * DotSize.x;
	float y0 = DrawOrigin.y + float( cellRow * 10 + LineStart[ band ] ) * DotSize.y;
	float y1 = DrawOrigin.y + float( cellRow * 10 + LineStart[ band + 1 ] ) * DotSize.y;

	//Pixels whose centres lie inside it: p + 0.5 in [ a, b ) is p in
	//[ ceil( a - 0.5 ), ceil( b - 0.5 ) ).
	int px0 = int( ceil( x0 - 0.5 ) );
	int px1 = int( ceil( x1 - 0.5 ) );
	int py0 = int( ceil( y0 - 0.5 ) );
	int py1 = int( ceil( y1 - 0.5 ) );
	px0 = clamp( px0, 0, InputSize.x );
	px1 = clamp( px1, 0, InputSize.x );
	py0 = clamp( py0, 0, InputSize.y );
	py1 = clamp( py1, 0, InputSize.y );

	//At 4K a sixel is 48 x 36 pixels; past 48 a side the taps are strided.
	int sx = max( 1, ( px1 - px0 + 47 ) / 48 );
	int sy = max( 1, ( py1 - py0 + 47 ) / 48 );

	vec3 sum = vec3( 0.0 );
	float n  = 0.0;
	for( int y = py0; y < py1; y += sy )
	{
		int glY = InputSize.y - 1 - y;   //the texture's row 0 is the bottom
		for( int x = px0; x < px1; x += sx )
		{
			vec4 t = texelFetch( InputTexture, ivec2( x, glY ), 0 );
			sum += vec3( toLinear( t.r ), toLinear( t.g ), toLinear( t.b ) );
			n += 1.0;
		}
	}
	fragColor = n > 0.0 ? vec4( sum / n, 1.0 ) : vec4( 0.0, 0.0, 0.0, 0.0 );
}
`;

const RENDER_BODY = `
uniform sampler2D Cells;
uniform sampler2D Glyphs;      //5 x 128 by 7, R8: glyph for code c at column c * 5
uniform sampler2D Means;       //the cells pass, for the no-quantise perturbation
uniform sampler2D Source;
uniform vec2 MaxUV;
uniform ivec2 OutSize;
uniform vec2 DrawOrigin;       //top-left of the dot grid, pixels, y down
uniform vec2 DotSize;
uniform int ShowCodes;
uniform float MixAmount;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

vec3 palette( int c )
{
	return vec3( float( c & 1 ), float( ( c >> 1 ) & 1 ), float( ( c >> 2 ) & 1 ) );
}

void main()
{
	vec4 source = texture( Source, uv * MaxUV );

	//This pixel, top-down, and the dot its centre falls in.
	vec2 p = vec2( floor( gl_FragCoord.x ), float( OutSize.y ) - 1.0 - floor( gl_FragCoord.y ) );
	vec2 d = ( p + 0.5 - DrawOrigin ) / DotSize;
	int dx = int( floor( d.x ) );
	int dy = int( floor( d.y ) );

	vec3 page = vec3( 0.0 );
	if( dx >= 0 && dx < 240 && dy >= 0 && dy < 240 )
	{
		int col  = dx / 6;
		int row  = dy / 10;
		int cx   = dx - col * 6;    //dot column in the cell, 0..5
		int line = dy - row * 10;   //dot line in the cell, 0..9

		vec4 cell = texelFetch( Cells, ivec2( col, row ), 0 );
		int fg    = int( cell.r * 255.0 + 0.5 );
		int bg    = int( cell.g * 255.0 + 0.5 );
		int code  = int( cell.b * 255.0 + 0.5 );
		int flags = int( cell.a * 255.0 + 0.5 );

		bool lit = false;
		if( ( flags & 1 ) != 0 )
		{
			//A mosaic: 2 x 3 blocks of 3 x 3, 3 x 4, 3 x 3 dots. Bit 6 of
			//the code is the sixth sixel; bit 5 is what makes it a mosaic.
			int bits  = ( code & 0x1F ) | ( ( code & 0x40 ) >> 1 );
			int sx    = cx < 3 ? 0 : 1;
			int sy    = line < 3 ? 0 : ( line < 7 ? 1 : 2 );
			lit       = ( ( bits >> ( sy * 2 + sx ) ) & 1 ) != 0;
			if( ( flags & 2 ) != 0 )
			{
				//Separated: each block loses its left column and bottom line.
				bool blankColumn = ( cx - sx * 3 ) == 0;
				bool blankLine   = line == 2 || line == 6 || line == 9;
				if( ( Perturb & 128 ) != 0 )
				{
					blankColumn = ( cx - sx * 3 ) == 2;
					blankLine   = line == 0 || line == 3 || line == 7;
				}
				lit = lit && !blankColumn && !blankLine;
			}
		}
		else if( code > 32 && cx < 5 && line >= 1 && line <= 7 )
		{
			//A glyph: 5 x 7 in the cell's dot columns 0..4, lines 1..7.
			lit = texelFetch( Glyphs, ivec2( code * 5 + cx, line - 1 ), 0 ).r > 0.5;
		}

		page = palette( lit ? fg : bg );

		if( ( Perturb & 64 ) != 0 )
		{
			//The sixel's mean colour instead of the palette: not teletext.
			int sx = cx < 3 ? 0 : 1;
			int sy = line < 3 ? 0 : ( line < 7 ? 1 : 2 );
			page = texelFetch( Means, ivec2( col * 2 + sx, row * 3 + sy ), 0 ).rgb;
		}

		if( ShowCodes != 0 && ( flags & 4 ) != 0 )
			page = mix( page, vec3( 0.35, 0.35, 0.65 ), 0.5 );
	}

	fragColor = mix( source, vec4( page, 1.0 ), MixAmount );
}
`;

const assemble = (body) => VERSION + body;

//===========================================================================
// Codes.h, ported. The standard's numbers.
//===========================================================================

const K_COLUMNS = 40;
const K_ROWS = 24;
const K_SIXEL_COLS = K_COLUMNS * 2; // 80
const K_SIXEL_ROWS = K_ROWS * 3; // 72
const K_COLOURS = 8;
const K_BLACK = 0;
const K_WHITE = 7;
const K_DOTS_X = 240;
const K_DOTS_Y = 240;

const CODE = {
  mosaicColourBase: 0x10, // 0x11..0x17
  contiguous: 0x19,
  separated: 0x1a,
  blackBackground: 0x1c,
  newBackground: 0x1d,
  holdMosaics: 0x1e,
  releaseMosaics: 0x1f,
  space: 0x20,
};

const isControl = (code7) => code7 < 0x20;
const isMosaicCode = (code7) => code7 >= 0x20 && (code7 & 0x20) !== 0;
const sixelBits = (code7) => (code7 & 0x1f) | ((code7 & 0x40) >> 1);
const mosaicCode = (bits) => 0x20 | (bits & 0x1f) | ((bits & 0x20) << 1);

function popCount8(b) {
  let n = 0;
  for (let i = 0; i < 8; i += 1) n += (b >> i) & 1;
  return n;
}
const withOddParity = (code7) => {
  const data = code7 & 0x7f;
  return (popCount8(data) & 1) ? data : (data | 0x80);
};
const parityOk = (byte) => (popCount8(byte) & 1) === 1;

//===========================================================================
// Decoder.cpp, ported. The receiver's half of the standard, and the reference
// for everything else on this page: the encoder is judged by what this makes
// of its bytes, and the render pass draws exactly the cells this produces.
//===========================================================================

const newDecoderState = () => ({ fg: K_WHITE, bg: K_BLACK, mosaics: false, hold: false, separated: false, held: CODE.space });

function decodeStep(s, byte) {
  const cell = { fg: K_WHITE, bg: K_BLACK, shown: CODE.space, mosaic: false, separated: false, control: false, parityBad: false };
  cell.parityBad = !parityOk(byte);

  // A parity failure is shown as a space. Whatever the byte was meant to be
  // -- a mosaic or a colour change -- is gone for this row.
  let code = byte & 0x7f;
  if (cell.parityBad) code = CODE.space;

  if (isControl(code)) {
    cell.control = true;

    // Set-at attributes take effect on this very cell.
    switch (code) {
      case CODE.blackBackground: s.bg = K_BLACK; break;
      case CODE.newBackground: s.bg = s.fg; break;
      case CODE.holdMosaics: s.hold = true; break;
      case CODE.contiguous: s.separated = false; break;
      case CODE.separated: s.separated = true; break;
      default: break;
    }

    // What the cell shows: the held mosaic under Hold in mosaics mode,
    // otherwise a space. The colours are the ones in effect NOW -- a colour
    // code is set-after, so its own cell still wears the old foreground.
    if (s.hold && s.mosaics && isMosaicCode(s.held)) {
      cell.shown = s.held;
      cell.mosaic = true;
    } else {
      cell.shown = CODE.space;
      cell.mosaic = s.mosaics;
    }
    cell.fg = s.fg;
    cell.bg = s.bg;
    cell.separated = s.separated;

    // Set-after attributes take effect from the next cell.
    if (code >= 0x01 && code <= 0x07) {
      s.fg = code;
      if (s.mosaics) s.held = CODE.space; // a change of mode resets the held character
      s.mosaics = false;
    } else if (code >= 0x11 && code <= 0x17) {
      s.fg = code & 7;
      if (!s.mosaics) s.held = CODE.space;
      s.mosaics = true;
    } else if (code === CODE.releaseMosaics) {
      s.hold = false;
    }
    return cell;
  }

  cell.fg = s.fg;
  cell.bg = s.bg;
  cell.separated = s.separated;
  cell.shown = code;
  cell.mosaic = s.mosaics && isMosaicCode(code);
  if (cell.mosaic) s.held = code;
  return cell;
}

function decodeRow(bytes, offset, count, cells) {
  const state = newDecoderState();
  for (let i = 0; i < count; i += 1) cells[i] = decodeStep(state, bytes[offset + i]);
}

const displayedSixels = (cell) => (cell.mosaic ? sixelBits(cell.shown) : 0);

//===========================================================================
// Encoder.cpp, ported. A row of 80 x 3 target sixel colours to 40 bytes, by a
// Viterbi programme over the decoder's own row state. Costs are integers
// (squared errors quantised to 1/65536) held in doubles, which are exact to
// 2^53 -- a whole row's cost is under 2^28.
//===========================================================================

const K_COST_SCALE = 65536.0;
const K_INFINITE = Number.MAX_SAFE_INTEGER / 4;

/// The cost table: d[ ( cell * 6 + sixel ) * 8 + colour ].
function buildCosts(rgb, count, errorSpace) {
  const table = { count: Math.min(Math.max(count, 0), K_COLUMNS), d: new Float64Array(K_COLUMNS * 6 * K_COLOURS) };
  // Rec. 709 luma shares, scaled to sum to 3 so the two spaces have the same
  // total weight on a neutral error.
  const luma = errorSpace === 1;
  const w = [luma ? 0.2126 * 3.0 : 1.0, luma ? 0.7152 * 3.0 : 1.0, luma ? 0.0722 * 3.0 : 1.0];
  const encoded = [0, 0, 0];
  for (let i = 0; i < table.count; i += 1) {
    for (let s = 0; s < 6; s += 1) {
      const t = (i * 6 + s) * 3;
      // The mean is in linear light (an area average has to be); the error is
      // measured on its sRGB encoding, which is nearer to how the eye weighs a
      // mid-tone. In linear light a (0.2, 0.6, 0.2) green is 0.32 green and
      // rounds to black.
      for (let ch = 0; ch < 3; ch += 1) {
        const v = Math.min(Math.max(rgb[t + ch], 0.0), 1.0);
        encoded[ch] = v <= 0.0031308 ? 12.92 * v : 1.055 * Math.pow(v, 1.0 / 2.4) - 0.055;
      }
      for (let c = 0; c < K_COLOURS; c += 1) {
        let e = 0.0;
        for (let ch = 0; ch < 3; ch += 1) {
          const p = (c >> ch) & 1 ? 1.0 : 0.0;
          const diff = encoded[ch] - p;
          e += w[ch] * diff * diff;
        }
        table.d[(i * 6 + s) * 8 + c] = Math.round(e * K_COST_SCALE);
      }
    }
  }
  return table;
}

/// The best mosaic for cell i under (fg, bg): each sixel takes the nearer of
/// the two, ties to the background. Returns the bits; the cost goes to `out`.
function bestMosaic(table, cell, fg, bg, out) {
  let bits = 0;
  let cost = 0;
  const base = cell * 6 * 8;
  for (let s = 0; s < 6; s += 1) {
    const f = table.d[base + s * 8 + fg];
    const b = table.d[base + s * 8 + bg];
    if (f < b) {
      bits |= 1 << s;
      cost += f;
    } else {
      cost += b;
    }
  }
  out.cost = cost;
  return bits;
}

function displayCost(table, cell, bits, fg, bg) {
  let cost = 0;
  const base = cell * 6 * 8;
  for (let s = 0; s < 6; s += 1) cost += table.d[base + s * 8 + ((bits >> s) & 1 ? fg : bg)];
  return cost;
}

/// What the decoder shows for these bytes, costed against the table. The
/// judge of every encoder.
function realisedCost(table, bytes, count) {
  const state = newDecoderState();
  let cost = 0;
  for (let i = 0; i < count; i += 1) {
    const cell = decodeStep(state, withOddParity(bytes[i]));
    cost += displayCost(table, i, displayedSixels(cell), cell.fg, cell.bg);
  }
  return cost;
}

// The programme's state: the decoder's row state without the held character.
// index = ( ( mosaics * 7 + ( fg - 1 ) ) * 8 + bg ) * 2 + hold, 224 states.
const K_STATES = 2 * 7 * 8 * 2;
const indexOf = (mosaics, fg, bg, hold) => (((mosaics ? 1 : 0) * 7 + (fg - 1)) * 8 + bg) * 2 + (hold ? 1 : 0);
const STATE_OF = [];
for (let index = 0; index < K_STATES; index += 1) {
  let i = index;
  const hold = (i & 1) !== 0; i >>= 1;
  const bg = i % 8; i = Math.floor(i / 8);
  const fg = (i % 7) + 1; i = Math.floor(i / 7);
  STATE_OF.push({ mosaics: i !== 0, fg, bg, hold });
}

/// What a cell offers every state, computed once per cell: the best mosaic
/// and its cost for each ( fg, bg ), and the cost of a space on each
/// background.
function buildCache(table, cell) {
  const cache = { bits: new Int32Array(64), cost: new Float64Array(64), space: new Float64Array(8) };
  const out = { cost: 0 };
  for (let fg = 1; fg <= 7; fg += 1) {
    for (let bg = 0; bg < 8; bg += 1) {
      cache.bits[fg * 8 + bg] = bestMosaic(table, cell, fg, bg, out);
      cache.cost[fg * 8 + bg] = out.cost;
    }
  }
  for (let bg = 0; bg < 8; bg += 1) cache.space[bg] = displayCost(table, cell, 0, 0, bg);
  return cache;
}

/// The mosaic the programme assumes is held when a control code is placed at
/// cell i under state s: the best mosaic of the cell before, under the same
/// colours. Exact when that cell was a mosaic. Nothing is held at cell 0 or in
/// alphanumerics.
const assumedHeld = (previous, s) => (previous === null || !s.mosaics ? -1 : previous.bits[s.fg * 8 + s.bg]);

/// Every transition out of state s at cell i: f( cost, byte, mosaics, fg, bg, hold ).
function forEachTransition(table, cell, here, previous, s, options, f) {
  // 1. Fill the cell with the best mosaic (mosaics mode) or a space.
  if (s.mosaics) f(here.cost[s.fg * 8 + s.bg], mosaicCode(here.bits[s.fg * 8 + s.bg]), s.mosaics, s.fg, s.bg, s.hold);
  else f(here.space[s.bg], CODE.space, s.mosaics, s.fg, s.bg, s.hold);

  // The cost of a control cell: the held mosaic under Hold, else a space, in
  // the foreground now in effect and the background AFTER any set-at.
  const held = assumedHeld(previous, s);
  const controlCost = (bgAfter, holdAfter) => {
    const bits = holdAfter && s.mosaics && held >= 0 ? held : 0;
    return bits === 0 ? here.space[bgAfter] : displayCost(table, cell, bits, s.fg, bgAfter);
  };

  // 2. A mosaic colour code: set-after.
  const colourCost = controlCost(s.bg, s.hold);
  for (let c = 1; c <= 7; c += 1) {
    if (s.mosaics && c === s.fg) continue; // a no-op
    f(colourCost, CODE.mosaicColourBase | c, true, c, s.bg, s.hold);
  }

  if (options.allowBackground) {
    // 3. New Background: set-at, bg <- fg.
    if (s.bg !== s.fg) f(controlCost(s.fg, s.hold), CODE.newBackground, s.mosaics, s.fg, s.fg, s.hold);
    // 4. Black Background: set-at.
    if (s.bg !== K_BLACK) f(controlCost(K_BLACK, s.hold), CODE.blackBackground, s.mosaics, s.fg, K_BLACK, s.hold);
  }

  // 5. Hold Mosaics: set-at, and only worth issuing in mosaics mode.
  if (options.hold && !s.hold && s.mosaics) f(controlCost(s.bg, true), CODE.holdMosaics, s.mosaics, s.fg, s.bg, true);
}

/// The Viterbi programme, from cell `first`, with or without Hold in the
/// alphabet. One layer per cell: without the free-codes perturbation every
/// transition advances the cell.
function viterbi(table, first, options, allowHold, caches) {
  const o = { allowBackground: options.allowBackground, hold: allowHold };
  const count = table.count;
  const layers = [];
  const start = { cost: new Float64Array(K_STATES).fill(K_INFINITE), prevState: new Int16Array(K_STATES).fill(-1), byte: new Int16Array(K_STATES).fill(-1) };
  start.cost[indexOf(false, K_WHITE, K_BLACK, false)] = 0;
  layers.push(start);

  for (let cell = first; cell < count; cell += 1) {
    const here = caches[cell];
    const previous = cell > 0 ? caches[cell - 1] : null;
    const current = layers[layers.length - 1];
    const next = { cost: new Float64Array(K_STATES).fill(K_INFINITE), prevState: new Int16Array(K_STATES).fill(-1), byte: new Int16Array(K_STATES).fill(-1) };
    for (let s = 0; s < K_STATES; s += 1) {
      const base = current.cost[s];
      if (base >= K_INFINITE) continue;
      forEachTransition(table, cell, here, previous, STATE_OF[s], o, (cost, byte, mosaics, fg, bg, hold) => {
        const n = indexOf(mosaics, fg, bg, hold);
        const total = base + cost;
        if (total < next.cost[n]) {
          next.cost[n] = total;
          next.prevState[n] = s;
          next.byte[n] = byte;
        }
      });
    }
    layers.push(next);
  }

  // The cheapest final state, then walk back.
  const final = layers[layers.length - 1];
  let best = 0;
  for (let s = 1; s < K_STATES; s += 1) if (final.cost[s] < final.cost[best]) best = s;

  const reversed = [];
  let state = best;
  for (let layer = layers.length - 1; layer > 0; layer -= 1) {
    const l = layers[layer];
    if (l.byte[state] >= 0) reversed.push(l.byte[state]);
    state = l.prevState[state];
  }
  return { bytes: reversed.reverse(), cost: final.cost[best] };
}

function realise(table, prefix, plan) {
  const all = prefix.concat(plan.bytes);
  // Whatever the programme emitted, the row is table.count bytes long.
  const bytes = new Uint8Array(K_COLUMNS).fill(CODE.space);
  for (let i = 0; i < table.count; i += 1) bytes[i] = i < all.length ? all[i] : CODE.space;
  return { bytes, cost: realisedCost(table, bytes, table.count), plannedCost: plan.cost };
}

/// Encode one row of `table.count` cells.
function encodeRow(table, options) {
  const prefix = options.separated ? [CODE.separated] : [];
  const first = prefix.length;
  const caches = [];
  for (let cell = 0; cell < table.count; cell += 1) caches.push(buildCache(table, cell));

  const plain = realise(table, prefix, viterbi(table, first, options, false, caches));
  if (!options.hold) return plain;
  const held = realise(table, prefix, viterbi(table, first, options, true, caches));
  // The realised cost decides, so Hold can never make a row worse than the
  // exact optimum without it.
  return held.cost < plain.cost ? held : plain;
}

//===========================================================================
// Transmission.cpp, ported. The page arrives as it is broadcast: a few rows a
// field, in row order, cycling, through a channel that flips bits by an
// integer hash of ( field, row, column ). The forced-error hook is not ported.
//===========================================================================

const K_FIELD_RATE = 50.0;
const K_MAX_FIELDS_PER_FRAME = 4;
const K_BYTE_ERROR_PEAK = 0.3;
const K_DOUBLE_SHARE = 0.25;
const K_DROP_SHARE = 0.25;

/// Three words folded into one LCG state, then PCG-XSH-RR's output mix, in
/// 32-bit unsigned arithmetic (Math.imul wraps as the C++ uint32_t does).
function hash(a, b, c) {
  let state = (Math.imul(a, 747796405) + 2891336453) >>> 0;
  state = (Math.imul(state ^ Math.imul(b, 2654435761), 747796405) + 2891336453) >>> 0;
  state = (Math.imul(state ^ Math.imul(c, 2246822519), 747796405) + 2891336453) >>> 0;
  const word = Math.imul((state >>> ((state >>> 28) + 4)) ^ state, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

const fieldIndex = (seconds) => Math.floor(Math.max(seconds, 0.0) * K_FIELD_RATE + 1e-6);
const cycleLength = (rowsPerField) => Math.floor((K_ROWS + rowsPerField - 1) / rowsPerField);

function rowsOfField(field, rowsPerField) {
  const r = Math.min(Math.max(rowsPerField, 1), K_ROWS);
  const cycle = cycleLength(r);
  const slot = ((field % cycle) + cycle) % cycle;
  const first = slot * r;
  return [first, Math.min(K_ROWS, first + r)];
}

class Page {
  constructor() {
    this.bytes = new Uint8Array(K_ROWS * K_COLUMNS);
    this.reset();
  }

  /// Forget everything: all spaces, no field seen.
  reset() {
    this.bytes.fill(withOddParity(CODE.space));
    this.lastField = -1;
  }

  /// Run the fields elapsed up to `field`. `encode( r )` returns the 40
  /// seven-bit codes for row r from the current picture; `header()` does the
  /// same for row 0 when a header is on (or is null). Returns how many
  /// fields were transmitted.
  advance(field, rowsPerField, quality, frozen, encode, header) {
    if (field <= this.lastField) return 0; // no new field (or the clock went backwards)

    // Every field elapsed, up to the cap; a longer gap is a jump and runs
    // only the field it lands on.
    let from = this.lastField < 0 ? field : this.lastField + 1;
    if (field - from + 1 > K_MAX_FIELDS_PER_FRAME) from = field;

    let sent = 0;
    if (!frozen) {
      for (let f = from; f <= field; f += 1) {
        this.transmitField(f, rowsPerField, quality, encode, header);
        sent += 1;
      }
    }
    this.lastField = field;
    return sent;
  }

  transmitField(field, rowsPerField, quality, encode, header) {
    const [first, last] = rowsOfField(field, rowsPerField);
    // The header goes out every field, before the page rows, as row 0.
    if (header) this.receive(0, header(), field, quality);
    for (let r = first; r < last; r += 1) {
      if (header && r === 0) continue; // row 0 is the header this field
      this.receive(r, encode(r), field, quality);
    }
  }

  receive(row, codes7, field, quality) {
    const f = field >>> 0;
    const q = Math.min(Math.max(quality, 0.0), 1.0);
    const pByte = K_BYTE_ERROR_PEAK * (1.0 - q) * (1.0 - q);

    // The row address. Hamming 8/4 corrects one bit and detects two: a packet
    // with two errors in its address is dropped, and the row keeps what it
    // had. An error never moves a row.
    const addressDraw = hash(f, row, 0xadd3);
    if (addressDraw < Math.floor(pByte * K_DROP_SHARE * 4294967296.0)) return;

    const pDraw = Math.floor(pByte * 4294967296.0);
    const doubleShare = Math.floor(K_DOUBLE_SHARE * 1000.0);
    for (let c = 0; c < K_COLUMNS; c += 1) {
      let byte = withOddParity(codes7[c]);
      const draw = hash(f, row, c);
      if (draw < pDraw) {
        const which = hash(f, row, c + 0x1000);
        const bitA = which & 7;
        byte ^= 1 << bitA;
        // A double error: the second flipped bit is another one.
        const isDouble = ((which >>> 8) % 1000) < doubleShare;
        if (isDouble) {
          const bitB = (bitA + 1 + ((which >>> 16) % 7)) & 7;
          byte ^= 1 << bitB;
        }
      }
      this.bytes[row * K_COLUMNS + c] = byte & 0xff;
    }
  }
}

//===========================================================================
// Header.cpp, ported. Row 0: page number, a service name, the page again and
// a running clock, in alphanumerics with their own colour codes. The clock is
// the composition's running time, not the wall clock.
//===========================================================================

const pad3 = (n) => String(n).padStart(3, '0');
const pad2 = (n) => String(n).padStart(2, '0');

function composeHeader(pageNumber, seconds) {
  const out = new Uint8Array(K_COLUMNS).fill(CODE.space);
  const t = Math.max(seconds, 0.0);
  const s = Math.floor(t + 1e-6);
  const hours = Math.floor(s / 3600) % 24;
  const minutes = Math.floor(s / 60) % 60;
  const secs = s % 60;

  // "P100" white, then a yellow service name, the page in white again, and
  // the clock in cyan at the right-hand end. Each colour code takes a cell.
  const text = `P${pad3(pageNumber)} \x03TELETEXT \x07${pad3(pageNumber)}`;
  const clock = `\x06${pad2(hours)}:${pad2(minutes)}/${pad2(secs)}`;
  for (let i = 0; i < text.length && i < K_COLUMNS; i += 1) out[i] = text.charCodeAt(i) & 0x7f;
  const clockStart = K_COLUMNS - clock.length;
  for (let i = 0; i < clock.length; i += 1) out[clockStart + i] = clock.charCodeAt(i) & 0x7f;
  return out;
}

//===========================================================================
// Layout.cpp, ported. Where the 240 x 240-dot page sits on the output and
// how big a dot is: the 4:3 safe area centred, or the whole frame, scaled by
// whole pixels where the region allows at least a pixel a dot.
//===========================================================================

const GRID_FIT_NAMES = ['4:3', 'Fill'];

function computeLayout(gridFit, outWidth, outHeight) {
  const l = { outWidth: Math.max(outWidth, 1), outHeight: Math.max(outHeight, 1) };
  if (gridFit === 0) {
    // The largest 4:3 rectangle in the frame, centred, in whole pixels.
    let w = l.outWidth;
    let h = l.outHeight;
    if (w * 3 > h * 4) w = Math.floor((h * 4) / 3);
    else h = Math.floor((w * 3) / 4);
    l.regionW = Math.max(w, 1);
    l.regionH = Math.max(h, 1);
  } else {
    l.regionW = l.outWidth;
    l.regionH = l.outHeight;
  }
  l.regionX = Math.floor((l.outWidth - l.regionW) / 2);
  l.regionY = Math.floor((l.outHeight - l.regionH) / 2);

  const kx = Math.floor(l.regionW / K_DOTS_X);
  const ky = Math.floor(l.regionH / K_DOTS_Y);
  if (kx >= 1 && ky >= 1) {
    l.wholePixel = true;
    l.dotW = kx;
    l.dotH = ky;
    l.drawW = kx * K_DOTS_X;
    l.drawH = ky * K_DOTS_Y;
    // Centred in whole pixels, so every dot edge is a pixel edge.
    l.drawX = l.regionX + Math.floor((l.regionW - l.drawW) / 2);
    l.drawY = l.regionY + Math.floor((l.regionH - l.drawH) / 2);
  } else {
    l.wholePixel = false;
    l.dotW = l.regionW / K_DOTS_X;
    l.dotH = l.regionH / K_DOTS_Y;
    l.drawX = l.regionX;
    l.drawY = l.regionY;
    l.drawW = l.regionW;
    l.drawH = l.regionH;
  }
  return l;
}

//===========================================================================
// Controls.cpp, ported.
//===========================================================================

const optionIndex = (value, count) => Math.min(count - 1, Math.max(0, Math.round(value)));
const K_ROWS_MIN = 1;
const K_ROWS_MAX = 24;
const K_PAGE_MIN = 100;
const K_PAGE_MAX = 899;
const rowsPerFieldOf = (value) => Math.min(Math.max(Math.round(value), K_ROWS_MIN), K_ROWS_MAX);
const pageNumberOf = (value) => Math.min(Math.max(Math.round(value), K_PAGE_MIN), K_PAGE_MAX);
const signalQualityOf = (value) => Math.min(Math.max(value, 0.0), 1.0);

//===========================================================================
// Font.cpp, copied: graticule's 5 x 7 bitmap font, codes 32..127, one string
// of five dots per line. Built into the 640 x 7 R8 texture the render pass
// reads, glyph c at column c * 5.
//===========================================================================

const FONT_GLYPHS = [
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 32
  ['..#..', '..#..', '..#..', '..#..', '.....', '.....', '..#..'], // 33
  ['.#.#.', '.#.#.', '.#.#.', '.....', '.....', '.....', '.....'], // 34
  ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'], // 35
  ['..#..', '.####', '#.#..', '.###.', '..#.#', '####.', '..#..'], // 36
  ['##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##'], // 37
  ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'], // 38
  ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'], // 39
  ['...#.', '..#..', '.#...', '.#...', '.#...', '..#..', '...#.'], // 40
  ['.#...', '..#..', '...#.', '...#.', '...#.', '..#..', '.#...'], // 41
  ['.....', '..#..', '#.#.#', '.###.', '#.#.#', '..#..', '.....'], // 42
  ['.....', '..#..', '..#..', '#####', '..#..', '..#..', '.....'], // 43
  ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'], // 44
  ['.....', '.....', '.....', '#####', '.....', '.....', '.....'], // 45
  ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'], // 46
  ['.....', '....#', '...#.', '..#..', '.#...', '#....', '.....'], // 47
  ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'], // 48
  ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 49
  ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'], // 50
  ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'], // 51
  ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'], // 52
  ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'], // 53
  ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'], // 54
  ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'], // 55
  ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'], // 56
  ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'], // 57
  ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'], // 58
  ['.....', '.##..', '.##..', '.....', '.##..', '..#..', '.#...'], // 59
  ['...#.', '..#..', '.#...', '#....', '.#...', '..#..', '...#.'], // 60
  ['.....', '.....', '#####', '.....', '#####', '.....', '.....'], // 61
  ['.#...', '..#..', '...#.', '....#', '...#.', '..#..', '.#...'], // 62
  ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'], // 63
  ['.###.', '#...#', '....#', '.##.#', '#.#.#', '#.#.#', '.###.'], // 64
  ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 65
  ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'], // 66
  ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'], // 67
  ['###..', '#..#.', '#...#', '#...#', '#...#', '#..#.', '###..'], // 68
  ['#####', '#....', '#....', '####.', '#....', '#....', '#####'], // 69
  ['#####', '#....', '#....', '####.', '#....', '#....', '#....'], // 70
  ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'], // 71
  ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 72
  ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 73
  ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'], // 74
  ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'], // 75
  ['#....', '#....', '#....', '#....', '#....', '#....', '#####'], // 76
  ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'], // 77
  ['#...#', '#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#'], // 78
  ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 79
  ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'], // 80
  ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'], // 81
  ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'], // 82
  ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'], // 83
  ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 84
  ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 85
  ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 86
  ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'], // 87
  ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'], // 88
  ['#...#', '#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..'], // 89
  ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'], // 90
  ['.###.', '.#...', '.#...', '.#...', '.#...', '.#...', '.###.'], // 91
  ['.....', '#....', '.#...', '..#..', '...#.', '....#', '.....'], // 92
  ['.###.', '...#.', '...#.', '...#.', '...#.', '...#.', '.###.'], // 93
  ['..#..', '.#.#.', '#...#', '.....', '.....', '.....', '.....'], // 94
  ['.....', '.....', '.....', '.....', '.....', '.....', '#####'], // 95
  ['.#...', '..#..', '...#.', '.....', '.....', '.....', '.....'], // 96
  ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'], // 97
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '####.'], // 98
  ['.....', '.....', '.###.', '#....', '#....', '#...#', '.###.'], // 99
  ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'], // 100
  ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'], // 101
  ['..##.', '.#..#', '.#...', '###..', '.#...', '.#...', '.#...'], // 102
  ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'], // 103
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 104
  ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'], // 105
  ['...#.', '.....', '..##.', '...#.', '...#.', '#..#.', '.##..'], // 106
  ['#....', '#....', '#..#.', '#.#..', '##...', '#.#..', '#..#.'], // 107
  ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 108
  ['.....', '.....', '##.#.', '#.#.#', '#.#.#', '#...#', '#...#'], // 109
  ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 110
  ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'], // 111
  ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'], // 112
  ['.....', '.....', '.####', '#...#', '.####', '....#', '....#'], // 113
  ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'], // 114
  ['.....', '.....', '.####', '#....', '.###.', '....#', '####.'], // 115
  ['.#...', '.#...', '###..', '.#...', '.#...', '.#..#', '..##.'], // 116
  ['.....', '.....', '#...#', '#...#', '#...#', '#..##', '.##.#'], // 117
  ['.....', '.....', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 118
  ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'], // 119
  ['.....', '.....', '#...#', '.#.#.', '..#..', '.#.#.', '#...#'], // 120
  ['.....', '.....', '#...#', '#...#', '.####', '....#', '.###.'], // 121
  ['.....', '.....', '#####', '...#.', '..#..', '.#...', '#####'], // 122
  ['...#.', '..#..', '..#..', '.#...', '..#..', '..#..', '...#.'], // 123
  ['..#..', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 124
  ['.#...', '..#..', '..#..', '...#.', '..#..', '..#..', '.#...'], // 125
  ['.....', '.#...', '#.#.#', '...#.', '.....', '.....', '.....'], // 126
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 127
];

const FONT_FIRST = 32;
const FONT_WIDTH = 5;
const FONT_HEIGHT = 7;
const FONT_TEXTURE_WIDTH = FONT_WIDTH * 128;

function fontTexture() {
  const out = new Uint8Array(FONT_TEXTURE_WIDTH * FONT_HEIGHT);
  for (let code = 0; code < 128; code += 1) {
    const glyph = FONT_GLYPHS[code - FONT_FIRST];
    if (!glyph) continue;
    for (let y = 0; y < FONT_HEIGHT; y += 1) {
      for (let x = 0; x < FONT_WIDTH; x += 1) {
        if (glyph[y][x] === '#') out[y * FONT_TEXTURE_WIDTH + code * FONT_WIDTH + x] = 255;
      }
    }
  }
  return out;
}

//===========================================================================
// The renderer: Teletext::ProcessOpenGL, in its order.
//
//   1. cells      80 x 72, RGBA32F: each sixel's mean in linear light
//   2. read-back  92 KB, readPixels as FLOAT
//   3. transmit   the fields elapsed, each carrying its rows, encoded on the CPU
//   4. decode     every row to what each cell shows; a 40 x 24 RGBA8 upload
//   5. render     the SAA5050's 6 x 10 cell at every output pixel, onto the canvas
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { field: -1, sent: 0, rowsPerField: 4, encodeMillis: 0, parityBad: 0, controls: 0 };

function createNearestTexture(gl) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return texture;
}

function createRenderer(gl, quad) {
  const cellsShader = new Program(gl, assemble(VERTEX_BODY), assemble(CELLS_BODY), 'cells');
  const renderShader = new Program(gl, assemble(VERTEX_BODY), assemble(RENDER_BODY), 'render');
  const cells = new PassBuffer(gl, { filter: 'nearest' });

  // The font, once. Nearest and unfiltered: a glyph dot is a dot.
  const glyphTexture = createNearestTexture(gl);
  gl.bindTexture(gl.TEXTURE_2D, glyphTexture);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, FONT_TEXTURE_WIDTH, FONT_HEIGHT, 0, gl.RED, gl.UNSIGNED_BYTE, fontTexture());
  gl.bindTexture(gl.TEXTURE_2D, null);

  // The cell texture: 40 x 24, rewritten every frame.
  const cellTexture = createNearestTexture(gl);
  gl.bindTexture(gl.TEXTURE_2D, cellTexture);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, K_COLUMNS, K_ROWS, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const means = new Float32Array(K_SIXEL_COLS * K_SIXEL_ROWS * 4);
  const cellBytes = new Uint8Array(K_ROWS * K_COLUMNS * 4);
  const rowRGB = new Float32Array(K_COLUMNS * 6 * 3);
  const decoded = new Array(K_COLUMNS);
  const page = new Page();

  // Teletext::encodeRow: the row's 40 cells x 6 sixels of linear RGB, from
  // the read-back means, costed and encoded.
  const encodeOne = (row, errorSpace, options) => {
    for (let i = 0; i < K_COLUMNS; i += 1) {
      for (let s = 0; s < 6; s += 1) {
        const sx = i * 2 + (s & 1);
        const sy = row * 3 + (s >> 1);
        const texel = (sy * K_SIXEL_COLS + sx) * 4;
        const target = (i * 6 + s) * 3;
        rowRGB[target] = means[texel];
        rowRGB[target + 1] = means[texel + 1];
        rowRGB[target + 2] = means[texel + 2];
      }
    }
    return encodeRow(buildCosts(rowRGB, K_COLUMNS, errorSpace), options).bytes;
  };

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      const p = (id) => params.get(id);
      const picture = input;
      const width = picture.width;
      const height = picture.height;

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const errorSpace = optionIndex(p('errorSpace'), 2);
      const options = {
        hold: p('hold') > 0.5,
        allowBackground: p('allowBackground') > 0.5,
        separated: p('separated') > 0.5,
      };
      const rowsPerField = rowsPerFieldOf(integerValue('rowsPerField', p('rowsPerField')));
      const quality = signalQualityOf(p('signalQuality'));
      const frozen = p('freeze') > 0.5;
      const gridFit = optionIndex(p('gridFit'), 2);
      const headerOn = p('header') > 0.5;
      const pageNumber = pageNumberOf(integerValue('pageNumber', p('pageNumber')));
      const showCodes = p('showCodes') > 0.5;
      const mixAmount = Math.min(Math.max(p('mix'), 0), 1);

      // The host hands the plugin an input the size of the output; the kit
      // renders its clip at the canvas size, so the layout is the picture's.
      const lay = computeLayout(gridFit, width, height);

      //------------------------------------------------------------------
      // 1. Cells: the sixel means, in linear light.
      //------------------------------------------------------------------
      cells.ensure(K_SIXEL_COLS, K_SIXEL_ROWS, gl.RGBA32F);
      cells.bind();
      gl.disable(gl.BLEND);
      cellsShader.use();
      bindTexture(gl, 0, picture.texture);
      cellsShader.setSampler('InputTexture', 0);
      gl.uniform2i(cellsShader.location('InputSize'), width, height);
      cellsShader.set('DrawOrigin', lay.drawX, lay.drawY);
      cellsShader.set('DotSize', lay.dotW, lay.dotH);
      quad.draw();

      // Read them back: 80 x 72 x 4 floats. The one stall, and the price of
      // an encoder that is exact.
      gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
      gl.readPixels(0, 0, K_SIXEL_COLS, K_SIXEL_ROWS, gl.RGBA, gl.FLOAT, means);

      //------------------------------------------------------------------
      // 2. The transmission: the fields elapsed since the last frame, each
      // carrying its rows of the page encoded from THIS frame's means.
      //------------------------------------------------------------------
      const started = performance.now();
      const field = fieldIndex(time);
      const header = headerOn ? () => composeHeader(pageNumber, time) : null;
      const sent = page.advance(field, rowsPerField, quality, frozen, (row) => encodeOne(row, errorSpace, options), header);

      //------------------------------------------------------------------
      // 3. Decode every row and upload the cell texture: R fg, G bg, B the
      // code shown, A flags (1 mosaic, 2 separated, 4 control).
      //------------------------------------------------------------------
      let parityBad = 0;
      let controls = 0;
      for (let r = 0; r < K_ROWS; r += 1) {
        decodeRow(page.bytes, r * K_COLUMNS, K_COLUMNS, decoded);
        for (let c = 0; c < K_COLUMNS; c += 1) {
          const cell = decoded[c];
          const px = (r * K_COLUMNS + c) * 4;
          cellBytes[px] = cell.fg;
          cellBytes[px + 1] = cell.bg;
          cellBytes[px + 2] = cell.shown;
          cellBytes[px + 3] = (cell.mosaic ? 1 : 0) | (cell.separated ? 2 : 0) | (cell.control ? 4 : 0);
          if (cell.parityBad) parityBad += 1;
          if (cell.control) controls += 1;
        }
      }
      gl.bindTexture(gl.TEXTURE_2D, cellTexture);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, K_COLUMNS, K_ROWS, gl.RGBA, gl.UNSIGNED_BYTE, cellBytes);
      gl.bindTexture(gl.TEXTURE_2D, null);

      telemetry.encodeMillis = performance.now() - started;
      telemetry.field = page.lastField;
      telemetry.sent = sent;
      telemetry.rowsPerField = rowsPerField;
      telemetry.parityBad = parityBad;
      telemetry.controls = controls;

      //------------------------------------------------------------------
      // 4. Render, onto the canvas. The host's viewport is the whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);

      renderShader.use();
      bindTexture(gl, 0, cellTexture);
      bindTexture(gl, 1, glyphTexture);
      bindTexture(gl, 2, cells.texture);
      bindTexture(gl, 3, picture.texture);
      renderShader.setSampler('Cells', 0);
      renderShader.setSampler('Glyphs', 1);
      renderShader.setSampler('Means', 2);
      renderShader.setSampler('Source', 3);
      renderShader.set('MaxUV', 1.0, 1.0);
      gl.uniform2i(renderShader.location('OutSize'), vpW, vpH);
      renderShader.set('DrawOrigin', lay.drawX, lay.drawY);
      renderShader.set('DotSize', lay.dotW, lay.dotH);
      renderShader.setInt('ShowCodes', showCodes ? 1 : 0);
      renderShader.set('MixAmount', mixAmount);
      renderShader.setInt('Perturb', 0);
      quad.draw();

      // Unbind so nothing reads a framebuffer's own texture next frame.
      bindTexture(gl, 1, null);
      bindTexture(gl, 2, null);
      bindTexture(gl, 3, null);
      gl.activeTexture(gl.TEXTURE0);
    },
  };
}

//===========================================================================
// The controls, read out of Teletext::Teletext(). Same names, same groups,
// same order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores these
/// as the integer itself. The kit has no integer control, so -- as copperlist
/// and galvo did -- they are dropdowns of every value in the plugin's range;
/// `integerValue` turns the dropdown's index back into it.
const INTEGER_RANGES = {
  rowsPerField: [K_ROWS_MIN, K_ROWS_MAX],
  pageNumber: [K_PAGE_MIN, K_PAGE_MAX],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: value - INTEGER_RANGES[id][0], group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const demo = mountDemo({
  name: 'Teletext',
  pluginId: 'TX01',
  tagline:
    'A picture sent as Level 1 teletext mosaic graphics. Forty columns by twenty-four rows of cells, eight colours, 2 × 3 sixels — and colour is not stored per cell: it is set by a control code that costs a cell, so every colour boundary wears a column of black, colour is rationed, and Hold Graphics bleeds the last shape into the gap. An exactly optimal encoder plans each row, the page arrives a few rows a field and tears between rows, and bit errors land the teletext way. The shaders here are the plugin’s own; the encoder, the transmission and the decoder are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/teletext',
  page: 'https://stoatworks-labs.com/software/teletext/',

  // The stock sentence says "same maths", which is only half true here: the
  // shaders are the plugin's, the encoder between them is a port.
  blurb:
    'It is Teletext’s own GLSL, ported from the repository to WebGL2, with the CPU half — the Viterbi row encoder, the field-by-field transmission and its bit errors, and the decoder — ported to JavaScript by hand; nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // The cells buffer is RGBA32F and read back as floats: the encoder costs
  // the sixel means in double, and a byte readback would quantise them first.
  needFloat: true,

  params: [
    opt('errorSpace', 'Error Space', ['RGB', 'Luma Weighted'], 0, 'Encoder',
      'How the distance from a sixel’s mean colour to each of the eight is measured: the three channels equally, or each weighted by its Rec. 709 luma share (×3), so brightness is kept at the expense of hue. Measured on the sRGB encoding of the linear mean.'),
    bool('hold', 'Hold Graphics', 1, 'Encoder',
      'Under Hold a control cell shows the last mosaic received instead of a space, in whatever colours are in effect there. The encoder runs the programme with and without Hold, decodes both, and keeps the cheaper.'),
    bool('allowBackground', 'Allow Background', 1, 'Encoder',
      'Whether the encoder may use New Background and Black Background. A new background costs two codes, but once it may move a solid boundary costs no black at all.'),
    bool('separated', 'Separated', 0, 'Encoder',
      'Separated mosaics: each block loses its left column and bottom line. Costs a cell — the code goes at column 0 of every row.'),

    integer('rowsPerField', 'Rows per Field', 4, 'Transmission',
      'How many rows each 50 Hz field carries, in row order, cycling. The page comes round every ⌈24 / R⌉ fields: six at 4, twenty-four at 1, one at 24.'),
    std('signalQuality', 'Signal Quality', 1.0, 'Transmission', {
      display: (v) => `${(100 * K_BYTE_ERROR_PEAK * (1 - v) * (1 - v)).toFixed(1)}% of bytes errored`,
      hint: '1 is a clean signal. Below it, bytes arrive with bit errors at 0.3 × (1 − q)²: a single bit fails parity and blanks the cell, two bits pass as the wrong character, and a row never moves.',
    }),
    bool('freeze', 'Freeze', 0, 'Transmission',
      'Holds the page memory: no field is transmitted while it is on, like the Hold key on a teletext remote.'),

    opt('gridFit', 'Grid Fit', GRID_FIT_NAMES, 0, 'Display',
      'Where the 240 × 240 dots go: the largest 4:3 rectangle centred in the frame (the teletext safe area), or the whole frame. Scaled by whole pixels wherever a dot is at least a pixel.'),
    bool('header', 'Header', 1, 'Display',
      'A real row 0: page number, the service name TELETEXT, the page again and a running clock, transmitted every field. It covers row 0 of the picture.'),
    integer('pageNumber', 'Page Number', 100, 'Display',
      'The number the header shows, as a magazine page. Changes nothing but the header.'),
    bool('showCodes', 'Show Codes', 0, 'Display',
      'A diagnostic: every cell holding a control code is tinted blue-grey, so you can count what each row cost. Not eight colours any more.'),
    std('mix', 'Mix', 1.0, 'Display'),
  ],

  // Bold shapes and solid colours are what the encoder shows best; the scene
  // moves, so the row-by-row arrival shows. Fine detail is what it destroys.
  sources: ['scene', 'bars', 'grid', 'spot', 'ramp', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'The gap, laid bare': { hold: 0, allowBackground: 0 },
    'One row a field': { rowsPerField: 0 },
    'Whole page every field': { rowsPerField: 23 },
    'Bad reception': { signalQuality: 0.45 },
    'Separated, no header': { separated: 1, header: 0 },
    'Fill the frame': { gridFit: 1 },
    'Count the codes': { showCodes: 1 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Teletext reads the 80 × 72 sixel means back to the CPU and runs a Viterbi programme per row over the decoder’s state, transmits the page a few rows per 50 Hz field through a channel that flips bits, and decodes it — Encoder.cpp, Transmission.cpp, Decoder.cpp, Header.cpp, Layout.cpp and the frame sequence in Teletext::ProcessOpenGL. All of that is ported here function for function, because without it the page would have no cells to draw. Nothing checks a port but a reader; the repository’s txtest checks the C++ against an exhaustive search and has never heard of this page.',
    'The GPU half is not a port. The cells pass and the render pass are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of either shader (or the shared vertex shader) drifts.',
    'The sixel means are read back from the GPU with readPixels as floats every frame, exactly as the plugin’s glGetTexImage does, and stall the pipeline where it does. The cost table is built from the same floats in double, quantised to the same integers.',
    'Fields run on the page’s clock with its unit declared as seconds, as the repository’s harness declares it. The plugin votes on Resolume’s clock unit over its first frames; that vote never runs here. Restart sends the clock to zero and the page memory keeps what it had, exactly as the plugin’s does across a scrub.',
    'Rows per Field and Page Number are FF_TYPE_INTEGER in the plugin. The kit has no integer control, so they are dropdowns of every value in the plugin’s range.',
    'The plugin’s Perturb test hooks, its greedy encoder (a negative control) and its forced-error hook are not ported: none of them is part of what the plugin does in a host.',
    'There is no audio caveat on this page: Teletext has no audio path.',
    'The plugin’s proof — the programme against an exhaustive search, the gap, the palette, the gutter, the carriage, the parity cases — is an offline harness in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported transmission did.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas. It reports the ported transmission's own numbers:
// which field was last transmitted, how many fields this frame carried, how
// many control cells the page cost and how many cells failed parity. Skipped
// in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const { field, rowsPerField, encodeMillis, parityBad, controls } = telemetry;
      if (field < 0) return;
      line.textContent =
        `Field ${field.toLocaleString('en-GB')} transmitted, ${rowsPerField} row${rowsPerField === 1 ? '' : 's'} a field `
        + `(the page every ${cycleLength(rowsPerField)} field${cycleLength(rowsPerField) === 1 ? '' : 's'}). `
        + `${controls} of 960 cells are control codes, ${parityBad} failed parity. `
        + `${encodeMillis.toFixed(1)} ms for the read-back, the encoder and the decode.`;
    }, 250);
  }
}
