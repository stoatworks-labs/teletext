#pragma once

#include <cstdint>

/**
	Row 0: the page header. Page number, a service name, the page again and
	a running clock, as a broadcaster's header row was laid out -- but with
	no broadcaster's name on it. The clock is the composition's running time
	(hours:minutes:seconds of host time), because a wall clock would make two
	renders of the same frame differ, and it is written as alphanumerics in
	the row's own colour codes, which cost their cells like any other.
*/
namespace teletext::header
{

/// Write 40 seven-bit codes for the header of `pageNumber` at `seconds`.
void Compose( int pageNumber, double seconds, uint8_t* out );

} // namespace teletext::header
