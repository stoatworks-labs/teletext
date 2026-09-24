#pragma once

#include "Decoder.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"
#include "Transmission.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
	Teletext -- a picture sent as Level 1 teletext mosaic graphics, as an
	FFGL effect.

	**The one idea.** A teletext page is 40 x 24 cells of 2 x 3 sixels in
	eight colours, and colour is not stored per cell: it is set by control
	codes that each OCCUPY a cell. So encoding a picture is an optimisation
	under a hard serial constraint, and running the clip through an exactly
	optimal encoder (a Viterbi programme per row, `Encoder.h`) is what gives
	the look: the black column at every colour boundary, colours rationed,
	Hold Mosaics filling the gaps, a page that arrives a few rows a field
	and tears, and bit errors that blank a cell or corrupt a mosaic but
	never move a row (`Transmission.h`).

	Two GPU passes and one CPU stage (`Shaders.h`): the cells pass reduces
	the source to 80 x 72 sixel means, which are read back; the encoder and
	the transmission run on the CPU and write a 40 x 24 cell texture; the
	render pass draws the SAA5050's picture from it. See AGENTS.md.
*/
class Teletext : public CFFGLPlugin
{
public:
	Teletext();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook: the harness DECLARES its unit rather than leaving the
	/// voting to infer one.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks, a bitmask of `codes::Perturb`. Always 0 in
	/// the plugin.
	void SetPerturbForTest( int bits );

	/// Force a bit error on every transmission of one cell (row < 0 clears).
	void SetForcedErrorForTest( const teletext::transmission::ForcedError& error );

	/// The page as decoded on the last frame: 24 x 40 cells, row-major.
	const std::vector< teletext::decoder::Cell >& PageForTest() const
	{
		return decoded;
	}

	/// The received bytes of a row on the last frame.
	const uint8_t* RowBytesForTest( int row ) const
	{
		return page.Bytes( row );
	}

	/// The sixel means read back on the last frame: 80 x 72 x RGBA, row 0 top.
	const std::vector< float >& MeansForTest() const
	{
		return means;
	}

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Encoder
		PT_ERROR_SPACE,
		PT_HOLD,
		PT_ALLOW_BACKGROUND,
		PT_SEPARATED,

		//Transmission
		PT_ROWS_PER_FIELD,
		PT_SIGNAL_QUALITY,
		PT_FREEZE,

		//Display
		PT_GRID_FIT,
		PT_HEADER,
		PT_PAGE_NUMBER,
		PT_SHOW_CODES,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	void encodeRow( int row, uint8_t* out ) const;
	void uploadCells();

	ffglex::FFGLShader cellsShader;
	ffglex::FFGLShader renderShader;
	ffglex::FFGLScreenQuad quad;

	teletext::PassBuffer cells;///< 80 x 72 sixel means, RGBA32F
	GLuint cellTexture  = 0;   ///< 40 x 24 RGBA8: fg, bg, code, flags
	GLuint glyphTexture = 0;   ///< the font, R8

	std::vector< float > means;                   ///< the cells pass, read back
	teletext::transmission::Page page;            ///< the receiver's page memory
	std::vector< teletext::decoder::Cell > decoded;///< 24 x 40, what the page shows
	std::vector< uint8_t > cellBytes;             ///< the cell texture's pixels
	int errorSpace     = 0;
	bool holdOn        = true;
	bool allowBackground = true;
	bool separated     = false;

	//--- the clock (readout's unit voting, by way of rebate) ---------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	int perturb = 0;
	teletext::transmission::ForcedError forced;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
