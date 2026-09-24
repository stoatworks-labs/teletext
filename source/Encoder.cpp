#include "Encoder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace teletext::encoder
{
namespace
{
constexpr int64_t kInfinite = std::numeric_limits< int64_t >::max() / 4;

//---------------------------------------------------------------------------
// The programme's state: the decoder's row state without the held character.
//---------------------------------------------------------------------------
struct State
{
	bool mosaics;
	int fg;///< 1..7
	int bg;///< 0..7
	bool hold;
};

constexpr int kStates = 2 * 7 * 8 * 2;

int indexOf( const State& s )
{
	return ( ( ( s.mosaics ? 1 : 0 ) * 7 + ( s.fg - 1 ) ) * 8 + s.bg ) * 2 + ( s.hold ? 1 : 0 );
}

State stateOf( int index )
{
	State s;
	s.hold = ( index & 1 ) != 0;
	index >>= 1;
	s.bg = index % 8;
	index /= 8;
	s.fg = index % 7 + 1;
	index /= 7;
	s.mosaics = index != 0;
	return s;
}

/// One layer of the trellis: the best cost to reach each state, and how.
struct Layer
{
	int64_t cost[ kStates ];
	int prevLayer[ kStates ];
	int prevState[ kStates ];
	int byte[ kStates ];///< the byte emitted to arrive here, -1 for none
	Layer()
	{
		std::fill( cost, cost + kStates, kInfinite );
		std::fill( prevLayer, prevLayer + kStates, -1 );
		std::fill( prevState, prevState + kStates, -1 );
		std::fill( byte, byte + kStates, -1 );
	}
};

/// A transition: what it costs at this cell, the byte it emits and where it
/// leads. `advances` is false only under kPerturbFreeCodes for a control.
struct Transition
{
	int64_t cost;
	uint8_t byte;
	State next;
};

/// The mosaic the programme assumes is held when a control code is placed at
/// cell i under state s: the best mosaic of the cell before, under the same
/// colours. Exact when that cell was a mosaic. Nothing is held at cell 0 or
/// in alphanumerics.
int assumedHeld( const CostTable& table, int cell, const State& s )
{
	if( cell == 0 || !s.mosaics )
		return -1;
	int64_t ignored;
	return BestMosaic( table, cell - 1, s.fg, s.bg, ignored );
}

/// Every transition out of state s at cell i.
void transitions( const CostTable& table, int cell, const State& s, const Options& options,
                  std::vector< Transition >& out, std::vector< bool >& advances )
{
	out.clear();
	advances.clear();
	const bool free = ( options.perturb & codes::kPerturbFreeCodes ) != 0;

	//1. Fill the cell with the best mosaic (mosaics mode) or a space.
	if( s.mosaics )
	{
		int64_t cost;
		const int bits = BestMosaic( table, cell, s.fg, s.bg, cost );
		out.push_back( { cost, codes::MosaicCode( bits ), s } );
	}
	else
		out.push_back( { DisplayCost( table, cell, 0, s.fg, s.bg ), codes::kSpace, s } );
	advances.push_back( true );

	//The cost of a control cell: the held mosaic under Hold, else a space,
	//in the foreground now in effect and the background AFTER any set-at.
	const int held = assumedHeld( table, cell, s );
	auto controlCost = [ & ]( int bgAfter, bool holdAfter ) {
		const int bits = ( holdAfter && s.mosaics && held >= 0 ) ? held : 0;
		return DisplayCost( table, cell, bits, s.fg, bgAfter );
	};

	//2. A mosaic colour code: set-after.
	for( int c = 1; c <= 7; ++c )
	{
		if( s.mosaics && c == s.fg )
			continue;//a no-op
		State n   = s;
		n.mosaics = true;
		n.fg      = c;
		out.push_back( { controlCost( s.bg, s.hold ), static_cast< uint8_t >( codes::kMosaicColourBase | c ), n } );
		advances.push_back( !free );
	}

	if( options.allowBackground )
	{
		//3. New Background: set-at, bg <- fg.
		if( s.bg != s.fg )
		{
			State n = s;
			n.bg    = s.fg;
			out.push_back( { controlCost( n.bg, s.hold ), codes::kNewBackground, n } );
			advances.push_back( !free );
		}
		//4. Black Background: set-at.
		if( s.bg != codes::kBlack )
		{
			State n = s;
			n.bg    = codes::kBlack;
			out.push_back( { controlCost( n.bg, s.hold ), codes::kBlackBackground, n } );
			advances.push_back( !free );
		}
	}

	//5. Hold Mosaics: set-at, and only worth issuing in mosaics mode.
	if( options.hold && !s.hold && s.mosaics )
	{
		State n = s;
		n.hold  = true;
		out.push_back( { controlCost( s.bg, true ), codes::kHoldMosaics, n } );
		advances.push_back( !free );
	}
}

/// The row after any prefix: every row starts alphanumeric, white on black.
State startState()
{
	return State{ false, codes::kWhite, codes::kBlack, false };
}

//---------------------------------------------------------------------------
// The Viterbi programme.
//---------------------------------------------------------------------------
struct Plan
{
	std::vector< uint8_t > bytes;
	int64_t cost = 0;
};

Plan viterbi( const CostTable& table, int first, const Options& options, bool allowHold )
{
	Options o = options;
	o.hold    = allowHold;
	const bool free = ( o.perturb & codes::kPerturbFreeCodes ) != 0;
	//Under free codes, controls do not advance the cell, so each cell gets a
	//few extra layers for chains of them (colour, New Background, colour is
	//the longest chain worth having).
	const int layersPerCell = free ? 4 : 1;

	std::vector< Layer > layers;
	layers.reserve( static_cast< size_t >( table.count - first + 1 ) * layersPerCell + 1 );
	layers.emplace_back();
	layers.back().cost[ indexOf( startState() ) ] = 0;

	std::vector< Transition > ts;
	std::vector< bool > advances;

	for( int cell = first; cell < table.count; ++cell )
	{
		//Within-cell layers first (free codes only), then the advancing one.
		for( int sub = 0; sub < layersPerCell; ++sub )
		{
			const bool last = sub == layersPerCell - 1;
			const int from  = static_cast< int >( layers.size() ) - 1;
			layers.emplace_back();
			Layer& next = layers.back();
			const Layer& here = layers[ static_cast< size_t >( from ) ];
			//A state may also stay put through a within-cell layer.
			if( !last )
				for( int s = 0; s < kStates; ++s )
					if( here.cost[ s ] < kInfinite )
					{
						next.cost[ s ]      = here.cost[ s ];
						next.prevLayer[ s ] = from;
						next.prevState[ s ] = s;
						next.byte[ s ]      = -1;
					}
			for( int s = 0; s < kStates; ++s )
			{
				if( here.cost[ s ] >= kInfinite )
					continue;
				transitions( table, cell, stateOf( s ), o, ts, advances );
				for( size_t t = 0; t < ts.size(); ++t )
				{
					if( advances[ t ] != last )
						continue;
					const int n         = indexOf( ts[ t ].next );
					const int64_t total = here.cost[ s ] + ts[ t ].cost;
					if( total < next.cost[ n ] )
					{
						next.cost[ n ]      = total;
						next.prevLayer[ n ] = from;
						next.prevState[ n ] = s;
						next.byte[ n ]      = ts[ t ].byte;
					}
				}
			}
		}
	}

	//The cheapest final state, then walk back.
	const Layer& final = layers.back();
	int best           = 0;
	for( int s = 1; s < kStates; ++s )
		if( final.cost[ s ] < final.cost[ best ] )
			best = s;

	Plan plan;
	plan.cost = final.cost[ best ];
	int layer = static_cast< int >( layers.size() ) - 1;
	int state = best;
	std::vector< uint8_t > reversed;
	while( layer > 0 )
	{
		const Layer& l = layers[ static_cast< size_t >( layer ) ];
		if( l.byte[ state ] >= 0 )
			reversed.push_back( static_cast< uint8_t >( l.byte[ state ] ) );
		const int pl = l.prevLayer[ state ];
		state        = l.prevState[ state ];
		layer        = pl;
	}
	plan.bytes.assign( reversed.rbegin(), reversed.rend() );
	return plan;
}

//---------------------------------------------------------------------------
// The negative control: one-cell-lookahead greedy.
//---------------------------------------------------------------------------
Plan greedy( const CostTable& table, int first, const Options& options )
{
	Plan plan;
	State s = startState();
	std::vector< Transition > ts, ts2;
	std::vector< bool > adv, adv2;
	for( int cell = first; cell < table.count; ++cell )
	{
		transitions( table, cell, s, options, ts, adv );
		int bestT        = 0;
		int64_t bestCost = kInfinite;
		for( size_t t = 0; t < ts.size(); ++t )
		{
			int64_t look = 0;
			if( cell + 1 < table.count )
			{
				transitions( table, cell + 1, ts[ t ].next, options, ts2, adv2 );
				look = kInfinite;
				for( const Transition& u : ts2 )
					look = std::min( look, u.cost );
			}
			const int64_t total = ts[ t ].cost + look;
			if( total < bestCost )
			{
				bestCost = total;
				bestT    = static_cast< int >( t );
			}
		}
		plan.bytes.push_back( ts[ static_cast< size_t >( bestT ) ].byte );
		plan.cost += ts[ static_cast< size_t >( bestT ) ].cost;
		s = ts[ static_cast< size_t >( bestT ) ].next;
	}
	return plan;
}

Result realise( const CostTable& table, const std::vector< uint8_t >& prefix, const Plan& plan, bool usedHold )
{
	Result r;
	r.usedHold    = usedHold;
	r.plannedCost = plan.cost;
	std::vector< uint8_t > all = prefix;
	all.insert( all.end(), plan.bytes.begin(), plan.bytes.end() );
	//Whatever the programme emitted, the row is table.count bytes long: a
	//longer string (free codes) is truncated, a shorter one padded.
	for( int i = 0; i < table.count; ++i )
		r.bytes[ i ] = i < static_cast< int >( all.size() ) ? all[ static_cast< size_t >( i ) ] : codes::kSpace;
	r.cost = RealisedCost( table, r.bytes, table.count );
	return r;
}
} // namespace

//---------------------------------------------------------------------------
void BuildCosts( const float* rgb, int count, int errorSpace, CostTable& out )
{
	out.count = std::clamp( count, 0, codes::kColumns );
	//Rec. 709 luma shares, scaled to sum to 3 so the two spaces have the same
	//total weight on a neutral error.
	const bool luma     = errorSpace == kErrorLuma;
	const double w[ 3 ] = { luma ? 0.2126 * 3.0 : 1.0, luma ? 0.7152 * 3.0 : 1.0, luma ? 0.0722 * 3.0 : 1.0 };
	for( int i = 0; i < out.count; ++i )
		for( int s = 0; s < 6; ++s )
		{
			const float* t = rgb + ( static_cast< size_t >( i ) * 6 + s ) * 3;
			//The mean is in linear light (an area average has to be); the
			//error is measured on its sRGB encoding, which is nearer to how
			//the eye weighs a mid-tone. In linear light a (0.2, 0.6, 0.2)
			//green is 0.32 green and rounds to black.
			double encoded[ 3 ];
			for( int ch = 0; ch < 3; ++ch )
			{
				const double v = std::clamp( static_cast< double >( t[ ch ] ), 0.0, 1.0 );
				encoded[ ch ]  = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow( v, 1.0 / 2.4 ) - 0.055;
			}
			for( int c = 0; c < codes::kColours; ++c )
			{
				double e = 0.0;
				for( int ch = 0; ch < 3; ++ch )
				{
					const double p    = ( c >> ch ) & 1 ? 1.0 : 0.0;
					const double diff = encoded[ ch ] - p;
					e += w[ ch ] * diff * diff;
				}
				out.d[ i ][ s ][ c ] = static_cast< int32_t >( std::lround( e * kCostScale ) );
			}
		}
}

int BestMosaic( const CostTable& table, int cell, int fg, int bg, int64_t& cost )
{
	int bits = 0;
	cost     = 0;
	for( int s = 0; s < 6; ++s )
	{
		const int32_t f = table.d[ cell ][ s ][ fg ];
		const int32_t b = table.d[ cell ][ s ][ bg ];
		if( f < b )
		{
			bits |= 1 << s;
			cost += f;
		}
		else
			cost += b;
	}
	return bits;
}

int64_t DisplayCost( const CostTable& table, int cell, int bits, int fg, int bg )
{
	int64_t cost = 0;
	for( int s = 0; s < 6; ++s )
		cost += table.d[ cell ][ s ][ ( bits >> s ) & 1 ? fg : bg ];
	return cost;
}

int64_t RealisedCost( const CostTable& table, const uint8_t* bytes, int count )
{
	decoder::State state;
	int64_t cost = 0;
	for( int i = 0; i < count; ++i )
	{
		const decoder::Cell cell = decoder::Step( state, codes::WithOddParity( bytes[ i ] ) );
		cost += DisplayCost( table, i, decoder::DisplayedSixels( cell ), cell.fg, cell.bg );
	}
	return cost;
}

Result EncodeRow( const CostTable& table, const Options& options )
{
	std::vector< uint8_t > prefix;
	if( options.separated )
		prefix.push_back( codes::kSeparated );
	const int first = static_cast< int >( prefix.size() );

	if( options.perturb & codes::kPerturbGreedy )
		return realise( table, prefix, greedy( table, first, options ), false );

	Result plain = realise( table, prefix, viterbi( table, first, options, false ), false );
	if( !options.hold )
		return plain;
	Result held = realise( table, prefix, viterbi( table, first, options, true ), true );
	//The realised cost decides, so Hold can never make a row worse than the
	//exact optimum without it.
	return held.cost < plain.cost ? held : plain;
}

} // namespace teletext::encoder
