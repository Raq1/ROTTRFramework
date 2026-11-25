#pragma once
// ROTTR Modding Suite: the game's materials, read for the preview (no Direct3D here).
//
// A material (resource type 10) has up to 10 passes; each names a pixel and a vertex shader library (type 9), the
// textures it binds (register = byte 13 >> 3) and its constants (the MaterialBuffer, b3). The game draws characters
// with a light pre-pass renderer:
//   pass 2  depth pre-pass (alpha tested: texture 0's alpha)      pass 1  the same for alpha materials (hair)
//   pass 7  normals: world normal and gloss into one target        pass 3  composite: albedo x the light buffers
//   pass 8  forward lit (hair: lights in a structured buffer)      passes 0, 9: depth and shadows (vertex shader only)
// A shader library holds variants (bit 0: fade dither, bit 1: velocity output); variant 0 is the plain one.
// See docs\materials.md.

#include "game_archive.h"
#include "game_data.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Materials {
using GameArchive::Key;

// A texture a pass binds. cls (byte 12): 0 data/mask, 1 color (sRGB), 2 reflectance/roughness, 3 normal map, 5 ramp.
// Where it is in the pass' list says who supplies it (cdc::CommonMaterial::SetupConstantsAndTextures, 0x1402a0f10,
// through 0x1402a12a0): the pass' +0x16 first entries are the material's own (bound as they are); the entries up to
// +0x17 the engine's (its render targets by class: (cls >> 3) - 6: 0x8B the normal buffer, 0x90 / 0x98 the diffuse /
// specular light, 0xA0 SSAO; the texture listed is a placeholder); the +0x15 entries after them the instance's (the
// model instance's texture (cls >> 3) - 1 when it sets one, CommonRenderModelInstance::SetInstanceTexture (a static
// mesh's, a light map), else the material's own: the blend map (0x1D) and ambient occlusion (0x27) of a weapon).
enum class SlotGroup : std::uint8_t { Material, Engine, Instance };
struct Slot {
    std::uint32_t texture = 0;
    std::uint8_t reg = 0;    // t#
    std::uint8_t kind = 0;   // byte 13 & 7
    std::uint8_t cls = 0;
    SlotGroup group = SlotGroup::Material;
    std::size_t at = 0;      // its entry in the resource (bytes from the start; 0: unknown)
    bool Engine() const { return group == SlotGroup::Engine; }   // the texture listed is a placeholder
};

struct Pass {
    bool present = false;
    std::uint32_t ps = 0, vs = 0;       // shader library IDs (0: none)
    std::uint32_t flags = 0;
    std::size_t at = 0;                  // its record in the resource (bytes from the start; +0 / +8: its pixel /
                                         // vertex shader's reference)
    std::vector<Slot> textures;          // pixel shader
    std::vector<float> constants;        // pixel shader MaterialBuffer, 4 floats each
    std::size_t constantsAt = 0;         // where they are in the resource (bytes; 0: none)
    // vertex shader MaterialBuffer (cb4; pass +0x40 count, +0x48 values), 4 floats each: on Lara's materials [0].x is
    // where the blend shapes' color offsets start to blend the corrective normal maps in (pants 0.25, tank top 0.8,
    // head 0.2: COLOR0.xy = saturate((2 c - [0].x) / (1 - [0].x)))
    std::vector<float> vsConstants;
};

struct Material {
    std::uint32_t id = 0;
    bool found = false;
    Pass passes[10];
    std::uint32_t stencil = 0;           // its stencil parameters (MaterialData +0x2c: ops, -, reference, mask)
    // MaterialData +0x18 (m_flags): 0x80 drawn from both sides (no culling: CommonMaterial::SetupSinglePassOpaque),
    // unless 0x800; 0x2000 its back faces only
    std::uint32_t flags = 0;
    static constexpr std::uint32_t kDoubleSided = 0x80;
    std::size_t bodyAt = 0;              // where its MaterialData starts in the resource (0: unknown)
    // How its color pass is blended (MaterialData +0x0c, m_blendMode, as PCDX11RenderContext::SetBlendModeInternal
    // reads it): bit 0 blending on; (>> 1) & 7 the operation (0 add); (>> 4) & 15 source, (>> 8) & 15 destination
    // factor (0 zero, 1 one, 4 source alpha, 5 its inverse...); 0x06540541: source alpha "over" (hair cards, eyebrows
    // and lashes, tear lines, torn cloth); 0x07010010: opaque.
    std::uint32_t blendMode = 0x07010010;
    std::uint8_t alphaTestRef = 128;     // +0x28: the opaque passes' alpha test (/ 255)
    std::uint8_t depthWriteAlpha = 255;  // +0x29: a blended material's depth pre-pass keeps alpha from this (/ 255) up
    bool Blended() const { return (blendMode & 1) != 0; }
    // The skin: drawn with the subsurface scattering stencil bit (0x10 in its stencil reference), so the engine blurs
    // its diffuse light (SSSRender) before the composite.
    bool Subsurface() const { return ((stencil >> 16) & 0x10) != 0; }
    std::string name;                    // its source path's last part (e.g. lara_eyes), or the file's
    std::string file;                    // the file it was read from (utf8; empty: from a .drm)
    std::vector<std::uint8_t> resource;  // as read (the editor writes the constants back into a copy)
    bool edited = false;                 // constants changed since it was read or saved
};

// A shader library's variant: the compiled shader (DXBC).
struct Shader {
    bool found = false;
    std::vector<std::uint8_t> code;
};

struct Set {
    std::vector<Material> materials;                 // in the order asked for
    std::map<std::uint32_t, Shader> shaders;         // shader library ID -> variant 0
    std::map<std::uint32_t, GameData::Image> textures;   // texture ID -> pixels (top mip levels trimmed)
    std::vector<std::uint32_t> missingTextures, missingShaders, missingMaterials;
    std::uint64_t textureBytes = 0;
    std::size_t notFound = 0;                        // textures in no .drm
    std::vector<std::string> problems;               // textures found but not readable (the first few)
    std::string note;                                // e.g. how long the search took
    std::string details;                             // per texture: where it came from, how it was read (a report)
    std::map<std::uint32_t, std::string> textureNames;   // texture ID -> its name (source path or file name)
    std::vector<std::uint32_t> extraTextures;        // read with the materials' (linear): e.g. the hair's dirt textures
};

bool ParseMaterial(const std::vector<std::uint8_t>& resource, Material& material, std::string& error);

// The material resource with its passes' (edited) constants, its texture slots' IDs and its flags written back: the
// .tr10material file.
std::vector<std::uint8_t> Serialize(const Material& material);

// A pass' pixel (vertex: false) or vertex shader library changed: its reference in the resource and the pass.
bool SetShader(Material& material, int pass, bool vertex, std::uint32_t id);

// A resource's name: the last part of the source path it carries (materials end with it; textures keep it before
// their pixels), e.g. "lara_eyes". Empty if none is found.
std::string ResourceName(const std::vector<std::uint8_t>& resource, bool texture);

// The DXBC of a shader library's variant (false: no such variant).
bool ShaderVariant(const std::vector<std::uint8_t>& library, unsigned variant, std::vector<std::uint8_t>& code);

// A texture resource (PCD9) or a .dds file, as GPU-ready mip levels no larger than maxSize (larger levels dropped).
// srgb: a .dds without an sRGB format is read as sRGB (color textures).
// how: (optional) the header and the levels used, for the report.
bool PrepareTexture(const std::vector<std::uint8_t>& data, bool dds, bool srgb, std::uint32_t maxSize,
                    GameData::Image& image, std::string& error, std::string* how = nullptr);

// Bytes of one mip level (block-compressed and plain formats; 0: unknown format).
std::uint64_t LevelBytes(std::uint32_t format, std::uint32_t width, std::uint32_t height);

// Where resources come from. dds: data is a .dds file (else the game's resource). file: the file it was read
// from (utf8), when not from a .drm.
using Fetch = std::function<bool(const Key& key, std::vector<std::uint8_t>& data, bool& dds, std::string& file)>;

// Where a resource would be read from, without reading it (e.g. "file:<path>:<size>:<time>", "game:<n>"): textures
// with the same answer are taken from a cache of the textures read before (empty: not cached).
using Identify = std::function<std::string(const Key& key)>;

// Reads the materials, the shaders their visible passes use (variant 0) and their textures. The textures are read
// on several threads at once: fetch must allow that for them.
void Load(const std::vector<std::uint32_t>& materialIds, const Fetch& fetch, std::uint32_t maxTextureSize, Set& set,
          const Identify& identify = nullptr);

// The passes the preview draws.
constexpr int kPassDepth = 2, kPassAlphaDepth = 1, kPassComposite = 3, kPassNormals = 7, kPassForward = 8;

// The depth pre-pass the game draws a material with (CommonMaterial::SetupDepthPass, the instance at full opacity):
//   0  pass 0, depth only (no pixel shader: no alpha test): an opaque blend mode (not blended, (m_blendMode &
//      0x07000000) == 0x07000000, its operation ((>> 1) & 7) not 5), e.g. 0x07010010; the color texture's alpha then
//      cuts nothing whatever pass 2's shader does with it (the handcuffs' en_grey_metal_dr: its alpha is a mask);
//   2  pass 2: operation 5 (alpha tested; the game also switches to it while an instance fades: 0x0701A01A);
//   1  pass 1: the rest (cut-outs, 0x06010010: torn cloth, foliage; blended materials, 0x06540541);
// a missing pass 1 or 2: pass 0.
inline int DepthPass(const Material& m) {
    const std::uint32_t b = m.blendMode;
    const bool blended = (b & 1) != 0, alphaTest = (b & 0xE) == 0xA;
    const int pass = !blended && (b & 0x07000000u) == 0x07000000u && !alphaTest ? 0 : !blended && alphaTest ? 2 : 1;
    return pass && m.passes[pass].present ? pass : 0;
}
}  // namespace Materials
