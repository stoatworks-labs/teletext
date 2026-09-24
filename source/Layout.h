#pragma once

/**
	Where the 240 x 240-dot page sits on the output, and how big a dot is.

	Grid Fit chooses the region: the teletext safe area, a 4:3 rectangle
	centred in the frame, or the whole frame. Inside the region the page is
	scaled up by WHOLE pixels where it can be -- a dot is kx by ky pixels,
	each the largest integer that fits, centred, the rest of the region
	black -- and by a fraction only where the region is too small for even
	one pixel a dot (a 320 x 180 frame is 180 lines for 240 dot lines).

	Coordinates are pixels, y = 0 at the TOP: the picture's rows read
	downwards like a page, and the shader flips once.

	Shared by the plugin (the cells pass samples the source over exactly the
	area the render pass paints, so the page sits on the picture it encodes)
	and the harness, which needs the geometry to say which pixel is which
	sixel.
*/
namespace teletext::layout
{

enum GridFit
{
	kFitSafe = 0,///< 4:3, centred
	kFitFill = 1,///< the whole frame
};
constexpr int kGridFitCount = 2;
const char* GridFitName( int index );

struct Layout
{
	int outWidth = 0, outHeight = 0;
	/// The region Grid Fit chose.
	int regionX = 0, regionY = 0, regionW = 0, regionH = 0;
	/// The rectangle the 240 x 240 dots are painted into.
	double drawX = 0, drawY = 0, drawW = 0, drawH = 0;
	/// A dot's size in pixels; integers when `wholePixel`.
	double dotW = 1, dotH = 1;
	bool wholePixel = false;

	/// Pixel bounds [x0, x1) x [y0, y1) of sixel ( sx, sy ), top-down, as
	/// doubles: at a whole-pixel scale they are integers.
	void SixelBounds( int sx, int sy, double& x0, double& y0, double& x1, double& y1 ) const;
};

Layout Compute( int gridFit, int outWidth, int outHeight );

} // namespace teletext::layout
