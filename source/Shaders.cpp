#include "Shaders.h"

namespace teletext::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader both passes share.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: cells. One fragment per sixel of the 80 x 72 grid; texture row 0
// is the TOP sixel row, so the CPU reads the page in reading order.
//---------------------------------------------------------------------------
const char* const kCellsBody = R"(
uniform sampler2D InputTexture;
uniform ivec2 InputSize;  //the picture's size in texels (not the hardware size)
uniform vec2 DrawOrigin;  //top-left of the dot grid on the output, pixels, y down
uniform vec2 DotSize;     //a dot in pixels

in vec2 uv;
out vec4 fragColor;

//The three sixel rows of a cell start at dot lines 0, 3, 7 and end at 10.
const int LineStart[ 4 ] = int[ 4 ]( 0, 3, 7, 10 );

float toLinear( float v )
{
	return v <= 0.04045 ? v / 12.92 : pow( ( v + 0.055 ) / 1.055, 2.4 );
}

void main()
{
	ivec2 sixel = ivec2( floor( gl_FragCoord.xy ) );   //x 0..79, y 0..71 (top-down)
	int cellRow = sixel.y / 3;
	int band    = sixel.y - cellRow * 3;

	//The sixel's rectangle on the output, y down.
	float x0 = DrawOrigin.x + float( sixel.x * 3 ) * DotSize.x;
	float x1 = DrawOrigin.x + float( sixel.x * 3 + 3 ) * DotSize.x;
	float y0 = DrawOrigin.y + float( cellRow * 10 + LineStart[ band ] ) * DotSize.y;
	float y1 = DrawOrigin.y + float( cellRow * 10 + LineStart[ band + 1 ] ) * DotSize.y;

	//Pixels whose centres lie inside it: p + 0.5 in [ a, b ) is p in
	//[ ceil( a - 0.5 ), ceil( b - 0.5 ) ).
	int px0 = int( ceil( x0 - 0.5 ) );
	int px1 = int( ceil( x1 - 0.5 ) );
	int py0 = int( ceil( y0 - 0.5 ) );
	int py1 = int( ceil( y1 - 0.5 ) );
	px0 = clamp( px0, 0, InputSize.x );
	px1 = clamp( px1, 0, InputSize.x );
	py0 = clamp( py0, 0, InputSize.y );
	py1 = clamp( py1, 0, InputSize.y );

	//At 4K a sixel is 48 x 36 pixels; past 48 a side the taps are strided.
	int sx = max( 1, ( px1 - px0 + 47 ) / 48 );
	int sy = max( 1, ( py1 - py0 + 47 ) / 48 );

	vec3 sum = vec3( 0.0 );
	float n  = 0.0;
	for( int y = py0; y < py1; y += sy )
	{
		int glY = InputSize.y - 1 - y;   //the texture's row 0 is the bottom
		for( int x = px0; x < px1; x += sx )
		{
			vec4 t = texelFetch( InputTexture, ivec2( x, glY ), 0 );
			sum += vec3( toLinear( t.r ), toLinear( t.g ), toLinear( t.b ) );
			n += 1.0;
		}
	}
	fragColor = n > 0.0 ? vec4( sum / n, 1.0 ) : vec4( 0.0, 0.0, 0.0, 0.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 2: render. The cells texture is 40 x 24 RGBA8: R foreground 0..7,
// G background 0..7, B the 7-bit code shown, A flags (1 mosaic, 2 separated,
// 4 control). Row 0 of the texture is the top row of the page.
//---------------------------------------------------------------------------
const char* const kRenderBody = R"(
uniform sampler2D Cells;
uniform sampler2D Glyphs;      //5 x 128 by 7, R8: glyph for code c at column c * 5
uniform sampler2D Means;       //the cells pass, for the no-quantise perturbation
uniform sampler2D Source;
uniform vec2 MaxUV;
uniform ivec2 OutSize;
uniform vec2 DrawOrigin;       //top-left of the dot grid, pixels, y down
uniform vec2 DotSize;
uniform int ShowCodes;
uniform float MixAmount;
uniform int Perturb;

in vec2 uv;
out vec4 fragColor;

vec3 palette( int c )
{
	return vec3( float( c & 1 ), float( ( c >> 1 ) & 1 ), float( ( c >> 2 ) & 1 ) );
}

void main()
{
	vec4 source = texture( Source, uv * MaxUV );

	//This pixel, top-down, and the dot its centre falls in.
	vec2 p = vec2( floor( gl_FragCoord.x ), float( OutSize.y ) - 1.0 - floor( gl_FragCoord.y ) );
	vec2 d = ( p + 0.5 - DrawOrigin ) / DotSize;
	int dx = int( floor( d.x ) );
	int dy = int( floor( d.y ) );

	vec3 page = vec3( 0.0 );
	if( dx >= 0 && dx < 240 && dy >= 0 && dy < 240 )
	{
		int col  = dx / 6;
		int row  = dy / 10;
		int cx   = dx - col * 6;    //dot column in the cell, 0..5
		int line = dy - row * 10;   //dot line in the cell, 0..9

		vec4 cell = texelFetch( Cells, ivec2( col, row ), 0 );
		int fg    = int( cell.r * 255.0 + 0.5 );
		int bg    = int( cell.g * 255.0 + 0.5 );
		int code  = int( cell.b * 255.0 + 0.5 );
		int flags = int( cell.a * 255.0 + 0.5 );

		bool lit = false;
		if( ( flags & 1 ) != 0 )
		{
			//A mosaic: 2 x 3 blocks of 3 x 3, 3 x 4, 3 x 3 dots. Bit 6 of
			//the code is the sixth sixel; bit 5 is what makes it a mosaic.
			int bits  = ( code & 0x1F ) | ( ( code & 0x40 ) >> 1 );
			int sx    = cx < 3 ? 0 : 1;
			int sy    = line < 3 ? 0 : ( line < 7 ? 1 : 2 );
			lit       = ( ( bits >> ( sy * 2 + sx ) ) & 1 ) != 0;
			if( ( flags & 2 ) != 0 )
			{
				//Separated: each block loses its left column and bottom line.
				bool blankColumn = ( cx - sx * 3 ) == 0;
				bool blankLine   = line == 2 || line == 6 || line == 9;
				if( ( Perturb & 128 ) != 0 )
				{
					blankColumn = ( cx - sx * 3 ) == 2;
					blankLine   = line == 0 || line == 3 || line == 7;
				}
				lit = lit && !blankColumn && !blankLine;
			}
		}
		else if( code > 32 && cx < 5 && line >= 1 && line <= 7 )
		{
			//A glyph: 5 x 7 in the cell's dot columns 0..4, lines 1..7.
			lit = texelFetch( Glyphs, ivec2( code * 5 + cx, line - 1 ), 0 ).r > 0.5;
		}

		page = palette( lit ? fg : bg );

		if( ( Perturb & 64 ) != 0 )
		{
			//The sixel's mean colour instead of the palette: not teletext.
			int sx = cx < 3 ? 0 : 1;
			int sy = line < 3 ? 0 : ( line < 7 ? 1 : 2 );
			page = texelFetch( Means, ivec2( col * 2 + sx, row * 3 + sy ), 0 ).rgb;
		}

		if( ShowCodes != 0 && ( flags & 4 ) != 0 )
			page = mix( page, vec3( 0.35, 0.35, 0.65 ), 0.5 );
	}

	fragColor = mix( source, vec4( page, 1.0 ), MixAmount );
}
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}

std::string Cells()
{
	return assemble( kCellsBody );
}

std::string Render()
{
	return assemble( kRenderBody );
}

} // namespace teletext::shaders
