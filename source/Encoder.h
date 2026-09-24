#pragma once

#include "Codes.h"
#include "Decoder.h"

#include <cstdint>
#include <vector>

/**
	The encoder: a row of 80 x 3 target sixel colours to 40 teletext bytes.

	**The one idea.** Colour is not stored per cell in teletext. It is set by
	spacing attributes that each cost a cell, so choosing the bytes for a row
	is an optimisation under a hard serial constraint, and the Ceefax look --
	the black column at every colour boundary, colours rationed, a row that
	starts white on black in alphanumerics -- is what the optimum looks like.

	The row is a Viterbi dynamic programme over the cells with the decoder's
	row state (mode, foreground, background, hold) as the state. Each
	transition is one byte: the best mosaic for the current colours, a space,
	a mosaic colour code, New Background, Black Background, or Hold Mosaics.
	Costs are integer-quantised squared errors in the chosen colour space, so
	"exactly optimal" is a fair claim and the harness checks it against an
	exhaustive search.

	**Hold Mosaics is approximate, and bounded.** Under Hold a control cell
	shows the last mosaic received, and which one that was depends on the
	path, not the state. The programme costs a held cell as if the previous
	cell were a mosaic placed under the state's own colours -- exact when it
	is, which is the common case of a colour change between two runs. Then
	both programmes are run, with and without Hold, each byte string is
	decoded by the real decoder and costed for what it actually shows, and
	the cheaper is kept. So the realised cost with Hold on is never worse
	than the exact optimum with Hold off; `txtest --optimal` measures that,
	and the gap to an exhaustive search over control placements.
*/
namespace teletext::encoder
{

/// The colour spaces the error is measured in.
enum ErrorSpace
{
	kErrorRGB  = 0,///< equal weights in linear light
	kErrorLuma = 1,///< each channel weighted by its Rec. 709 luma share (x 3)
};

/// Squared-error costs, quantised: for cell i, sixel s and palette colour c,
/// the cost of showing colour c where the target wants sixel s of cell i.
struct CostTable
{
	int count = 0;///< cells in the row, <= 40
	int32_t d[ codes::kColumns ][ 6 ][ codes::kColours ] = {};
};

/// One unit of quantised cost: the squared error is scaled by this and
/// rounded to the nearest integer.
constexpr double kCostScale = 65536.0;

/// Build the table from `count` cells of six sixels of linear RGB, laid
/// out cell-major: rgb[ ( cell * 6 + sixel ) * 3 + channel ], sixel order
/// as in Codes.h (top-left, top-right, middle-left, ...).
void BuildCosts( const float* rgb, int count, int errorSpace, CostTable& out );

/// The best mosaic for cell i under (fg, bg): each sixel takes the nearer of
/// the two, ties to the background. Returns the sixel bits; `cost` its cost.
int BestMosaic( const CostTable& table, int cell, int fg, int bg, int64_t& cost );

/// The cost of a cell showing `bits` in (fg, bg) at cell i.
int64_t DisplayCost( const CostTable& table, int cell, int bits, int fg, int bg );

struct Options
{
	bool allowBackground = true; ///< New Background and Black Background may be used
	bool hold            = true; ///< Hold Mosaics may be used
	bool separated       = false;///< the row is prefixed with the Separated code
	int perturb          = 0;    ///< codes::Perturb bits
};

struct Result
{
	uint8_t bytes[ codes::kColumns ] = {};///< 7-bit codes, no parity
	int64_t cost                     = 0; ///< what the decoder makes of them, costed
	bool usedHold                    = false;
	int64_t plannedCost              = 0; ///< what the programme believed
};

/// Encode one row of `table.count` cells.
Result EncodeRow( const CostTable& table, const Options& options );

/// What the decoder shows for these bytes, costed against the table. The
/// judge of every encoder: `Result::cost` is this number.
int64_t RealisedCost( const CostTable& table, const uint8_t* bytes, int count );

} // namespace teletext::encoder
