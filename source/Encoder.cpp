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

/// What a cell offers every state, computed once per cell rather than once
/// per state: the best mosaic and its cost for each ( fg, bg ), and the cost
/// of a space on each background. 224 states x 11 transitions a cell would
/// otherwise recompute these from the table every time.
struct CellCache
{
	int bits[ 8 ][ 8 ];
	int64_t cost[ 8 ][ 8 ];
	int64_t space[ 8 ];
};

void buildCache( const CostTable& table, int cell, CellCache& out )
{
	for( int fg = 1; fg <= 7; ++fg )
		for( int bg = 0; bg < 8; ++bg )
			out.bits[ fg ][ bg ] = BestMosaic( table, cell, fg, bg, out.cost[ fg ][ bg ] );
	for( int bg = 0; bg < 8; ++bg )
		out.space[ bg ] = DisplayCost( table, cell, 0, 0, bg );
}

/// The mosaic the programme assumes is held when a control code is placed at
/// cell i under state s: the best mosaic of the cell before, under the same
/// colours. Exact when that cell was a mosaic. Nothing is held at cell 0 or
/// in alphanumerics.
int assumedHeld( const CellCache* previous, const State& s )
{
	if( previous == nullptr || !s.mosaics )
		return -1;
	return previous->bits[ s.fg ][ s.bg ];
}

/// Every transition out of state s at cell i, handed to `f( cost, byte,
/// next, advances )`. A callback rather than a vector: this runs 224 states
/// x 40 cells x 2 programmes per row, and a row is encoded 50 times a
/// second per Rows per Field.
template< typename F >
void forEachTransition( const CostTable& table, int cell, const CellCache& here, const CellCache* previous, const State& s,
                        const Options& options, F&& f )
{
	const bool free = ( options.perturb & codes::kPerturbFreeCodes ) != 0;

	//1. Fill the cell with the best mosaic (mosaics mode) or a space.
	if( s.mosaics )
		f( here.cost[ s.fg ][ s.bg ], codes::MosaicCode( here.bits[ s.fg ][ s.bg ] ), s, true );
	else
		f( here.space[ s.bg ], codes::kSpace, s, true );

	//The cost of a control cell: the held mosaic under Hold, else a space,
	//in the foreground now in effect and the background AFTER any set-at.
	const int held = assumedHeld( previous, s );
	auto controlCost = [ & ]( int bgAfter, bool holdAfter ) {
		const int bits = ( holdAfter && s.mosaics && held >= 0 ) ? held : 0;
		return bits == 0 ? here.space[ bgAfter ] : DisplayCost( table, cell, bits, s.fg, bgAfter );
	};

	//2. A mosaic colour code: set-after.
	const int64_t colourCost = controlCost( s.bg, s.hold );
	for( int c = 1; c <= 7; ++c )
	{
		if( s.mosaics && c == s.fg )
			continue;//a no-op
		State n   = s;
		n.mosaics = true;
		n.fg      = c;
		f( colourCost, static_cast< uint8_t >( codes::kMosaicColourBase | c ), n, !free );
	}

	if( options.allowBackground )
	{
		//3. New Background: set-at, bg <- fg.
		if( s.bg != s.fg )
		{
			State n = s;
			n.bg    = s.fg;
			f( controlCost( n.bg, s.hold ), codes::kNewBackground, n, !free );
		}
		//4. Black Background: set-at.
		if( s.bg != codes::kBlack )
		{
			State n = s;
			n.bg    = codes::kBlack;
			f( controlCost( n.bg, s.hold ), codes::kBlackBackground, n, !free );
		}
	}

	//5. Hold Mosaics: set-at, and only worth issuing in mosaics mode.
	if( options.hold && !s.hold && s.mosaics )
	{
		State n = s;
		n.hold  = true;
		f( controlCost( s.bg, true ), codes::kHoldMosaics, n, !free );
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

	std::vector< CellCache > caches( static_cast< size_t >( table.count ) );
	for( int cell = 0; cell < table.count; ++cell )
		buildCache( table, cell, caches[ static_cast< size_t >( cell ) ] );

	for( int cell = first; cell < table.count; ++cell )
	{
		const CellCache& here     = caches[ static_cast< size_t >( cell ) ];
		const CellCache* previous = cell > 0 ? &caches[ static_cast< size_t >( cell ) - 1 ] : nullptr;
		//Within-cell layers first (free codes only), then the advancing one.
		for( int sub = 0; sub < layersPerCell; ++sub )
		{
			const bool last = sub == layersPerCell - 1;
			const int from  = static_cast< int >( layers.size() ) - 1;
			layers.emplace_back();
			Layer& next = layers.back();
			const Layer& current = layers[ static_cast< size_t >( from ) ];
			//A state may also stay put through a within-cell layer.
			if( !last )
				for( int s = 0; s < kStates; ++s )
					if( current.cost[ s ] < kInfinite )
					{
						next.cost[ s ]      = current.cost[ s ];
						next.prevLayer[ s ] = from;
						next.prevState[ s ] = s;
						next.byte[ s ]      = -1;
					}
			for( int s = 0; s < kStates; ++s )
			{
				if( current.cost[ s ] >= kInfinite )
					continue;
				const int64_t base = current.cost[ s ];
				forEachTransition( table, cell, here, previous, stateOf( s ), o,
				                   [ & ]( int64_t cost, uint8_t byte, const State& nextState, bool advances ) {
					                   if( advances != last )
						                   return;
					                   const int n         = indexOf( nextState );
					                   const int64_t total = base + cost;
					                   if( total < next.cost[ n ] )
					                   {
						                   next.cost[ n ]      = total;
						                   next.prevLayer[ n ] = from;
						                   next.prevState[ n ] = s;
						                   next.byte[ n ]      = byte;
					                   }
				                   } );
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
	std::vector< CellCache > caches( static_cast< size_t >( table.count ) );
	for( int cell = 0; cell < table.count; ++cell )
		buildCache( table, cell, caches[ static_cast< size_t >( cell ) ] );
	auto cacheAt = [ & ]( int cell ) -> const CellCache* {
		return cell >= 0 && cell < table.count ? &caches[ static_cast< size_t >( cell ) ] : nullptr;
	};
	for( int cell = first; cell < table.count; ++cell )
	{
		int64_t bestTotal = kInfinite, bestCost = 0;
		uint8_t bestByte  = codes::kSpace;
		State bestNext    = s;
		forEachTransition( table, cell, *cacheAt( cell ), cacheAt( cell - 1 ), s, options,
		                   [ & ]( int64_t cost, uint8_t byte, const State& nextState, bool ) {
			                   int64_t look = 0;
			                   if( cell + 1 < table.count )
			                   {
				                   look = kInfinite;
				                   forEachTransition( table, cell + 1, *cacheAt( cell + 1 ), cacheAt( cell ), nextState, options,
				                                      [ & ]( int64_t c2, uint8_t, const State&, bool ) { look = std::min( look, c2 ); } );
			                   }
			                   if( cost + look < bestTotal )
			                   {
				                   bestTotal = cost + look;
				                   bestCost  = cost;
				                   bestByte  = byte;
				                   bestNext  = nextState;
			                   }
		                   } );
		plan.bytes.push_back( bestByte );
		plan.cost += bestCost;
		s = bestNext;
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
