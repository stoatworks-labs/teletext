#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace teletext::controls
{

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

const char* ErrorSpaceName( int index )
{
	static const char* const names[ kErrorSpaceCount ] = { "RGB", "Luma Weighted" };
	return names[ std::clamp( index, 0, kErrorSpaceCount - 1 ) ];
}

int RowsPerField( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kRowsPerFieldMin, kRowsPerFieldMax );
}

int PageNumber( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kPageMin, kPageMax );
}

double SignalQuality( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}

} // namespace teletext::controls
