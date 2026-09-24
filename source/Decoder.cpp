#include "Decoder.h"

namespace teletext::decoder
{

Cell Step( State& s, uint8_t byte, bool ignoreParity )
{
	Cell cell;
	cell.parityBad = !codes::ParityOk( byte );

	//A parity failure is shown as a space. Whatever the byte was meant to be
	//-- a mosaic or a colour change -- is gone for this row.
	uint8_t code = static_cast< uint8_t >( byte & 0x7F );
	if( cell.parityBad && !ignoreParity )
		code = codes::kSpace;

	if( codes::IsControl( code ) )
	{
		cell.control = true;

		//Set-at attributes take effect on this very cell.
		switch( code )
		{
		case codes::kBlackBackground: s.bg = codes::kBlack; break;
		case codes::kNewBackground: s.bg = s.fg; break;
		case codes::kHoldMosaics: s.hold = true; break;
		case codes::kContiguous: s.separated = false; break;
		case codes::kSeparated: s.separated = true; break;
		default: break;
		}

		//What the cell shows: the held mosaic under Hold in mosaics mode,
		//otherwise a space. The colours are the ones in effect NOW -- a
		//colour code is set-after, so its own cell still wears the old
		//foreground.
		if( s.hold && s.mosaics && codes::IsMosaicCode( s.held ) )
		{
			cell.shown  = s.held;
			cell.mosaic = true;
		}
		else
		{
			cell.shown  = codes::kSpace;
			cell.mosaic = s.mosaics;
		}
		cell.fg        = s.fg;
		cell.bg        = s.bg;
		cell.separated = s.separated;

		//Set-after attributes take effect from the next cell.
		if( code >= 0x01 && code <= 0x07 )
		{
			s.fg = code;
			if( s.mosaics )
				s.held = codes::kSpace;//a change of mode resets the held character
			s.mosaics = false;
		}
		else if( code >= 0x11 && code <= 0x17 )
		{
			s.fg = static_cast< uint8_t >( code & 7 );
			if( !s.mosaics )
				s.held = codes::kSpace;
			s.mosaics = true;
		}
		else if( code == codes::kReleaseMosaics )
			s.hold = false;

		return cell;
	}

	cell.fg        = s.fg;
	cell.bg        = s.bg;
	cell.separated = s.separated;
	cell.shown     = code;
	cell.mosaic    = s.mosaics && codes::IsMosaicCode( code );
	if( cell.mosaic )
		s.held = code;
	return cell;
}

void Row( const uint8_t* bytes, int count, Cell* cells, bool ignoreParity )
{
	State state;
	for( int i = 0; i < count; ++i )
		cells[ i ] = Step( state, bytes[ i ], ignoreParity );
}

} // namespace teletext::decoder
