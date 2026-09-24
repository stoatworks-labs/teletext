#pragma once

#include <cstdint>

/**
	Level 1 teletext, the parts a mosaic picture needs.

	A page is 24 rows of 40 character cells. Each cell holds one 7-bit code
	carried in a byte with odd parity. Codes below 0x20 are spacing
	attributes: they occupy the cell and show as background (or, under Hold
	Mosaics, as the last mosaic character), and they change the state of the
	row from that cell onwards. Which cell -- this one or the next -- depends
	on the code: colour codes are "set-after" (the cell they sit in still
	shows the old colours), New Background, Black Background, Hold Mosaics and
	the contiguous/separated switch are "set-at".

	Every row starts alphanumeric, white on black, hold off, contiguous, with
	no held character. Nothing carries over from the row above.

	These are the facts of ETS 300 706 (the teletext specification) as
	implemented by every decoder; see AGENTS.md for what is a reading.
*/
namespace teletext::codes
{
constexpr int kColumns = 40;
constexpr int kRows    = 24;

/// Sixels per cell: 2 wide, 3 high.
constexpr int kSixelsX  = 2;
constexpr int kSixelsY  = 3;
constexpr int kSixelCols = kColumns * kSixelsX;///< 80
constexpr int kSixelRows = kRows * kSixelsY;   ///< 72

/// The eight colours, indexed by their 3-bit code: bit 0 red, bit 1 green,
/// bit 2 blue. 0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta,
/// 6 cyan, 7 white.
constexpr int kColours = 8;
constexpr int kBlack   = 0;
constexpr int kWhite   = 7;

/// Spacing attributes (7-bit values).
constexpr uint8_t kAlphaColourBase  = 0x00;///< 0x01..0x07: alphanumerics, colour = code
constexpr uint8_t kMosaicColourBase = 0x10;///< 0x11..0x17: mosaics, colour = code & 7
constexpr uint8_t kContiguous       = 0x19;
constexpr uint8_t kSeparated        = 0x1A;
constexpr uint8_t kBlackBackground  = 0x1C;
constexpr uint8_t kNewBackground    = 0x1D;
constexpr uint8_t kHoldMosaics      = 0x1E;
constexpr uint8_t kReleaseMosaics   = 0x1F;
constexpr uint8_t kSpace            = 0x20;

inline bool IsControl( uint8_t code7 )
{
	return code7 < 0x20;
}

/// In mosaics mode, codes 0x20..0x3F and 0x60..0x7F are mosaic characters;
/// 0x40..0x5F stay as capital letters.
inline bool IsMosaicCode( uint8_t code7 )
{
	return code7 >= 0x20 && ( code7 & 0x20 ) != 0;
}

/// The six sixel bits of a mosaic character: bit 0 top-left, 1 top-right,
/// 2 middle-left, 3 middle-right, 4 bottom-left, 5 bottom-right. In the
/// code the sixth sixel is bit 6, because bit 5 is what makes it a mosaic.
inline int SixelBits( uint8_t code7 )
{
	return ( code7 & 0x1F ) | ( ( code7 & 0x40 ) >> 1 );
}

inline uint8_t MosaicCode( int sixelBits )
{
	return static_cast< uint8_t >( 0x20 | ( sixelBits & 0x1F ) | ( ( sixelBits & 0x20 ) << 1 ) );
}

/// Odd parity: the transmitted byte has an odd number of set bits.
inline int PopCount8( uint8_t b )
{
	int n = 0;
	for( int i = 0; i < 8; ++i )
		n += ( b >> i ) & 1;
	return n;
}

inline uint8_t WithOddParity( uint8_t code7 )
{
	const uint8_t data = static_cast< uint8_t >( code7 & 0x7F );
	return ( PopCount8( data ) & 1 ) ? data : static_cast< uint8_t >( data | 0x80 );
}

inline bool ParityOk( uint8_t byte )
{
	return ( PopCount8( byte ) & 1 ) == 1;
}

//---------------------------------------------------------------------------
// The SAA5050's character cell: 6 dots wide, 10 lines high. A mosaic cell
// is 2 x 3 blocks of 3 x 3, 3 x 4 and 3 x 3 dots. In separated mode each
// block loses its LEFT column and its BOTTOM row -- the layout jsbeeb's
// SAA5050 emulation uses, and Wikipedia's "two fewer in each direction"
// (in the doubled 12 x 20 raster). See AGENTS.md: this is a reading of the
// chip, not a quote from its datasheet.
//---------------------------------------------------------------------------
constexpr int kCellDotsX = 6;
constexpr int kCellDotsY = 10;
constexpr int kDotsX     = kColumns * kCellDotsX;///< 240
constexpr int kDotsY     = kRows * kCellDotsY;   ///< 240

/// Which sixel row a dot line (0..9) belongs to: lines 0-2, 3-6, 7-9.
inline int SixelRowOfLine( int line )
{
	return line < 3 ? 0 : ( line < 7 ? 1 : 2 );
}

/// The first dot line of each sixel row, and one past the last.
constexpr int kSixelRowStart[ 4 ] = { 0, 3, 7, 10 };

/// Is this dot (dx 0..5, line 0..9) part of the drawn body of its block in
/// separated mode? Blank column: the left of each block (0 and 3). Blank
/// line: the bottom of each block (2, 6, 9).
inline bool SeparatedDotLit( int dx, int line )
{
	const bool blankColumn = ( dx % 3 ) == 0;
	const bool blankLine   = line == 2 || line == 6 || line == 9;
	return !blankColumn && !blankLine;
}

/// Negative-control hooks. Always 0 in the plugin; each bit perturbs the
/// model so a harness check can be shown to FAIL. Bits are documented at
/// their point of use.
enum Perturb : int
{
	kPerturbFreeCodes      = 1,  ///< encoder: control codes take no cell
	kPerturbGreedy         = 2,  ///< encoder: one-cell-lookahead greedy instead of the DP
	kPerturbAllRows        = 4,  ///< transmission: every field carries every row
	kPerturbIgnoreParity   = 8,  ///< decoder: a parity failure is shown anyway
	kPerturbErrorsMoveRows = 16, ///< transmission: a byte error lands the row one down
	kPerturbSameBitTwice   = 32, ///< transmission: a double-bit error flips one bit twice
	kPerturbNoQuantise     = 64, ///< render: the sixel's mean colour instead of the palette colour
	kPerturbGutterFlipped  = 128,///< render: separated blanks the right column and the top line
};

} // namespace teletext::codes
