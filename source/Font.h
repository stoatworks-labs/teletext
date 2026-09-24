#pragma once

#include <cstdint>
#include <vector>

/**
    A 5x7 bitmap font, for the edge print.

    graticule's font, copied unchanged (same organisation, same licence): the
    glyphs are written as pictures in Font.cpp so they can be read. Here it is
    used for one thing only -- `Frame.cpp` draws the edge print's stock code
    and frame numbers into a small texture, which the film pass exposes as a
    latent image. Printable ASCII only (32..126), one row per glyph line, top
    row first, and column 0 is the leftmost pixel.
*/
namespace teletext::font
{
constexpr int kFirst  = 32;///< first code covered
constexpr int kCount  = 96;///< 32..127
constexpr int kWidth  = 5;
constexpr int kHeight = 7;
/// Horizontal advance: the glyph plus one blank column.
constexpr int kAdvance = kWidth + 1;

/// The glyph for `code` as seven rows of five characters, '#' for a lit pixel.
/// Anything outside the covered range is a blank glyph.
const char* const* Glyph( int code );

/// Is pixel (x, y) of glyph `code` lit?
bool Bit( int code, int x, int y );

/// The whole table as an 8-bit single-channel image, kWidth*128 wide and
/// kHeight tall, indexed by ASCII code directly: glyph for code c starts at
/// column c*kWidth. Codes below kFirst are blank so the shader never has to
/// subtract anything.
std::vector< uint8_t > Texture();

constexpr int kTextureWidth  = kWidth * 128;
constexpr int kTextureHeight = kHeight;

} // namespace teletext::font
