#pragma once

#include "Codes.h"

#include <cstdint>

/**
	The decoder: one row of received bytes to what each cell shows.

	This is the receiver's half of the standard, and it is the reference for
	everything else in the repo: the encoder's Viterbi programme is judged by
	what THIS makes of its bytes (`Encoder::RealisedCost`), the harness's
	exhaustive search drives this step by step, and the render pass draws
	exactly the cells this produces.

	Parity: a byte whose parity is even is a transmission error, and the
	decoder shows a space in its place -- a mosaic loses its shape, and a
	control code loses its effect for the rest of the row.
*/
namespace teletext::decoder
{

/// What one cell shows.
struct Cell
{
	uint8_t fg     = codes::kWhite;///< 0..7
	uint8_t bg     = codes::kBlack;///< 0..7
	uint8_t shown  = codes::kSpace;///< the 7-bit code drawn: a mosaic, a letter, or a space
	bool mosaic    = false;        ///< `shown` is a mosaic character (draw sixels, not a glyph)
	bool separated = false;        ///< separated mosaics in effect
	bool control   = false;        ///< the received code was a spacing attribute
	bool parityBad = false;        ///< the received byte failed parity
};

/// The row state the decoder carries from cell to cell.
struct State
{
	uint8_t fg    = codes::kWhite;
	uint8_t bg    = codes::kBlack;
	bool mosaics  = false;///< mosaics mode (after a 0x11..0x17), else alphanumerics
	bool hold     = false;
	bool separated = false;
	uint8_t held  = codes::kSpace;///< the last mosaic character received in mosaics mode
};

/// Decode one received byte in `state` (which it advances), returning what
/// the cell shows. `ignoreParity` is the kPerturbIgnoreParity hook.
Cell Step( State& state, uint8_t byte, bool ignoreParity = false );

/// A whole row: `count` bytes to `count` cells.
void Row( const uint8_t* bytes, int count, Cell* cells, bool ignoreParity = false );

/// The sixel bits a cell displays: its mosaic's bits, or 0 for a space or a
/// letter (letters are drawn from the font, not as sixels).
inline int DisplayedSixels( const Cell& cell )
{
	return cell.mosaic ? codes::SixelBits( cell.shown ) : 0;
}

} // namespace teletext::decoder
