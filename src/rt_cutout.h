#pragma once
// (v0.9.552) The ray tracing probe's leaf cutouts: what a game pipeline's alpha test reads, from its shaders (DXBC).
// The game's alpha-tested materials (foliage, hair cards, fences) discard in their pixel shaders: one texture's
// component at the interpolated TEXCOORD (x the material's tiling, + its offset) against DrawableBuffer.AlphaThreshold.
// That TEXCOORD is the vertex shader's TEXCOORD input x StreamDeclBuffer.TexcoordScales (the stored UVs are scaled
// integers). Analyze follows the first discard back to them: the texture's register and component, the vertex input the
// UV comes from, and the constants (constant buffer register and float, or a literal) of each step. Pipelines whose
// alpha test takes another shape are reported, not followed (the probe takes them as opaque).
#include <cstddef>
#include <cstdint>

namespace RtCutout {

// A value the alpha test reads: a float of the constant buffer bound to the stage at register `slot` (at = float index
// from the buffer's start: element x 4 + component), or a literal (slot -1)
struct Constant {
    std::int8_t slot = -1;
    std::uint16_t at = 0;
    float literal = 0.0f;
};

enum Result : std::uint8_t {
    kNone = 0,         // no discard in the pixel shader (or no pixel shader)
    kFollowed = 1,
    kUnfollowed = 2,   // an alpha test the analysis does not follow (why says what)
};

struct Program {
    std::uint8_t result = kNone;
    std::uint8_t texture = 0;             // the pixel shader's texture register (its SRV slot)
    std::uint8_t channel = 3;             // the texel's component compared
    std::uint8_t texcoord = 0;            // the vertex shader's TEXCOORD input the UV comes from (its semantic index)
    std::uint8_t component[2] = {0, 1};   // that element's components for u and v
    // u, v = (texcoord x vsScale + vsOffset) x psTiling + psOffset
    Constant vsScale[2] = {{-1, 0, 1.0f}, {-1, 0, 1.0f}};
    Constant vsOffset[2] = {{-1, 0, 0.0f}, {-1, 0, 0.0f}};
    Constant psTiling[2] = {{-1, 0, 1.0f}, {-1, 0, 1.0f}};
    Constant psOffset[2] = {{-1, 0, 0.0f}, {-1, 0, 0.0f}};
    Constant threshold = {-1, 0, 0.5f};   // discarded below it
    char why[40] = {};                     // kUnfollowed: the first step not followed
};

// From a pipeline's vertex and pixel shaders
void Analyze(const void* vs, std::size_t vsSize, const void* ps, std::size_t psSize, Program& out);

// The pixel shader alone (offline tests): its UV's input semantic instead of the vertex shader's TEXCOORD
void AnalyzePixel(const void* ps, std::size_t psSize, Program& out, char (&semantic)[32], unsigned& semanticIndex,
                  unsigned (&components)[2]);
// The vertex shader's part: the output semantic (name, index) and its components (u, v) back to its TEXCOORD input
bool AnalyzeVertex(const void* vs, std::size_t vsSize, const char* semantic, unsigned semanticIndex,
                   const unsigned (&components)[2], Program& out);

}  // namespace RtCutout
