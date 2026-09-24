#pragma once

#include <string>

/**
	The two passes.

	1. **cells** -- 80 x 72, RGBA32F, one fragment per sixel. The mean of the
	   source pixels whose centres fall inside the sixel's rectangle on the
	   output (`Layout::SixelBounds`), in linear light. Read back to the CPU,
	   where the encoder runs: the Viterbi programme is a serial dynamic
	   programme over a row, and exactness against an exhaustive search is
	   what the harness proves, so it is integer arithmetic on the CPU and not
	   a shader.

	2. **render** -- the host's framebuffer. The SAA5050's picture: for each
	   output pixel, which cell and which dot of its 6 x 10 matrix, then the
	   cell's foreground or background by the mosaic's sixel bits (contiguous
	   or separated) or the glyph's dot; the eight colours and nothing between.
	   Show Codes tints the control cells; Mix blends with the source.

	`txtest --dump-shaders DIR` writes exactly these strings, which is what
	`tools/verify.sh` hands to glslc.
*/
namespace teletext::shaders
{

std::string Vertex();
std::string Cells();
std::string Render();

} // namespace teletext::shaders
