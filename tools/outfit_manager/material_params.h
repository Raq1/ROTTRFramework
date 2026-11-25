#pragma once
// ROTTR Modding Suite: what a material's constants do, read from its shaders' code.
//
// The game keeps a material's parameters as one float4 array per pass (the shaders' MaterialBuffer, "MaterialParams";
// the parameters themselves have no names in the game's files). What each value does can be read from the pixel
// shaders' code: which texture's coordinates it scales or moves, which texture it multiplies. Only those are
// offered for editing; a parameter used by several passes (e.g. a texture's tiling) is one entry that writes them all.

#include "materials.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace MaterialParams {
enum class Kind {
    UvScale,         // texture coordinates x this (u, v)
    UvOffset,        // texture coordinates + this (u, v)
    Tint,            // a color texture's r, g, b x this
    AlphaScale,      // a texture's alpha x this (also the alpha test's)
    Scale,           // a data texture's channels x this (normal maps: the bumps' strength)
    Bias,            // added to a texture's channels after scaling
    HairHighlight,   // strand hair: the highlights' strength and sharpness (read by the engine's hair pass)
    NormalStrength,  // a normal map's bumps x this (both of its channels)
    Color,           // a color x a texture's channel (e.g. a layer's specular color x its reflectance map)
    Range,           // lerp(at 0, at 1, a texture's channel): e.g. the gloss a roughness map selects
    LayerBlend,      // a layer's blend: saturate(mask x a + ramp x b + c) (the material graph's height blend)
    Named,           // a value whose meaning is known from the shader (label)
    Area,            // where on the model's texture coordinates a texture is laid (from, to; u or v)
    EffectScale,     // a value x lerp(1, this, a vertex effect's mask): e.g. the color darkened where wet
    Strength,        // lerp(1, a texture, this): how strongly the texture applies (e.g. the ambient occlusion)
    ViewDistance,    // the camera distance the near look blends to the far one over (stored squared, in cm; shown in m)
};
const char* KindName(Kind kind);

// One float of a pass' constants.
struct Location {
    int pass = 0;
    int index = 0;   // into Pass::constants (register x 4 + component)
};

struct Param {
    Kind kind = Kind::Tint;
    std::uint32_t texture = 0;      // the texture it applies to (0: none)
    std::string textureName;        // its name (or "texture <id>")
    std::string slotName;           // the shader's name for the texture's register (first pass seen)
    int components = 1;             // 1..4 (u, v / r, g, b / ...)
    std::vector<Location> where[4]; // per component: every float it is stored in
    std::string note;               // where it comes from, e.g. "composite (pass 3): MaterialParams[10].zw"
    std::string label;              // shown instead of the kind's name (empty: KindName)
    std::string componentNames[4];  // per component (empty: R, G, B, A / u, v)
    std::string help;               // what it does (tooltip)
};

// The parameters of a material whose shaders are in the set. Analyses are cached per shader.
std::vector<Param> Analyze(const Materials::Material& material, const Materials::Set& set);

// Value of a parameter's component (its first location).
float Value(const Materials::Material& material, const Param& param, int component);
// Sets a parameter's component everywhere it is stored.
void SetValue(Materials::Material& material, const Param& param, int component, float value);
// The component's locations hold different values (the passes disagree).
bool Mixed(const Materials::Material& material, const Param& param, int component);

const char* PassName(int pass);

// What a material's textures are for, measured: each pass' pixel shader is run (an interpreter for its disassembly,
// made-up inputs, the material's constants) with one texture channel nudged at a time, and the outputs it reaches
// name it (the normal pass' normal: a normal map; its gloss: gloss; the color only: a color / specular map; normal and
// color together: a mask; the alpha test: a cut-out); runs with one vertex effect lane at 0 (COLOR1: dirt, blood,
// snow, wet; COLOR0 x, y: Lara's blend shapes) find the layers' own textures. The texture's class (what its format
// holds) decides what the shaders leave open. By texture ID; a few tens of milliseconds per material.
struct TextureRole {
    std::string role;          // e.g. "Color (A: cut-out)", "Blood layer: normal map", "Effects mask (R: dirt, G: blood)"
    std::string why;           // how it was found
    bool fromShaders = false;  // false: from the texture's format only (not read, or no effect found)
};
std::map<std::uint32_t, TextureRole> TextureRoles(const Materials::Material& material, const Materials::Set& set);

// Every value of a material, as one list (as a material editor's property list): each pass' MaterialParams in order
// (the forward pass first, then the composite, the normal pass and the depth passes), a value another pass mirrors
// listed once (where it is first). Each is named by a parameter of Analyze() holding it (a texture's tiling, a known
// value), else by what changing it does, measured by running the pass' shader with it nudged: what it reaches (the
// normal pass' normal or gloss, the color or the lit color, the opacity, the alpha test), the vertex effect it only acts
// with (its layer: "Snow layer: ..."), and its kind where the code says (a power: sharpness; a comparison: threshold).
// Values the shader never reads are listed too (marked). A few hundred shader runs per pass; cached per shader and
// values by the caller.
struct Property {
    int pass = 0;
    int index = 0;           // into the pass' constants (register x 4 + component)
    int param = -1;          // Analyze()'s parameter holding it (-1: none; then name / help)
    int component = 0;       // (of that parameter)
    std::string name, help;
    bool read = true;        // the shader reads it
    bool effect = true;      // changing it changes something (with the material's values)
};
std::vector<Property> Properties(const Materials::Material& material, const Materials::Set& set, const std::vector<Param>& params);
}  // namespace MaterialParams
