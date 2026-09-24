#include "Layout.h"

#include "Codes.h"

#include <algorithm>
#include <cmath>

namespace teletext::layout
{

const char* GridFitName( int index )
{
	static const char* const names[ kGridFitCount ] = { "4:3", "Fill" };
	return names[ std::clamp( index, 0, kGridFitCount - 1 ) ];
}

Layout Compute( int gridFit, int outWidth, int outHeight )
{
	Layout l;
	l.outWidth  = std::max( outWidth, 1 );
	l.outHeight = std::max( outHeight, 1 );

	if( gridFit == kFitSafe )
	{
		//The largest 4:3 rectangle in the frame, centred, in whole pixels.
		int w = l.outWidth, h = l.outHeight;
		if( w * 3 > h * 4 )
			w = ( h * 4 ) / 3;
		else
			h = ( w * 3 ) / 4;
		l.regionW = std::max( w, 1 );
		l.regionH = std::max( h, 1 );
	}
	else
	{
		l.regionW = l.outWidth;
		l.regionH = l.outHeight;
	}
	l.regionX = ( l.outWidth - l.regionW ) / 2;
	l.regionY = ( l.outHeight - l.regionH ) / 2;

	const int kx = l.regionW / codes::kDotsX;
	const int ky = l.regionH / codes::kDotsY;
	if( kx >= 1 && ky >= 1 )
	{
		l.wholePixel = true;
		l.dotW       = kx;
		l.dotH       = ky;
		l.drawW      = kx * codes::kDotsX;
		l.drawH      = ky * codes::kDotsY;
		//Centred in whole pixels, so every dot edge is a pixel edge.
		l.drawX = l.regionX + ( l.regionW - static_cast< int >( l.drawW ) ) / 2;
		l.drawY = l.regionY + ( l.regionH - static_cast< int >( l.drawH ) ) / 2;
	}
	else
	{
		l.wholePixel = false;
		l.dotW       = static_cast< double >( l.regionW ) / codes::kDotsX;
		l.dotH       = static_cast< double >( l.regionH ) / codes::kDotsY;
		l.drawX      = l.regionX;
		l.drawY      = l.regionY;
		l.drawW      = l.regionW;
		l.drawH      = l.regionH;
	}
	return l;
}

void Layout::SixelBounds( int sx, int sy, double& x0, double& y0, double& x1, double& y1 ) const
{
	const int row  = sy / codes::kSixelsY;
	const int band = sy % codes::kSixelsY;
	const int line0 = row * codes::kCellDotsY + codes::kSixelRowStart[ band ];
	const int line1 = row * codes::kCellDotsY + codes::kSixelRowStart[ band + 1 ];
	x0 = drawX + sx * 3 * dotW;
	x1 = drawX + ( sx + 1 ) * 3 * dotW;
	y0 = drawY + line0 * dotH;
	y1 = drawY + line1 * dotH;
}

} // namespace teletext::layout
