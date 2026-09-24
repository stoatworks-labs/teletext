#pragma once

#include <cstdint>

/**
	Host parameters are 0..1 (sliders), real integers (FF_TYPE_INTEGER) or
	element indices (options); these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange`
	can widen it, so the two counts here -- Rows per Field and Page Number --
	are declared FF_TYPE_INTEGER, which is exempt and holds its real value.
	An option's range reads back 0..1 from the SDK whatever its element
	count, so options are mapped by INDEX here and nowhere else.
*/
namespace teletext::controls
{

/// An option's value to its index, rounded and clamped.
int OptionIndex( float value, int count );

/// Error Space, in menu order.
constexpr int kErrorSpaceCount = 2;
const char* ErrorSpaceName( int index );

/// Rows per Field: 1..24 packets a field.
constexpr int kRowsPerFieldMin = 1;
constexpr int kRowsPerFieldMax = 24;
int RowsPerField( float value );

/// Page Number: 100..899, as a magazine page.
constexpr int kPageMin = 100;
constexpr int kPageMax = 899;
int PageNumber( float value );

/// Signal Quality: 0..1, 1 perfect. Linear.
double SignalQuality( float value );

} // namespace teletext::controls
