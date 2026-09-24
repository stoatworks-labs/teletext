#include "Header.h"

#include "Codes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace teletext::header
{

void Compose( int pageNumber, double seconds, uint8_t* out )
{
	std::fill( out, out + codes::kColumns, codes::kSpace );

	//"P100" white, then a yellow service name, the page in white again, and
	//the clock in cyan at the right-hand end. Each colour code takes a cell.
	const double t     = std::max( seconds, 0.0 );
	const long long s  = static_cast< long long >( std::floor( t + 1e-6 ) );
	const int hours    = static_cast< int >( ( s / 3600 ) % 24 );
	const int minutes  = static_cast< int >( ( s / 60 ) % 60 );
	const int secs     = static_cast< int >( s % 60 );

	char text[ 64 ];
	std::snprintf( text, sizeof( text ), "P%03d \x03TELETEXT \x07%03d", pageNumber, pageNumber );
	char clock[ 16 ];
	std::snprintf( clock, sizeof( clock ), "\x06%02d:%02d/%02d", hours, minutes, secs );

	const int textLength  = static_cast< int >( std::strlen( text ) );
	const int clockLength = static_cast< int >( std::strlen( clock ) );
	for( int i = 0; i < textLength && i < codes::kColumns; ++i )
		out[ i ] = static_cast< uint8_t >( text[ i ] & 0x7F );
	const int clockStart = codes::kColumns - clockLength;
	for( int i = 0; i < clockLength; ++i )
		out[ clockStart + i ] = static_cast< uint8_t >( clock[ i ] & 0x7F );
}

} // namespace teletext::header
