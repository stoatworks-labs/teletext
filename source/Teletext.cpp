#include "Teletext.h"

#include "Codes.h"
#include "Controls.h"
#include "Diag.h"
#include "Encoder.h"
#include "Font.h"
#include "Header.h"
#include "Layout.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace teletext;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Teletext >,                                   // Create method
	"TX01",                                                      // Plugin unique ID of maximum length 4.
	"SW Teletext",                                               // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"A picture sent as Level 1 teletext mosaic graphics.\n\n40 x 24 cells of 2 x 3 sixels in eight colours, where every change of colour is a control code that costs a cell. An exactly optimal encoder runs each row, the page arrives a few rows a field, and bit errors land the teletext way: a single bit blanks a cell, two bits corrupt a mosaic, and a row never moves.",// Plugin description
	"Teletext FFGL effect"                                       // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

//---------------------------------------------------------------------------
Teletext::Teletext()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The page arrives by the field, and a field is a function of the host's
	//clock: a re-render of the same composition must tear the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. RGB error, Hold Mosaics on (the artists' trick), backgrounds
	// allowed, contiguous mosaics; four rows a field (a page every six
	// fields), a clean signal, running; the 4:3 safe area with a header on
	// page 100, codes not shown, the effect fully in.
	//---------------------------------------------------------------------
	params[ PT_ERROR_SPACE ]      = 0.0f;
	params[ PT_HOLD ]             = 1.0f;
	params[ PT_ALLOW_BACKGROUND ] = 1.0f;
	params[ PT_SEPARATED ]        = 0.0f;

	params[ PT_ROWS_PER_FIELD ] = 4.0f;
	params[ PT_SIGNAL_QUALITY ] = 1.0f;
	params[ PT_FREEZE ]         = 0.0f;

	params[ PT_GRID_FIT ]    = 0.0f;
	params[ PT_HEADER ]      = 1.0f;
	params[ PT_PAGE_NUMBER ] = 100.0f;
	params[ PT_SHOW_CODES ]  = 0.0f;
	params[ PT_MIX ]         = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. The two counts are FF_TYPE_INTEGER, which SetParamInfo
	// does not clamp into 0..1; the two option lists are mapped by index in
	// Controls.cpp because an option's range reads back 0..1 whatever its
	// element count.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};
	auto declareInteger = [ this ]( unsigned int id, const char* name, float lo, float hi ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, lo, hi );
	};

	declareOptions( PT_ERROR_SPACE, "Error Space", controls::kErrorSpaceCount, controls::ErrorSpaceName );
	SetParamInfo( PT_HOLD, "Hold Graphics", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_ALLOW_BACKGROUND, "Allow Background", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_SEPARATED, "Separated", FF_TYPE_BOOLEAN, false );

	declareInteger( PT_ROWS_PER_FIELD, "Rows per Field", static_cast< float >( controls::kRowsPerFieldMin ),
	                static_cast< float >( controls::kRowsPerFieldMax ) );
	SetParamInfof( PT_SIGNAL_QUALITY, "Signal Quality", FF_TYPE_STANDARD );
	SetParamInfo( PT_FREEZE, "Freeze", FF_TYPE_BOOLEAN, false );

	declareOptions( PT_GRID_FIT, "Grid Fit", layout::kGridFitCount, layout::GridFitName );
	SetParamInfo( PT_HEADER, "Header", FF_TYPE_BOOLEAN, true );
	declareInteger( PT_PAGE_NUMBER, "Page Number", static_cast< float >( controls::kPageMin ),
	                static_cast< float >( controls::kPageMax ) );
	SetParamInfo( PT_SHOW_CODES, "Show Codes", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_ERROR_SPACE; i <= PT_SEPARATED; ++i )
		SetParamGroup( i, "Encoder" );
	for( FFUInt32 i = PT_ROWS_PER_FIELD; i <= PT_FREEZE; ++i )
		SetParamGroup( i, "Transmission" );
	for( FFUInt32 i = PT_GRID_FIT; i <= PT_MIX; ++i )
		SetParamGroup( i, "Display" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	means.assign( static_cast< size_t >( codes::kSixelCols ) * codes::kSixelRows * 4, 0.0f );
	decoded.assign( static_cast< size_t >( codes::kRows ) * codes::kColumns, decoder::Cell() );
	cellBytes.assign( static_cast< size_t >( codes::kRows ) * codes::kColumns * 4, 0 );

	FFGLLog::LogToHost( "Created Teletext effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Teletext::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &cellsShader, shaders::Cells(), "cells" },
		{ &renderShader, shaders::Render(), "render" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Teletext: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Teletext: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//The font, once. Nearest and unfiltered: a glyph dot is a dot.
	{
		const std::vector< uint8_t > glyphs = font::Texture();
		glGenTextures( 1, &glyphTexture );
		glBindTexture( GL_TEXTURE_2D, glyphTexture );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, font::kTextureWidth, font::kTextureHeight, 0, GL_RED, GL_UNSIGNED_BYTE,
		              glyphs.data() );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//The cell texture: 40 x 24, rewritten every frame.
	{
		glGenTextures( 1, &cellTexture );
		glBindTexture( GL_TEXTURE_2D, cellTexture );
		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, codes::kColumns, codes::kRows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	page.Reset();
	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Teletext::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Teletext::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
void Teletext::encodeRow( int row, uint8_t* out ) const
{
	//The row's 40 cells x 6 sixels of linear RGB, from the read-back means.
	float rgb[ codes::kColumns * 6 * 3 ];
	for( int i = 0; i < codes::kColumns; ++i )
		for( int s = 0; s < 6; ++s )
		{
			const int sx        = i * 2 + ( s & 1 );
			const int sy        = row * 3 + ( s >> 1 );
			const float* texel  = means.data() + ( static_cast< size_t >( sy ) * codes::kSixelCols + sx ) * 4;
			float* target       = rgb + ( static_cast< size_t >( i ) * 6 + s ) * 3;
			target[ 0 ]         = texel[ 0 ];
			target[ 1 ]         = texel[ 1 ];
			target[ 2 ]         = texel[ 2 ];
		}

	encoder::CostTable table;
	encoder::BuildCosts( rgb, codes::kColumns, errorSpace, table );

	encoder::Options options;
	options.allowBackground = allowBackground;
	options.hold            = holdOn;
	options.separated       = separated;
	options.perturb         = perturb;

	const encoder::Result result = encoder::EncodeRow( table, options );
	std::copy( result.bytes, result.bytes + codes::kColumns, out );
}

void Teletext::uploadCells()
{
	const bool ignoreParity = ( perturb & codes::kPerturbIgnoreParity ) != 0;
	for( int r = 0; r < codes::kRows; ++r )
	{
		decoder::Cell* cells = decoded.data() + static_cast< size_t >( r ) * codes::kColumns;
		page.Decode( r, cells, ignoreParity );
		for( int c = 0; c < codes::kColumns; ++c )
		{
			const decoder::Cell& cell = cells[ c ];
			uint8_t* px = cellBytes.data() + ( static_cast< size_t >( r ) * codes::kColumns + c ) * 4;
			px[ 0 ]     = cell.fg;
			px[ 1 ]     = cell.bg;
			px[ 2 ]     = cell.shown;
			px[ 3 ]     = static_cast< uint8_t >( ( cell.mosaic ? 1 : 0 ) | ( cell.separated ? 2 : 0 ) | ( cell.control ? 4 : 0 ) );
		}
	}
	glBindTexture( GL_TEXTURE_2D, cellTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, codes::kColumns, codes::kRows, GL_RGBA, GL_UNSIGNED_BYTE, cellBytes.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

//---------------------------------------------------------------------------
FFResult Teletext::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock. One thing reads it: the field counter (and the header's
	// clock, which is the same seconds). Reduced in double here; nothing
	// absolute crosses into GLSL.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	errorSpace      = controls::OptionIndex( params[ PT_ERROR_SPACE ], controls::kErrorSpaceCount );
	holdOn          = params[ PT_HOLD ] > 0.5f;
	allowBackground = params[ PT_ALLOW_BACKGROUND ] > 0.5f;
	separated       = params[ PT_SEPARATED ] > 0.5f;

	const int rowsPerField = controls::RowsPerField( params[ PT_ROWS_PER_FIELD ] );
	const double quality   = controls::SignalQuality( params[ PT_SIGNAL_QUALITY ] );
	const bool frozen      = params[ PT_FREEZE ] > 0.5f;

	const int gridFit     = controls::OptionIndex( params[ PT_GRID_FIT ], layout::kGridFitCount );
	const bool headerOn   = params[ PT_HEADER ] > 0.5f;
	const int pageNumber  = controls::PageNumber( params[ PT_PAGE_NUMBER ] );
	const bool showCodes  = params[ PT_SHOW_CODES ] > 0.5f;
	const float mixAmount = std::clamp( params[ PT_MIX ], 0.0f, 1.0f );

	const layout::Layout lay = layout::Compute( gridFit, width, height );

	//---------------------------------------------------------------------
	// Buffers. Allocation happens here, before anything binds a texture:
	// allocating leaves the active unit bound to nothing, and the symptom
	// of getting the order wrong is correct on every frame except the one
	// that allocates. The page memory is CPU-side and 24 x 40 whatever the
	// raster, so a resize cannot clear it.
	//---------------------------------------------------------------------
	if( !cells.Ensure( codes::kSixelCols, codes::kSixelRows, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the cells buffer" );
		return FF_FAIL;
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//---------------------------------------------------------------------
	// 1. Cells: the sixel means, in linear light.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( cells.GetGLID(), ScopedFBOBinding::RB_REVERT );
		cells.ResizeViewPort();
		ScopedShaderBinding shader( cellsShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		cellsShader.Set( "InputTexture", 0 );
		glUniform2i( cellsShader.FindUniform( "InputSize" ), width, height );
		cellsShader.Set( "DrawOrigin", static_cast< float >( lay.drawX ), static_cast< float >( lay.drawY ) );
		cellsShader.Set( "DotSize", static_cast< float >( lay.dotW ), static_cast< float >( lay.dotH ) );
		quad.Draw();
	}

	//Read them back: 80 x 72 x 4 floats, 92 KB. The one stall in the plugin,
	//and the price of an encoder that is exact.
	glBindTexture( GL_TEXTURE_2D, cells.TextureID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, means.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	//---------------------------------------------------------------------
	// 2. The transmission: the fields elapsed since the last frame, each
	// carrying its rows of the page encoded from THIS frame's means.
	//---------------------------------------------------------------------
	const int64_t field = transmission::FieldIndex( now );
	std::function< void( uint8_t* ) > header;
	if( headerOn )
		header = [ & ]( uint8_t* out ) { header::Compose( pageNumber, now, out ); };
	page.Advance( field, rowsPerField, quality, frozen, perturb, [ this ]( int row, uint8_t* out ) { encodeRow( row, out ); },
	              header, forced );

	uploadCells();

	//---------------------------------------------------------------------
	// 3. Render, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( renderShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding cellsBinding( cellTexture );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding glyphBinding( glyphTexture );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding meansBinding( cells.TextureID() );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding sourceBinding( input.Handle );

		renderShader.Set( "Cells", 0 );
		renderShader.Set( "Glyphs", 1 );
		renderShader.Set( "Means", 2 );
		renderShader.Set( "Source", 3 );
		renderShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( renderShader.FindUniform( "OutSize" ), width, height );
		renderShader.Set( "DrawOrigin", static_cast< float >( lay.drawX ), static_cast< float >( lay.drawY ) );
		renderShader.Set( "DotSize", static_cast< float >( lay.dotW ), static_cast< float >( lay.dotH ) );
		renderShader.Set( "ShowCodes", showCodes ? 1 : 0 );
		renderShader.Set( "MixAmount", mixAmount );
		renderShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Teletext::DeInitGL()
{
	cellsShader.FreeGLResources();
	renderShader.FreeGLResources();
	quad.Release();
	cells.Destroy();
	if( cellTexture != 0 )
	{
		glDeleteTextures( 1, &cellTexture );
		cellTexture = 0;
	}
	if( glyphTexture != 0 )
	{
		glDeleteTextures( 1, &glyphTexture );
		glyphTexture = 0;
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Teletext::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Teletext::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Teletext::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Teletext::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Teletext::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Teletext::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Teletext::SetForcedErrorForTest( const transmission::ForcedError& error )
{
	forced = error;
}
