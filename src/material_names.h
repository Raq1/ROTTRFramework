#pragma once
// The Material Editor's parameter names: what each of a live material's constants does, read from its pixel shaders
// by the Modding Suite's analysis (tools/outfit_manager/material_params.cpp, compiled into the framework as it is).
// The game keeps a material's constants as float4 arrays per pass without names; the analysis follows each value
// through the shader code (which texture's coordinates it scales or moves, which texture it multiplies, which value
// it blends, ...). A parameter used by several passes is one entry that writes them all.
//
// The analysis disassembles the shaders (D3DDisassemble, a few ms each, cached per shader) and runs them for the
// textures' roles (tens of ms), so it runs on a worker thread: Request it with a material's input
// (Features::CopyMaterialAnalysisInput) and Poll for the result.

#include "features.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MaterialNames {
enum class Widget {
    Values,        // one value per component (componentNames)
    Color,         // r, g, b (a color or a tint, HDR)
    Pair,          // u, v (tiling, offset)
    Distance,      // from, to: the squared camera distance in cm, shown in metres
    Highlights,    // the strand hair's 4 highlight values
};

struct Location {
    int pass = 0;
    int index = 0;   // into the pass' pixel constants (register x 4 + component)
};

struct Param {
    std::string group;              // the texture it applies to (or "General")
    std::string label;              // what it is ("Tiling (UV scale)", "Gloss range", ...)
    std::string help;               // what it does (tooltip)
    std::string note;               // where it is: "composite (pass 3): MaterialParams[10].zw"
    std::string slotName;           // the shader's name for the texture's register
    Widget widget = Widget::Values;
    int components = 1;
    std::string componentNames[4];  // (empty: R, G, B, A)
    bool plainNumbers = false;      // components shown without a letter (alpha scale, normal strength)
    std::vector<Location> where[4]; // per component: every float it is stored in
};

// What a texture slot is for (the Suite's MaterialParams::TextureRoles): measured by running the material's shaders with
// each texture channel nudged (which outputs it reaches: normal, gloss, color, alpha test) and each vertex effect off
// (the dirt / blood / snow / wet layers' own textures). E.g. "Color (A: cut-out)", "Blood layer: normal map",
// "Effects mask (R: dirt, G: blood)".
struct SlotRole {
    int pass = 0;
    int reg = 0;                    // t#
    std::string role;
    std::string why;                // how it was found
    bool measured = false;          // false: from the texture's format only
};

struct Result {
    std::uintptr_t address = 0;
    std::uint64_t detailVersion = 0;
    bool done = false;
    std::vector<Param> params;
    std::vector<SlotRole> slotRoles;
    int shaders = 0;                // pixel shaders analysed
    std::string problem;            // why there are no names (empty: none)
};

// Starts the analysis of a material (a newer request replaces one not started yet).
void Request(const Features::MaterialAnalysisInput& input);
// The latest finished result (false: none yet).
bool Poll(Result& out);
// The pass' name ("composite (3)", ...).
const char* PassName(int pass);
}  // namespace MaterialNames
