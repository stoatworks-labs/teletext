#pragma once

#include "Codes.h"
#include "Decoder.h"

#include <cstdint>
#include <functional>

/**
	The transmission: the page arrives as it is broadcast.

	Teletext rows travel as packets in the vertical blanking interval, a few
	per field, so a page is never updated at once. This holds the receiver's
	page memory -- 24 rows of 40 received bytes -- and on each field writes
	the rows that field carries, in row order, cycling: with R rows a field
	the page takes ceil( 24 / R ) fields to come round, and a moving picture
	tears between rows.

	Bit errors land the teletext way. Each row's bytes carry odd parity, so a
	single-bit error is caught by the decoder and blanks the cell, a
	double-bit error passes and shows as the wrong character, and the row
	address is Hamming-protected: an error never puts a row anywhere but
	where it belongs. (A packet whose address is beyond repair is dropped,
	and the row keeps what it had.)

	Every error comes from an integer hash of ( field, row, column ), so the
	same field with the same quality always breaks the same bits.
*/
namespace teletext::transmission
{

/// PAL fields a second: teletext lives in the 625-line system's VBI.
constexpr double kFieldRate = 50.0;

/// The most fields one host frame may run; a longer gap is a jump.
constexpr int kMaxFieldsPerFrame = 4;

/// Errors from Signal Quality q in 0..1: the fraction of transmitted bytes
/// that arrive with an error is kByteErrorPeak x ( 1 - q )^2; of those,
/// kDoubleShare carry two flipped bits and the rest one; a packet is
/// dropped for an unrepairable address at kDropShare of the byte rate.
constexpr double kByteErrorPeak = 0.3;
constexpr double kDoubleShare   = 0.25;
constexpr double kDropShare     = 0.25;

/// A forced error, for the harness: every transmission of `row` flips
/// `bitA` (and `bitB` if >= 0) of the data byte at `column`.
struct ForcedError
{
	int row    = -1;
	int column = -1;
	int bitA   = 0;
	int bitB   = -1;
};

/// The field index for a host time in seconds. The 1e-6 allowance is
/// copperlist's: 1/60 is not a double, and a frame that sits on a field
/// boundary would otherwise land a field early.
int64_t FieldIndex( double seconds );

/// How many fields the page cycle takes at R rows a field.
int CycleLength( int rowsPerField );

/// The rows field `field` carries, as [first, last).
void RowsOfField( int64_t field, int rowsPerField, int& first, int& last );

/// The page in memory: received bytes (with parity) and what they decode to.
class Page
{
public:
	Page();

	/// Forget everything: all spaces, no field seen.
	void Reset();

	/// Run the fields elapsed up to `field`. `encodeRow( r, out )` writes the
	/// 40 seven-bit codes for row r from the current picture; `header( out )`
	/// does the same for row 0 when a header is on (or is null). Rows are
	/// re-encoded only when a field carries them.
	void Advance( int64_t field, int rowsPerField, double quality, bool frozen, int perturb,
	              const std::function< void( int, uint8_t* ) >& encodeRow,
	              const std::function< void( uint8_t* ) >& header, const ForcedError& forced );

	/// Transmit exactly one field's rows (what Advance does per field).
	void TransmitField( int64_t field, int rowsPerField, double quality, int perturb,
	                    const std::function< void( int, uint8_t* ) >& encodeRow,
	                    const std::function< void( uint8_t* ) >& header, const ForcedError& forced );

	/// The received bytes, with parity.
	const uint8_t* Bytes( int row ) const
	{
		return bytes[ row ];
	}

	/// What the decoder makes of a row. `ignoreParity` is the perturb hook.
	void Decode( int row, decoder::Cell* cells, bool ignoreParity ) const;

	int64_t LastField() const
	{
		return lastField;
	}

private:
	void receive( int row, const uint8_t* codes7, int64_t field, double quality, int perturb, const ForcedError& forced );

	uint8_t bytes[ codes::kRows ][ codes::kColumns ];
	int64_t lastField = -1;
};

/// The channel's hash: PCG's output mix on a 32-bit state, exact in
/// integers. Public so the harness can predict a field's errors.
uint32_t Hash( uint32_t a, uint32_t b, uint32_t c );

} // namespace teletext::transmission
