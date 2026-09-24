/**
	txtest -- render Teletext offline, and read the page back out of it.

	Every check renders a synthetic source through the REAL plugin class in a
	headless GL context, or drives the plugin's own encoder with no GL at all,
	and measures rather than eyeballs.

		txtest --out /tmp/frame.png     a picture, on the test card
		txtest --list                   every parameter, its kind and default
		txtest --optimal                the row programme's cost equals an
		                                exhaustive search, exactly (no GL)
		txtest --gap                    a red-then-blue row has exactly one
		                                background cell at the boundary; a change
		                                of background exactly two
		txtest --palette                every output pixel is one of eight colours
		                                and every sixel rectangle is uniform
		txtest --separated              the gutter is where the SAA5050 puts it
		txtest --carriage               row r shows the frame from the field that
		                                carried it; a resize mid-run changes nothing
		txtest --parity                 a single-bit error blanks its cell, a
		                                double-bit error changes the mosaic, and
		                                no error moves a row
		txtest --negative               every check above can FAIL
		txtest --bench                  the render cost
		txtest --dump-shaders DIR       the exact GLSL the plugin compiles
		txtest --pipe                   raw frames in, raw frames out

	Every check drives a synthetic 60 fps clock through `SetTime`. Run each at
	two rasters at least -- the one you develop at and 320x180, which is what
	CI uses. AGENTS.md has one line per check on where each tolerance comes
	from.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the same format as the fleet's other harnesses. `--pipe` takes the fleet's
	frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | txtest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Codes.h"
#include "Controls.h"
#include "Decoder.h"
#include "Encoder.h"
#include "Layout.h"
#include "Shaders.h"
#include "Teletext.h"
#include "Transmission.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace teletext;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources. Rows are top-first in every source, the way a picture is, and
// flipped on the way into GL. The checks read back through the same flip,
// so "row 0" is the top of the picture everywhere in this file.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

/// The test card: a sky gradient over a warm ground, a row of greys, six
/// colour patches, a skin tone, a deep shadow, a specular highlight, and a
/// disc on a slow Lissajous path so consecutive frames differ.
Image buildCard( int width, int height, int frame )
{
	Image card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	const float discX = 0.5f * w + 0.30f * w * std::sin( t * 0.05f );
	const float discY = 0.42f * h + 0.12f * h * std::sin( t * 0.037f + 1.1f );
	const float discR = 0.09f * h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			float r, g, b;
			if( v < 0.55f )
			{
				const float k = v / 0.55f;
				r             = 0.15f + 0.55f * k;
				g             = 0.35f + 0.45f * k;
				b             = 0.95f - 0.15f * k;
			}
			else
			{
				r = 0.42f;
				g = 0.33f;
				b = 0.22f;
			}

			if( v > 0.84f )
			{
				const int step = std::min( 10, static_cast< int >( u * 11.0f ) );
				r = g = b = static_cast< float >( step ) / 10.0f;
			}
			else if( v > 0.66f && v < 0.80f )
			{
				static const float patches[ 8 ][ 3 ] = {
					{ 0.75f, 0.12f, 0.10f }, { 0.20f, 0.60f, 0.20f }, { 0.12f, 0.20f, 0.75f },
					{ 0.10f, 0.65f, 0.70f }, { 0.70f, 0.15f, 0.60f }, { 0.90f, 0.80f, 0.15f },
					{ 0.87f, 0.64f, 0.52f },//skin
					{ 0.03f, 0.03f, 0.03f },//deep shadow
				};
				const int patch = std::min( 7, static_cast< int >( u * 8.0f ) );
				r               = patches[ patch ][ 0 ];
				g               = patches[ patch ][ 1 ];
				b               = patches[ patch ][ 2 ];
			}

			const float hx = u - 0.85f, hy = v - 0.15f;
			if( hx * hx + hy * hy < 0.0015f )
				r = g = b = 1.0f;

			const float px   = static_cast< float >( x ) + 0.5f;
			const float dx   = px - discX;
			const float dy   = ( static_cast< float >( y ) + 0.5f ) - discY;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.2f ) );
				r                = r + ( 0.95f - r ) * edge;
				g                = g + ( 0.55f - g ) * edge;
				b                = b + ( 0.15f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}
	}

	return card;
}

/// The eight palette colours as 8-bit RGB.
void paletteRgb( int c, unsigned char* out )
{
	out[ 0 ] = ( c & 1 ) ? 255 : 0;
	out[ 1 ] = ( c & 2 ) ? 255 : 0;
	out[ 2 ] = ( c & 4 ) ? 255 : 0;
}

/// A source painted in SIXEL space: `colourAt( sx, sy )` gives a palette
/// index for each of the 80 x 72 sixels, and each output pixel takes the
/// colour of the sixel its centre falls in (black outside the grid). The
/// plugin's own layout says which -- so the picture the encoder sees is the
/// one intended, at any raster and either grid fit.
Image buildSixels( int width, int height, int gridFit, const std::function< int( int, int ) >& colourAt )
{
	Image image( static_cast< size_t >( width ) * height * 4, 0 );
	const layout::Layout lay = layout::Compute( gridFit, width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double dx = ( x + 0.5 - lay.drawX ) / lay.dotW;
			const double dy = ( y + 0.5 - lay.drawY ) / lay.dotH;
			unsigned char* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 3 ]           = 255;
			if( dx < 0.0 || dy < 0.0 || dx >= codes::kDotsX || dy >= codes::kDotsY )
				continue;
			const int dot  = static_cast< int >( std::floor( dx ) );
			const int line = static_cast< int >( std::floor( dy ) );
			const int sx   = dot / 3;
			const int sy   = ( line / codes::kCellDotsY ) * 3 + codes::SixelRowOfLine( line % codes::kCellDotsY );
			paletteRgb( colourAt( sx, sy ), p );
		}
	return image;
}

Image buildFlatColour( int width, int height, int colour )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		paletteRgb( colour, image.data() + i );
		image[ i + 3 ] = 255;
	}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Teletext::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Teletext& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Teletext::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range. An integer's real range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Teletext& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Teletext& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}

	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

bool set( Teletext& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Teletext plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderAt( int frame )
	{
		//A synthetic clock, and it has to be synthetic: left to the wall
		//clock the harness renders a hundred frames in a few milliseconds and
		//no field ever passes. The unit is declared, not inferred.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	/// Render one frame of 8-bit `pixels` (top row first) at frame `frame`.
	bool render( int frame, const Image& pixels )
	{
		const Image flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	/// The output, top row first, 8-bit.
	Image readBack()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

//---------------------------------------------------------------------------
// The baseline every GL check starts from: RGB error, Hold OFF, backgrounds
// allowed, contiguous; 24 rows a field (the whole page every field, so a
// still picture is complete after one frame), a clean signal, running; the
// Fill grid, NO header, page 100, codes hidden, the effect fully in. Each
// check then moves the one or two things it is about.
//---------------------------------------------------------------------------
using Settings = std::vector< std::pair< const char*, float > >;

void baseline( Teletext& p )
{
	set( p, "Error Space", 0.0f );
	set( p, "Hold Graphics", 0.0f );
	set( p, "Allow Background", 1.0f );
	set( p, "Separated", 0.0f );
	set( p, "Rows per Field", 24.0f );
	set( p, "Signal Quality", 1.0f );
	set( p, "Freeze", 0.0f );
	set( p, "Grid Fit", static_cast< float >( layout::kFitFill ) );
	set( p, "Header", 0.0f );
	set( p, "Page Number", 100.0f );
	set( p, "Show Codes", 0.0f );
	set( p, "Mix", 1.0f );
}

int gridFitOf( const Settings& settings )
{
	for( const auto& s : settings )
		if( std::strcmp( s.first, "Grid Fit" ) == 0 )
			return static_cast< int >( std::lround( s.second ) );
	return layout::kFitFill;
}

/// A session prepared with the baseline and `settings`.
bool prepare( Session& session, const Settings& settings, int perturb, int width, int height )
{
	baseline( session.plugin );
	for( const auto& s : settings )
		if( !set( session.plugin, s.first, s.second ) )
			return false;
	session.plugin.SetPerturbForTest( perturb );
	return session.begin( width, height );
}

/// Render `frames` frames of one still image through a fresh instance and
/// read the last one back, plus the page as decoded.
bool renderStill( int width, int height, const Settings& settings, const Image& source, int perturb, Image& out,
                  std::vector< decoder::Cell >* pageOut = nullptr, int frames = 2 )
{
	Session session;
	if( !prepare( session, settings, perturb, width, height ) )
		return false;
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, source ) )
		{
			session.end();
			return false;
		}
	out = session.readBack();
	if( pageOut )
		*pageOut = session.plugin.PageForTest();
	session.end();
	return true;
}

//---------------------------------------------------------------------------
// Reading pixels.
//---------------------------------------------------------------------------
const unsigned char* pixelAt( const Image& image, int width, int x, int y )
{
	return image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
}

/// The palette index of a pixel, or -1 if it is not one of the eight.
int paletteIndexOf( const unsigned char* p )
{
	int c = 0;
	for( int ch = 0; ch < 3; ++ch )
	{
		if( p[ ch ] == 255 )
			c |= 1 << ch;
		else if( p[ ch ] != 0 )
			return -1;
	}
	return c;
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

/// For pixel (x, y) top-down: the dot it falls in, or false if outside the
/// grid or (fractional scales only) within `margin` dots of a dot boundary,
/// where a float and a double may disagree about which side it is on.
bool dotOfPixel( const layout::Layout& lay, int x, int y, int& dx, int& line, double margin = 1e-3 )
{
	const double fx = ( x + 0.5 - lay.drawX ) / lay.dotW;
	const double fy = ( y + 0.5 - lay.drawY ) / lay.dotH;
	if( fx < 0.0 || fy < 0.0 || fx >= codes::kDotsX || fy >= codes::kDotsY )
		return false;
	if( !lay.wholePixel )
	{
		const double rx = fx - std::floor( fx ), ry = fy - std::floor( fy );
		if( rx < margin || rx > 1.0 - margin || ry < margin || ry > 1.0 - margin )
			return false;
	}
	dx   = static_cast< int >( std::floor( fx ) );
	line = static_cast< int >( std::floor( fy ) );
	return true;
}

/// Pixels of a cell's dot grid, as ( x, y, dotX 0..5, line 0..9 ).
struct DotPixel
{
	int x, y, dx, line;
};

std::vector< DotPixel > pixelsOfCell( const layout::Layout& lay, int col, int row )
{
	std::vector< DotPixel > out;
	const int x0 = static_cast< int >( std::floor( lay.drawX + col * 6 * lay.dotW ) ) - 1;
	const int x1 = static_cast< int >( std::ceil( lay.drawX + ( col + 1 ) * 6 * lay.dotW ) ) + 1;
	const int y0 = static_cast< int >( std::floor( lay.drawY + row * 10 * lay.dotH ) ) - 1;
	const int y1 = static_cast< int >( std::ceil( lay.drawY + ( row + 1 ) * 10 * lay.dotH ) ) + 1;
	for( int y = std::max( 0, y0 ); y < std::min( lay.outHeight, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( lay.outWidth, x1 ); ++x )
		{
			int dx, line;
			if( !dotOfPixel( lay, x, y, dx, line ) )
				continue;
			if( dx / 6 != col || line / 10 != row )
				continue;
			out.push_back( { x, y, dx % 6, line % 10 } );
		}
	return out;
}

//---------------------------------------------------------------------------
// --optimal
//
// No GL. Random rows of 3..8 cells: the sixel targets are half pure palette
// colours and half arbitrary colours, from an integer hash. For each row:
//
//   Hold off: the programme's realised cost (what the decoder makes of its
//   bytes, costed) EQUALS the minimum over an exhaustive search of every
//   control placement, with the best mosaic at every mosaic cell. Costs are
//   integers, so equality is the claim. The exhaustive search drives the
//   plugin's decoder step by step and shares nothing with the programme's
//   trellis.
//
//   Hold on: the realised cost is never MORE than the hold-off optimum (the
//   encoder keeps the cheaper of its two programmes, so this holds by
//   construction, and is measured anyway), and never LESS than the
//   exhaustive search with Hold in the alphabet -- which, since every
//   mosaic cell takes the best mosaic for its colours, is a search over
//   control placements rather than the true optimum; the gap between the
//   two is reported.
//
// Negative control: the one-cell-lookahead greedy encoder must cost more
// than the optimum on at least one row.
//---------------------------------------------------------------------------
struct Exhaustive
{
	const encoder::CostTable* table;
	bool allowBackground;
	bool allowHold;
	int64_t best;
	int64_t nodes;

	void search( int cell, decoder::State state, int64_t cost )
	{
		++nodes;
		if( cost >= best )
			return;//a branch already dearer than the best row cannot win
		if( cell == table->count )
		{
			best = cost;
			return;
		}
		std::vector< uint8_t > options;
		if( state.mosaics )
		{
			int64_t ignored;
			options.push_back( codes::MosaicCode( encoder::BestMosaic( *table, cell, state.fg, state.bg, ignored ) ) );
		}
		else
			options.push_back( codes::kSpace );
		for( int c = 1; c <= 7; ++c )
			options.push_back( static_cast< uint8_t >( codes::kMosaicColourBase | c ) );
		if( allowBackground )
		{
			options.push_back( codes::kNewBackground );
			options.push_back( codes::kBlackBackground );
		}
		if( allowHold )
		{
			options.push_back( codes::kHoldMosaics );
			options.push_back( codes::kReleaseMosaics );
		}
		for( uint8_t byte : options )
		{
			decoder::State next      = state;
			const decoder::Cell shown = decoder::Step( next, codes::WithOddParity( byte ) );
			const int64_t here = encoder::DisplayCost( *table, cell, decoder::DisplayedSixels( shown ), shown.fg, shown.bg );
			search( cell + 1, next, cost + here );
		}
	}

	int64_t run()
	{
		best  = std::numeric_limits< int64_t >::max();
		nodes = 0;
		search( 0, decoder::State(), 0 );
		return best;
	}
};

void randomRow( uint32_t seed, int count, std::vector< float >& rgb )
{
	rgb.assign( static_cast< size_t >( count ) * 6 * 3, 0.0f );
	for( int i = 0; i < count * 6; ++i )
	{
		const uint32_t h = transmission::Hash( seed, static_cast< uint32_t >( i ), 77u );
		float* t         = rgb.data() + static_cast< size_t >( i ) * 3;
		if( h & 1u )
		{
			const int c = static_cast< int >( ( h >> 1 ) & 7u );
			t[ 0 ]      = static_cast< float >( c & 1 );
			t[ 1 ]      = static_cast< float >( ( c >> 1 ) & 1 );
			t[ 2 ]      = static_cast< float >( ( c >> 2 ) & 1 );
		}
		else
			for( int ch = 0; ch < 3; ++ch )
				t[ ch ] = static_cast< float >( transmission::Hash( seed, static_cast< uint32_t >( i ), 100u + ch ) % 1001u ) / 1000.0f;
	}
}

int runOptimal( int perturb = 0, bool quiet = false )
{
	constexpr int kRowsTried = 24;
	int failures = 0, holdWins = 0, holdEqual = 0;
	int64_t worstGap = 0, totalOpt = 0, totalHeld = 0, totalRestricted = 0;
	int greedyWorse = 0;

	for( int n = 0; n < kRowsTried; ++n )
	{
		const uint32_t seed = 1000u + static_cast< uint32_t >( n );
		const int count     = 3 + static_cast< int >( transmission::Hash( seed, 1u, 2u ) % 6u );//3..8
		std::vector< float > rgb;
		randomRow( seed, count, rgb );

		encoder::CostTable table;
		encoder::BuildCosts( rgb.data(), count, n % 2 == 0 ? encoder::kErrorRGB : encoder::kErrorLuma, table );

		encoder::Options options;
		options.hold    = false;
		options.perturb = perturb;
		const encoder::Result plain = encoder::EncodeRow( table, options );
		options.hold                = true;
		const encoder::Result held  = encoder::EncodeRow( table, options );

		Exhaustive ex{ &table, true, false, 0, 0 };
		const int64_t optimum = ex.run();
		const int64_t nodesOff = ex.nodes;
		Exhaustive exHold{ &table, true, true, 0, 0 };
		const int64_t restricted = exHold.run();

		const bool exactOk = plain.cost == optimum;
		const bool heldOk  = held.cost <= optimum && held.cost >= restricted;
		if( held.cost < plain.cost )
			++holdWins;
		else if( held.cost == plain.cost )
			++holdEqual;
		worstGap = std::max( worstGap, held.cost - restricted );
		totalOpt += optimum;
		totalHeld += held.cost;
		totalRestricted += restricted;

		//The negative control's own measurement, independent of `perturb`.
		encoder::Options g = options;
		g.hold             = false;
		g.perturb          = codes::kPerturbGreedy;
		if( encoder::EncodeRow( table, g ).cost > optimum )
			++greedyWorse;

		if( !quiet )
			std::printf( "optimal row %2d (%d cells, %s): programme %lld, exhaustive %lld (%lld nodes)  %s;  hold %lld in [%lld, %lld]  %s\n",
			             n, count, n % 2 == 0 ? "RGB " : "luma", static_cast< long long >( plain.cost ),
			             static_cast< long long >( optimum ), static_cast< long long >( nodesOff ), verdict( exactOk ),
			             static_cast< long long >( held.cost ), static_cast< long long >( restricted ),
			             static_cast< long long >( optimum ), verdict( heldOk ) );
		if( !exactOk || !heldOk )
			++failures;
	}

	if( !quiet )
	{
		std::printf( "optimal: hold on beat hold off on %d of %d rows, tied on %d; realised - restricted-exhaustive gap: worst %lld, "
		             "total %lld over a total of %lld (optimum without hold %lld)\n",
		             holdWins, kRowsTried, holdEqual, static_cast< long long >( worstGap ),
		             static_cast< long long >( totalHeld - totalRestricted ), static_cast< long long >( totalHeld ),
		             static_cast< long long >( totalOpt ) );
		std::printf( "optimal: the greedy encoder is worse than the optimum on %d of %d rows\n", greedyWorse, kRowsTried );
		std::printf( "%s\n", failures == 0 ? "optimal: the row programme is exactly optimal without Hold, and bounded with it"
		                                    : "optimal: FAILURES" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --gap
//
// Two rows, built in sixel space and read back from the decoded page AND
// the picture.
//
//   A: the left twenty cells red, the right twenty blue. Exactly two
//   control cells: column 0 (the mosaic colour code every row needs before
//   its first mosaic; the row starts alphanumeric) and ONE at the boundary,
//   column 19 or 20 -- either costs one cell of black in a pure colour, and
//   the tie is the programme's. Every other cell is a full mosaic of the
//   right colour, and the boundary cell is black on the picture.
//
//   B: the left twenty red; the right twenty red with the top-left sixel of
//   each cell blue. The cheapest way to show that is blue ON red, and a red
//   background costs two codes: New Background (background <- red) and then
//   the blue colour code. Exactly three control cells, the two new ones
//   adjacent, the right twenty blue-on-red mosaics with bit 0 set.
//
// Negative control: control codes that take no cell (kPerturbFreeCodes)
// push the structure right and truncate the row; both rows fail.
//---------------------------------------------------------------------------
int runGap( int width, int height, int perturb = 0, bool quiet = false )
{
	const int gridFit = layout::kFitFill;
	const int row     = 11;
	int failures      = 0;

	struct Case
	{
		const char* name;
		std::function< int( int, int ) > colourAt;
		int expectedControls;
		bool allowBackground;
	};
	const auto redBlue = []( int sx, int ) { return sx < 40 ? 1 : 4; };
	const Case cases[] = {
		{ "A red | blue, foreground only", redBlue, 2, false },
		{ "B red | blue on red", []( int sx, int sy ) { return sx < 40 ? 1 : ( ( sx % 2 == 0 && sy % 3 == 0 ) ? 4 : 1 ); }, 3, true },
		{ "C red | blue, backgrounds allowed", redBlue, 0, true },
	};

	for( const Case& c : cases )
	{
		const Image source = buildSixels( width, height, gridFit, c.colourAt );
		Image out;
		std::vector< decoder::Cell > page;
		if( !renderStill( width, height,
		                  { { "Grid Fit", static_cast< float >( gridFit ) }, { "Allow Background", c.allowBackground ? 1.0f : 0.0f } },
		                  source, perturb, out, &page ) )
			return failures + 1;

		const decoder::Cell* cells = page.data() + static_cast< size_t >( row ) * codes::kColumns;
		std::vector< int > controls;
		for( int i = 0; i < codes::kColumns; ++i )
			if( cells[ i ].control )
				controls.push_back( i );

		bool ok      = !controls.empty() && controls[ 0 ] == 0;
		int boundary = -1;
		if( c.expectedControls == 0 )
		{
			//C: with backgrounds allowed the boundary is FREE. New Background
			//(shows red), the blue code (shows the red background), New
			//Background again (shows blue): three cells, none of them black.
			//On the picture: cell 0 black, 1..19 red, 20..39 blue, exactly.
			ok                       = ok && controls.size() >= 3;
			const layout::Layout lay = layout::Compute( gridFit, width, height );
			size_t wrong = 0, checked = 0;
			for( int i = 0; i < codes::kColumns; ++i )
			{
				const int want = i == 0 ? 0 : ( i < 20 ? 1 : 4 );
				for( const DotPixel& d : pixelsOfCell( lay, i, row ) )
				{
					++checked;
					if( paletteIndexOf( pixelAt( out, width, d.x, d.y ) ) != want )
						++wrong;
				}
			}
			ok = ok && wrong == 0 && checked > 0;
			if( !quiet )
			{
				std::printf( "gap %s: control cells at", c.name );
				for( int i : controls )
					std::printf( " %d", i );
				std::printf( "; %zu pixels of the row checked, %zu not the target colour  %s\n", checked, wrong, verdict( ok ) );
			}
			if( !ok )
				++failures;
			continue;
		}
		ok = ok && static_cast< int >( controls.size() ) == c.expectedControls;
		if( c.expectedControls == 2 )
		{
			ok       = ok && controls.size() == 2 && ( controls[ 1 ] == 19 || controls[ 1 ] == 20 );
			boundary = ok ? controls[ 1 ] : -1;
			for( int i = 1; ok && i < codes::kColumns; ++i )
			{
				if( i == boundary )
					continue;
				const bool red = i < boundary;
				ok = ok && cells[ i ].mosaic && codes::SixelBits( cells[ i ].shown ) == 0x3F && cells[ i ].fg == ( red ? 1 : 4 );
			}
		}
		else
		{
			ok = ok && controls.size() == 3 && controls[ 2 ] == controls[ 1 ] + 1 && controls[ 2 ] <= 20;
			//The two codes: New Background then the blue colour code.
			ok = ok && ( cells[ controls[ 1 ] ].bg == 1 ) && ( cells[ controls[ 2 ] ].bg == 1 );
			for( int i = 20; ok && i < codes::kColumns; ++i )
				ok = ok && cells[ i ].mosaic && cells[ i ].fg == 4 && cells[ i ].bg == 1 && codes::SixelBits( cells[ i ].shown ) == 0x01;
			for( int i = 1; ok && i < controls[ 1 ]; ++i )
				ok = ok && cells[ i ].mosaic && cells[ i ].fg == 1 && codes::SixelBits( cells[ i ].shown ) == 0x3F;
		}

		//On the picture: the control cells of case A are black, their
		//neighbours red and blue.
		int blackPixels = 0, wrongPixels = 0;
		if( ok && c.expectedControls == 2 )
		{
			const layout::Layout lay = layout::Compute( gridFit, width, height );
			for( const DotPixel& d : pixelsOfCell( lay, boundary, row ) )
			{
				const int colour = paletteIndexOf( pixelAt( out, width, d.x, d.y ) );
				if( colour == 0 )
					++blackPixels;
				else
					++wrongPixels;
			}
			for( const DotPixel& d : pixelsOfCell( lay, boundary - 1, row ) )
				if( paletteIndexOf( pixelAt( out, width, d.x, d.y ) ) != 1 )
					++wrongPixels;
			for( const DotPixel& d : pixelsOfCell( lay, boundary + 1, row ) )
				if( paletteIndexOf( pixelAt( out, width, d.x, d.y ) ) != 4 )
					++wrongPixels;
			ok = ok && blackPixels > 0 && wrongPixels == 0;
		}

		if( !quiet )
		{
			std::printf( "gap %s: control cells at", c.name );
			for( int i : controls )
				std::printf( " %d", i );
			std::printf( " (expected %d)", c.expectedControls );
			if( c.expectedControls == 2 )
				std::printf( "; boundary cell %d: %d black pixels, %d wrong", boundary, blackPixels, wrongPixels );
			std::printf( "  %s\n", verdict( ok ) );
		}
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "gap: a change of colour costs exactly one cell, a change of background exactly two"
		                                    : "gap: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --palette
//
// The test card, Hold on, no header, both grid fits, contiguous and
// separated. Every output pixel is one of the eight colours -- channels
// exactly 0 or 255, which an RGBA8 target holds exactly for a shader that
// writes 0.0 or 1.0 -- and within the dot grid every SIXEL rectangle is
// uniform: all its pixels one colour. At a fractional scale, pixels within
// 1e-3 dot of a dot boundary are left out (the harness in double and the
// shader in float may disagree about which side they are on); at a
// whole-pixel scale nothing is left out. Distinct colours must be at least
// 4, so the claim is not made of a black frame.
//
// Negative controls: the sixel's mean colour instead of the palette
// (kPerturbNoQuantise); Show Codes on, which tints the control cells.
//---------------------------------------------------------------------------
int runPalette( int width, int height, int perturb = 0, bool quiet = false, bool showCodes = false )
{
	int failures = 0;
	const Image card = buildCard( width, height, 7 );
	for( int gridFit = 0; gridFit < layout::kGridFitCount; ++gridFit )
		for( int sep = 0; sep < 2; ++sep )
		{
			Settings s = { { "Grid Fit", static_cast< float >( gridFit ) }, { "Hold Graphics", 1.0f },
			               { "Separated", static_cast< float >( sep ) } };
			if( showCodes )
				s.push_back( { "Show Codes", 1.0f } );
			Image out;
			if( !renderStill( width, height, s, card, perturb, out ) )
				return failures + 1;

			const layout::Layout lay = layout::Compute( gridFit, width, height );
			size_t offPalette = 0, checked = 0, nonUniform = 0;
			std::set< int > seen;
			//The colour each sixel was first seen with, -2 for not yet.
			std::vector< int > sixelColour( static_cast< size_t >( codes::kSixelCols ) * codes::kSixelRows * 2, -2 );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width; ++x )
				{
					const int colour = paletteIndexOf( pixelAt( out, width, x, y ) );
					++checked;
					if( colour < 0 )
					{
						++offPalette;
						continue;
					}
					seen.insert( colour );
					int dx, line;
					if( !dotOfPixel( lay, x, y, dx, line ) )
						continue;
					const int sx = dx / 3;
					const int sy = ( line / 10 ) * 3 + codes::SixelRowOfLine( line % 10 );
					//In separated mode a sixel is two classes of dot -- its
					//body and its gutter -- and each class is uniform.
					const size_t gutter = sep && !codes::SeparatedDotLit( dx % 6, line % 10 ) ? 1 : 0;
					int& first = sixelColour[ ( static_cast< size_t >( sy ) * codes::kSixelCols + sx ) * 2 + gutter ];
					if( first == -2 )
						first = colour;
					else if( first != colour )
						++nonUniform;
				}
			const bool ok = offPalette == 0 && nonUniform == 0 && seen.size() >= 4;
			if( !quiet )
				std::printf( "palette %s %s: %zu pixels, %zu off the palette, %zu in a non-uniform sixel, %zu colours  %s\n",
				             layout::GridFitName( gridFit ), sep ? "separated " : "contiguous", checked, offPalette, nonUniform,
				             seen.size(), verdict( ok ) );
			if( !ok )
				++failures;
		}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "palette: eight colours and nothing between, and every sixel is one of them"
		                                    : "palette: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --separated
//
// A white frame, Separated on, Hold off. Every row is then: 0x1A, the white
// mosaic colour code, and 38 full mosaics. In each of those cells, the dot
// at ( dx, line ) is white iff the SAA5050 lights it in separated mode:
// not the left column of its block ( dx mod 3 = 0 ) and not the bottom line
// of its block ( line 2, 6 or 9 ), so 28 of the 60 dots. Checked pixel by
// pixel over every mosaic cell of the page. At a whole-pixel scale every
// pixel of the cell is checked and the white count per cell is exactly
// 28 x kx x ky; at a fractional scale pixels within 1e-3 dot of a boundary
// are left out and only dots that own a pixel are seen.
//
// Negative control: the gutter on the other side (kPerturbGutterFlipped).
//---------------------------------------------------------------------------
int runSeparated( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( int gridFit = 0; gridFit < layout::kGridFitCount; ++gridFit )
	{
		const Image white = buildFlatColour( width, height, 7 );
		Image out;
		std::vector< decoder::Cell > page;
		//Backgrounds off: with them on, the encoder shows a white frame as
		//spaces on a white background and draws no mosaic at all.
		if( !renderStill( width, height,
		                  { { "Grid Fit", static_cast< float >( gridFit ) }, { "Separated", 1.0f }, { "Allow Background", 0.0f } },
		                  white, perturb, out, &page ) )
			return failures + 1;

		const layout::Layout lay = layout::Compute( gridFit, width, height );
		size_t wrong = 0, checked = 0, cellsChecked = 0, countWrong = 0;
		for( int row = 0; row < codes::kRows; ++row )
			for( int col = 0; col < codes::kColumns; ++col )
			{
				const decoder::Cell& cell = page[ static_cast< size_t >( row ) * codes::kColumns + col ];
				if( !cell.mosaic || cell.control || codes::SixelBits( cell.shown ) != 0x3F || cell.fg != 7 )
					continue;
				++cellsChecked;
				size_t whiteHere = 0;
				for( const DotPixel& d : pixelsOfCell( lay, col, row ) )
				{
					const int colour   = paletteIndexOf( pixelAt( out, width, d.x, d.y ) );
					const bool expectW = codes::SeparatedDotLit( d.dx, d.line );
					++checked;
					if( colour != ( expectW ? 7 : 0 ) )
						++wrong;
					if( colour == 7 )
						++whiteHere;
				}
				if( lay.wholePixel && whiteHere != static_cast< size_t >( 28 * lay.dotW * lay.dotH ) )
					++countWrong;
			}
		const bool ok = cellsChecked >= 24 * 38 && wrong == 0 && countWrong == 0 && checked > 0;
		if( !quiet )
			std::printf( "separated %s: %zu full white cells, %zu pixels checked, %zu wrong%s  %s\n",
			             layout::GridFitName( gridFit ), cellsChecked, checked, wrong,
			             lay.wholePixel ? ( countWrong == 0 ? ", every cell exactly 28 x kx x ky white" : ", white counts wrong" )
			                            : " (fractional scale: dots that own a pixel)",
			             verdict( ok ) );
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "separated: the gutter is the SAA5050's -- left column and bottom line of each block"
		                                    : "separated: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --carriage
//
// The source at frame k is a flat palette colour 1 + ( k mod 7 ), so every
// frame is a different colour from the last. Rows per Field 5, so the page
// takes five fields: rows 0-4 in a field with index = 0 mod 5, 5-9 in
// 1 mod 5, ... 20-23 in 4 mod 5. After N frames at fps f, row r must show
// the colour of the frame during which the LAST field carrying it fell --
// which the harness computes on its own: field( k ) = floor( 50 k / f +
// 1e-6 ) in double, frame k running fields field( k - 1 ) + 1 .. field( k ).
// Measured at cell ( r, 20 ) on the picture, at 60 fps and at 24 fps (two
// or three fields a frame). A resize to another raster halfway through the
// 60 fps run must change nothing: the page memory is not a texture.
//
// Negative control: every row every field (kPerturbAllRows).
//---------------------------------------------------------------------------
int runCarriage( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	const int rowsPerField = 5;
	auto colourOfFrame     = []( int k ) { return 1 + ( k % 7 ); };

	struct Run
	{
		double fps;
		int frames;
		bool resize;
	};
	const Run runs[] = { { 60.0, 17, false }, { 60.0, 17, true }, { 24.0, 9, false } };

	for( const Run& run : runs )
	{
		//What the harness expects, on its own arithmetic.
		std::vector< int > expected( codes::kRows, -1 );
		int64_t lastField = -1;
		for( int k = 0; k < run.frames; ++k )
		{
			const int64_t field = static_cast< int64_t >( std::floor( transmission::kFieldRate * k / run.fps + 1e-6 ) );
			int64_t from        = lastField < 0 ? field : lastField + 1;
			if( field - from + 1 > transmission::kMaxFieldsPerFrame )
				from = field;
			for( int64_t f = from; f <= field; ++f )
			{
				const int cycle = ( codes::kRows + rowsPerField - 1 ) / rowsPerField;
				const int slot  = static_cast< int >( f % cycle );
				for( int r = slot * rowsPerField; r < std::min( codes::kRows, slot * rowsPerField + rowsPerField ); ++r )
					expected[ static_cast< size_t >( r ) ] = colourOfFrame( k );
			}
			lastField = field;
		}

		Session session;
		session.fps = run.fps;
		if( !prepare( session, { { "Rows per Field", static_cast< float >( rowsPerField ) } }, perturb, width, height ) )
			return failures + 1;
		int w = width, h = height;
		for( int k = 0; k < run.frames; ++k )
		{
			if( run.resize && k == run.frames / 2 )
			{
				w = width == 320 ? 640 : 320;
				h = height == 180 ? 360 : 180;
				session.resize( w, h );
			}
			if( !session.render( k, buildFlatColour( w, h, colourOfFrame( k ) ) ) )
			{
				session.end();
				return failures + 1;
			}
		}
		const Image out = session.readBack();
		session.end();

		const layout::Layout lay = layout::Compute( layout::kFitFill, w, h );
		int wrongRows = 0, unseen = 0;
		std::string rowsShown;
		for( int r = 0; r < codes::kRows; ++r )
		{
			const std::vector< DotPixel > px = pixelsOfCell( lay, 20, r );
			int colour                       = -1;
			if( px.empty() )
				++unseen;
			else
				colour = paletteIndexOf( pixelAt( out, w, px[ px.size() / 2 ].x, px[ px.size() / 2 ].y ) );
			rowsShown += std::to_string( colour );
			if( colour != expected[ static_cast< size_t >( r ) ] )
				++wrongRows;
		}
		const bool ok = wrongRows == 0 && unseen == 0;
		if( !quiet )
		{
			std::string want;
			for( int e : expected )
				want += std::to_string( e );
			std::printf( "carriage %g fps, %d frames%s: rows show %s, expected %s: %d rows wrong  %s\n", run.fps, run.frames,
			             run.resize ? ", resized halfway" : "", rowsShown.c_str(), want.c_str(), wrongRows, verdict( ok ) );
		}
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "carriage: each row shows the frame of the field that carried it, through a resize"
		                                    : "carriage: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --parity
//
// The test card, Hold off, a clean signal, rendered once clean and then with
// one forced error on a mosaic cell ( row 10, the first full-or-partial
// mosaic cell from column 5 ), on the same frame:
//
//   single bit (bit 3 of the data): parity fails, the decoder shows a space,
//   and on the picture every pixel of the cell is the cell's background
//   colour; every pixel OUTSIDE the cell is identical to the clean render.
//
//   double bit (bits 0 and 1): parity passes, the mosaic is a different
//   mosaic -- the cell's pixels differ from the clean render somewhere, are
//   all on the palette, and every pixel outside the cell is identical.
//
// "Identical outside the cell" is what "no error moves a row" means on the
// picture. Then, with Signal Quality 0.3 for twenty frames, every pixel is
// still on the palette and the page is still 24 rows of 40 decodable cells.
//
// Negative controls: a decoder that ignores parity (the single-bit cell is
// not blanked); a double error that flips the same bit twice (the mosaic
// does not change); errors that move rows (pixels outside the cell change).
//---------------------------------------------------------------------------
int runParity( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures      = 0;
	const int gridFit = layout::kFitFill;
	const Image card  = buildCard( width, height, 3 );
	const Settings s = { { "Grid Fit", static_cast< float >( gridFit ) } };

	Image clean;
	std::vector< decoder::Cell > page;
	if( !renderStill( width, height, s, card, perturb, clean, &page ) )
		return 1;

	//A mosaic cell with at least one lit and one unlit sixel, so both a
	//blank and a change are visible: the first such cell from row 1 down.
	int row = -1, col = -1;
	for( int r = 1; r < codes::kRows && row < 0; ++r )
		for( int c = 1; c < codes::kColumns - 1; ++c )
		{
			const decoder::Cell& cell = page[ static_cast< size_t >( r ) * codes::kColumns + c ];
			const int bits            = codes::SixelBits( cell.shown );
			if( cell.mosaic && !cell.control && bits != 0 && bits != 0x3F && cell.fg != cell.bg )
			{
				row = r;
				col = c;
				break;
			}
		}
	if( col < 0 )
	{
		if( !quiet )
			std::printf( "parity: no partial mosaic cell on the page to break  FAILED\n" );
		return 1;
	}
	const decoder::Cell& target = page[ static_cast< size_t >( row ) * codes::kColumns + col ];
	const layout::Layout lay    = layout::Compute( gridFit, width, height );
	const std::vector< DotPixel > cellPixels = pixelsOfCell( lay, col, row );
	//"Outside the cell" is judged generously: a pixel within a pixel of the
	//cell's rectangle is the cell's, whichever side of a fractional boundary
	//the shader put it. The inside assertions use the strict set above.
	const int cx0 = static_cast< int >( std::floor( lay.drawX + col * 6 * lay.dotW ) ) - 1;
	const int cx1 = static_cast< int >( std::ceil( lay.drawX + ( col + 1 ) * 6 * lay.dotW ) ) + 1;
	const int cy0 = static_cast< int >( std::floor( lay.drawY + row * 10 * lay.dotH ) ) - 1;
	const int cy1 = static_cast< int >( std::ceil( lay.drawY + ( row + 1 ) * 10 * lay.dotH ) ) + 1;
	auto inCell   = [ & ]( int x, int y ) {
		return x >= cx0 && x < cx1 && y >= cy0 && y < cy1;
	};

	for( int kind = 0; kind < 2; ++kind )
	{
		Session session;
		if( !prepare( session, s, perturb, width, height ) )
			return failures + 1;
		transmission::ForcedError forced;
		forced.row    = row;
		forced.column = col;
		forced.bitA   = kind == 0 ? 3 : 0;
		forced.bitB   = kind == 0 ? -1 : 1;
		session.plugin.SetForcedErrorForTest( forced );
		for( int frame = 0; frame < 2; ++frame )
			session.render( frame, card );
		const Image out = session.readBack();
		const std::vector< decoder::Cell > broken = session.plugin.PageForTest();
		session.end();

		std::vector< char > strict( static_cast< size_t >( width ) * height, 0 );
		for( const DotPixel& d : cellPixels )
			strict[ static_cast< size_t >( d.y ) * width + d.x ] = 1;
		size_t outsideChanged = 0, insideBg = 0, insideChanged = 0, insideOff = 0;
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				const unsigned char* a = pixelAt( clean, width, x, y );
				const unsigned char* b = pixelAt( out, width, x, y );
				const bool same        = std::memcmp( a, b, 4 ) == 0;
				if( !inCell( x, y ) )
				{
					if( !same )
						++outsideChanged;
					continue;
				}
				if( !strict[ static_cast< size_t >( y ) * width + x ] )
					continue;
				const int colour = paletteIndexOf( b );
				if( colour < 0 )
					++insideOff;
				if( colour == target.bg )
					++insideBg;
				if( !same )
					++insideChanged;
			}
		const decoder::Cell& shown = broken[ static_cast< size_t >( row ) * codes::kColumns + col ];
		bool ok;
		if( kind == 0 )
			ok = shown.parityBad && insideBg == cellPixels.size() && outsideChanged == 0 && !cellPixels.empty();
		else
			ok = !shown.parityBad && shown.mosaic && shown.shown != target.shown && insideChanged > 0 && insideOff == 0
			     && outsideChanged == 0;
		if( !quiet )
			std::printf( "parity %s at (%d, %d), mosaic 0x%02X -> 0x%02X%s: %zu of %zu cell pixels background, %zu changed, %zu off "
			             "palette; %zu pixels changed outside the cell  %s\n",
			             kind == 0 ? "single bit" : "double bit", row, col, target.shown, shown.shown,
			             shown.parityBad ? " (parity failed)" : "", insideBg, cellPixels.size(), insideChanged, insideOff,
			             outsideChanged, verdict( ok ) );
		if( !ok )
			++failures;
	}

	//A noisy signal: still eight colours, still a page.
	{
		Settings noisy = s;
		noisy.push_back( { "Signal Quality", 0.3f } );
		Image out;
		std::vector< decoder::Cell > noisyPage;
		if( !renderStill( width, height, noisy, card, perturb, out, &noisyPage, 20 ) )
			return failures + 1;
		size_t off = 0, bad = 0;
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
				if( paletteIndexOf( pixelAt( out, width, x, y ) ) < 0 )
					++off;
		for( const decoder::Cell& c : noisyPage )
			if( c.parityBad )
				++bad;
		const bool ok = off == 0 && bad > 0;
		if( !quiet )
			std::printf( "parity at Signal Quality 0.3: %zu pixels off the palette, %zu of %zu cells failed parity  %s\n", off, bad,
			             noisyPage.size(), verdict( ok ) );
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "parity: a single bit blanks the cell, two bits change the mosaic, and no row moves"
		                                    : "parity: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each of these perturbs the MODEL
// -- through a hook the shipped plugin carries at zero, or a real control --
// and asserts that the check catches it.
//---------------------------------------------------------------------------
int runNegative( int width, int height )
{
	struct Control
	{
		const char* name;
		int failuresSeen;
	};
	const Control controls[] = {
		{ "optimal with the greedy encoder                 ", runOptimal( codes::kPerturbGreedy, true ) },
		{ "gap with control codes that take no cell        ", runGap( width, height, codes::kPerturbFreeCodes, true ) },
		{ "palette with the sixel mean instead of a colour ", runPalette( width, height, codes::kPerturbNoQuantise, true ) },
		{ "palette with Show Codes on                      ", runPalette( width, height, 0, true, true ) },
		{ "separated with the gutter on the other side     ", runSeparated( width, height, codes::kPerturbGutterFlipped, true ) },
		{ "carriage with every row every field             ", runCarriage( width, height, codes::kPerturbAllRows, true ) },
		{ "parity with a decoder that ignores parity       ", runParity( width, height, codes::kPerturbIgnoreParity, true ) },
		{ "parity with a double error on one bit twice     ", runParity( width, height, codes::kPerturbSameBitTwice, true ) },
		{ "parity with errors that move rows               ", runParity( width, height, codes::kPerturbErrorsMoveRows, true ) },
	};

	int failures = 0;
	for( const Control& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Teletext& plugin, int width, int height, int frames, double fps )
{
	Session session;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Teletext::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card moves, so every field re-encodes rows that changed -- the real
	//cost, not a cached one. Eight frames of it are uploaded ONCE into eight
	//textures and cycled by handle, the way a host hands over a texture it
	//already has: the upload is not in the figure.
	constexpr int kCards = 8;
	GLuint textures[ kCards ];
	for( int i = 0; i < kCards; ++i )
	{
		const Image flipped = flipRows( buildCard( width, height, i * 5 ), width, height );
		textures[ i ]       = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, flipped.data() );
	}
	auto renderCard = [ & ]( int frame ) {
		session.inputStruct.Handle = textures[ frame % kCards ];
		session.renderAt( frame );
	};

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		renderCard( frame );
	glFinish();

	//The best of three runs: this machine's GPU is shared with whatever else
	//is rendering, and the minimum is the run nothing else interrupted.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			renderCard( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}

	session.inputStruct.Handle = session.sourceTexture;
	glDeleteTextures( kCards, textures );
	session.end();
	return best;
}

int runBench( Teletext& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( plugin, size.width, size.height, frames, fps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0 );
	}

	std::printf( "\nThe figure includes the source upload, the 80x72 read-back and the CPU\n"
	             "encoder for the rows each field carries (Rows per Field %d here).\n",
	             controls::RowsPerField( plugin.GetFloatParameter( Teletext::PT_ROWS_PER_FIELD ) ) );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },
		{ "cells.frag", shaders::Cells() },
		{ "render.frag", shaders::Render() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line. Same format as the rest
// of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"txtest -- render and measure the Teletext effect\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/teletext.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --source S          card (default), flat, or white\n"
		"  --level N           the flat source's palette colour, 0..7 (default 1)\n"
		"  --set \"Name=V\"      set a parameter by its display name. Repeatable.\n"
		"  --list              print every parameter, its kind, default and range, then exit\n"
		"  --optimal           the row programme equals an exhaustive search (no GL)\n"
		"  --gap               a colour change costs one cell, a background change two\n"
		"  --palette           eight colours, every sixel uniform\n"
		"  --separated         the separated gutter is the SAA5050's\n"
		"  --carriage          each row shows the frame of the field that carried it\n"
		"  --parity            one bit blanks a cell, two change it, none moves a row\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks against a perturbed model (Codes.h), verbosely\n"
		"  --bench             time ProcessOpenGL at 720p through 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/teletext.png";
	std::string scriptPath;
	std::string sourceName = "card";
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int level      = 1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::atoi( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--optimal" || argument == "--gap" || argument == "--palette" || argument == "--separated"
		         || argument == "--carriage" || argument == "--parity" || argument == "--negative" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed, so it is answered before a context is made -- which also
		//means it works on a machine where creating one fails, and in CI.
		Teletext plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low,
			             p.high );
		return 0;
	}

	//--optimal alone needs no context either.
	if( checks.size() == 1 && checks[ 0 ] == "--optimal" )
		return runOptimal( perturb ) == 0 ? 0 : 1;

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( !checks.empty() )
	{
		int failures = 0;
		for( const std::string& check : checks )
		{
			int result = 0;
			if( check == "--optimal" )
				result = runOptimal( perturb );
			else if( check == "--gap" )
				result = runGap( width, height, perturb );
			else if( check == "--palette" )
				result = runPalette( width, height, perturb );
			else if( check == "--separated" )
				result = runSeparated( width, height, perturb );
			else if( check == "--carriage" )
				result = runCarriage( width, height, perturb );
			else if( check == "--parity" )
				result = runParity( width, height, perturb );
			else if( check == "--negative" )
				result = runNegative( width, height );
			failures += result;
			std::printf( "\n" );
		}
		return finish( failures == 0 ? 0 : 1 );
	}

	Session session;
	session.fps = fps;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantBench )
		return finish( runBench( session.plugin, frames, fps ) );

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		//A reader that hangs up must end the take with exit 1 and a message,
		//not SIGPIPE's silent 141: write() then fails and the loop says so.
		std::signal( SIGPIPE, SIG_IGN );
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		Image frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame at the end of a pipe is the end of the stream,
			//not a frame to render: the stream ends cleanly, and only whole
			//frames ever come out.
			if( got < frame.size() )
				break;

			//Through the plugin's own setter, so a cue moves the same thing an
			//operator's slider would.
			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			if( !session.render( index, frame ) )
			{
				status = 1;
				break;
			}

			const Image out = session.readBack();
			size_t written  = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone. Rendering on into a closed pipe is work
			//nobody will see, and a short frame on stdout is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}

		session.end();
		return finish( status );
	}

	for( int frame = 0; frame < frames; ++frame )
	{
		Image pixels;
		if( sourceName == "flat" )
			pixels = buildFlatColour( width, height, std::clamp( level, 0, 7 ) );
		else if( sourceName == "white" )
			pixels = buildFlatColour( width, height, 7 );
		else
			pixels = buildCard( width, height, frame );
		if( !session.render( frame, pixels ) )
			return finish( 1 );
	}

	const Image image = session.readBack();
	session.end();

	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
