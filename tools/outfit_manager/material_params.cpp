#include "material_params.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace MaterialParams {
namespace {
// IID_ID3D11ShaderReflection of d3dcompiler_47 (as preview_shaded.cpp).
const GUID kReflectionId = {0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};

// Where a register component's value comes from.
struct Source {
    char kind = 0;   // 't' a texture's channel (a = register, b = channel), 'c' a constant (a = register, b = component,
                     // role: 'm' multiplied in, 'a' added in, 'o' other), 'v' an input (a = register), 'D' the camera's
                     // distance (its square less cb3[a].b: the start of a blend with the distance)
    int a = 0, b = 0;
    char role = 0;
    bool operator<(const Source& o) const { return std::tie(kind, a, b, role) < std::tie(o.kind, o.a, o.b, o.role); }
    bool operator==(const Source& o) const { return kind == o.kind && a == o.a && b == o.b && role == o.role; }
};
using Sources = std::vector<Source>;   // sorted, unique, capped

void Merge(Sources& into, const Sources& from) {
    for (const Source& s : from)
        if (into.size() < 160 && std::find(into.begin(), into.end(), s) == into.end()) into.push_back(s);
}

// What one constant component does in a shader.
struct Use {
    char what = 0;   // 'u' texture coordinates, 'c' a texture's channel
    char role = 0;   // 'm' multiplied, 'a' added
    int texture = -1, lane = 0;
};

struct Operand {
    char kind = 0;   // 'r', 'v', 'o', 'c' (cb3), 't', 'l' (a literal), other 0
    int reg = 0, index = 0;
    std::string swizzle = "xyzw";
    bool negated = false;
    float literal = 0.0f;   // 'l': its first value
};

int Lane(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1; }

Operand ParseOperand(std::string text) {
    Operand op;
    const std::size_t lead = text.find_first_not_of(' ');
    op.negated = lead != std::string::npos && text[lead] == '-';
    if (text.find("l(") != std::string::npos) {   // a literal: l(2.00000000e+00, ...)
        op.kind = 'l';
        const std::size_t open = text.find("l(");
        op.literal = std::strtof(text.c_str() + open + 2, nullptr);
        if (op.negated) op.literal = -op.literal;
        return op;
    }
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '|' || c == '-' || c == ' '; }), text.end());
    std::size_t i = 0;
    std::string kind;
    while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) kind += text[i++];
    if (kind != "r" && kind != "v" && kind != "o" && kind != "cb" && kind != "t") return Operand{};
    std::size_t digits = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    if (digits == i) return op;
    op.reg = std::stoi(text.substr(digits, i - digits));
    if (kind == "cb") {
        if (op.reg != 3 || i >= text.size() || text[i] != '[') return Operand{};
        const std::size_t close = text.find(']', i);
        const std::string inside = text.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
        if (inside.empty() || inside.find_first_not_of("0123456789") != std::string::npos) return Operand{};   // dynamic
        op.index = std::stoi(inside);
        i = close + 1;
        op.kind = 'c';
    } else {
        op.kind = kind[0];
    }
    if (i < text.size() && text[i] == '.') op.swizzle = text.substr(i + 1);
    for (char c : op.swizzle)
        if (Lane(c) < 0) return Operand{};
    return op;
}

std::vector<std::string> SplitOperands(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string current;
    for (char c : s) {
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) out.push_back(current);
    for (auto& o : out) {
        const std::size_t a = o.find_first_not_of(' '), b = o.find_last_not_of(' ');
        o = a == std::string::npos ? std::string() : o.substr(a, b - a + 1);
    }
    return out;
}

// lerp(cb3[at0], cb3[at1], factor): "add r, -cb3[A], cb3[B]" then "mad r', factor, r, cb3[A]".
struct RangeUse {
    std::pair<int, int> at0, at1;   // (register, component)
    int texture = -1, lane = 0;     // the factor's texture channel (-1: not one texture)
};
// saturate(dot(cb3[K].xyz, (mask, ramp, 1))): the material graph's layer blend.
struct BlendUse {
    int reg = 0;
    int texture = -1, lane = 0;     // the mask's texture channel
    int effect = -1;                // the mask is scaled by the vertex effects (COLOR1): its lane (0 dirt, 1 blood,
                                    // 2 snow, 3 wet)
};

// A texture-coordinate area: smoothstep((uv - cb3[start]) / (cb3[end] - cb3[start])) addresses textures: they cover
// that rectangle of the model's texture (Lara's head: the lips' textures over the mouth).
struct AreaUse {
    std::pair<int, int> start, end;   // (register, component)
    int axis = 0;                     // 0 u, 1 v
    int input = -1;                   // the texture coordinates' input register
    std::set<int> textures;           // the texture registers read there
};
// x * lerp(1, cb3[k], mask): a value scaled where a vertex effect shows ("mad t, x, cb3[k], -x" then "mad y, mask, t, x").
struct EffectScale {
    std::vector<std::pair<int, int>> constants;   // per lane written (distinct, in order)
    int effect = -1;                              // the mask's vertex effect lane (0 dirt, 1 blood, 2 snow, 3 wet)
    bool distance = false;                        // the mask: the camera's distance (near to far)
    std::set<int> maskTextures;                   // texture registers in the mask
    std::set<std::pair<int, int>> valueTextures;  // (texture register, channel) the value is made of
};
// lerp(1, texture, cb3[k]) = saturate(cb3[k] x t + 1 - cb3[k]): how strongly a texture applies (e.g. the AO).
struct StrengthUse {
    std::pair<int, int> constant;
    int texture = -1, lane = 0;
};

struct ShaderAnalysis {
    bool ok = false;
    std::vector<AreaUse> areas;
    std::vector<EffectScale> effectScales;
    std::vector<StrengthUse> strengths;
    // A blend with the camera's distance: smoothstep((|position - camera|^2 - cb3[start]) / (cb3[end] - cb3[start])),
    // then values lerped to constants (lerp(x, cb3[k], blend)) or scaled (effectScales with distance).
    std::pair<int, int> distanceStart{-1, -1}, distanceEnd{-1, -1};
    std::vector<std::pair<int, int>> distanceTargets;
    bool strands = false;                                     // writes fragment lists (UAVs)
    std::map<std::pair<int, int>, std::vector<Use>> uses;     // (register, component) -> what it does
    std::map<int, std::string> textureNames;                  // texture register -> the shader's name for it
    std::vector<RangeUse> ranges;
    std::vector<BlendUse> blends;
    std::set<std::pair<int, int>> gloss;                      // constants that reach the gloss
    std::set<int> screenTextures;   // texture registers read at the pixel's screen position (a dither / noise pattern)
};

ShaderAnalysis AnalyzeShader(const std::vector<std::uint8_t>& code) {
    ShaderAnalysis result;
    int effectsRegister = -1;   // the input holding the vertex effects (COLOR1: dirt, blood, snow, wet)
    int positionRegister = -1;  // SV_POSITION (the pixel's place on the screen)
    std::map<std::pair<int, int>, std::pair<std::pair<int, int>, std::pair<int, int>>> differences;   // r lane -> (A, B) of
                                                                                                         // "-cb3[A] + cb3[B]"
    ID3DBlob* text = nullptr;
    if (FAILED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text)) || !text) return result;
    const std::string program(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
    text->Release();
    ID3D11ShaderReflection* reflection = nullptr;
    if (SUCCEEDED(D3DReflect(code.data(), code.size(), kReflectionId, reinterpret_cast<void**>(&reflection)))) {
        D3D11_SHADER_DESC desc{};
        reflection->GetDesc(&desc);
        for (UINT i = 0; i < desc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bind{};
            if (SUCCEEDED(reflection->GetResourceBindingDesc(i, &bind)) && bind.Type == D3D_SIT_TEXTURE && bind.Name)
                result.textureNames[int(bind.BindPoint)] = bind.Name;
        }
        for (UINT i = 0; i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC in{};
            if (FAILED(reflection->GetInputParameterDesc(i, &in)) || !in.SemanticName) continue;
            if (_stricmp(in.SemanticName, "COLOR") == 0 && in.SemanticIndex == 1) effectsRegister = int(in.Register);
            if (_stricmp(in.SemanticName, "SV_POSITION") == 0) positionRegister = int(in.Register);
        }
        reflection->Release();
    }

    std::map<std::pair<int, int>, Sources> temps;   // (r register, component) -> sources
    std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> areaStarts;   // ((cb3 start), (input register, axis))
    std::set<std::pair<std::pair<int, int>, std::pair<int, int>>> allDifferences;   // every "-cb3[A] + cb3[B]": (A, B)
    std::map<int, std::set<std::pair<int, int>>> coordinateConstants;   // texture register -> constants in its coordinates
    struct PendingScale {
        std::pair<int, int> constant;
        int reg = 0, lane = 0;   // the value x
    };
    std::map<std::pair<int, int>, PendingScale> pendingScales;   // r lane holding x * cb3[k] - x
    std::map<std::pair<int, int>, std::pair<int, int>> inverses;   // r lane holding 1 - cb3[k]
    std::set<std::pair<int, int>> eyeOffsets, distancesSquared;    // r lanes: position - camera; its length squared
    std::map<std::pair<int, int>, PendingScale> pendingTargets;     // r lane holding cb3[k] - x
    std::istringstream lines(program);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        line = line.substr(first);
        if (line.rfind("dcl_uav", 0) == 0) result.strands = true;
        if (line.rfind("dcl", 0) == 0 || line.rfind("//", 0) == 0 || line.rfind("ps_", 0) == 0) continue;
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        std::string opcode = line.substr(0, space);
        const std::vector<std::string> operands = SplitOperands(line.substr(space + 1));
        if (operands.size() < 2) continue;
        if (const std::size_t paren = opcode.find('('); paren != std::string::npos) opcode.resize(paren);
        std::string base = opcode;
        if (const std::size_t sat = base.find("_sat"); sat != std::string::npos) base.resize(sat);
        const Operand dest = ParseOperand(operands[0]);
        if (dest.kind != 'r' && dest.kind != 'o' && dest.kind != 0) continue;
        std::vector<Operand> sources;
        for (std::size_t k = 1; k < operands.size(); ++k) sources.push_back(ParseOperand(operands[k]));
        const bool sample = base.rfind("sample", 0) == 0 || base.rfind("ld", 0) == 0 || base.rfind("gather", 0) == 0;
        const int dotCount = base == "dp2" ? 2 : base == "dp3" ? 3 : base == "dp4" ? 4 : 0;

        // The sources of one operand's value at a destination lane.
        auto sourcesOf = [&](const Operand& op, int lane) {
            Sources out;
            std::vector<char> comps;
            if (dotCount) {
                for (int k = 0; k < dotCount && k < int(op.swizzle.size()); ++k) comps.push_back(op.swizzle[k]);
            } else {
                comps.push_back(op.swizzle.size() == 1 ? op.swizzle[0] : op.swizzle[std::min<std::size_t>(lane, op.swizzle.size() - 1)]);
            }
            for (char c : comps) {
                if (op.kind == 'r') {
                    auto it = temps.find({op.reg, Lane(c)});
                    if (it != temps.end()) Merge(out, it->second);
                } else if (op.kind == 'v') {
                    Merge(out, {Source{'v', op.reg, Lane(c), 0}});
                }
            }
            return out;
        };
        auto constantOf = [&](const Operand& op, int lane) -> int {   // the cb3 component read at a lane
            if (op.kind != 'c') return -1;
            return Lane(op.swizzle.size() == 1 ? op.swizzle[0] : op.swizzle[std::min<std::size_t>(lane, op.swizzle.size() - 1)]);
        };

        // Patterns of the material graph's nodes.
        auto laneOf = [&](const Operand& op, int lane) {
            return Lane(op.swizzle.size() == 1 ? op.swizzle[0] : op.swizzle[std::min<std::size_t>(lane, op.swizzle.size() - 1)]);
        };
        // uv - cb3[start]: the start of an area (its end: the difference "cb3[end] - cb3[start]" the shader divides by)
        if (dest.kind == 'r' && base == "add" && sources.size() == 2) {
            const int vi = sources[0].kind == 'v' ? 0 : sources[1].kind == 'v' ? 1 : -1;
            if (vi >= 0 && sources[1 - vi].kind == 'c' && sources[1 - vi].negated && !sources[vi].negated)
                for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                    const int lane = Lane(dest.swizzle[k]);
                    const int axis = laneOf(sources[vi], lane);
                    if (axis == 0 || axis == 1)
                        areaStarts.push_back({{sources[1 - vi].index, constantOf(sources[1 - vi], lane)}, {sources[vi].reg, axis}});
                }
        }
        // The camera's distance: position - cb2[10] (the camera's position), its dp3 with itself, less cb3[start].
        bool distanceStart = false;
        // (what this instruction leaves in its destination: an earlier pattern's value there is gone)
        bool setEye = false, setDistance = false, setTarget = false, setScale = false, setInverse = false;
        if (dest.kind == 'r' && base == "add" && operands.size() == 3 &&
            (operands[1].find("cb2[10]") != std::string::npos || operands[2].find("cb2[10]") != std::string::npos))
            for (char c : dest.swizzle) {
                eyeOffsets.insert({dest.reg, Lane(c)});
                setEye = true;
            }
        if (dest.kind == 'r' && base == "dp3" && sources.size() == 2 && sources[0].kind == 'r' && sources[1].kind == 'r' &&
            sources[0].reg == sources[1].reg && sources[0].swizzle.size() >= 3 && sources[0].swizzle.substr(0, 3) == sources[1].swizzle.substr(0, 3)) {
            int lanes = 0;
            for (int k = 0; k < 3; ++k) {
                const std::pair<int, int> key(sources[0].reg, Lane(sources[0].swizzle[std::size_t(k)]));
                if (eyeOffsets.find(key) != eyeOffsets.end()) ++lanes;
            }
            const bool eye = lanes == 3;
            if (eye && !dest.swizzle.empty()) {
                distancesSquared.insert({dest.reg, Lane(dest.swizzle[0])});
                setDistance = true;
            }
        }
        if (dest.kind == 'r' && base == "add" && sources.size() == 2 && dest.swizzle.size() == 1) {
            const Operand* constant = nullptr;
            const Operand* value = nullptr;
            if (sources[0].kind == 'c' && sources[1].kind == 'r') {
                constant = &sources[0];
                value = &sources[1];
            } else if (sources[1].kind == 'c' && sources[0].kind == 'r') {
                constant = &sources[1];
                value = &sources[0];
            }
            if (constant && value && constant->negated && !value->swizzle.empty()) {
                const std::pair<int, int> key(value->reg, Lane(value->swizzle[0]));
                const int component = constant->swizzle.empty() ? -1 : Lane(constant->swizzle[0]);
                if (component >= 0 && distancesSquared.find(key) != distancesSquared.end()) {
                    result.distanceStart = std::make_pair(constant->index, component);
                    distanceStart = true;
                }
            }
        }
        // cb3[k] - x, then mask * that + x: lerp(x, cb3[k], mask) (the mask: the camera's distance)
        if (dest.kind == 'r' && base == "add" && sources.size() == 2) {
            const int ci = sources[0].kind == 'c' ? 0 : sources[1].kind == 'c' ? 1 : -1;
            if (ci >= 0 && !sources[ci].negated && sources[1 - ci].kind == 'r' && sources[1 - ci].negated)
                for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                    const int lane = Lane(dest.swizzle[k]);
                    setTarget = true;
                    pendingTargets[{dest.reg, lane}] = {{sources[ci].index, constantOf(sources[ci], lane)}, sources[1 - ci].reg,
                                                        laneOf(sources[1 - ci], lane)};
                }
        }
        if (dest.kind == 'r' && base == "mad" && sources.size() == 3 && sources[0].kind == 'r' && sources[1].kind == 'r' &&
            sources[2].kind == 'r' && !sources[2].negated)
            for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                const int lane = Lane(dest.swizzle[k]);
                auto pending = pendingTargets.find({sources[1].reg, laneOf(sources[1], lane)});
                if (pending == pendingTargets.end() || pending->second.reg != sources[2].reg ||
                    pending->second.lane != laneOf(sources[2], lane))
                    continue;
                bool distance = false;
                for (const Source& m : sourcesOf(sources[0], lane)) distance = distance || m.kind == 'D';
                if (distance && std::find(result.distanceTargets.begin(), result.distanceTargets.end(), pending->second.constant) ==
                                    result.distanceTargets.end())
                    result.distanceTargets.push_back(pending->second.constant);
            }
        // x * cb3[k] - x, then mask * that + x: x scaled by lerp(1, cb3[k], mask)
        if (dest.kind == 'r' && base == "mad" && sources.size() == 3 && sources[0].kind == 'r' && sources[1].kind == 'c' &&
            sources[2].kind == 'r' && sources[2].negated && !sources[0].negated && sources[2].reg == sources[0].reg)
            for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                const int lane = Lane(dest.swizzle[k]);
                if (laneOf(sources[0], lane) != laneOf(sources[2], lane)) continue;
                pendingScales[{dest.reg, lane}] = {{sources[1].index, constantOf(sources[1], lane)}, sources[0].reg, laneOf(sources[0], lane)};
                setScale = true;
            }
        if (dest.kind == 'r' && base == "mad" && sources.size() == 3 && sources[1].kind == 'r' && sources[2].kind == 'r' &&
            !sources[2].negated && sources[0].kind == 'r') {
            EffectScale scale;
            bool any = false;
            for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                const int lane = Lane(dest.swizzle[k]);
                auto pending = pendingScales.find({sources[1].reg, laneOf(sources[1], lane)});
                if (pending == pendingScales.end() || pending->second.reg != sources[2].reg ||
                    pending->second.lane != laneOf(sources[2], lane))
                    continue;
                any = true;
                if (std::find(scale.constants.begin(), scale.constants.end(), pending->second.constant) == scale.constants.end())
                    scale.constants.push_back(pending->second.constant);
                for (const Source& m : sourcesOf(sources[0], lane)) {
                    if (m.kind == 'v' && m.a == effectsRegister) scale.effect = m.b;
                    if (m.kind == 't') scale.maskTextures.insert(m.a);
                    if (m.kind == 'D') scale.distance = true;
                }
                auto x = temps.find({pending->second.reg, pending->second.lane});
                if (x != temps.end())
                    for (const Source& v : x->second)
                        if (v.kind == 't') scale.valueTextures.insert({v.a, v.b});
            }
            if (scale.distance) scale.effect = -1;
            if (any && (scale.effect >= 0 || scale.distance)) result.effectScales.push_back(std::move(scale));
        }
        // 1 - cb3[k], then cb3[k] x texture + that: the texture's strength
        if (dest.kind == 'r' && base == "add" && sources.size() == 2) {
            const int ci = sources[0].kind == 'c' ? 0 : sources[1].kind == 'c' ? 1 : -1;
            if (ci >= 0 && sources[ci].negated && sources[1 - ci].kind == 'l' && sources[1 - ci].literal == 1.0f)
                for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                    const int lane = Lane(dest.swizzle[k]);
                    inverses[{dest.reg, lane}] = {sources[ci].index, constantOf(sources[ci], lane)};
                    setInverse = true;
                }
        }
        if (dest.kind == 'r' && base == "mad" && sources.size() == 3 && sources[2].kind == 'r') {
            const int ci = sources[0].kind == 'c' ? 0 : sources[1].kind == 'c' ? 1 : -1;
            if (ci >= 0 && sources[1 - ci].kind == 'r')
                for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                    const int lane = Lane(dest.swizzle[k]);
                    auto inverse = inverses.find({sources[2].reg, laneOf(sources[2], lane)});
                    const std::pair<int, int> constant{sources[ci].index, constantOf(sources[ci], lane)};
                    if (inverse == inverses.end() || inverse->second != constant) continue;
                    std::set<std::pair<int, int>> textures;
                    bool other = false;
                    for (const Source& t : sourcesOf(sources[1 - ci], lane)) {
                        if (t.kind == 't') textures.insert({t.a, t.b});
                        else if (t.kind == 'v') other = true;
                    }
                    if (textures.size() == 1 && !other &&
                        std::none_of(result.strengths.begin(), result.strengths.end(), [&](const StrengthUse& s) { return s.constant == constant; }))
                        result.strengths.push_back({constant, textures.begin()->first, textures.begin()->second});
                }
        }
        if (dest.kind == 'r' && base == "add" && sources.size() == 2 && sources[0].kind == 'c' && sources[1].kind == 'c' &&
            sources[0].negated != sources[1].negated) {
            const Operand& a = sources[0].negated ? sources[0] : sources[1];
            const Operand& b = sources[0].negated ? sources[1] : sources[0];
            for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                const int lane = Lane(dest.swizzle[k]);
                const int ca = constantOf(a, lane), cb = constantOf(b, lane);
                if (ca >= 0 && cb >= 0) {
                    differences[{dest.reg, lane}] = {{a.index, ca}, {b.index, cb}};
                    allDifferences.insert({{a.index, ca}, {b.index, cb}});
                }
            }
        }
        if (dest.kind == 'r' && base == "mad" && sources.size() == 3 && sources[2].kind == 'c')
            for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                const int lane = Lane(dest.swizzle[k]);
                const int at0 = constantOf(sources[2], lane);
                for (int j = 0; j < 2; ++j) {
                    const Operand& d = sources[j];
                    if (d.kind != 'r') continue;
                    const char c = d.swizzle.size() == 1 ? d.swizzle[0] : d.swizzle[std::min<std::size_t>(lane, d.swizzle.size() - 1)];
                    auto diff = differences.find({d.reg, Lane(c)});
                    if (diff == differences.end() || diff->second.first != std::make_pair(sources[2].index, at0)) continue;
                    RangeUse range;
                    range.at0 = diff->second.first;
                    range.at1 = diff->second.second;
                    const Sources factor = sourcesOf(sources[1 - j], lane);
                    std::set<int> textures;   // (a luminance reads several channels of one texture)
                    for (const Source& f : factor)
                        if (f.kind == 't') {
                            if (textures.insert(f.a).second && textures.size() == 1) {
                                range.texture = f.a;
                                range.lane = f.b;
                            }
                        }
                    if (textures.size() != 1) range.texture = -1;
                    if (std::none_of(result.ranges.begin(), result.ranges.end(), [&](const RangeUse& r) {
                            return r.at0 == range.at0 && r.at1 == range.at1;
                        }))
                        result.ranges.push_back(range);
                }
            }
        if (dest.kind == 'r' && base == "dp3" && sources.size() == 2 && (sources[0].kind == 'c') != (sources[1].kind == 'c')) {
            const Operand& k = sources[0].kind == 'c' ? sources[0] : sources[1];
            const Operand& f = sources[0].kind == 'c' ? sources[1] : sources[0];
            if (f.kind == 'r' && f.swizzle.size() >= 3) {
                BlendUse blend;
                blend.reg = k.index;
                Sources mask;
                if (auto it = temps.find({f.reg, Lane(f.swizzle[0])}); it != temps.end()) mask = it->second;
                std::set<int> textures;
                int effect = -1;
                for (const Source& m : mask) {
                    if (m.kind == 't' && textures.insert(m.a).second && textures.size() == 1) {
                        blend.texture = m.a;
                        blend.lane = m.b;
                    }
                    if (m.kind == 'v' && m.a == effectsRegister) effect = m.b;
                }
                // (a mask made of several textures: earlier layers' masks take their share; not one texture's)
                if (textures.size() != 1) blend.texture = -1;
                else blend.effect = effect;
                if (std::none_of(result.blends.begin(), result.blends.end(), [&](const BlendUse& b) { return b.reg == blend.reg; }))
                    result.blends.push_back(blend);
            }
        }
        // The gloss: what the normal pass encodes (max(gloss, 2), log2) or the composite normalizes (gloss + 2).
        if ((base == "max" || base == "add") && sources.size() == 2) {
            const bool two = (sources[0].kind == 'l' && sources[0].literal == 2.0f) || (sources[1].kind == 'l' && sources[1].literal == 2.0f);
            const Operand& other = sources[0].kind == 'l' ? sources[1] : sources[0];
            if (two && other.kind == 'r')
                for (const Source& g : sourcesOf(other, Lane(dest.swizzle.empty() ? 'x' : dest.swizzle[0])))
                    if (g.kind == 'c') result.gloss.insert({g.a, g.b});
        }


        std::map<std::pair<int, int>, Sources> written;
        for (int lane = 0; lane < 4; ++lane) {
            if ((dest.kind == 'r' || dest.kind == 'o') && dest.swizzle.find("xyzw"[lane]) == std::string::npos) continue;
            if (dest.kind == 0 && lane) break;
            Sources out;
            if (sample) {
                int texture = -1;
                char channel = 'x';
                for (const Operand& op : sources)
                    if (op.kind == 't') {
                        texture = op.reg;
                        channel = op.swizzle.size() == 1 ? op.swizzle[0] : op.swizzle[std::min<std::size_t>(lane, op.swizzle.size() - 1)];
                        break;
                    }
                if (texture < 0) continue;
                // Constants in the coordinates: the texture's tiling or offset.
                const Operand& coords = sources[0];
                if (coords.kind == 'r' && lane == Lane(dest.swizzle[0]) && positionRegister >= 0) {
                    bool screen = true, any = false;   // coordinates made from the screen position only
                    for (int j = 0; j < 2 && j < int(coords.swizzle.size()); ++j)
                        if (auto it = temps.find({coords.reg, Lane(coords.swizzle[j])}); it != temps.end())
                            for (const Source& s : it->second) {
                                if (s.kind == 'v') any = true;
                                if ((s.kind == 'v' && s.a != positionRegister) || s.kind == 't') screen = false;
                            }
                    if (screen && any) result.screenTextures.insert(texture);
                }
                if (coords.kind == 'r')
                    for (int j = 0; j < 2 && j < int(coords.swizzle.size()); ++j)
                        if (auto it = temps.find({coords.reg, Lane(coords.swizzle[j])}); it != temps.end())
                            for (const Source& s : it->second)
                                if (s.kind == 'c') coordinateConstants[texture].insert({s.a, s.b});
                if (lane == Lane(dest.swizzle[0]) && coords.kind == 'r')
                    for (int j = 0; j < 2 && j < int(coords.swizzle.size()); ++j) {
                        auto it = temps.find({coords.reg, Lane(coords.swizzle[j])});
                        if (it == temps.end()) continue;
                        for (const Source& s : it->second)
                            if (s.kind == 'c' && (s.role == 'M' || s.role == 'A'))
                                result.uses[{s.a, s.b}].push_back(Use{'u', s.role == 'M' ? 'm' : 'a', texture, j});
                    }
                out.push_back(Source{'t', texture, Lane(channel), 0});
            } else {
                // Constants multiplying (or added to) exactly one texture channel: its tint, scale or bias.
                const bool mul = base == "mul", mad = base == "mad", add = base == "add";
                for (std::size_t k = 0; k < sources.size(); ++k) {
                    const int component = constantOf(sources[k], lane);
                    if (component < 0) continue;
                    char role = 'o';
                    Sources other;
                    if ((mul && sources.size() == 2) || (mad && sources.size() == 3 && k < 2)) {
                        role = 'm';
                        other = sourcesOf(sources[k == 0 ? 1 : 0], lane);
                    } else if ((add && sources.size() == 2) || (mad && sources.size() == 3 && k == 2)) {
                        role = 'a';
                        if (add) other = sourcesOf(sources[0], lane);
                        else {
                            other = sourcesOf(sources[0], lane);
                            Merge(other, sourcesOf(sources[1], lane));
                        }
                    }
                    // On exactly one texture (and constants): its tint, scale or bias. (Its channel: the one of this lane
                    // when several are in it, e.g. a normal map's x and y both in its normalization.)
                    int textures = 0, inputCount = 0;
                    const Source* texture = nullptr;
                    std::set<int> registers;
                    for (const Source& s : other) {
                        if (s.kind == 't') {
                            if (registers.insert(s.a).second) ++textures;
                            if (!texture || (s.a == texture->a && s.b == lane)) texture = &s;
                        }
                        if (s.kind == 'v') ++inputCount;
                    }
                    if (role != 'o' && textures == 1 && !inputCount)
                        result.uses[{sources[k].index, component}].push_back(Use{'c', role, texture->a, texture->b});
                    // On values made from the shader's inputs (texture coordinates) without a texture read: a
                    // candidate tiling ('M') or offset ('A') if the value then addresses a texture.
                    if (inputCount && !textures) role = role == 'm' ? 'M' : role == 'a' ? 'A' : role;
                    out.push_back(Source{'c', sources[k].index, component, role});
                }
                for (const Operand& op : sources)
                    if (op.kind == 'r' || op.kind == 'v') Merge(out, sourcesOf(op, lane));
            }
            if (distanceStart) out = {Source{'D', result.distanceStart.first, result.distanceStart.second, 0}};
            if (dest.kind == 'r') written[{dest.reg, lane}] = std::move(out);
        }
        for (auto& [key, value] : written) temps[key] = std::move(value);
        if (dest.kind == 'r') {
            // a move (or saturate) keeps the patterns' state: copied from its source
            const bool move = base == "mov" && sources.size() == 1 && sources[0].kind == 'r' && !sources[0].negated;
            std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> copies;   // (to, from)
            for (std::size_t k = 0; k < dest.swizzle.size(); ++k) {
                const std::pair<int, int> key(dest.reg, Lane(dest.swizzle[k]));
                if (move) copies.push_back({key, {sources[0].reg, laneOf(sources[0], Lane(dest.swizzle[k]))}});
            }
            std::vector<std::pair<std::pair<int, int>, int>> keep;   // (to, which: bit 0 eye, 1 distance)
            std::map<std::pair<int, int>, std::pair<int, int>> keepInverses;
            std::map<std::pair<int, int>, PendingScale> keepTargets, keepScales;
            for (const auto& [to, from] : copies) {
                if (eyeOffsets.count(from)) keep.push_back({to, 1});
                if (distancesSquared.count(from)) keep.push_back({to, 2});
                if (auto it = inverses.find(from); it != inverses.end()) keepInverses[to] = it->second;
                if (auto it = pendingTargets.find(from); it != pendingTargets.end()) keepTargets[to] = it->second;
                if (auto it = pendingScales.find(from); it != pendingScales.end()) keepScales[to] = it->second;
            }
            for (char c : dest.swizzle) {
                const std::pair<int, int> key(dest.reg, Lane(c));
                if (!setEye) eyeOffsets.erase(key);
                if (!setDistance) distancesSquared.erase(key);
                if (!setTarget) pendingTargets.erase(key);
                if (!setScale) pendingScales.erase(key);
                if (!setInverse) inverses.erase(key);
            }
            for (const auto& [to, which] : keep) (which == 1 ? eyeOffsets : distancesSquared).insert(to);
            for (const auto& [to, value] : keepInverses) inverses[to] = value;
            for (const auto& [to, value] : keepTargets) pendingTargets[to] = value;
            for (const auto& [to, value] : keepScales) pendingScales[to] = value;
        }
    }
    // the areas: a start whose difference to an end the shader divides by, and the textures read there
    for (const auto& start : areaStarts) {
        for (const auto& diff : allDifferences) {
            if (diff.first != start.first) continue;
            if (std::any_of(result.areas.begin(), result.areas.end(), [&](const AreaUse& a) { return a.start == start.first; })) break;
            AreaUse area;
            area.start = start.first;
            area.end = diff.second;
            area.input = start.second.first;
            area.axis = start.second.second;
            for (const auto& [texture, constants] : coordinateConstants)
                if (constants.count(area.start) && constants.count(area.end)) area.textures.insert(texture);
            if (!area.textures.empty()) result.areas.push_back(area);
            break;
        }
    }
    // the distance blend's end: the difference "cb3[end] - cb3[start]" it divides by
    if (result.distanceStart.first >= 0)
        for (const auto& diff : allDifferences)
            if (diff.first == result.distanceStart) result.distanceEnd = diff.second;
    if (result.distanceEnd.first < 0) {
        result.distanceTargets.clear();
        result.effectScales.erase(std::remove_if(result.effectScales.begin(), result.effectScales.end(),
                                                 [](const EffectScale& e) { return e.distance; }),
                                  result.effectScales.end());
    }
    result.ok = true;
    return result;
}

const ShaderAnalysis& Cached(std::uint32_t id, const std::vector<std::uint8_t>& code) {
    static std::mutex mutex;
    static std::map<std::uint32_t, ShaderAnalysis> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(id);
    if (it == cache.end()) it = cache.emplace(id, AnalyzeShader(code)).first;
    return it->second;
}

bool EngineSlot(const Materials::Slot& slot) { return slot.Engine(); }

// Values whose meaning was read from the game's shaders by hand (they feed math, not a texture): the eyes' shaders
// (a refracting cornea over an iris plane). Each shader keeps them in its own layout.
constexpr std::uint32_t kHairCardShader = 0x9FF001;
struct KnownValue {
    const char* name;
    const char* help;
    std::uint32_t shader;
    int reg, comp;
};
const KnownValue kKnown[] = {
    {"Iris radius", "The cornea's radius in the eye's texture coordinates: the round, refracting part over the iris "
                    "(the sclera outside it).", 621, 5, 0},
    {"Iris radius", nullptr, 623, 5, 0}, {"Iris radius", nullptr, 625, 5, 0},
    {"Iris radius", nullptr, 718, 5, 0}, {"Iris radius", nullptr, 720, 5, 0}, {"Iris radius", nullptr, 722, 5, 0},
    {"Iris depth", "How deep the iris sits under the cornea, as a fraction of the iris radius (the parallax of the "
                   "refraction).", 621, 8, 2},
    {"Iris depth", nullptr, 623, 9, 0}, {"Iris depth", nullptr, 625, 8, 2},
    {"Iris depth", nullptr, 718, 8, 1}, {"Iris depth", nullptr, 720, 8, 3}, {"Iris depth", nullptr, 722, 8, 1},
    {"Cornea refraction index", "The cornea's index of refraction (1: no refraction; higher bends the view of the "
                                "iris more).", 621, 8, 3},
    {"Cornea refraction index", nullptr, 623, 9, 1}, {"Cornea refraction index", nullptr, 625, 8, 3},
    {"Cornea refraction index", nullptr, 718, 8, 2}, {"Cornea refraction index", nullptr, 720, 9, 0},
    {"Cornea refraction index", nullptr, 722, 8, 2},
    {"Limbus blend", "Near the iris' edge (this fraction of the depth), the iris' normal and texture blend into the "
                     "sclera's: the limbus ring's softness.", 623, 4, 2},
    {"Limbus blend", nullptr, 625, 4, 3}, {"Limbus blend", nullptr, 720, 4, 2}, {"Limbus blend", nullptr, 722, 4, 3},
    {"Specular x brightness", "The specular reflection's strength, scaled by the eye color's brightness.", 621, 4, 0},
    {"Specular x brightness", nullptr, 623, 3, 3}, {"Specular x brightness", nullptr, 718, 4, 0},
    {"Specular x brightness", nullptr, 720, 3, 3},
    // the hair card shader (jonah_hair's forward pass with an AO map, wetness and the mesh's tangents: the Blender
    // add-on's Hair preset, shader library 10481665, MaterialParams [15] to [19])
    {"AO map strength", "How strongly the AO map (t12) darkens the hair: 1 fully, 0 not at all.", kHairCardShader, 15, 0},
    {"AO map on UV2", "Which texture coordinates the AO map is read with: 0 the first UV map (as the color), 1 the "
                      "second (unique occlusion for cards that share their color texture).", kHairCardShader, 15, 1},
    {"AO on the lights", "How much of the AO also darkens the lights' diffuse and specular light: 0 only the ambient "
                         "light and the reflections, 1 everything.", kHairCardShader, 15, 2},
    {"Wetness strength", "The wet vertex effect (Lara's wetness, COLOR1.w) x this: 0 never wet, 1 as wet as Lara gets.",
     kHairCardShader, 15, 3},
    {"Wet color x", "The hair's color x this where fully wet (darker: below 1).", kHairCardShader, 16, 0},
    {"Wet specular x", "The highlights' strength x this where fully wet.", kHairCardShader, 16, 1},
    {"Wet gloss x", "The highlights' sharpness (their power) x this where fully wet.", kHairCardShader, 16, 2},
    {"Mesh tangents", "1: the strand highlights and the normal map follow the mesh's smooth tangents (a mesh "
                      "without them: strands taken to hang down); 0: each triangle's own, from its screen-space "
                      "derivatives (the original jonah_hair: the polygons show in the highlights).",
     kHairCardShader, 16, 3},
    {"Glint lookup", "0: the glints (which sharpen and brighten the highlights in spots) are read smoothly, from the "
                     "view in the mesh's tangent frame along the card; 1: jonah_hair's own lookup, from the reflected "
                     "normal x10, which bends at every triangle edge (the cards' polygons show in the highlights).",
     kHairCardShader, 17, 0},
    {"Glint reduction", "The smooth glint lookup x (1 - this): it is on more often than jonah_hair's (which drops out in "
                        "patches); 0.25 gives the original's average on hair cards. Higher: duller, softer highlights.",
     kHairCardShader, 17, 1},
    {"Specular x", "Every highlight's strength x this (the lights' and the sky's; after the gloss's own scaling: the "
                   "highlights grow brighter as they sharpen, x (gloss + 2) / 100).", kHairCardShader, 18, 0},
    {"Gloss x", "The highlights' sharpness x this (the gloss texture's range, then x this): below 1 broader, softer "
                "highlights, above 1 tighter ones. The second highlight follows (its own gloss x below).",
     kHairCardShader, 18, 1},
    {"Anisotropy", "1: the strands' own highlights (Kajiya-Kay: bands across the strands, as jonah_hair); 0: round "
                   "highlights about the surface's normal (Blinn-Phong); in between: a mix.", kHairCardShader, 18, 2},
    {"Highlight controls", "1: Specular x, Gloss x and Anisotropy are used; 0: as jonah_hair (each as 1).",
     kHairCardShader, 18, 3},
    {"Second highlight's gloss x", "The second, tinted highlight (its color: MaterialParams [3]) is as sharp as the "
                                   "gloss x this. jonah_hair: 0.02, a wide sheen over the whole hair; 0.3 - 1: a "
                                   "second, narrower strand highlight (shifted along the strands by the shift values).",
     kHairCardShader, 12, 1},
    {"Flatten the normal map", "0: the normal map as it is; 1: none (the mesh's own smooth normals); between: "
                               "its bumps x (1 - this).", kHairCardShader, 19, 0},
    {"Flatten towards the strands", "The normal turned towards each strand's side that faces the camera (as a round "
                                    "fibre is seen): 0 the cards' own normals, 1 fully. Smooth shading, highlights and "
                                    "reflections over crossing cards (with Mesh tangents on); the cards' own facing "
                                    "no longer shows.", kHairCardShader, 19, 1},
    // jonah_hair's forward shader (639; the hair card shader too, made from it), read from its code (2.0.36). Where
    // the texture analysis already offers the value (a channel's scale), these name it.
    {"Strand direction: along u", "The strands' direction for the highlights: this x the tangent (along the texture's "
                                  "u) + the next x the binormal (along v). jonah_hair: 0.2, 1 (strands along v).", 639, 7, 0},
    {"Strand direction: along v", "The strands' direction for the highlights: the previous x the tangent (along u) + this "
                                  "x the binormal (along v).", 639, 7, 1},
    {"Wrap lighting", "Added to the light's N . L (then clamped): light reaching round past the edge. 0 none.", 639, 12, 2},
    {"First highlight shift", "The first highlight moved along the strands (its direction tilted towards the normal by "
                              "this x the shift texture's R).", 639, 13, 0},
    {"Second highlight shift", "The second highlight moved along the strands (this x the shift texture's R).", 639, 12, 3},
    {"Sky highlight shift", "The sky highlight moved along the strands (this x the shift texture's R).", 639, 13, 3},
    {"Sky highlight's gloss x", "The highlight from above the camera (always there, no light needed): as sharp as the "
                                "gloss x this.", 639, 13, 2},
    {"Specular strength", "The highlights' strength (dry hair): this x the specular texture's R.", 639, 1, 3},
    {"Cut-out sharpness at a distance", "The cut-out (the color's alpha) read at its mip level x this: below 1 a sharper "
                                        "cut-out far away.", 639, 13, 1},
    {"Screen-space occlusion response", "The engine's ambient occlusion on the highlights: saturate(1 - a - b + a x AO + "
                                        "b x AO^2): this is a (linear).", 639, 14, 1},
    {"Screen-space occlusion response (squared)", "The engine's ambient occlusion on the highlights: saturate(1 - a - b "
                                                  "+ a x AO + b x AO^2): this is b (squared).", 639, 14, 2},
};

// A known value's shader matches the pass': the same, or the hair card shader for jonah_hair's (it is made from 639
// and keeps its values).
bool ShaderMatches(std::uint32_t known, std::uint32_t ps) { return known == ps || (known == 639 && ps == kHairCardShader); }

const char* const kEffectNames[4] = {"Dirt", "Blood", "Snow", "Wet"};

// Values of one shader read by hand, several components each: lara_head's composite (shader 755: the skin's light).
struct KnownGroup {
    const char* label;
    const char* help;
    std::uint32_t shader;
    bool color;                       // shown as a color (3 components)
    int components;
    int reg[4], comp[4];
    const char* names[4];
};
const KnownGroup kKnownGroups[] = {
    {"Scattering color", "The subsurface scattering's color: the light's tint where it falls off into shadow on the skin "
                         "(lerp(this, white, the light that reaches it)).",
     755, true, 3, {4, 4, 4, 0}, {0, 1, 2, 0}, {nullptr, nullptr, nullptr, nullptr}},
    {"Scattering falloff", "How the scattering color gives way to white with the light: saturate(1 - a - b + a x light + "
                           "b x light^2) (a: linear, b: squared).",
     755, false, 2, {17, 17, 0, 0}, {2, 3, 0, 0}, {"linear", "squared", nullptr, nullptr}},
    {"Reflections in shadow / light", "The environment's reflections on the skin scaled by its diffuse light: from this "
                                      "(in shadow) to this (fully lit).",
     755, false, 2, {15, 15, 0, 0}, {1, 3, 0, 0}, {"shadow", "light", nullptr, nullptr}},
    {"Sheen", "A rim of light at grazing angles (Fresnel), written for the engine's subsurface pass: its strength and how "
              "wide it spreads.",
     755, false, 2, {18, 18, 0, 0}, {1, 2, 0, 0}, {"strength", "spread", nullptr, nullptr}},
    {"First highlight color", "The first (sharp) strand highlight's color and strength: the light's highlight x this.",
     639, true, 3, {1, 1, 1, 0}, {0, 1, 2, 0}, {nullptr, nullptr, nullptr, nullptr}},
    {"Second highlight color", "The second (wider, shifted) strand highlight's color and strength.", 639, true, 3,
     {3, 3, 3, 0}, {0, 1, 2, 0}, {nullptr, nullptr, nullptr, nullptr}},
    {"Sky highlight color", "A strand highlight from above the camera, there without any light: its color and strength.",
     639, true, 3, {2, 2, 2, 0}, {0, 1, 2, 0}, {nullptr, nullptr, nullptr, nullptr}},
};
}  // namespace

const char* KindName(Kind kind) {
    switch (kind) {
        case Kind::UvScale: return "Tiling (UV scale)";
        case Kind::UvOffset: return "UV offset";
        case Kind::Tint: return "Color tint";
        case Kind::AlphaScale: return "Alpha scale";
        case Kind::Scale: return "Channel scale";
        case Kind::Bias: return "Channel offset";
        case Kind::HairHighlight: return "Strand highlights";
        case Kind::NormalStrength: return "Normal strength";
        case Kind::Color: return "Color";
        case Kind::Range: return "Range";
        case Kind::LayerBlend: return "Layer blend";
        case Kind::Named: return "Value";
        case Kind::Area: return "Area";
        case Kind::EffectScale: return "Effect multiplier";
        case Kind::Strength: return "Strength";
        case Kind::ViewDistance: return "View distance";
    }
    return "";
}

const char* PassName(int pass) {
    switch (pass) {
        case 1: return "alpha pre-pass (1)";
        case 2: return "depth pre-pass (2)";
        case 3: return "composite (3)";
        case 7: return "normals (7)";
        case 8: return "forward (8)";
        default: return "pass";
    }
}

std::vector<Param> Analyze(const Materials::Material& material, const Materials::Set& set) {
    std::vector<Param> params;
    if (!material.found) return params;
    // key: kind, texture, the shader's name for its register (the material graph's node: the same in every pass),
    // ordinal (the n-th register a pass uses for it)
    std::map<std::tuple<int, std::uint32_t, std::string, int>, std::size_t> index;
    std::map<std::pair<std::uint32_t, std::string>, int> copies;   // (texture, node) seen: its number for the label
    for (const int p : {Materials::kPassAlphaDepth, Materials::kPassDepth, Materials::kPassNormals, Materials::kPassComposite,
                        Materials::kPassForward}) {
        const Materials::Pass& pass = material.passes[p];
        if (!pass.present || !pass.ps) continue;
        auto shader = set.shaders.find(pass.ps);
        if (shader == set.shaders.end() || !shader->second.found) continue;
        const ShaderAnalysis& analysis = Cached(pass.ps, shader->second.code);
        if (!analysis.ok) continue;
        const int floats = static_cast<int>(pass.constants.size());

        struct Found {
            Kind kind;
            std::uint32_t texture;
            int textureRegister;
            int lane;
            int location;
        };
        std::vector<Found> found;
        auto slotOf = [&](int textureRegister) -> const Materials::Slot* {
            const Materials::Slot* slot = nullptr;
            for (const auto& s : pass.textures)
                if (s.reg == textureRegister) slot = &s;
            return slot && !EngineSlot(*slot) && slot->texture ? slot : nullptr;
        };
        // One use, or uses that agree (same kind, texture and lane).
        auto single = [](const std::vector<Use>& uses, bool lanes) {
            const Use& u = uses.front();
            for (const Use& other : uses)
                if (other.what != u.what || other.role != u.role || other.texture != u.texture || (lanes && other.lane != u.lane))
                    return false;
            return true;
        };
        // A register whose x, y, z multiply one texture channel: a color (e.g. a specular color x a reflectance map).
        std::set<int> colors;
        for (const auto& [where, uses] : analysis.uses) {
            if (where.second != 0) continue;
            bool color = true;
            int texture = -1, lane = -1;
            for (int c = 0; c < 3 && color; ++c) {
                auto it = analysis.uses.find({where.first, c});
                color = it != analysis.uses.end() && !it->second.empty() && single(it->second, true) &&
                        it->second.front().what == 'c' && it->second.front().role == 'm';
                if (!color) break;
                const Use& u = it->second.front();
                if (c == 0) {
                    texture = u.texture;
                    lane = u.lane;
                }
                color = u.texture == texture && u.lane == lane;
            }
            if (color && slotOf(texture) && !(slotOf(texture)->cls == 1 && lane == 0)) colors.insert(where.first);
        }
        std::set<std::pair<int, int>> inRanges;   // shown as the range's ends, not as a texture's bias too
        for (const RangeUse& r : analysis.ranges)
            if (r.texture >= 0) {
                inRanges.insert(r.at0);
                inRanges.insert(r.at1);
            }
        for (const auto& [where, uses] : analysis.uses) {
            const int location = where.first * 4 + where.second;
            if (location >= floats || uses.empty() || inRanges.count(where)) continue;
            const Use& u = uses.front();
            const Materials::Slot* slot = slotOf(u.texture);
            if (!slot) continue;
            if (colors.count(where.first) && where.second < 3) {
                found.push_back({Kind::Color, slot->texture, u.texture, where.second, location});
                continue;
            }
            // One role only: every use the same kind, texture and lane (else what it does is not one thing). A
            // normal map's two channels scaled by one value: its strength.
            if (!single(uses, true)) {
                if (single(uses, false) && u.what == 'c' && u.role == 'm' && slot->cls == 3)
                    found.push_back({Kind::NormalStrength, slot->texture, u.texture, 0, location});
                continue;
            }
            Kind kind;
            int lane = u.lane;
            if (u.what == 'u') {
                kind = u.role == 'm' ? Kind::UvScale : Kind::UvOffset;
            } else if (u.role == 'a') {
                kind = Kind::Bias;
            } else if (slot->cls == 1 && lane < 3) {
                kind = Kind::Tint;
            } else if (lane == 3) {
                kind = Kind::AlphaScale;
                lane = 0;
            } else {
                kind = Kind::Scale;
            }
            found.push_back({kind, slot->texture, u.texture, lane, location});
        }
        auto nodeOf = [&](int textureRegister) {
            auto it = analysis.textureNames.find(textureRegister);
            return it != analysis.textureNames.end() ? it->second : "t" + std::to_string(textureRegister);
        };
        {   // the layers' numbers: a texture read by several of the graph's nodes, numbered in the nodes' order (the
            // same whichever of their parameters are found)
            std::vector<std::pair<std::uint32_t, std::string>> pairs;
            for (const auto& sl : pass.textures)
                if (!EngineSlot(sl) && sl.texture) pairs.push_back({sl.texture, nodeOf(sl.reg)});
            std::sort(pairs.begin(), pairs.end());
            for (const auto& pr : pairs)
                if (!copies.count(pr)) {
                    int n = 0;
                    for (const auto& [k, v] : copies) n += k.first == pr.first ? 1 : 0;
                    copies.emplace(pr, n);
                }
        }
        // Within the pass: the registers used for one (kind, texture, node), in order: their ordinal.
        std::map<std::tuple<int, std::uint32_t, std::string>, std::vector<int>> registers;
        for (const Found& f : found) {
            auto& list = registers[{int(f.kind), f.texture, nodeOf(f.textureRegister)}];
            if (std::find(list.begin(), list.end(), f.location / 4) == list.end()) list.push_back(f.location / 4);
        }
        for (auto& [key, list] : registers) std::sort(list.begin(), list.end());
        for (const Found& f : found) {
            const std::string node = nodeOf(f.textureRegister);
            const auto& list = registers[{int(f.kind), f.texture, node}];
            const int ordinal = int(std::find(list.begin(), list.end(), f.location / 4) - list.begin());
            const auto key = std::make_tuple(int(f.kind), f.texture, node, ordinal);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = f.kind;
                param.texture = f.texture;
                auto name = set.textureNames.find(f.texture);
                param.textureName = name != set.textureNames.end() ? name->second : "texture " + std::to_string(f.texture);
                // The same texture under several nodes (layers): numbered in the order met.
                auto copy = copies.find({f.texture, node});
                if (copy == copies.end()) {
                    int n = 0;
                    for (const auto& [k, v] : copies) n += k.first == f.texture ? 1 : 0;
                    copy = copies.emplace(std::make_pair(f.texture, node), n).first;
                }
                if (copy->second) param.textureName += " (layer " + std::to_string(copy->second + 1) + ")";
                if (ordinal) param.textureName += " #" + std::to_string(ordinal + 1);
                param.slotName = node;
                param.components = f.kind == Kind::Tint || f.kind == Kind::Color ? 3
                                   : f.kind == Kind::AlphaScale || f.kind == Kind::NormalStrength ? 1
                                   : f.kind == Kind::UvScale || f.kind == Kind::UvOffset ? 2 : 4;
                if (f.kind == Kind::Color) {
                    auto factor = std::find_if(pass.textures.begin(), pass.textures.end(),
                                               [&](const Materials::Slot& s) { return s.reg == f.textureRegister; });
                    (void)factor;
                    param.help = "A color the texture's channel scales (its other uses: a layer's specular color, "
                                 "a tint of a mask).";
                }
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            Param& param = params[it->second];
            if (f.lane < param.components) {
                auto& list = param.where[f.lane];
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == f.location; }))
                    list.push_back({p, f.location});
            }
        }
        // A texture's name, numbered when several of the graph's nodes read it (layers).
        auto labelFor = [&](std::uint32_t texture, const std::string& node) {
            auto name = set.textureNames.find(texture);
            std::string label = name != set.textureNames.end() ? name->second : "texture " + std::to_string(texture);
            auto copy = copies.find({texture, node});
            if (copy == copies.end()) {
                int n = 0;
                for (const auto& [k, v] : copies) n += k.first == texture ? 1 : 0;
                copy = copies.emplace(std::make_pair(texture, node), n).first;
            }
            if (copy->second) label += " (layer " + std::to_string(copy->second + 1) + ")";
            return label;
        };
        // lerp(at 0, at 1, a texture's channel): named gloss when it reaches the gloss.
        std::map<std::pair<std::uint32_t, std::string>, int> rangeCount, blendCount;
        for (const RangeUse& r : analysis.ranges) {
            const Materials::Slot* slot = r.texture >= 0 ? slotOf(r.texture) : nullptr;
            if (!slot || r.at0.first * 4 + r.at0.second >= floats || r.at1.first * 4 + r.at1.second >= floats) continue;
            const std::string node = nodeOf(r.texture);
            const int ordinal = rangeCount[{slot->texture, node}]++;
            const auto key = std::make_tuple(int(Kind::Range), slot->texture, node, ordinal);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::Range;
                param.texture = slot->texture;
                param.textureName = labelFor(slot->texture, node);
                if (ordinal) param.textureName += " #" + std::to_string(ordinal + 1);
                param.slotName = node;
                param.components = 2;
                param.componentNames[0] = "at 0";
                param.componentNames[1] = "at 1";
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            Param& param = params[it->second];
            const bool gloss = analysis.gloss.count(r.at0) && analysis.gloss.count(r.at1);
            if (gloss) {
                param.label = std::string("Gloss range (") + "RGBA"[r.lane] + ")";
                param.help = "The specular gloss (highlight sharpness: 2 broad to 1000s mirror-like) the texture's "
                             "channel selects: its value at 0 (black) and at 1 (white).";
            } else if (param.label.empty()) {
                param.label = std::string("Range (") + "RGBA"[r.lane] + ")";
                param.help = "What the texture's channel selects between: its value at 0 (black) and at 1 (white).";
            }
            for (int c = 0; c < 2; ++c) {
                const auto& at = c ? r.at1 : r.at0;
                auto& list = param.where[c];
                const int location = at.first * 4 + at.second;
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                    list.push_back({p, location});
            }
        }
        // Layer blends (the vertex effects' layers: dirt, blood, snow, wet).
        for (const BlendUse& b : analysis.blends) {
            if (b.reg * 4 + 2 >= floats) continue;
            const Materials::Slot* slot = b.texture >= 0 ? slotOf(b.texture) : nullptr;
            const std::uint32_t texture = slot ? slot->texture : 0;
            const std::string node = slot ? nodeOf(b.texture) : std::string();
            const int ordinal = blendCount[{texture, node}]++;
            const auto key = std::make_tuple(int(Kind::LayerBlend), texture, node, ordinal);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::LayerBlend;
                param.texture = texture;
                if (slot) {
                    param.textureName = labelFor(texture, node);
                } else {
                    param.textureName = "Layers";
                }
                if (ordinal) param.textureName += " #" + std::to_string(ordinal + 1);
                param.slotName = node;
                param.components = 3;
                param.componentNames[0] = "mask";
                param.componentNames[1] = "ramp";
                param.componentNames[2] = "offset";
                param.label = b.effect >= 0 ? std::string(kEffectNames[b.effect]) + " layer"
                                            : slot ? std::string("Layer blend (") + "RGBA"[b.lane] + ")" : "Layer blend";
                param.help = b.effect >= 0
                                 ? std::string("Where the ") + kEffectNames[b.effect] +
                                       " effect shows (the game raises it per bone): saturate(mask x a + ramp x b + c), the "
                                       "mask being the effects mask times the effect's amount"
                                 : "Where the layer covers the one below: saturate(mask x a + ramp x b + c) (a: the "
                                   "mask's weight, b: the height ramp's, c: an offset)";
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            Param& param = params[it->second];
            for (int c = 0; c < 3; ++c) {
                auto& list = param.where[c];
                const int location = b.reg * 4 + c;
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                    list.push_back({p, location});
            }
        }
        // Areas: textures laid over a rectangle of the model's texture coordinates (Lara's lips over the mouth).
        for (const AreaUse& a : analysis.areas) {
            if (a.start.first * 4 + a.start.second >= floats || a.end.first * 4 + a.end.second >= floats) continue;
            std::vector<const Materials::Slot*> slots;
            for (const int t : a.textures)
                if (const Materials::Slot* slot = slotOf(t)) slots.push_back(slot);
            if (slots.empty()) continue;
            // (one entry for the area in every pass: keyed by its lowest texture ID, the passes bind them differently)
            std::sort(slots.begin(), slots.end(), [](const Materials::Slot* x, const Materials::Slot* y) { return x->texture < y->texture; });
            const std::string node = nodeOf(slots.front()->reg);
            const auto key = std::make_tuple(int(Kind::Area), slots.front()->texture, std::string("area"), a.axis);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::Area;
                param.texture = slots.front()->texture;
                param.textureName = labelFor(slots.front()->texture, node);
                param.slotName = node;
                param.components = 2;
                param.label = a.axis ? "Area: V (top, bottom)" : "Area: U (left, right)";
                param.componentNames[0] = "from";
                param.componentNames[1] = "to";
                std::string names;
                for (const Materials::Slot* slot : slots) {
                    auto n = set.textureNames.find(slot->texture);
                    names += (names.empty() ? "" : ", ") + (n != set.textureNames.end() ? n->second : "texture " + std::to_string(slot->texture));
                }
                param.help = std::string("Where these textures are laid on the model: they cover the model's texture "
                                         "coordinates from this ") + (a.axis ? "v (top) to this v (bottom)" : "u (left) to this u (right)") +
                             ", stretched to fit (with smoothed edges), instead of the whole texture. " + names +
                             ".\n(0..1 over the model's main texture; e.g. Lara's lips over her mouth.)";
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            Param& param = params[it->second];
            for (int c = 0; c < 2; ++c) {
                const auto& at = c ? a.end : a.start;
                auto& list = param.where[c];
                const int location = at.first * 4 + at.second;
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                    list.push_back({p, location});
            }
        }
        // Effect multipliers: a value scaled where a vertex effect (dirt, blood, snow, wet) shows.
        std::map<std::string, int> effectCount;
        for (const EffectScale& e : analysis.effectScales) {
            if ((!e.distance && (e.effect < 0 || e.effect > 3)) || e.constants.empty()) continue;
            bool fits = true;
            for (const auto& c : e.constants) fits = fits && c.first * 4 + c.second < floats;
            if (!fits) continue;
            // what the value is: a color (a color texture's), the gloss or the specular (a reflectance map's G / R)
            std::string what = "value";
            for (const auto& [t, channel] : e.valueTextures)
                if (const Materials::Slot* slot = slotOf(t)) {
                    if (slot->cls == 1) what = "color";
                    else if (slot->cls == 2 && what != "color") what = channel == 1 ? "gloss" : "specular";
                }
            if (what == "value" && e.constants.size() >= 3) what = "color";
            std::string mask;
            for (const int t : e.maskTextures)
                if (const Materials::Slot* slot = slotOf(t)) {
                    auto n = set.textureNames.find(slot->texture);
                    mask = n != set.textureNames.end() ? n->second : "texture " + std::to_string(slot->texture);
                }
            if (e.distance && what == "value") what = analysis.gloss.count(e.constants.front()) ? "gloss" : "specular";
            // (the mask texture's name is in the help only: in the label it pushed the field off the panel)
            std::string label = e.distance ? "Far: " + what + " x" : std::string(kEffectNames[e.effect]) + ": " + what + " x";
            const int ordinal = effectCount[label]++;
            if (ordinal) label += " #" + std::to_string(ordinal + 1);
            const auto key = std::make_tuple(int(Kind::EffectScale), 0u, label, 0);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::EffectScale;
                param.textureName = e.distance ? "View distance" : "Effects";
                param.label = label;
                param.components = int(std::min<std::size_t>(e.constants.size(), 4));
                if (param.components == 1) param.componentNames[0] = " ";
                if (e.distance)
                    param.help = "Seen from far (past the view distance blend), the material's " + what +
                                 " is multiplied by this (1: the same as up close). E.g. a sparkling metal toned down "
                                 "at the game's camera distance.";
                else
                    param.help = std::string("Where the ") + kEffectNames[e.effect] + " effect shows" +
                                 (mask.empty() ? "" : " (within " + mask + ")") + ", the material's " + what +
                                 " is multiplied by this (1: unchanged; the game raises the effect per bone, e.g. a wet "
                                 "face after swimming).";
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            Param& param = params[it->second];
            for (int c = 0; c < param.components && c < int(e.constants.size()); ++c) {
                auto& list = param.where[c];
                const int location = e.constants[std::size_t(c)].first * 4 + e.constants[std::size_t(c)].second;
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                    list.push_back({p, location});
            }
        }
        // The camera's distance: where the near look blends to the far one, and what the far one sets.
        if (analysis.distanceEnd.first >= 0 && analysis.distanceStart.first * 4 + analysis.distanceStart.second < floats &&
            analysis.distanceEnd.first * 4 + analysis.distanceEnd.second < floats) {
            const auto key = std::make_tuple(int(Kind::ViewDistance), 0u, std::string("distance"), 0);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::ViewDistance;
                param.textureName = "View distance";
                param.label = "Near to far blend";
                param.components = 2;
                param.componentNames[0] = "from";
                param.componentNames[1] = "to";
                param.help = "The material looks different with the camera's distance: up close (nearer than \"from\") "
                             "it has its own look, past \"to\" the far one (the values below), blending in between. "
                             "In metres (the game stores the squared distance in centimetres).";
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            for (int c = 0; c < 2; ++c) {
                const auto& at = c ? analysis.distanceEnd : analysis.distanceStart;
                auto& list = params[it->second].where[c];
                const int location = at.first * 4 + at.second;
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                    list.push_back({p, location});
            }
            for (const auto& target : analysis.distanceTargets) {
                const int location = target.first * 4 + target.second;
                if (location >= floats) continue;
                const bool gloss = analysis.gloss.count(target) > 0;
                const std::string label = gloss ? "Far: gloss" : "Far: value";
                const auto tkey = std::make_tuple(int(Kind::Named), 2u, label, 0);
                auto t = index.find(tkey);
                if (t == index.end()) {
                    Param param;
                    param.kind = Kind::Named;
                    param.textureName = "View distance";
                    param.label = label;
                    param.components = 1;
                    param.componentNames[0] = " ";
                    param.help = gloss ? "Seen from far (past the view distance blend), the material's gloss becomes this "
                                         "(highlight sharpness: 2 broad to 1000s mirror-like) instead of its own."
                                       : "Seen from far (past the view distance blend), the value becomes this.";
                    t = index.emplace(tkey, params.size()).first;
                    params.push_back(std::move(param));
                }
                auto& list = params[t->second].where[0];
                if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                    list.push_back({p, location});
            }
        }
        // Strengths: lerp(1, texture, k).
        for (const StrengthUse& u : analysis.strengths) {
            const Materials::Slot* slot = slotOf(u.texture);
            const int location = u.constant.first * 4 + u.constant.second;
            if (!slot || location >= floats) continue;
            const std::string node = nodeOf(u.texture);
            const auto key = std::make_tuple(int(Kind::Strength), slot->texture, node, 0);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::Strength;
                param.texture = slot->texture;
                param.textureName = labelFor(slot->texture, node);
                param.slotName = node;
                param.components = 1;
                param.componentNames[0] = " ";
                param.help = "How strongly the texture applies: 1 fully, 0 not at all (lerp(1, the texture, this)).";
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            auto& list = params[it->second].where[0];
            if (std::none_of(list.begin(), list.end(), [&](const Location& l) { return l.pass == p && l.index == location; }))
                list.push_back({p, location});
        }
        // Several values known by hand (the skin).
        for (const KnownGroup& known : kKnownGroups) {
            if (!ShaderMatches(known.shader, pass.ps)) continue;
            bool fits = true;
            for (int c = 0; c < known.components; ++c) fits = fits && known.reg[c] * 4 + known.comp[c] < floats;
            if (!fits) continue;
            const auto key = std::make_tuple(int(Kind::Named), 1u, std::string(known.label), 0);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = known.color ? Kind::Color : Kind::Named;
                param.textureName = known.shader == 755 ? "Skin" : "Hair";
                param.label = known.label;
                param.help = known.help;
                param.components = known.components;
                for (int c = 0; c < known.components; ++c)
                    if (known.names[c]) param.componentNames[c] = known.names[c];
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            for (int c = 0; c < known.components; ++c)
                params[it->second].where[c].push_back({p, known.reg[c] * 4 + known.comp[c]});
        }
        // Values known by hand (the eyes, the hair card shader).
        for (const KnownValue& known : kKnown) {
            if (!ShaderMatches(known.shader, pass.ps) || known.reg * 4 + known.comp >= floats) continue;
            // (a value the texture analysis already offers alone: named, not offered twice)
            if (known.shader == 639) {
                bool renamed = false;
                for (Param& other : params)
                    if (other.kind != Kind::Named && other.components == 1 && other.where[0].size() >= 1 &&
                        std::any_of(other.where[0].begin(), other.where[0].end(),
                                    [&](const Location& l) { return l.pass == p && l.index == known.reg * 4 + known.comp; })) {
                        other.label = known.name;
                        if (known.help) other.help = known.help;
                        renamed = true;
                    }
                if (renamed) continue;
            }
            const auto key = std::make_tuple(int(Kind::Named), 0u, std::string(known.name), 0);
            auto it = index.find(key);
            if (it == index.end()) {
                Param param;
                param.kind = Kind::Named;
                param.textureName = known.shader == kHairCardShader ? "Hair cards" : known.shader == 639 ? "Hair" : "Eye";
                param.label = known.name;
                param.components = 1;
                param.componentNames[0] = " ";
                for (const KnownValue& k : kKnown)
                    if (k.help && std::string(k.name) == known.name) param.help = k.help;
                it = index.emplace(key, params.size()).first;
                params.push_back(std::move(param));
            }
            params[it->second].where[0].push_back({p, known.reg * 4 + known.comp});
        }
        if (analysis.strands && floats >= 16) {   // read by the engine's hair pass (and by the preview's)
            Param param;
            param.kind = Kind::HairHighlight;
            param.components = 4;
            for (int c = 0; c < 4; ++c) param.where[c].push_back({p, 12 + c});
            param.note = std::string(PassName(p)) + ": MaterialParams[3]";
            params.push_back(std::move(param));
        }
    }
    // Notes: where each one is stored.
    for (Param& param : params) {
        if (param.kind == Kind::HairHighlight) continue;
        std::map<std::pair<int, int>, std::string> registers;   // (pass, register) -> components
        for (int c = 0; c < param.components; ++c)
            for (const Location& l : param.where[c]) {
                std::string& comps = registers[{l.pass, l.index / 4}];
                const char ch = "xyzw"[l.index % 4];
                if (comps.find(ch) == std::string::npos) comps += ch;
            }
        for (const auto& [where, comps] : registers) {
            if (!param.note.empty()) param.note += "\n";
            param.note += std::string(PassName(where.first)) + ": MaterialParams[" + std::to_string(where.second) + "]." + comps;
        }
    }
    std::stable_sort(params.begin(), params.end(), [](const Param& a, const Param& b) {
        return std::tie(a.textureName, a.kind) < std::tie(b.textureName, b.kind);
    });
    return params;
}


// ---- What a texture does, measured: the pixel shader run on made-up inputs ----
//
// A shader's disassembly is run by a small interpreter: every texture read returns a smooth made-up value, the vertex
// effects (COLOR1) and the other inputs are set, the material's constants are its own. Changing one texture channel a
// little shows which outputs it reaches (the normal pass: normal, gloss; the composite / forward pass: color, alpha),
// and running again with one vertex effect at 0 shows the channels only that effect switches on (its layer's).
namespace {
union Word {
    float f;
    std::uint32_t u;
    std::int32_t i;
};
struct Vec4 {
    Word c[4];
};

float Hash(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return float(h & 0xFFFFFF) / float(0x1000000);
}

enum class Op : std::uint8_t {
    Unknown, Mov, Movc, Add, Mul, Mad, Div, Min, Max, Sqrt, Rsq, Rcp, Log, Exp, Frc, RoundNi, RoundPi, RoundZ, RoundNe,
    Dp2, Dp3, Dp4, Lt, Ge, Eq, Ne, Sincos, DerivX, DerivY, Ftoi, Ftou, Itof, Utof, And, Or, Xor, Not, Iadd, Ineg, Imul,
    Umul, Imad, Udiv, Ishl, Ishr, Ushr, Ilt, Ige, Ieq, Ine, Ult, Uge, Imin, Imax, Umin, Umax, Bfi, Ubfe, Ibfe, Countbits,
    Sample, SampleC, Ld, LdStructured, LdBuffer, Lod, Store, Atomic, If, Else, Endif, Loop, Endloop, Break, Breakc,
    Continue, Continuec, Ret, Retc, Discard, Nop
};

struct Arg {
    char kind = 0;   // 'r' temp, 'v' input, 'o' output, 'c' constant buffer, 'l' literal, 't' texture, 's' sampler,
                     // 'n' null, 'u' UAV, 0 other (reads 0)
    int reg = 0;     // register (cb: the buffer)
    int index = 0;   // cb: element
    int relReg = -1, relLane = 0;   // cb[r#.c + index]
    std::uint8_t swz[4] = {0, 1, 2, 3};
    std::uint8_t mask = 0xF;        // a destination's lanes
    bool neg = false, abs = false;
    Word lit[4] = {};
};

struct Ins {
    Op op = Op::Unknown;
    bool sat = false;
    bool onZero = false;   // if_z, breakc_z, discard_z...
    std::vector<Arg> a;
    int jump = -1;         // if -> else / endif; else -> endif; loop <-> endloop; break, continue -> their loop's endloop / loop
    bool feedsDiscard = false;   // a comparison a discard tests: its a - b is the alpha test's margin
};

struct Program {
    bool ok = false;
    int temps = 0;
    std::vector<Ins> code;
    int materialBuffer = 3;
    int effectsReg = -1;      // COLOR1 (dirt, blood, snow, wet)
    std::uint8_t effectsMask = 0;
    int colorReg = -1;        // COLOR0
    std::uint8_t colorMask = 0;
    int positionReg = -1, frontReg = -1;
    std::set<int> sampled;    // texture registers it reads
};

int LaneIndex(char c) {
    switch (c) {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    default: return -1;
    }
}

Word ParseLiteral(std::string t) {
    Word w;
    w.u = 0;
    const std::size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
    if (a == std::string::npos) return w;
    t = t.substr(a, b - a + 1);
    if (t.find("0x") != std::string::npos) {
        const bool neg = t[0] == '-';
        w.u = std::uint32_t(std::strtoul(t.c_str() + (neg ? 1 : 0), nullptr, 16));
        if (neg) w.i = -w.i;
    } else if (t.find_first_of(".eE") != std::string::npos || t.find("inf") != std::string::npos ||
               t.find("nan") != std::string::npos) {
        w.f = std::strtof(t.c_str(), nullptr);
    } else {
        w.i = std::int32_t(std::strtoll(t.c_str(), nullptr, 10));
    }
    return w;
}

Arg ParseArg(std::string s, bool dest) {
    Arg a;
    const std::size_t lead = s.find_first_not_of(' '), tail = s.find_last_not_of(' ');
    if (lead == std::string::npos) return a;
    s = s.substr(lead, tail - lead + 1);
    if (!s.empty() && s[0] == '-') {
        a.neg = true;
        s = s.substr(1);
    }
    if (!s.empty() && s[0] == '|') {
        a.abs = true;
        s.erase(std::remove(s.begin(), s.end(), '|'), s.end());
    }
    if (s == "null") {
        a.kind = 'n';
        a.mask = 0;
        return a;
    }
    if (s.rfind("l(", 0) == 0) {
        a.kind = 'l';
        const std::size_t close = s.rfind(')');
        const std::string inner = s.substr(2, close == std::string::npos ? std::string::npos : close - 2);
        std::vector<Word> values;
        std::string current;
        for (char c : inner) {
            if (c == ',') {
                values.push_back(ParseLiteral(current));
                current.clear();
            } else {
                current += c;
            }
        }
        values.push_back(ParseLiteral(current));
        for (int k = 0; k < 4; ++k) a.lit[k] = values[std::min<std::size_t>(std::size_t(k), values.size() - 1)];
        return a;
    }
    std::size_t p = 0;
    while (p < s.size() && std::isalpha(static_cast<unsigned char>(s[p]))) ++p;
    const std::string name = s.substr(0, p);
    std::size_t q = p;
    while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) ++q;
    a.reg = q > p ? std::atoi(s.substr(p, q - p).c_str()) : 0;
    if (name == "r") a.kind = 'r';
    else if (name == "v") a.kind = 'v';
    else if (name == "o") a.kind = 'o';
    else if (name == "cb") a.kind = 'c';
    else if (name == "t") a.kind = 't';
    else if (name == "s") a.kind = 's';
    else if (name == "u") a.kind = 'u';
    else a.kind = 0;
    std::size_t rest = q;
    if (rest < s.size() && s[rest] == '[') {
        const std::size_t close = s.find(']', rest);
        std::string inner = s.substr(rest + 1, close == std::string::npos ? std::string::npos : close - rest - 1);
        rest = close == std::string::npos ? s.size() : close + 1;
        const std::size_t plus = inner.find('+');
        std::string relative = plus == std::string::npos ? inner : inner.substr(0, plus);
        relative.erase(std::remove(relative.begin(), relative.end(), ' '), relative.end());
        if (!relative.empty() && relative[0] == 'r') {
            const std::size_t dot = relative.find('.');
            a.relReg = std::atoi(relative.substr(1, dot == std::string::npos ? std::string::npos : dot - 1).c_str());
            a.relLane = dot == std::string::npos ? 0 : std::max(0, LaneIndex(relative[dot + 1]));
            a.index = plus == std::string::npos ? 0 : std::atoi(inner.substr(plus + 1).c_str());
        } else {
            a.index = std::atoi(inner.c_str());
        }
    }
    if (rest < s.size() && s[rest] == '.') {
        const std::string sw = s.substr(rest + 1);
        if (dest) {
            a.mask = 0;
            for (char c : sw)
                if (LaneIndex(c) >= 0) a.mask |= std::uint8_t(1u << LaneIndex(c));
        } else if (!sw.empty()) {
            for (int k = 0; k < 4; ++k) {
                const char c = sw.size() == 1 ? sw[0] : sw[std::min<std::size_t>(std::size_t(k), sw.size() - 1)];
                a.swz[k] = std::uint8_t(std::max(0, LaneIndex(c)));
            }
        }
    }
    return a;
}

Op OpOf(const std::string& name, bool& onZero) {
    static const std::map<std::string, Op> ops = {
        {"mov", Op::Mov}, {"movc", Op::Movc}, {"add", Op::Add}, {"mul", Op::Mul}, {"mad", Op::Mad}, {"div", Op::Div},
        {"min", Op::Min}, {"max", Op::Max}, {"sqrt", Op::Sqrt}, {"rsq", Op::Rsq}, {"rcp", Op::Rcp}, {"log", Op::Log},
        {"exp", Op::Exp}, {"frc", Op::Frc}, {"round_ni", Op::RoundNi}, {"round_pi", Op::RoundPi},
        {"round_z", Op::RoundZ}, {"round_ne", Op::RoundNe}, {"dp2", Op::Dp2}, {"dp3", Op::Dp3}, {"dp4", Op::Dp4},
        {"lt", Op::Lt}, {"ge", Op::Ge}, {"eq", Op::Eq}, {"ne", Op::Ne}, {"sincos", Op::Sincos},
        {"deriv_rtx", Op::DerivX}, {"deriv_rty", Op::DerivY}, {"deriv_rtx_fine", Op::DerivX},
        {"deriv_rty_fine", Op::DerivY}, {"deriv_rtx_coarse", Op::DerivX}, {"deriv_rty_coarse", Op::DerivY},
        {"ftoi", Op::Ftoi}, {"ftou", Op::Ftou}, {"itof", Op::Itof}, {"utof", Op::Utof}, {"and", Op::And},
        {"or", Op::Or}, {"xor", Op::Xor}, {"not", Op::Not}, {"iadd", Op::Iadd}, {"ineg", Op::Ineg},
        {"imul", Op::Imul}, {"umul", Op::Umul}, {"imad", Op::Imad}, {"umad", Op::Imad}, {"udiv", Op::Udiv},
        {"ishl", Op::Ishl}, {"ishr", Op::Ishr}, {"ushr", Op::Ushr}, {"ilt", Op::Ilt}, {"ige", Op::Ige},
        {"ieq", Op::Ieq}, {"ine", Op::Ine}, {"ult", Op::Ult}, {"uge", Op::Uge}, {"imin", Op::Imin},
        {"imax", Op::Imax}, {"umin", Op::Umin}, {"umax", Op::Umax}, {"bfi", Op::Bfi}, {"ubfe", Op::Ubfe},
        {"ibfe", Op::Ibfe}, {"countbits", Op::Countbits}, {"sample", Op::Sample}, {"sample_l", Op::Sample},
        {"sample_b", Op::Sample}, {"sample_d", Op::Sample}, {"sample_c", Op::SampleC}, {"sample_c_lz", Op::SampleC},
        {"gather4", Op::Sample}, {"ld", Op::Ld}, {"ld_ms", Op::Ld}, {"ld_structured", Op::LdStructured},
        {"ld_raw", Op::LdStructured}, {"lod", Op::Lod}, {"resinfo", Op::Lod}, {"store_structured", Op::Store},
        {"store_raw", Op::Store}, {"store_uav_typed", Op::Store}, {"if_nz", Op::If}, {"if_z", Op::If},
        {"else", Op::Else}, {"endif", Op::Endif}, {"loop", Op::Loop}, {"endloop", Op::Endloop},
        {"break", Op::Break}, {"breakc_nz", Op::Breakc}, {"breakc_z", Op::Breakc}, {"continue", Op::Continue},
        {"continuec_nz", Op::Continuec}, {"continuec_z", Op::Continuec}, {"ret", Op::Ret}, {"retc_nz", Op::Retc},
        {"retc_z", Op::Retc}, {"discard_nz", Op::Discard}, {"discard_z", Op::Discard}, {"nop", Op::Nop},
    };
    onZero = name.size() > 2 && name.compare(name.size() - 2, 2, "_z") == 0;
    if (name.rfind("imm_atomic", 0) == 0) return Op::Atomic;
    if (name.rfind("atomic", 0) == 0) return Op::Nop;
    auto it = ops.find(name);
    return it == ops.end() ? Op::Unknown : it->second;
}

Program BuildProgram(const std::vector<std::uint8_t>& code) {
    Program prog;
    ID3DBlob* text = nullptr;
    if (FAILED(D3DDisassemble(code.data(), code.size(), 0, nullptr, &text)) || !text) return prog;
    const std::string program(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
    text->Release();
    ID3D11ShaderReflection* reflection = nullptr;
    if (SUCCEEDED(D3DReflect(code.data(), code.size(), kReflectionId, reinterpret_cast<void**>(&reflection)))) {
        D3D11_SHADER_DESC desc{};
        reflection->GetDesc(&desc);
        for (UINT i = 0; i < desc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bind{};
            if (SUCCEEDED(reflection->GetResourceBindingDesc(i, &bind)) && bind.Type == D3D_SIT_CBUFFER && bind.Name &&
                std::string(bind.Name) == "MaterialBuffer")
                prog.materialBuffer = int(bind.BindPoint);
        }
        for (UINT i = 0; i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC in{};
            if (FAILED(reflection->GetInputParameterDesc(i, &in)) || !in.SemanticName) continue;
            if (_stricmp(in.SemanticName, "COLOR") == 0 && in.SemanticIndex == 1) {
                prog.effectsReg = int(in.Register);
                prog.effectsMask = std::uint8_t(in.ReadWriteMask ? in.ReadWriteMask : in.Mask);
            }
            if (_stricmp(in.SemanticName, "COLOR") == 0 && in.SemanticIndex == 0) {
                prog.colorReg = int(in.Register);
                prog.colorMask = std::uint8_t(in.ReadWriteMask ? in.ReadWriteMask : in.Mask);
            }
            if (_stricmp(in.SemanticName, "SV_POSITION") == 0) prog.positionReg = int(in.Register);
            if (_stricmp(in.SemanticName, "SV_IsFrontFace") == 0) prog.frontReg = int(in.Register);
        }
        reflection->Release();
    }
    std::istringstream lines(program);
    std::string line;
    std::vector<int> stack;   // open ifs / loops
    std::vector<int> loops;
    while (std::getline(lines, line)) {
        const std::size_t first = line.find_first_not_of(' ');
        if (first == std::string::npos) continue;
        line = line.substr(first);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("//", 0) == 0 || line.rfind("ps_", 0) == 0) continue;
        if (line.rfind("dcl_temps", 0) == 0) {
            prog.temps = std::atoi(line.c_str() + 10);
            continue;
        }
        if (line.rfind("dcl", 0) == 0) continue;
        // the opcode: up to the first space outside parentheses ("ld_structured_indexable(structured_buffer, stride=384)(...)")
        std::size_t end = 0;
        int depth = 0;
        for (; end < line.size(); ++end) {
            if (line[end] == '(') ++depth;
            if (line[end] == ')') --depth;
            if (line[end] == ' ' && depth == 0) break;
        }
        std::string opcode = line.substr(0, end);
        if (const std::size_t paren = opcode.find('('); paren != std::string::npos) opcode.resize(paren);
        Ins ins;
        if (const std::size_t sat = opcode.find("_sat"); sat != std::string::npos) {
            ins.sat = true;
            opcode.erase(sat, 4);
        }
        if (const std::size_t idx = opcode.find("_indexable"); idx != std::string::npos) opcode.erase(idx, 10);
        if (opcode == "ld" && line.find("(buffer)") != std::string::npos) opcode = "ld_buffer";
        ins.op = opcode == "ld_buffer" ? Op::LdBuffer : OpOf(opcode, ins.onZero);
        const std::vector<std::string> operands = end < line.size() ? SplitOperands(line.substr(end + 1)) : std::vector<std::string>{};
        for (std::size_t k = 0; k < operands.size(); ++k) {
            const bool dest = k == 0 || ((ins.op == Op::Sincos || ins.op == Op::Imul || ins.op == Op::Umul ||
                                          ins.op == Op::Udiv) && k == 1);
            const bool hasDest = !(ins.op == Op::If || ins.op == Op::Breakc || ins.op == Op::Continuec ||
                                   ins.op == Op::Retc || ins.op == Op::Discard || ins.op == Op::Store);
            ins.a.push_back(ParseArg(operands[k], dest && hasDest));
        }
        const int at = int(prog.code.size());
        switch (ins.op) {
        case Op::If: stack.push_back(at); break;
        case Op::Else:
            if (!stack.empty()) {
                prog.code[std::size_t(stack.back())].jump = at;
                stack.back() = at;
            }
            break;
        case Op::Endif:
            if (!stack.empty()) {
                prog.code[std::size_t(stack.back())].jump = at;
                stack.pop_back();
            }
            break;
        case Op::Loop:
            stack.push_back(at);
            loops.push_back(at);
            break;
        case Op::Endloop:
            if (!stack.empty()) {
                ins.jump = stack.back();
                prog.code[std::size_t(stack.back())].jump = at;
                stack.pop_back();
            }
            if (!loops.empty()) loops.pop_back();
            break;
        case Op::Break: case Op::Breakc: case Op::Continue: case Op::Continuec:
            ins.jump = loops.empty() ? -1 : loops.back();   // the loop (its endloop: the loop's jump, once known)
            break;
        default: break;
        }
        if (ins.op == Op::Sample || ins.op == Op::SampleC || ins.op == Op::Ld || ins.op == Op::LdStructured ||
            ins.op == Op::LdBuffer)
            for (const Arg& x : ins.a)
                if (x.kind == 't') prog.sampled.insert(x.reg);
        prog.code.push_back(std::move(ins));
    }
    // the comparisons the discards test (through and / or / movc)
    std::function<void(int, int, int, int)> markWriter = [&](int before, int reg, int lane, int depth) {
        for (int k = before - 1; k >= 0; --k) {
            Ins& w = prog.code[std::size_t(k)];
            if (w.a.empty() || w.a[0].kind != 'r' || w.a[0].reg != reg || !(w.a[0].mask & (1u << lane))) continue;
            if (w.op == Op::Lt || w.op == Op::Ge || w.op == Op::Eq || w.op == Op::Ne) {
                w.feedsDiscard = true;
            } else if (depth > 0 && (w.op == Op::And || w.op == Op::Or || w.op == Op::Movc || w.op == Op::Mov)) {
                for (std::size_t j = 1; j < w.a.size(); ++j)
                    if (w.a[j].kind == 'r') markWriter(k, w.a[j].reg, w.a[j].swz[lane], depth - 1);
            }
            return;
        }
    };
    for (std::size_t k = 0; k < prog.code.size(); ++k) {
        const Ins& d = prog.code[k];
        if (d.op == Op::Discard && !d.a.empty() && d.a[0].kind == 'r') markWriter(int(k), d.a[0].reg, d.a[0].swz[0], 3);
    }
    prog.temps = std::max(prog.temps, 1);
    prog.ok = !prog.code.empty();
    return prog;
}

// One run's made-up world.
struct Setup {
    std::uint32_t seed = 1;
    const std::vector<float>* constants = nullptr;   // the MaterialBuffer (nullptr: made up)
    // (low enough that layers summing to 1 still leave the base: Lara's normal pass weights her base normal map by
    // 1 - COLOR0.x - COLOR0.y)
    float effects[4] = {0.3f, 0.3f, 0.3f, 0.3f};
    float color[4] = {0.2f, 0.2f, 0.2f, 0.2f};
    const std::set<int>* materialTextures = nullptr; // registers of the material's own textures
    int perturbReg = -1, perturbChannel = -1;   // (channel -2: all four, by different amounts)
    float delta = 0.0f;
    // the engine's other buffers read as 1 .. 3 (not 0.1 .. 0.9): their counts (lights, probes) then run the loops
    bool loops = false;
};

struct Outputs {
    float v[9] = {};   // o0.xyzw, (4-7 unused), the alpha tests' margins (summed)
    bool any[9] = {};
    // what the shader stores in its UAVs (strand hair: packed integers), hashed per lane: any bit counts
    std::uint32_t stored[4] = {};
};

Vec4 ReadArg(const Arg& a, const std::vector<Vec4>& r, const Vec4* v, const Vec4* o, const Setup& s, const Program& p) {
    Vec4 out{};
    switch (a.kind) {
    case 'r':
        if (a.reg >= 0 && a.reg < int(r.size())) out = r[std::size_t(a.reg)];
        break;
    case 'v':
        if (a.reg >= 0 && a.reg < 32) out = v[a.reg];
        break;
    case 'o':
        if (a.reg >= 0 && a.reg < 8) out = o[a.reg];
        break;
    case 'l':
        for (int k = 0; k < 4; ++k) out.c[k] = a.lit[k];
        return out;   // (a literal's lanes are not swizzled)
    case 'c': {
        int element = a.index;
        if (a.relReg >= 0 && a.relReg < int(r.size())) element += r[std::size_t(a.relReg)].c[a.relLane].i;
        for (int k = 0; k < 4; ++k) {
            if (a.reg == p.materialBuffer && s.constants) {
                const long long at = (long long)element * 4 + k;
                out.c[k].f = at >= 0 && at < (long long)s.constants->size() ? (*s.constants)[std::size_t(at)] : 0.0f;
            } else if (a.reg == p.materialBuffer) {
                out.c[k].f = 0.1f + 0.9f * Hash(s.seed * 7 + 3, std::uint32_t(element), std::uint32_t(k));
            } else if (s.loops) {
                out.c[k].f = 1.0f + 2.0f * Hash(s.seed + 11 * std::uint32_t(a.reg), std::uint32_t(element), std::uint32_t(k));
            } else {
                out.c[k].f = 0.1f + 0.8f * Hash(s.seed + 11 * std::uint32_t(a.reg), std::uint32_t(element), std::uint32_t(k));
            }
        }
        break;
    }
    default: break;
    }
    Vec4 sw;
    for (int k = 0; k < 4; ++k) sw.c[k] = out.c[a.swz[k]];
    return sw;
}

float Clean(float x) { return std::isfinite(x) ? x : 0.0f; }

Outputs Run(const Program& p, const Setup& s) {
    Outputs result;
    std::vector<Vec4> r(std::size_t(p.temps) + 1);
    Vec4 v[32]{}, o[8]{};
    for (int reg = 0; reg < 32; ++reg)
        for (int k = 0; k < 4; ++k) v[reg].c[k].f = 0.1f + 0.8f * Hash(s.seed, 500 + std::uint32_t(reg), std::uint32_t(k));
    if (p.effectsReg >= 0 && p.effectsReg < 32)
        for (int k = 0; k < 4; ++k) v[p.effectsReg].c[k].f = s.effects[k];
    if (p.colorReg >= 0 && p.colorReg < 32)
        for (int k = 0; k < 4; ++k) v[p.colorReg].c[k].f = s.color[k];
    if (p.positionReg >= 0 && p.positionReg < 32) {
        v[p.positionReg].c[0].f = 100.5f + 37.0f * Hash(s.seed, 1, 1);
        v[p.positionReg].c[1].f = 60.5f + 29.0f * Hash(s.seed, 1, 2);
        v[p.positionReg].c[2].f = 0.4f;
        v[p.positionReg].c[3].f = 3.0f;
    }
    if (p.frontReg >= 0 && p.frontReg < 32)
        for (int k = 0; k < 4; ++k) v[p.frontReg].c[k].u = 0xFFFFFFFFu;

    auto texel = [&](int reg, float x, float y, float z, int lane) -> float {
        const bool own = s.materialTextures && s.materialTextures->count(reg);
        const std::uint32_t key = own ? std::uint32_t(reg) : 1000u + std::uint32_t(reg);
        float value = (own ? 0.2f + 0.6f * Hash(s.seed, key, std::uint32_t(lane))
                           : 0.25f + 0.5f * Hash(s.seed, key, std::uint32_t(lane))) +
                      0.08f * std::sin(5.1f * Clean(x) + 3.7f * Clean(y) + 2.3f * Clean(z) +
                                       6.28f * Hash(s.seed, key, 8u + std::uint32_t(lane)));
        if (reg == s.perturbReg && (lane == s.perturbChannel || s.perturbChannel == -2))
            value += s.perturbChannel == -2 ? s.delta * (1.0f + 0.37f * float(lane)) : s.delta;
        return value;
    };

    std::vector<int> iterations(p.code.size(), 0);
    int steps = 0;
    for (std::size_t pc = 0; pc < p.code.size() && steps < 60000; ++pc, ++steps) {
        const Ins& ins = p.code[pc];
        auto arg = [&](std::size_t k) -> Vec4 {
            return k < ins.a.size() ? ReadArg(ins.a[k], r, v, o, s, p) : Vec4{};
        };
        // operands with their modifiers, as floats or as integers
        auto fArg = [&](std::size_t k) {
            Vec4 x = arg(k);
            if (k < ins.a.size())
                for (int l = 0; l < 4; ++l) {
                    if (ins.a[k].abs) x.c[l].f = std::fabs(x.c[l].f);
                    if (ins.a[k].neg) x.c[l].f = -x.c[l].f;
                }
            return x;
        };
        auto iArg = [&](std::size_t k) {
            Vec4 x = arg(k);
            if (k < ins.a.size() && ins.a[k].neg)
                for (int l = 0; l < 4; ++l) x.c[l].i = -x.c[l].i;
            return x;
        };
        auto write = [&](std::size_t k, const Vec4& value, bool isFloat) {
            if (k >= ins.a.size()) return;
            const Arg& d = ins.a[k];
            Vec4* target = d.kind == 'r' && d.reg >= 0 && d.reg < int(r.size()) ? &r[std::size_t(d.reg)]
                         : d.kind == 'o' && d.reg >= 0 && d.reg < 8 ? &o[d.reg] : nullptr;
            if (!target) return;
            for (int l = 0; l < 4; ++l)
                if (d.mask & (1u << l)) {
                    Word w = value.c[l];
                    if (isFloat && ins.sat) w.f = std::isnan(w.f) ? 0.0f : std::min(1.0f, std::max(0.0f, w.f));
                    // (made-up inputs can divide 0 by 0, e.g. lights weighted by a sum of none: kept finite, so one
                    // such value does not hide what everything else does)
                    if (isFloat && !std::isfinite(w.f)) w.f = std::isnan(w.f) ? 0.0f : w.f > 0 ? 1e30f : -1e30f;
                    target->c[l] = w;
                }
        };
        Vec4 out{};
        auto perLaneF = [&](auto fn, std::size_t count) {
            Vec4 x[3] = {count > 0 ? fArg(1) : Vec4{}, count > 1 ? fArg(2) : Vec4{}, count > 2 ? fArg(3) : Vec4{}};
            for (int l = 0; l < 4; ++l) out.c[l].f = fn(x[0].c[l].f, x[1].c[l].f, x[2].c[l].f);
            write(0, out, true);
        };
        auto perLaneI = [&](auto fn, std::size_t first, std::size_t count) {
            Vec4 x[3] = {count > 0 ? iArg(first) : Vec4{}, count > 1 ? iArg(first + 1) : Vec4{}, count > 2 ? iArg(first + 2) : Vec4{}};
            for (int l = 0; l < 4; ++l) out.c[l] = fn(x[0].c[l], x[1].c[l], x[2].c[l]);
            write(first - 1, out, false);
        };
        auto cmpF = [&](auto fn) {
            Vec4 a = fArg(1), b = fArg(2);
            for (int l = 0; l < 4; ++l) out.c[l].u = fn(a.c[l].f, b.c[l].f) ? 0xFFFFFFFFu : 0u;
            if (ins.feedsDiscard && !ins.a.empty())
                for (int l = 0; l < 4; ++l)
                    if (ins.a[0].mask & (1u << l)) {
                        result.v[8] += Clean(a.c[l].f - b.c[l].f);
                        result.any[8] = true;
                    }
            write(0, out, false);
        };
        auto cmpI = [&](auto fn) {
            Vec4 a = iArg(1), b = iArg(2);
            for (int l = 0; l < 4; ++l) out.c[l].u = fn(a.c[l], b.c[l]) ? 0xFFFFFFFFu : 0u;
            write(0, out, false);
        };
        auto dot = [&](int n) {
            Vec4 a = fArg(1), b = fArg(2);
            float sum = 0.0f;
            for (int l = 0; l < n; ++l) sum += a.c[l].f * b.c[l].f;
            for (int l = 0; l < 4; ++l) out.c[l].f = sum;
            write(0, out, true);
        };
        auto condition = [&](std::size_t k) {
            const Vec4 c = arg(k);
            const bool nz = c.c[0].u != 0;
            return ins.onZero ? !nz : nz;
        };
        auto endOfLoop = [&](int loopAt) { return loopAt >= 0 ? p.code[std::size_t(loopAt)].jump : -1; };
        using W = Word;
        switch (ins.op) {
        case Op::Mov: perLaneF([](float a, float, float) { return a; }, 1); break;
        case Op::Movc: {
            const Vec4 c = arg(1), a = fArg(2), b = fArg(3);
            for (int l = 0; l < 4; ++l) out.c[l] = c.c[l].u ? a.c[l] : b.c[l];
            write(0, out, true);
            break;
        }
        case Op::Add: perLaneF([](float a, float b, float) { return a + b; }, 2); break;
        case Op::Mul: perLaneF([](float a, float b, float) { return a * b; }, 2); break;
        case Op::Mad: perLaneF([](float a, float b, float c) { return a * b + c; }, 3); break;
        case Op::Div: perLaneF([](float a, float b, float) { return b != 0.0f ? a / b : 0.0f; }, 2); break;
        case Op::Min: perLaneF([](float a, float b, float) { return std::min(a, b); }, 2); break;
        case Op::Max: perLaneF([](float a, float b, float) { return std::max(a, b); }, 2); break;
        case Op::Sqrt: perLaneF([](float a, float, float) { return std::sqrt(a); }, 1); break;
        case Op::Rsq: perLaneF([](float a, float, float) { return 1.0f / std::sqrt(a); }, 1); break;
        case Op::Rcp: perLaneF([](float a, float, float) { return 1.0f / a; }, 1); break;
        case Op::Log: perLaneF([](float a, float, float) { return std::log2(a); }, 1); break;
        case Op::Exp: perLaneF([](float a, float, float) { return std::exp2(a); }, 1); break;
        case Op::Frc: perLaneF([](float a, float, float) { return a - std::floor(a); }, 1); break;
        case Op::RoundNi: perLaneF([](float a, float, float) { return std::floor(a); }, 1); break;
        case Op::RoundPi: perLaneF([](float a, float, float) { return std::ceil(a); }, 1); break;
        case Op::RoundZ: perLaneF([](float a, float, float) { return std::trunc(a); }, 1); break;
        case Op::RoundNe: perLaneF([](float a, float, float) { return std::nearbyint(a); }, 1); break;
        case Op::Dp2: dot(2); break;
        case Op::Dp3: dot(3); break;
        case Op::Dp4: dot(4); break;
        case Op::Lt: cmpF([](float a, float b) { return a < b; }); break;
        case Op::Ge: cmpF([](float a, float b) { return a >= b; }); break;
        case Op::Eq: cmpF([](float a, float b) { return a == b; }); break;
        case Op::Ne: cmpF([](float a, float b) { return a != b; }); break;
        case Op::Sincos: {
            const Vec4 a = fArg(2);
            Vec4 sn, cs;
            for (int l = 0; l < 4; ++l) {
                sn.c[l].f = std::sin(a.c[l].f);
                cs.c[l].f = std::cos(a.c[l].f);
            }
            write(0, sn, true);
            write(1, cs, true);
            break;
        }
        // screen-space derivatives: made up but tied to the value (a tangent frame built from them still turns with
        // what it is made of)
        case Op::DerivX: perLaneF([](float a, float, float) { return 0.37f * a + 0.11f; }, 1); break;
        case Op::DerivY: perLaneF([](float a, float, float) { return -0.23f * a + 0.07f; }, 1); break;
        case Op::Ftoi: {
            const Vec4 a = fArg(1);
            for (int l = 0; l < 4; ++l) out.c[l].i = std::isfinite(a.c[l].f) ? std::int32_t(std::max(-2.0e9f, std::min(2.0e9f, a.c[l].f))) : 0;
            write(0, out, false);
            break;
        }
        case Op::Ftou: {
            const Vec4 a = fArg(1);
            for (int l = 0; l < 4; ++l) out.c[l].u = std::isfinite(a.c[l].f) ? std::uint32_t(std::max(0.0f, std::min(4.0e9f, a.c[l].f))) : 0;
            write(0, out, false);
            break;
        }
        case Op::Itof: {
            const Vec4 a = iArg(1);
            for (int l = 0; l < 4; ++l) out.c[l].f = float(a.c[l].i);
            write(0, out, true);
            break;
        }
        case Op::Utof: {
            const Vec4 a = iArg(1);
            for (int l = 0; l < 4; ++l) out.c[l].f = float(a.c[l].u);
            write(0, out, true);
            break;
        }
        case Op::And: perLaneI([](W a, W b, W) { W w; w.u = a.u & b.u; return w; }, 1, 2); break;
        case Op::Or: perLaneI([](W a, W b, W) { W w; w.u = a.u | b.u; return w; }, 1, 2); break;
        case Op::Xor: perLaneI([](W a, W b, W) { W w; w.u = a.u ^ b.u; return w; }, 1, 2); break;
        case Op::Not: perLaneI([](W a, W, W) { W w; w.u = ~a.u; return w; }, 1, 1); break;
        case Op::Iadd: perLaneI([](W a, W b, W) { W w; w.u = a.u + b.u; return w; }, 1, 2); break;
        case Op::Ineg: perLaneI([](W a, W, W) { W w; w.u = 0u - a.u; return w; }, 1, 1); break;
        case Op::Imul: case Op::Umul: perLaneI([](W a, W b, W) { W w; w.u = a.u * b.u; return w; }, 2, 2); break;
        case Op::Imad: perLaneI([](W a, W b, W c) { W w; w.u = a.u * b.u + c.u; return w; }, 1, 3); break;
        case Op::Udiv: perLaneI([](W a, W b, W) { W w; w.u = b.u ? a.u / b.u : 0xFFFFFFFFu; return w; }, 2, 2); break;
        case Op::Ishl: perLaneI([](W a, W b, W) { W w; w.u = a.u << (b.u & 31); return w; }, 1, 2); break;
        case Op::Ishr: perLaneI([](W a, W b, W) { W w; w.i = a.i >> (b.u & 31); return w; }, 1, 2); break;
        case Op::Ushr: perLaneI([](W a, W b, W) { W w; w.u = a.u >> (b.u & 31); return w; }, 1, 2); break;
        case Op::Ilt: cmpI([](W a, W b) { return a.i < b.i; }); break;
        case Op::Ige: cmpI([](W a, W b) { return a.i >= b.i; }); break;
        case Op::Ieq: cmpI([](W a, W b) { return a.u == b.u; }); break;
        case Op::Ine: cmpI([](W a, W b) { return a.u != b.u; }); break;
        case Op::Ult: cmpI([](W a, W b) { return a.u < b.u; }); break;
        case Op::Uge: cmpI([](W a, W b) { return a.u >= b.u; }); break;
        case Op::Imin: perLaneI([](W a, W b, W) { return a.i < b.i ? a : b; }, 1, 2); break;
        case Op::Imax: perLaneI([](W a, W b, W) { return a.i > b.i ? a : b; }, 1, 2); break;
        case Op::Umin: perLaneI([](W a, W b, W) { return a.u < b.u ? a : b; }, 1, 2); break;
        case Op::Umax: perLaneI([](W a, W b, W) { return a.u > b.u ? a : b; }, 1, 2); break;
        case Op::Countbits: perLaneI([](W a, W, W) { W w; w.u = 0; for (std::uint32_t x = a.u; x; x &= x - 1) ++w.u; return w; }, 1, 1); break;
        case Op::Ubfe: case Op::Ibfe: {
            const Vec4 width = iArg(1), offset = iArg(2), value = iArg(3);
            for (int l = 0; l < 4; ++l) {
                const std::uint32_t wdt = width.c[l].u & 31, off = offset.c[l].u & 31;
                out.c[l].u = wdt ? (value.c[l].u >> off) & ((1u << wdt) - 1u) : 0u;
            }
            write(0, out, false);
            break;
        }
        case Op::Bfi: {
            const Vec4 width = iArg(1), offset = iArg(2), insert = iArg(3), base = iArg(4);
            for (int l = 0; l < 4; ++l) {
                const std::uint32_t wdt = width.c[l].u & 31, off = offset.c[l].u & 31;
                const std::uint32_t m = (((1u << wdt) - 1u) << off);
                out.c[l].u = ((insert.c[l].u << off) & m) | (base.c[l].u & ~m);
            }
            write(0, out, false);
            break;
        }
        case Op::Sample: case Op::Ld: {
            const Vec4 at = ins.op == Op::Ld ? Vec4{} : fArg(1);
            float x, y, z;
            if (ins.op == Op::Ld) {
                const Vec4 coords = arg(1);
                x = float(coords.c[0].i) * 0.01f;
                y = float(coords.c[1].i) * 0.01f;
                z = float(coords.c[2].i) * 0.01f;
            } else {
                x = at.c[0].f;
                y = at.c[1].f;
                z = at.c[2].f;
            }
            const Arg& t = ins.a.size() > 2 ? ins.a[2] : Arg{};
            for (int l = 0; l < 4; ++l) out.c[l].f = texel(t.reg, x, y, z, t.swz[l]);
            write(0, out, true);
            break;
        }
        case Op::SampleC: {
            for (int l = 0; l < 4; ++l) out.c[l].f = 0.7f;
            write(0, out, true);
            break;
        }
        case Op::LdStructured: {
            // (lights: per element, signed, so some of them face the surface)
            const Arg& t = ins.a.size() > 3 ? ins.a[3] : Arg{};
            const Vec4 index = arg(1), offset = arg(2);
            for (int l = 0; l < 4; ++l)
                out.c[l].f = -1.0f + 2.0f * Hash(s.seed + 77, std::uint32_t(t.reg) * 7919u + (index.c[0].u & 1023u),
                                                 offset.c[0].u / 4 + t.swz[l]);
            write(0, out, true);
            break;
        }
        case Op::LdBuffer: {
            const Arg& t = ins.a.size() > 2 ? ins.a[2] : Arg{};
            for (int l = 0; l < 4; ++l) out.c[l].u = std::uint32_t(Hash(s.seed + 91, std::uint32_t(t.reg), t.swz[l]) * 4294967295.0);
            write(0, out, false);
            break;
        }
        case Op::Lod: write(0, Vec4{}, true); break;
        case Op::Atomic: write(0, Vec4{}, false); break;
        case Op::Store: {   // what a strand shader writes (its color) counts as its output
            const Vec4 value = ins.a.empty() ? Vec4{} : ReadArg(ins.a.back(), r, v, o, s, p);
            for (int l = 0; l < 4; ++l) result.stored[l] = (result.stored[l] ^ value.c[l].u) * 16777619u + 1u;
            break;
        }
        case Op::If:
            if (!condition(0) && ins.jump >= 0) pc = std::size_t(ins.jump);   // to the else (then past it) or the endif
            break;
        case Op::Else:
            if (ins.jump >= 0) pc = std::size_t(ins.jump);   // the if's branch ran: skip to the endif
            break;
        case Op::Loop: iterations[pc] = 0; break;
        case Op::Endloop:
            if (ins.jump >= 0 && ++iterations[std::size_t(ins.jump)] < 3) pc = std::size_t(ins.jump);   // (at most 3 times)
            break;
        case Op::Break:
            if (endOfLoop(ins.jump) >= 0) pc = std::size_t(endOfLoop(ins.jump));
            break;
        case Op::Breakc:
            if (condition(0) && endOfLoop(ins.jump) >= 0) pc = std::size_t(endOfLoop(ins.jump));
            break;
        case Op::Continue:
            if (endOfLoop(ins.jump) >= 0) pc = std::size_t(endOfLoop(ins.jump)) - 1;
            break;
        case Op::Continuec:
            if (condition(0) && endOfLoop(ins.jump) >= 0) pc = std::size_t(endOfLoop(ins.jump)) - 1;
            break;
        case Op::Ret: pc = p.code.size(); break;
        case Op::Retc:
            if (condition(0)) pc = p.code.size();
            break;
        default: break;
        }
        if (pc >= p.code.size()) break;
    }
    for (int l = 0; l < 4; ++l) {
        result.v[l] = o[0].c[l].f;
        result.any[l] = true;
    }
    return result;
}

const Program& CachedProgram(std::uint32_t id, const std::vector<std::uint8_t>& code) {
    static std::mutex mutex;
    static std::map<std::uint32_t, std::unique_ptr<Program>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto& slot = cache[id];
    if (!slot) slot = std::make_unique<Program>(BuildProgram(code));
    return *slot;
}

// What one pass' texture channels reach, measured.
struct Measured {
    // per texture register: per channel, outputs reached (bit 0 o0.xyz, 1 o0.w, 3 an alpha test, 4 the UAV writes:
    // strand hair's fragments), with the vertex effects partly on
    std::map<int, std::array<std::uint8_t, 4>> reach;      // with the material's constants
    std::map<int, std::array<std::uint8_t, 4>> reachAny;   // with made-up ones too (what it could do)
    // per texture register: per channel, inputs lanes that switch it off at 0 (bits 0-3 COLOR1 dirt, blood, snow, wet;
    // 4-7 COLOR0 x, y, z, w)
    std::map<int, std::array<std::uint8_t, 4>> gates;
};

Measured Measure(const Program& p, const std::vector<float>& constants, const std::set<int>& textures) {
    Measured m;
    struct Lane {
        int bit;
        bool effects;
        int lane;
    };
    std::vector<Lane> lanes;
    for (int l = 0; l < 4; ++l) {
        if (p.effectsReg >= 0 && (p.effectsMask & (1u << l))) lanes.push_back({l, true, l});
        if (p.colorReg >= 0 && (p.colorMask & (1u << l))) lanes.push_back({4 + l, false, l});
    }
    std::vector<int> regs;
    for (int t : textures)
        if (p.sampled.count(t)) regs.push_back(t);
    using Reach = std::map<int, std::array<std::uint8_t, 4>>;
    auto measure = [&](const Setup& s, Reach& into) {
        const Outputs base = Run(p, s);
        auto bitsOf = [&](const Outputs& changed) {
            std::uint8_t bits = 0;
            for (int k = 0; k < 9; ++k) {
                if (k >= 4 && k < 8) continue;
                const float a = base.v[k], b = changed.v[k];
                if (!std::isfinite(a) || !std::isfinite(b)) continue;
                if (std::fabs(a - b) > 1e-6f * std::max(1.0f, std::fabs(a))) bits |= std::uint8_t(k < 3 ? 1 : k == 3 ? 2 : 8);
            }
            for (int l = 0; l < 4; ++l)
                if (base.stored[l] != changed.stored[l]) bits |= 16;
            return bits;
        };
        for (int t : regs) {
            auto& r = into[t];
            Setup q = s;
            q.perturbReg = t;
            q.delta = 0.03f;
            q.perturbChannel = -2;   // every channel at once first: a texture that changes nothing needs no more runs
            if (!bitsOf(Run(p, q))) continue;
            for (int c = 0; c < 4; ++c) {
                q.perturbChannel = c;
                r[std::size_t(c)] |= bitsOf(Run(p, q));
            }
        }
    };
    // ten worlds: six with the material's constants (made-up views and lights: a highlight needs the light and the
    // view on the surface's side), four with made-up constants (what the textures could do); the vertex effects partly
    // and fully on (a layer's blend can need more than a little of its effect to show at all)
    constexpr int kSets = 10, kOwnSets = 6;
    Reach used[kSets], ungated[kSets][8];   // per set: with the effects on; with one lane at 0 (the others as on)
    for (int setIndex = 0; setIndex < kSets; ++setIndex)
        for (const float level : {0.3f, 1.0f}) {
            Setup s;
            s.seed = 1 + std::uint32_t(setIndex) * 101;
            s.constants = setIndex < kOwnSets ? &constants : nullptr;
            s.materialTextures = &textures;
            for (float& e : s.effects) e = level;
            Reach on;
            measure(s, on);
            for (const auto& [t, r] : on)
                for (int c = 0; c < 4; ++c) used[setIndex][t][std::size_t(c)] |= r[std::size_t(c)];
            for (const Lane& lane : lanes) {
                if (!lane.effects && level != 0.3f) continue;   // (COLOR0: Lara's blend shapes; once is enough)
                Setup off = s;
                (lane.effects ? off.effects : off.color)[lane.lane] = 0.0f;
                Reach r;
                measure(off, r);
                // a channel used here with the lane on but not with it off is gated at this level: keep "used" marks
                // only for channels this level shows (the other level's verdict stands for the rest)
                for (const auto& [t, bits] : on)
                    for (int c = 0; c < 4; ++c)
                        if (bits[std::size_t(c)]) {
                            auto& g = ungated[setIndex][lane.bit][t][std::size_t(c)];
                            const auto it = r.find(t);
                            if (it != r.end() && it->second[std::size_t(c)]) g |= it->second[std::size_t(c)];
                        }
            }
        }
    for (int t : regs) {
        std::array<std::uint8_t, 4> reach{}, g{};
        std::array<std::uint8_t, 4> any{};
        for (int c = 0; c < 4; ++c) {
            bool own = false;
            for (int k = 0; k < kSets; ++k) {
                any[std::size_t(c)] |= used[k][t][std::size_t(c)];
                if (k < kOwnSets) reach[std::size_t(c)] |= used[k][t][std::size_t(c)];
                if (k < kOwnSets && used[k][t][std::size_t(c)]) own = true;
            }
            if (!any[std::size_t(c)]) continue;
            // the material's own constants decide (made-up ones can switch a layer on with no effect at all), unless
            // they leave the channel unused
            std::uint8_t gated = 0xFF;
            for (int k = own ? 0 : kOwnSets; k < (own ? kOwnSets : kSets); ++k) {
                if (!used[k][t][std::size_t(c)]) continue;
                std::uint8_t here = 0;
                for (const Lane& lane : lanes)
                    if (!ungated[k][lane.bit][t][std::size_t(c)]) here |= std::uint8_t(1u << lane.bit);
                gated &= here;
            }
            g[std::size_t(c)] = gated == 0xFF ? 0 : gated;
        }
        m.reach[t] = reach;
        m.reachAny[t] = any;
        m.gates[t] = g;
    }
    return m;
}
}  // namespace

namespace {
std::string ChannelLetters(const std::set<int>& channels) {
    std::string out;
    for (int c : channels)
        if (c >= 0 && c < 4) out += "RGBA"[c];
    return out;
}
// A gate bit's name: COLOR1's lanes are the vertex effects; COLOR0's x, y where Lara's blend shapes squash and
// stretch the mesh (her vertex shaders' COLOR0.xy: the compression and expansion maps' weights).
const char* GateName(int bit) {
    static const char* names[8] = {"Dirt", "Blood", "Snow", "Wet", "Squashed", "Stretched", "Vertex color B",
                                   "Vertex color A"};
    return bit >= 0 && bit < 8 ? names[bit] : "Effect";
}
std::string Lower(std::string s) {
    if (!s.empty()) s[0] = char(std::tolower(static_cast<unsigned char>(s[0])));
    return s;
}
}  // namespace

std::map<std::uint32_t, TextureRole> TextureRoles(const Materials::Material& material, const Materials::Set& set) {
    struct Acc {
        std::uint8_t kinds[4] = {};   // per channel: 1 normal, 2 gloss, 4 color, 8 alpha, 16 alpha test, 32 strand
                                      // fragments (measured)
        std::uint8_t kindsAny[4] = {};   // the same with made-up constants too
        std::uint8_t gates[4] = {0xFF, 0xFF, 0xFF, 0xFF};   // per channel: the lanes that switch it off in every pass using it
        std::set<int> strength;
        bool screen = false, sampled = false, read = false;
        bool strand = false;   // reaches strand hair's fragments (packed with the strand's color)
        int cls = -1;
    };
    std::map<std::uint32_t, Acc> acc;
    if (!material.found) return {};
    for (int p = 0; p < 10; ++p) {
        const Materials::Pass& pass = material.passes[p];
        if (!pass.present) continue;
        std::set<int> regs;
        for (const auto& slot : pass.textures)
            if (!EngineSlot(slot) && slot.texture) {
                if (acc[slot.texture].cls < 0) acc[slot.texture].cls = slot.cls;
                regs.insert(slot.reg);
            }
        if (!pass.ps) continue;
        auto shader = set.shaders.find(pass.ps);
        if (shader == set.shaders.end() || !shader->second.found) continue;
        auto texturesAt = [&](int reg) {
            std::vector<Acc*> out;
            for (const auto& slot : pass.textures)
                if (slot.reg == reg && !EngineSlot(slot) && slot.texture) out.push_back(&acc[slot.texture]);
            return out;
        };
        for (const auto& slot : pass.textures)
            if (!EngineSlot(slot) && slot.texture) acc[slot.texture].read = true;
        // read from the code: the screen-position reads (a dither), the strengths (lerp(1, texture, k): an occlusion)
        const ShaderAnalysis& analysis = Cached(pass.ps, shader->second.code);
        if (analysis.ok) {
            for (int reg : analysis.screenTextures)
                for (Acc* a : texturesAt(reg)) a->screen = true;
            for (const auto& st : analysis.strengths)
                for (Acc* a : texturesAt(st.texture)) a->strength.insert(st.lane);
        }
        // measured: the normal pass' normal (xyz) and gloss (w), the composite's / forward pass' color and alpha, every
        // pass' alpha test
        const bool normals = p == Materials::kPassNormals;
        const bool colors = p == Materials::kPassComposite || p == Materials::kPassForward;
        // (the depth pre-pass the game draws: an opaque blend mode has none with a pixel shader, so its alpha test
        // cuts nothing, 2.0.27)
        const bool depth = (p == Materials::kPassDepth || p == Materials::kPassAlphaDepth) && p == Materials::DepthPass(material);
        const Program& program = CachedProgram(pass.ps, shader->second.code);
        if (!program.ok) continue;
        for (int reg : regs)
            if (program.sampled.count(reg))
                for (Acc* a : texturesAt(reg)) a->sampled = true;
        if (!normals && !colors && !depth) continue;
        const Measured m = Measure(program, pass.constants, regs);
        auto kindsOf = [&](std::uint8_t bits) {
            std::uint8_t kinds = bits & 8 ? 16 : 0;
            if (normals) kinds |= std::uint8_t((bits & 1 ? 1 : 0) | (bits & 2 ? 2 : 0));
            else if (colors) kinds |= std::uint8_t((bits & 1 ? 4 : 0) | (bits & 2 ? 8 : 0) | (bits & 16 ? 4 | 32 : 0));
            return kinds;
        };
        for (const auto& [reg, reach] : m.reachAny) {
            const auto& gates = m.gates.at(reg);
            const auto& own = m.reach.at(reg);
            for (Acc* a : texturesAt(reg))
                for (int c = 0; c < 4; ++c) {
                    const std::uint8_t kinds = kindsOf(reach[std::size_t(c)]);
                    if (!kinds) continue;
                    a->kinds[c] |= kindsOf(own[std::size_t(c)]);
                    a->kindsAny[c] |= kinds;
                    if (kinds & 15) a->gates[c] &= gates[std::size_t(c)];
                    if (kinds & 32) a->strand = true;
                }
        }
    }

    std::map<std::uint32_t, TextureRole> roles;
    for (const auto& [texture, a] : acc) {
        TextureRole r;
        std::string why;
        auto add = [&](const std::string& text) { why += (why.empty() ? "" : "; ") + text; };
        std::set<int> mask, normal, gloss, color, alpha, used, discard;
        // what it does with the material's values; when that is nothing, what it could do (marked)
        bool ownUse = false;
        for (int c = 0; c < 4; ++c) ownUse = ownUse || a.kinds[c];
        bool potential = false;
        for (int c = 0; c < 4; ++c) potential = potential || (!ownUse && a.kindsAny[c]);
        const std::uint8_t* kindsUsed = ownUse ? a.kinds : a.kindsAny;
        for (int c = 0; c < 4; ++c) {
            if (kindsUsed[c] & 16) discard.insert(c);
            const std::uint8_t k = kindsUsed[c] & 15;
            if (!k) continue;
            used.insert(c);
            const bool n = k & 1, g = k & 2, col = k & 4, al = k & 8;
            if (n && (g || col) && a.cls != 3) mask.insert(c);
            else if (n) normal.insert(c);
            else if (g) gloss.insert(c);
            else if (col) color.insert(c);
            else if (al) alpha.insert(c);
        }
        // the lanes that switch the whole texture off (its layer), over the channels that are not masks
        std::uint8_t layerGates = 0xFF;
        bool anyContent = false;
        for (int c : used)
            if (!mask.count(c)) {
                layerGates &= a.gates[c];
                anyContent = true;
            }
        if (!anyContent) layerGates = 0;
        auto gateList = [](std::uint8_t bits, bool effectsOnly) {
            std::vector<int> out;
            for (int b = 0; b < 8; ++b)
                if ((bits & (1u << b)) && (!effectsOnly || b < 4)) out.push_back(b);
            return out;
        };
        std::vector<std::string> parts;
        if (a.screen && normal.empty() && mask.empty() && gloss.empty()) {
            parts.push_back("Dither pattern");
            add("read at the pixel's screen position: the fade-out's dither, not the model's texture");
        } else {
            if (!mask.empty()) {
                bool allEffects = true;
                std::string list;
                for (int c : mask) {
                    const std::vector<int> g = gateList(a.gates[c], false);
                    list += (list.empty() ? "" : ", ") + std::string(1, "RGBA"[c]);
                    if (!g.empty()) {
                        std::string names;
                        for (int b : g) names += (names.empty() ? "" : " + ") + Lower(GateName(b));
                        list += ": " + names;
                    }
                    if (g.empty() || g.front() >= 4) allEffects = false;
                }
                std::uint8_t common = 0xFF;
                for (int c : mask) common &= a.gates[c];
                const std::vector<int> shared = gateList(common, true);
                if (a.cls == 5 && shared.size() == 1) {
                    parts.push_back(std::string(GateName(shared[0])) + " layer: blend pattern (" + ChannelLetters(mask) + ")");
                    add("its " + ChannelLetters(mask) + " changes the normal and the color together: a pattern in the " +
                        Lower(GateName(shared[0])) + " layer's blend (it breaks up where the layers meet)");
                } else if (a.cls == 5) {
                    parts.push_back("Blend pattern (" + list + ")");
                    add("its " + ChannelLetters(mask) + " changes the normal and the color together: a pattern in a layer "
                        "blend (it breaks up where the layers meet)");
                } else {
                    parts.push_back((allEffects ? "Effects mask (" : "Layer mask (") + list + ")");
                    add("its " + ChannelLetters(mask) + " changes the normal and the color together: it blends layers");
                    if (allEffects) add("each channel only matters where its vertex effect shows");
                }
            }
            if (!normal.empty() || (a.cls == 3 && !color.empty())) {
                parts.push_back("Normal map");
                add(!normal.empty() ? "the normal pass builds the surface normal from its " + ChannelLetters(normal)
                                    : "changes the lit color (forward pass) and its format holds a normal map");
            }
            if (!color.empty() && a.cls != 2 && a.cls != 3) {
                std::set<int> rgb;
                for (int c : color)
                    if (c < 3) rgb.insert(c);
                if (!rgb.empty()) {
                    if (a.cls == 5) parts.push_back("Ramp");
                    else if (a.strand && a.cls != 1 && rgb.size() < 3) parts.push_back("Strand highlight shift (" + ChannelLetters(rgb) + ")");
                    else if (!a.strength.empty() && a.cls != 1) parts.push_back("Ambient occlusion (" + ChannelLetters(a.strength) + ")");
                    else if (a.cls == 1 || rgb.size() >= 3) parts.push_back("Color");
                    else parts.push_back("Color mask (" + ChannelLetters(rgb) + ")");
                    if (a.strand && a.cls != 1 && a.cls != 5 && rgb.size() < 3)
                        add("its " + ChannelLetters(rgb) + " is packed beside the strand's color into the hair's fragments "
                            "(where the engine's hair pass reads the highlights' shift)");
                    else
                        add("its " + ChannelLetters(rgb) + " changes the color only" +
                            (a.cls != 1 && a.cls != 5 && rgb.size() < 3 ? " (a tint, occlusion or similar mask)" : ""));
                }
            }
            std::set<int> specular;
            if (a.cls == 2)
                for (int c : color) specular.insert(c);
            if (!gloss.empty() && !specular.empty()) {
                parts.push_back("Specular (" + ChannelLetters(specular) + ") and gloss (" + ChannelLetters(gloss) + ")");
                add("its " + ChannelLetters(gloss) + " reaches the normal pass' gloss (highlight sharpness), its " +
                    ChannelLetters(specular) + " only the lit color (the highlight's strength)");
            } else if (!gloss.empty()) {
                parts.push_back("Gloss (" + ChannelLetters(gloss) + ")");
                add("its " + ChannelLetters(gloss) + " reaches the normal pass' gloss (highlight sharpness)");
            } else if (!specular.empty()) {
                parts.push_back(a.cls == 2 && normal.empty() && mask.empty() && specular.count(0) && specular.count(1)
                                    ? "Specular (R) and gloss (G)" : "Specular (" + ChannelLetters(specular) + ")");
                add("its " + ChannelLetters(specular) + " changes the lit color; its format holds reflectance and roughness");
            }
        }
        // the alpha channel's own job
        std::string alphaNote;
        const bool dither = !parts.empty() && parts[0] == "Dither pattern";
        if (!discard.empty() && !dither) {
            alphaNote = ChannelLetters(discard) + ": cut-out";
            add("its " + ChannelLetters(discard) + " decides the alpha test (the surface is cut where it is low)");
        } else if (material.Blended() && (!alpha.empty() || (color.count(3) && a.cls == 1))) {
            alphaNote = "A: opacity";
            add("its alpha is the blended surface's opacity");
        }
        if (parts.empty() && !alphaNote.empty()) {
            parts.push_back("Cut-out (" + ChannelLetters(discard.empty() ? std::set<int>{3} : discard) + ")");
            alphaNote.clear();
        }
        if (!parts.empty()) {
            r.fromShaders = true;
            std::string role = parts[0];
            for (std::size_t k = 1; k < parts.size(); ++k) role += ", " + Lower(parts[k]);
            if (!alphaNote.empty()) role += " (" + alphaNote + ")";
            const std::vector<int> layer = gateList(layerGates, false);
            if (!layer.empty() && mask.empty()) {
                std::string names;
                for (int b : layer) names += (names.empty() ? "" : " + ") + std::string(names.empty() ? GateName(b) : Lower(GateName(b)));
                const bool effects = layer.front() < 4;
                role = names + (effects ? " layer: " : ": ") + Lower(role);
                add(effects ? "it only matters where " + Lower(names) + " shows (nothing where that vertex effect is 0)"
                            : std::string("it only matters where the mesh is ") + Lower(names) +
                                  " (by the blend shapes: the vertex color's " + (layer.front() == 4 ? "R" : "G") + ")");
            }
            if (potential) {
                role += " (no effect with its values)";
                add("with the material's own values it changes nothing; with others it would act as named");
            }
            r.role = role;
        } else {
            r.role = a.cls == 1 ? "Color" : a.cls == 2 ? "Specular (R) and gloss (G)" : a.cls == 3 ? "Normal map"
                   : a.cls == 5 ? "Ramp" : "Mask / data";
            if (!a.read) add("by its format (the shaders were not read)");
            else if (!a.sampled) {
                r.role += " (not read by its shaders)";
                add("bound, but its shaders never read it: by its format");
            } else {
                add("read, but with these constants it changes nothing: by its format");
            }
        }
        r.why = why;
        roles[texture] = std::move(r);
    }
    return roles;
}

// ---- Every value, named: what each MaterialParams value does, measured (Properties) ----
namespace {
struct ConstUse {
    bool read = false;
    std::uint8_t reach = 0, reachAny = 0;   // (Measure's bits: 1 o0.xyz, 2 o0.w, 8 an alpha test, 16 the UAV writes)
    std::uint8_t gates = 0;                 // lanes it only acts with (0-3 COLOR1's effects, 4-7 COLOR0)
    bool power = false, threshold = false;
    // what the code combines it with: texture register -> (channels, instructions away: the nearest kept)
    std::map<int, std::pair<std::uint8_t, int>> with;
};

// Where a temp lane's value comes from, roughly (straight through the code, branches ignored): textures' channels and
// how many instructions back they were read.
struct Taint {
    std::map<int, std::pair<std::uint8_t, int>> textures;
    void Add(const Taint& o, int step = 0) {
        for (const auto& [t, c] : o.textures) {
            if (textures.size() >= 12 && !textures.count(t)) continue;
            auto [it, fresh] = textures.try_emplace(t, std::uint8_t(0), 1 << 20);
            const int hops = c.second + step;
            if (hops < it->second.second) it->second = {c.first, hops};
            else if (hops == it->second.second) it->second.first |= c.first;
        }
    }
};

bool NoDest(Op op) { return op == Op::If || op == Op::Breakc || op == Op::Retc || op == Op::Continuec || op == Op::Discard; }

// The lanes of a source operand an instruction reads (its swizzle over the destination's lanes; a dot product's
// first 2 - 4).
std::uint8_t LanesRead(const Ins& ins, const Arg& a) {
    const int dot = ins.op == Op::Dp2 ? 2 : ins.op == Op::Dp3 ? 3 : ins.op == Op::Dp4 ? 4 : 0;
    const std::uint8_t mask = !NoDest(ins.op) && !ins.a.empty() ? ins.a[0].mask : 1;
    std::uint8_t lanes = 0;
    if (dot)
        for (int l = 0; l < dot; ++l) lanes |= std::uint8_t(1u << a.swz[l]);
    else
        for (int l = 0; l < 4; ++l)
            if (mask & (1u << l)) lanes |= std::uint8_t(1u << a.swz[l]);
    return lanes;
}
// The lanes of temp reg (those in lanes) an instruction reads.
bool ReadsLanes(const Ins& ins, int reg, std::uint8_t lanes) {
    for (std::size_t k = NoDest(ins.op) ? 0 : 1; k < ins.a.size(); ++k)
        if (ins.a[k].kind == 'r' && ins.a[k].reg == reg && (LanesRead(ins, ins.a[k]) & lanes)) return true;
    return false;
}

// One of a multiply's two factors (not skipArg) was last written by a log: x^k is exp(k log x).
bool TimesLog(const Program& p, std::size_t at, int skipArg) {
    const Ins& ins = p.code[at];
    if (ins.op != Op::Mul && ins.op != Op::Mad) return false;
    for (int q = 1; q <= 2 && q < int(ins.a.size()); ++q) {
        if (q == skipArg || ins.a[std::size_t(q)].kind != 'r') continue;
        const int reg = ins.a[std::size_t(q)].reg;
        std::uint8_t lanes = LanesRead(ins, ins.a[std::size_t(q)]);
        for (int j = int(at) - 1; j >= 0 && j >= int(at) - 16 && lanes; --j) {
            const Ins& w = p.code[std::size_t(j)];
            if (NoDest(w.op) || w.a.empty() || w.a[0].kind != 'r' || w.a[0].reg != reg || !(w.a[0].mask & lanes)) continue;
            if (w.op == Op::Log) return true;
            lanes = std::uint8_t(lanes & ~w.a[0].mask);
        }
    }
    return false;
}

std::vector<ConstUse> MeasureConstants(const Program& p, const std::vector<float>& constants) {
    const int n = int(constants.size());
    std::vector<ConstUse> use(std::size_t(std::max(n, 0)));
    if (!p.ok || n == 0) return use;
    bool dynamic = false;
    std::vector<std::array<Taint, 4>> taint(std::size_t(p.temps) + 1);
    auto taintOf = [&](const Ins& ins, const Arg& a) {
        Taint t;
        if (a.kind != 'r' || a.reg < 0 || a.reg >= int(taint.size())) return t;
        const std::uint8_t lanes = LanesRead(ins, a);
        for (int l = 0; l < 4; ++l)
            if (lanes & (1u << l)) t.Add(taint[std::size_t(a.reg)][std::size_t(l)]);
        return t;
    };
    for (std::size_t i = 0; i < p.code.size(); ++i) {
        const Ins& ins = p.code[i];
        const bool noDest = NoDest(ins.op);
        // what the constants here are combined with: the other operands' sources
        for (std::size_t k = noDest ? 0 : 1; k < ins.a.size(); ++k) {
            const Arg& a = ins.a[k];
            if (a.kind != 'c' || a.reg != p.materialBuffer || a.relReg >= 0) continue;
            Taint partner;
            for (std::size_t q = noDest ? 0 : 1; q < ins.a.size(); ++q)
                if (q != k) partner.Add(taintOf(ins, ins.a[q]));
            const std::uint8_t comps = LanesRead(ins, a);
            for (int c = 0; c < 4; ++c)
                if (comps & (1u << c)) {
                    const long long at = (long long)a.index * 4 + c;
                    if (at < 0 || at >= n) continue;
                    for (const auto& [t, ch] : partner.textures) {
                        auto [it, fresh] = use[std::size_t(at)].with.try_emplace(t, std::uint8_t(0), 1 << 20);
                        if (ch.second < it->second.second) it->second = ch;
                        else if (ch.second == it->second.second) it->second.first |= ch.first;
                    }
                }
        }
        // the destination's sources: a texture read's channels, else its operands' (lane by lane)
        if (!noDest && !ins.a.empty() && ins.a[0].kind == 'r' && ins.a[0].reg >= 0 && ins.a[0].reg < int(taint.size())) {
            std::array<Taint, 4> next = taint[std::size_t(ins.a[0].reg)];
            const bool texture = ins.op == Op::Sample || ins.op == Op::Ld;
            const int dot = ins.op == Op::Dp2 || ins.op == Op::Dp3 || ins.op == Op::Dp4;
            Taint all;
            if (!texture && dot)
                for (std::size_t q = 1; q < ins.a.size(); ++q) all.Add(taintOf(ins, ins.a[q]), 1);
            for (int l = 0; l < 4; ++l) {
                if (!(ins.a[0].mask & (1u << l))) continue;
                Taint t;
                if (texture) {
                    const Arg& tex = ins.a.size() > 2 ? ins.a[2] : Arg{};
                    if (tex.kind == 't') t.textures[tex.reg] = {std::uint8_t(1u << tex.swz[l]), 0};
                } else if (dot) {
                    t = all;
                } else {
                    for (std::size_t q = 1; q < ins.a.size(); ++q) {
                        const Arg& a = ins.a[q];
                        if (a.kind == 'r' && a.reg >= 0 && a.reg < int(taint.size())) t.Add(taint[std::size_t(a.reg)][a.swz[l]], 1);
                    }
                }
                next[std::size_t(l)] = std::move(t);
            }
            taint[std::size_t(ins.a[0].reg)] = std::move(next);
        }
        const std::uint8_t mask = !noDest && !ins.a.empty() ? ins.a[0].mask : 1;
        const int dotLanes = ins.op == Op::Dp2 ? 2 : ins.op == Op::Dp3 ? 3 : ins.op == Op::Dp4 ? 4 : 0;
        for (std::size_t k = noDest ? 0 : 1; k < ins.a.size(); ++k) {
            const Arg& a = ins.a[k];
            if (a.kind != 'c' || a.reg != p.materialBuffer) continue;
            if (a.relReg >= 0) {
                dynamic = true;
                continue;
            }
            std::uint8_t comps = 0;
            if (dotLanes)
                for (int l = 0; l < dotLanes; ++l) comps |= std::uint8_t(1u << a.swz[l]);
            else
                for (int l = 0; l < 4; ++l)
                    if (mask & (1u << l)) comps |= std::uint8_t(1u << a.swz[l]);
            // a power: it multiplies a log (before an exp), or what it makes does, a few instructions on
            bool power = TimesLog(p, i, int(k));
            if (!power && !noDest && !ins.a.empty() && ins.a[0].kind == 'r') {
                // (followed lane by lane until written over)
                const int dest = ins.a[0].reg;
                std::uint8_t lanes = ins.a[0].mask;
                for (std::size_t j = i + 1; j < p.code.size() && j < i + 600 && lanes; ++j) {
                    const Ins& w = p.code[j];
                    const bool reads = ReadsLanes(w, dest, lanes);
                    if (reads && TimesLog(p, j, -1)) {
                        power = true;
                        break;
                    }
                    if (!reads && !NoDest(w.op) && !w.a.empty() && w.a[0].kind == 'r' && w.a[0].reg == dest)
                        lanes = std::uint8_t(lanes & ~w.a[0].mask);
                }
            }
            const bool compare = ins.op == Op::Lt || ins.op == Op::Ge || ins.op == Op::Eq || ins.op == Op::Ne || ins.feedsDiscard;
            for (int c = 0; c < 4; ++c)
                if (comps & (1u << c)) {
                    const long long at = (long long)a.index * 4 + c;
                    if (at < 0 || at >= n) continue;
                    auto& u = use[std::size_t(at)];
                    u.read = true;
                    u.power = u.power || power;
                    u.threshold = u.threshold || compare;
                }
        }
    }
    if (dynamic)
        for (auto& u : use) u.read = true;
    std::vector<int> read;
    for (int i = 0; i < n; ++i)
        if (use[std::size_t(i)].read) read.push_back(i);
    if (read.empty()) return use;

    auto bitsOf = [](const Outputs& base, const Outputs& changed) {
        std::uint8_t bits = 0;
        for (int k = 0; k < 9; ++k) {
            if (k >= 4 && k < 8) continue;
            const float a = base.v[k], b = changed.v[k];
            if (!std::isfinite(a) || !std::isfinite(b)) continue;
            if (std::fabs(a - b) > 1e-6f * std::max(1.0f, std::fabs(a))) bits |= std::uint8_t(k < 3 ? 1 : k == 3 ? 2 : 8);
        }
        for (int l = 0; l < 4; ++l)
            if (base.stored[l] != changed.stored[l]) bits |= 16;
        return bits;
    };
    auto nudged = [](const std::vector<float>& values, int i) {
        std::vector<float> out = values;
        const float v = out[std::size_t(i)];
        out[std::size_t(i)] = v + std::max(0.05f, 0.1f * std::fabs(v));
        return out;
    };
    struct Lane {
        int bit;
        bool effects;
        int lane;
    };
    std::vector<Lane> lanes;
    for (int l = 0; l < 4; ++l) {
        if (p.effectsReg >= 0 && (p.effectsMask & (1u << l))) lanes.push_back({l, true, l});
        if (p.colorReg >= 0 && (p.colorMask & (1u << l))) lanes.push_back({4 + l, false, l});
    }
    // with the material's values: three scenes, each with the engine's loops (lights) off and on, the vertex effects
    // partly and fully on; with the effects on, one lane at 0 at a time shows what only acts with it
    std::vector<std::uint8_t> gateAll(std::size_t(n), 0xFF), seen(std::size_t(n), 0);
    for (int scene = 0; scene < 3; ++scene)
        for (const bool loops : {false, true})
            for (const float level : {0.3f, 1.0f}) {
                Setup s;
                s.seed = 1 + std::uint32_t(scene) * 101;
                s.loops = loops;
                s.constants = &constants;
                for (float& e : s.effects) e = level;
                const Outputs base = Run(p, s);
                std::vector<std::uint8_t> on(std::size_t(n), 0);
                for (const int i : read) {
                    const std::vector<float> changed = nudged(constants, i);
                    Setup q = s;
                    q.constants = &changed;
                    on[std::size_t(i)] = bitsOf(base, Run(p, q));
                    use[std::size_t(i)].reach |= on[std::size_t(i)];
                }
                if (level != 1.0f) continue;
                std::vector<std::uint8_t> here(std::size_t(n), 0);
                for (const Lane& lane : lanes) {
                    Setup off = s;
                    (lane.effects ? off.effects : off.color)[lane.lane] = 0.0f;
                    const Outputs baseOff = Run(p, off);
                    for (const int i : read) {
                        if (!on[std::size_t(i)]) continue;
                        const std::vector<float> changed = nudged(constants, i);
                        Setup q = off;
                        q.constants = &changed;
                        if (!bitsOf(baseOff, Run(p, q))) here[std::size_t(i)] |= std::uint8_t(1u << lane.bit);
                    }
                }
                for (const int i : read)
                    if (on[std::size_t(i)]) {
                        gateAll[std::size_t(i)] &= here[std::size_t(i)];
                        seen[std::size_t(i)] = 1;
                    }
            }
    for (const int i : read) use[std::size_t(i)].gates = seen[std::size_t(i)] ? gateAll[std::size_t(i)] : 0;
    // with made-up values (what it could do: a value the material's own leave without effect)
    for (int scene = 0; scene < 2; ++scene) {
        Setup s;
        s.seed = 7 + std::uint32_t(scene) * 37;
        s.loops = scene == 1;
        std::vector<float> made(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) made[std::size_t(i)] = 0.1f + 0.9f * Hash(s.seed * 7 + 3, std::uint32_t(i / 4), std::uint32_t(i % 4));
        s.constants = &made;
        const Outputs base = Run(p, s);
        for (const int i : read) {
            if (use[std::size_t(i)].reach) continue;
            const std::vector<float> changed = nudged(made, i);
            Setup q = s;
            q.constants = &changed;
            use[std::size_t(i)].reachAny |= bitsOf(base, Run(p, q));
        }
    }
    for (auto& u : use) u.reachAny |= u.reach;
    return use;
}

// A measured value's name: [its layer: ] what it reaches[: its kind].
std::string MeasuredName(const ConstUse& u, int pass, bool blended, const std::map<int, std::string>& textures, std::string& help) {
    if (!u.read) {
        help = "The pass' shader never reads it (a value of the material graph another shader of it uses, or unused).";
        return "(not read)";
    }
    const std::uint8_t bits = u.reach ? u.reach : u.reachAny;
    std::vector<std::string> what;
    const bool normals = pass == Materials::kPassNormals;
    const bool forward = pass == Materials::kPassForward;
    const bool colors = pass == Materials::kPassComposite || forward;
    if (normals) {
        if (bits & 1) what.push_back("normal");
        if (bits & 2) what.push_back("gloss");
    } else if (colors) {
        if (bits & 1) what.push_back(forward ? "lit color" : "color");
        if (bits & 2) what.push_back(blended ? "opacity" : "alpha");
    } else if (bits & 3) {
        what.push_back("output");
    }
    if (bits & 8) what.push_back("alpha test");
    if (bits & 16) what.push_back("strand fragments");
    std::string name;
    for (std::size_t k = 0; k < what.size(); ++k) name += (k ? (k + 1 == what.size() ? " and " : ", ") : "") + what[k];
    if (name.empty() && u.power) {
        // (a sharp highlight's power: nudging it changes next to nothing in made-up scenes, where the highlights miss)
        name = normals ? "gloss" : forward ? "lit color" : colors ? "color" : "output";
        help = "It is a power (x^this) in the shader's " + name + ": a highlight's or a falloff's sharpness (too sharp "
               "to change anything measurable in the made-up scenes)";
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        return name + ": sharpness";
    }
    if (name.empty()) {
        help = "Read by the shader, but changing it changes nothing it outputs (in every measured scene).";
        return "(no effect)";
    }
    help = "Changing it changes the " + name + " (measured: the pass' shader run with it nudged)";
    if (u.power && (normals || forward || what.size() == 1)) {
        name += ": sharpness";
        help += "; it is a power (x^this): a highlight's or a falloff's sharpness";
    } else if (u.threshold) {
        name += ": threshold";
        help += "; it is compared against (a threshold)";
    }
    std::string layer;
    for (int b = 0; b < 8; ++b)
        if (u.gates & (1u << b)) layer += (layer.empty() ? "" : " + ") + std::string(layer.empty() ? GateName(b) : Lower(GateName(b)));
    if (!layer.empty()) {
        const bool effects = (u.gates & 0x0F) != 0;
        name = layer + (effects ? " layer: " : ": ") + name;
        help += effects ? "; only where " + Lower(layer) + " shows (it does nothing where that vertex effect is 0)"
                        : "; only where the mesh is " + Lower(layer) + " (Lara's blend shapes)";
    }
    // what it is combined with (the material's textures, by name)
    std::vector<std::string> partners;
    int nearest = 1 << 20;
    for (const auto& [reg, ch] : u.with)
        if (textures.count(reg)) nearest = std::min(nearest, ch.second);
    for (const auto& [reg, ch] : u.with) {
        auto t = textures.find(reg);
        if (t == textures.end() || ch.second > nearest + 1) continue;   // (the nearest only)
        const std::uint8_t channels = ch.first;
        std::string letters;
        for (int c = 0; c < 4; ++c)
            if (channels & (1u << c)) letters += "RGBA"[c];
        partners.push_back(t->second + (letters.size() < 4 ? " " + letters : std::string()));
    }
    // (in the tooltip: the name stays short)
    if (!partners.empty()) {
        std::string list;
        for (std::size_t k = 0; k < partners.size(); ++k) list += (k ? ", " : "") + partners[k];
        help += ".\nCombined in the code with: " + list;
    }
    if (!u.reach) help += ".\nWith the material's own values it changes nothing (others would)";
    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

std::mutex g_propertiesMutex;
std::map<std::pair<std::uint32_t, std::vector<float>>, std::vector<ConstUse>> g_propertiesCache;
}  // namespace

std::vector<Property> Properties(const Materials::Material& material, const Materials::Set& set, const std::vector<Param>& params) {
    std::vector<Property> out;
    if (!material.found) return out;
    std::map<std::pair<int, int>, std::pair<int, int>> held;   // (pass, index) -> (param, component)
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i].kind == Kind::HairHighlight) continue;
        for (int c = 0; c < params[i].components; ++c)
            for (const Location& l : params[i].where[c]) {
                auto it = held.find({l.pass, l.index});
                // (a named value wins over a texture's analysis of the same float)
                if (it == held.end() || (params[i].kind == Kind::Named && params[std::size_t(it->second.first)].kind != Kind::Named))
                    held[{l.pass, l.index}] = {int(i), c};
            }
    }
    std::set<int> listed;   // params already in the list
    for (const int p : {Materials::kPassForward, Materials::kPassComposite, Materials::kPassNormals, Materials::kPassAlphaDepth,
                        Materials::kPassDepth, 0, 4, 5, 6, 9}) {
        const Materials::Pass& pass = material.passes[p];
        if (!pass.present || pass.constants.empty()) continue;
        std::vector<ConstUse> uses;
        std::map<int, std::string> textureNames;   // the material's own textures, by register
        for (const auto& slot : pass.textures)
            if (!EngineSlot(slot) && slot.texture) {
                auto named = set.textureNames.find(slot.texture);
                textureNames[int(slot.reg)] = named != set.textureNames.end() ? named->second : "texture " + std::to_string(slot.texture);
            }
        auto shader = pass.ps ? set.shaders.find(pass.ps) : set.shaders.end();
        if (shader != set.shaders.end() && shader->second.found) {
            const Program& program = CachedProgram(pass.ps, shader->second.code);
            const auto key = std::make_pair(pass.ps, pass.constants);
            {
                std::lock_guard<std::mutex> lock(g_propertiesMutex);
                if (auto it = g_propertiesCache.find(key); it != g_propertiesCache.end()) uses = it->second;
            }
            if (uses.empty()) {
                uses = MeasureConstants(program, pass.constants);
                std::lock_guard<std::mutex> lock(g_propertiesMutex);
                if (g_propertiesCache.size() > 256) g_propertiesCache.clear();
                g_propertiesCache[key] = uses;
            }
        }
        for (int i = 0; i < int(pass.constants.size()); ++i) {
            Property prop;
            prop.pass = p;
            prop.index = i;
            if (auto it = held.find({p, i}); it != held.end()) {
                if (listed.count(it->second.first)) continue;   // (mirrored: listed where it is first)
                listed.insert(it->second.first);
                prop.param = it->second.first;
                prop.component = it->second.second;
                out.push_back(std::move(prop));
                continue;
            }
            if (!uses.empty()) {
                const ConstUse& u = uses[std::size_t(i)];
                prop.read = u.read;
                prop.effect = u.reach != 0;
                prop.name = MeasuredName(u, p, material.Blended(), textureNames, prop.help);
            } else {
                prop.name = pass.ps ? "(its shader was not read)" : "(no pixel shader)";
                prop.read = false;
                prop.effect = false;
                prop.help = pass.ps ? "The pass' pixel shader could not be read: what this value does is unknown."
                                    : "This pass has no pixel shader (its values go to the vertex shader or unused).";
            }
            out.push_back(std::move(prop));
        }
    }
    return out;
}

float Value(const Materials::Material& material, const Param& param, int component) {
    if (component < 0 || component >= 4 || param.where[component].empty()) return 0.0f;
    const Location& l = param.where[component].front();
    const auto& constants = material.passes[l.pass].constants;
    return l.index < int(constants.size()) ? constants[std::size_t(l.index)] : 0.0f;
}

void SetValue(Materials::Material& material, const Param& param, int component, float value) {
    if (component < 0 || component >= 4) return;
    for (const Location& l : param.where[component]) {
        auto& constants = material.passes[l.pass].constants;
        if (l.index < int(constants.size())) constants[std::size_t(l.index)] = value;
    }
    material.edited = true;
}

bool Mixed(const Materials::Material& material, const Param& param, int component) {
    if (component < 0 || component >= 4) return false;
    const float first = Value(material, param, component);
    for (const Location& l : param.where[component]) {
        const auto& constants = material.passes[l.pass].constants;
        if (l.index < int(constants.size()) && std::fabs(constants[std::size_t(l.index)] - first) > 1e-6f) return true;
    }
    return false;
}
}  // namespace MaterialParams
