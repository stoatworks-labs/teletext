#include "Transmission.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace teletext::transmission
{

uint32_t Hash( uint32_t a, uint32_t b, uint32_t c )
{
	//Three words folded into one LCG state, then PCG-XSH-RR's output mix.
	uint32_t state = a * 747796405u + 2891336453u;
	state          = ( state ^ ( b * 2654435761u ) ) * 747796405u + 2891336453u;
	state          = ( state ^ ( c * 2246822519u ) ) * 747796405u + 2891336453u;
	const uint32_t word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

int64_t FieldIndex( double seconds )
{
	return static_cast< int64_t >( std::floor( std::max( seconds, 0.0 ) * kFieldRate + 1e-6 ) );
}

int CycleLength( int rowsPerField )
{
	const int r = std::clamp( rowsPerField, 1, codes::kRows );
	return ( codes::kRows + r - 1 ) / r;
}

void RowsOfField( int64_t field, int rowsPerField, int& first, int& last )
{
	const int r     = std::clamp( rowsPerField, 1, codes::kRows );
	const int cycle = CycleLength( r );
	const int slot  = static_cast< int >( ( ( field % cycle ) + cycle ) % cycle );
	first           = slot * r;
	last            = std::min( codes::kRows, first + r );
}

Page::Page()
{
	Reset();
}

void Page::Reset()
{
	const uint8_t space = codes::WithOddParity( codes::kSpace );
	for( auto& row : bytes )
		std::fill( row, row + codes::kColumns, space );
	lastField = -1;
}

void Page::Advance( int64_t field, int rowsPerField, double quality, bool frozen, int perturb,
                    const std::function< void( int, uint8_t* ) >& encodeRow,
                    const std::function< void( uint8_t* ) >& header, const ForcedError& forced )
{
	if( field <= lastField )
		return;//the clock has not reached a new field (or went backwards)

	//Every field elapsed, up to the cap; a longer gap is a jump and runs only
	//the field it lands on.
	int64_t from = lastField < 0 ? field : lastField + 1;
	if( field - from + 1 > kMaxFieldsPerFrame )
		from = field;

	if( !frozen )
		for( int64_t f = from; f <= field; ++f )
			TransmitField( f, rowsPerField, quality, perturb, encodeRow, header, forced );

	lastField = field;
}

void Page::TransmitField( int64_t field, int rowsPerField, double quality, int perturb,
                          const std::function< void( int, uint8_t* ) >& encodeRow,
                          const std::function< void( uint8_t* ) >& header, const ForcedError& forced )
{
	uint8_t row[ codes::kColumns ];
	int first, last;
	if( perturb & codes::kPerturbAllRows )
	{
		first = 0;
		last  = codes::kRows;
	}
	else
		RowsOfField( field, rowsPerField, first, last );

	//The header goes out every field, before the page rows, as row 0.
	if( header )
	{
		header( row );
		receive( 0, row, field, quality, perturb, forced );
	}
	for( int r = first; r < last; ++r )
	{
		if( header && r == 0 )
			continue;//row 0 is the header this field
		encodeRow( r, row );
		receive( r, row, field, quality, perturb, forced );
	}
}

void Page::receive( int row, const uint8_t* codes7, int64_t field, double quality, int perturb,
                    const ForcedError& forced )
{
	const uint32_t f = static_cast< uint32_t >( field & 0xFFFFFFFFu );
	const double q   = std::clamp( quality, 0.0, 1.0 );
	const double pByte = kByteErrorPeak * ( 1.0 - q ) * ( 1.0 - q );

	//The row address. Hamming 8/4 corrects one bit and detects two: a packet
	//with two errors in its address is dropped, and the row keeps what it
	//had. An error never moves a row -- unless the perturbation says so.
	const uint32_t addressDraw = Hash( f, static_cast< uint32_t >( row ), 0xADD3u );
	if( addressDraw < static_cast< uint32_t >( pByte * kDropShare * 4294967296.0 ) )
		return;
	int target = row;
	if( perturb & codes::kPerturbErrorsMoveRows )
	{
		//The perturbation: an error in the packet lands the row one down --
		//a forced one always, a random one at the byte error rate.
		const bool forcedHere = forced.row == row;
		const bool randomHere = pByte > 0.0
		                        && Hash( f, static_cast< uint32_t >( row ), 0xB0B0u ) < static_cast< uint32_t >( pByte * 4294967296.0 );
		if( forcedHere || randomHere )
			target = ( row + 1 ) % codes::kRows;
	}

	for( int c = 0; c < codes::kColumns; ++c )
	{
		uint8_t byte = codes::WithOddParity( codes7[ c ] );

		const uint32_t draw = Hash( f, static_cast< uint32_t >( row ), static_cast< uint32_t >( c ) );
		if( draw < static_cast< uint32_t >( pByte * 4294967296.0 ) )
		{
			const uint32_t which = Hash( f, static_cast< uint32_t >( row ), static_cast< uint32_t >( c ) + 0x1000u );
			const int bitA       = static_cast< int >( which & 7u );
			byte ^= static_cast< uint8_t >( 1u << bitA );
			//A double error: the second flipped bit is another one.
			const bool isDouble = ( which >> 8 ) % 1000u < static_cast< uint32_t >( kDoubleShare * 1000.0 );
			if( isDouble )
			{
				int bitB = ( perturb & codes::kPerturbSameBitTwice ) ? bitA : static_cast< int >( ( bitA + 1 + ( which >> 16 ) % 7u ) & 7u );
				byte ^= static_cast< uint8_t >( 1u << bitB );
			}
		}

		if( forced.row == row && forced.column == c )
		{
			byte ^= static_cast< uint8_t >( 1u << ( forced.bitA & 7 ) );
			if( forced.bitB >= 0 )
				byte ^= static_cast< uint8_t >( 1u << ( ( ( perturb & codes::kPerturbSameBitTwice ) ? forced.bitA : forced.bitB ) & 7 ) );
		}

		bytes[ target ][ c ] = byte;
	}
}

void Page::Decode( int row, decoder::Cell* cells, bool ignoreParity ) const
{
	decoder::Row( bytes[ row ], codes::kColumns, cells, ignoreParity );
}

} // namespace teletext::transmission
