// (v0.9.552) The ray tracing probe's leaf cutouts: see rt_cutout.h.
// DXBC shader model 4/5 tokens (d3d11TokenizedProgramFormat.hpp): an instruction's opcode token holds its opcode (bits
// 0..10), its controls (11..23; the test of discard / if at bit 18: nonzero) and its length in dwords (24..30); bit 31:
// extended opcode tokens follow. An operand token: its components (bits 0..1: none, one, four), their selection
// (2..3: mask, swizzle, one) and that selection (4..11), its type (12..19), its index dimension (20..21) and each index's
// representation (22..30: immediate 32 / 64, relative, immediate + relative); bit 31: an extended operand token (its
// modifier: negate, abs) follows; then the indices, then an immediate's values.
#include "rt_cutout.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace RtCutout {
namespace {

enum : unsigned {
    kAdd = 0, kBreak = 2, kBreakc = 3, kCall = 4, kCallc = 5, kCase = 6, kContinue = 7, kContinuec = 8, kCut = 9,
    kDefault = 10, kDiscard = 13, kDp2 = 15, kDp3 = 16, kDp4 = 17, kElse = 18, kEmit = 19, kEmitThenCut = 20,
    kEndif = 21, kEndloop = 22, kEndswitch = 23, kGe = 29, kIf = 31, kImul = 38, kLabel = 44, kLoop = 48, kLt = 49,
    kMad = 50, kMin = 51, kCustomData = 53, kMov = 54, kMul = 56, kNop = 58, kRet = 62, kRetc = 63, kSample = 69, kSampleB = 74,
    kSwitch = 76, kSincos = 77, kUdiv = 78, kUmul = 81, kUaddc = 132, kUsubb = 133, kSwapc = 142,
};
enum : unsigned { kTemp = 0, kInput = 1, kOutput = 2, kImm32 = 4, kImm64 = 5, kResource = 7, kCb = 8 };

struct Operand {
    unsigned type = 0xFF;
    unsigned mask = 0xF;              // a destination's components
    unsigned sel[4] = {0, 1, 2, 3};   // a source's component for each destination component
    unsigned index[3] = {};
    unsigned dims = 0;
    bool relative = false, negate = false;
    float imm[4] = {};
};

struct Instruction {
    unsigned opcode = 0;
    bool nonzero = false;   // discard / if: the test
    Operand ops[6];
    unsigned count = 0;
};

struct Element {
    char name[32] = {};
    unsigned index = 0, reg = 0, mask = 0;
};

struct Shader {
    std::vector<Instruction> code;   // the instructions after the declarations' (declarations left out)
    std::vector<Element> inputs, outputs;
};

bool ReadOperand(const std::uint32_t*& p, const std::uint32_t* end, Operand& o, int depth = 0) {
    if (p >= end || depth > 3) return false;
    const std::uint32_t t = *p++;
    o = Operand{};
    const unsigned n = t & 3;
    if (n == 2) {
        const unsigned mode = (t >> 2) & 3;
        if (mode == 0) {
            o.mask = (t >> 4) & 0xF;
        } else if (mode == 1) {
            for (unsigned i = 0; i < 4; ++i) o.sel[i] = (t >> (4 + 2 * i)) & 3;
        } else {
            for (unsigned i = 0; i < 4; ++i) o.sel[i] = (t >> 4) & 3;
        }
    } else if (n == 1) {
        o.mask = 1;
        for (unsigned i = 0; i < 4; ++i) o.sel[i] = 0;
    }
    o.type = (t >> 12) & 0xFF;
    o.dims = (t >> 20) & 3;
    bool extended = (t >> 31) != 0;
    while (extended) {
        if (p >= end) return false;
        const std::uint32_t e = *p++;
        if ((e & 0x3F) == 1) {
            const unsigned m = (e >> 6) & 0xFF;
            o.negate = m == 1 || m == 3;
        }
        extended = (e >> 31) != 0;
    }
    for (unsigned d = 0; d < o.dims; ++d) {
        const unsigned rep = (t >> (22 + 3 * d)) & 7;
        if (rep > 4) return false;
        if (rep == 0 || rep == 3) {
            if (p >= end) return false;
            o.index[d] = *p++;
        } else if (rep == 1 || rep == 4) {
            if (p + 2 > end) return false;
            o.index[d] = p[1];   // (the low dword second)
            p += 2;
        }
        if (rep >= 2) {
            Operand r;
            if (!ReadOperand(p, end, r, depth + 1)) return false;
            o.relative = true;
        }
    }
    if (o.type == kImm32) {
        const unsigned k = n == 1 ? 1 : 4;
        if (p + k > end) return false;
        for (unsigned i = 0; i < k; ++i) std::memcpy(&o.imm[i], p + i, 4);
        if (k == 1) o.imm[1] = o.imm[2] = o.imm[3] = o.imm[0];
        p += k;
    } else if (o.type == kImm64) {
        const unsigned k = n == 1 ? 2 : 8;
        if (p + k > end) return false;
        p += k;
    }
    return true;
}

bool IsDeclaration(unsigned op) {
    return (op >= 88 && op <= 106) || (op >= 143 && op <= 162) || op == kCustomData || op == 206 ||
           (op >= 113 && op <= 116);   // (113..116: the hull shader's phase marks)
}

// a chunk's data and length (0: none)
const std::uint8_t* FindChunk(const std::uint8_t* b, std::size_t size, const char* tag, std::uint32_t& length) {
    length = 0;
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) return nullptr;
    std::uint32_t chunks = 0;
    std::memcpy(&chunks, b + 28, 4);
    for (std::uint32_t i = 0; i < chunks && 32 + 4 * (i + 1) <= size; ++i) {
        std::uint32_t at = 0;
        std::memcpy(&at, b + 32 + 4 * i, 4);
        if (static_cast<std::size_t>(at) + 8 > size || std::memcmp(b + at, tag, 4) != 0) continue;
        std::memcpy(&length, b + at + 4, 4);
        if (static_cast<std::size_t>(at) + 8 + length > size) return nullptr;
        return b + at + 8;
    }
    return nullptr;
}

// ISGN / OSGN (24 bytes an element), OSG5 (28: the stream first), ISG1 / OSG1 (32: the stream first, the precision last)
void ReadSignature(const std::uint8_t* b, std::size_t size, bool output, std::vector<Element>& out) {
    out.clear();
    const char* const tags[3] = {output ? "OSGN" : "ISGN", output ? "OSG5" : "ISG1", output ? "OSG1" : "ISG1"};
    const unsigned strides[3] = {24, 28, 32};
    const unsigned skips[3] = {0, 4, 4};
    for (int k = 0; k < 3; ++k) {
        std::uint32_t length = 0;
        const std::uint8_t* c = FindChunk(b, size, tags[k], length);
        if (!c || length < 8) continue;
        std::uint32_t count = 0;
        std::memcpy(&count, c, 4);
        for (std::uint32_t e = 0; e < count && 8 + strides[k] * (e + 1) <= length; ++e) {
            const std::uint8_t* element = c + 8 + strides[k] * e + skips[k];
            std::uint32_t nameAt = 0;
            Element x;
            std::memcpy(&nameAt, element, 4);
            std::memcpy(&x.index, element + 4, 4);
            std::memcpy(&x.reg, element + 16, 4);
            x.mask = element[20] & 0xF;
            if (nameAt >= length) continue;
            const char* name = reinterpret_cast<const char*>(c + nameAt);
            const void* zero = std::memchr(name, 0, length - nameAt);
            if (!zero) continue;
            std::snprintf(x.name, sizeof(x.name), "%s", name);
            out.push_back(x);
        }
        return;
    }
}

bool ReadShader(const void* bytes, std::size_t size, Shader& s) {
    const auto* b = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t length = 0;
    const std::uint8_t* c = FindChunk(b, size, "SHEX", length);
    if (!c) c = FindChunk(b, size, "SHDR", length);
    if (!c || length < 8) return false;
    const auto* p = reinterpret_cast<const std::uint32_t*>(c);
    std::uint32_t dwords = p[1];
    if (dwords * 4ull > length) dwords = length / 4;
    const std::uint32_t* end = p + dwords;
    p += 2;
    s.code.clear();
    s.code.reserve(dwords / 4);
    while (p < end) {
        const std::uint32_t t = *p;
        const unsigned op = t & 0x7FF;
        unsigned len = op == kCustomData ? (p + 1 < end ? p[1] : 0) : (t >> 24) & 0x7F;
        if (!len || p + len > end) break;
        if (!IsDeclaration(op)) {
            Instruction ins;
            ins.opcode = op;
            ins.nonzero = ((t >> 18) & 1) != 0;
            const std::uint32_t* q = p + 1;
            const std::uint32_t* stop = p + len;
            bool extended = (t >> 31) != 0;
            while (extended && q < stop) extended = (*q++ >> 31) != 0;
            while (q < stop && ins.count < 6) {
                if (!ReadOperand(q, stop, ins.ops[ins.count])) break;
                ++ins.count;
            }
            s.code.push_back(ins);
        }
        p += len;
    }
    ReadSignature(b, size, false, s.inputs);
    ReadSignature(b, size, true, s.outputs);
    return true;
}

bool NoDestination(unsigned op) {
    switch (op) {
        case kBreak: case kBreakc: case kCall: case kCallc: case kCase: case kContinue: case kContinuec: case kCut:
        case kDefault: case kDiscard: case kElse: case kEmit: case kEmitThenCut: case kEndif: case kEndloop:
        case kEndswitch: case kIf: case kLabel: case kLoop: case kNop: case kRet: case kRetc: case kSwitch:
            return true;
        default:
            return false;
    }
}
bool TwoDestinations(unsigned op) {
    return op == kImul || op == kSincos || op == kUdiv || op == kUmul || op == kUaddc || op == kUsubb || op == kSwapc;
}
bool IsSample(unsigned op) { return op >= kSample && op <= kSampleB; }

// the last instruction before `before` writing component c of register (type, reg); -1: none
int Writer(const Shader& s, int before, unsigned type, unsigned reg, unsigned c) {
    for (int i = before - 1; i >= 0; --i) {
        const Instruction& ins = s.code[i];
        if (!ins.count || NoDestination(ins.opcode)) continue;
        for (unsigned k = 0; k < (TwoDestinations(ins.opcode) ? 2u : 1u) && k < ins.count; ++k) {
            const Operand& d = ins.ops[k];
            if (d.type == type && d.dims >= 1 && d.index[0] == reg && ((d.mask >> c) & 1)) return i;
        }
    }
    return -1;
}

void Fail(Program& out, const char* why) {
    out.result = kUnfollowed;
    std::snprintf(out.why, sizeof(out.why), "%s", why);
}

// A constant operand's value for destination component c: a constant buffer's float (no relative index) or a literal
bool ConstantOf(const Operand& o, unsigned c, Constant& out) {
    if (o.type == kImm32) {
        out.slot = -1;
        out.literal = o.negate ? -o.imm[c] : o.imm[c];
        return true;
    }
    if (o.type == kCb && o.dims == 2 && !o.relative && o.index[0] < 15 && o.index[1] < 4096) {
        out.slot = static_cast<std::int8_t>(o.index[0]);
        out.at = static_cast<std::uint16_t>(o.index[1] * 4 + o.sel[c]);
        return true;
    }
    return false;
}
bool IsConstant(const Operand& o) { return o.type == kImm32 || o.type == kCb; }

const Element* ElementAt(const std::vector<Element>& signature, unsigned reg, unsigned c) {
    for (const Element& e : signature)
        if (e.reg == reg && ((e.mask >> c) & 1)) return &e;
    return nullptr;
}
unsigned FirstComponent(unsigned mask) {
    for (unsigned i = 0; i < 4; ++i)
        if ((mask >> i) & 1) return i;
    return 0;
}

// One UV component's step: `ins` (writing component c) as input x scale + offset, its input register and component.
// Accepted: mov input; mul input, k (either order); add input, k; mad input, k, k (input either of the first two).
bool LinearStep(const Instruction& ins, unsigned c, unsigned inputType, unsigned& reg, unsigned& component, Constant& scale,
                Constant& offset) {
    scale = {-1, 0, 1.0f};
    offset = {-1, 0, 0.0f};
    const unsigned op = ins.opcode;
    if (ins.count < 2) return false;
    const Operand* input = nullptr;
    if (op == kMov) {
        input = &ins.ops[1];
    } else if (op == kMul && ins.count >= 3) {
        const int i = ins.ops[1].type == inputType ? 1 : ins.ops[2].type == inputType ? 2 : 0;
        if (!i || !ConstantOf(ins.ops[3 - i], c, scale)) return false;
        input = &ins.ops[i];
    } else if (op == kAdd && ins.count >= 3) {
        const int i = ins.ops[1].type == inputType ? 1 : ins.ops[2].type == inputType ? 2 : 0;
        if (!i || !ConstantOf(ins.ops[3 - i], c, offset)) return false;
        input = &ins.ops[i];
    } else if (op == kMad && ins.count >= 4) {
        const int i = ins.ops[1].type == inputType ? 1 : ins.ops[2].type == inputType ? 2 : 0;
        if (!i || !ConstantOf(ins.ops[3 - i], c, scale) || !ConstantOf(ins.ops[3], c, offset)) return false;
        input = &ins.ops[i];
    } else {
        return false;
    }
    if (input->type != inputType || input->dims < 1 || input->relative || input->negate) return false;
    reg = input->index[0];
    component = input->sel[c];
    return true;
}

// The pixel shader's sample address component j (of a sample's address operand) back to an input register
bool PixelUv(const Shader& s, int sampleAt, unsigned j, unsigned& reg, unsigned& component, Constant& tiling,
             Constant& offset) {
    const Operand& a = s.code[sampleAt].ops[1];
    const unsigned c = a.sel[j];
    tiling = {-1, 0, 1.0f};
    offset = {-1, 0, 0.0f};
    if (a.type == kInput && a.dims >= 1 && !a.relative) {
        reg = a.index[0];
        component = c;
        return true;
    }
    if (a.type != kTemp || a.dims < 1) return false;
    const int w = Writer(s, sampleAt, kTemp, a.index[0], c);
    if (w < 0) return false;
    return LinearStep(s.code[w], c, kInput, reg, component, tiling, offset);
}

struct Pending {
    int before;
    unsigned reg, c;
};

// The sample instructions a temp's component (read before instruction `before`) comes from, and which component of
// each one's result
void SliceSamples(const Shader& s, int before, unsigned reg, unsigned c, std::vector<std::pair<int, unsigned>>& found) {
    std::vector<Pending> work{{before, reg, c}};
    std::vector<std::uint64_t> seen;
    unsigned budget = 512;
    while (!work.empty() && budget--) {
        const Pending w = work.back();
        work.pop_back();
        const int at = Writer(s, w.before, kTemp, w.reg, w.c);
        if (at < 0) continue;
        const std::uint64_t id = (static_cast<std::uint64_t>(at) << 8) | (w.reg << 2) | w.c;
        bool already = false;
        for (std::uint64_t v : seen) already |= v == id;
        if (already) continue;
        seen.push_back(id);
        const Instruction& ins = s.code[at];
        if (IsSample(ins.opcode)) {
            found.emplace_back(at, w.c);
            continue;
        }
        const unsigned first = TwoDestinations(ins.opcode) ? 2 : 1;
        const unsigned dot = ins.opcode == kDp2 ? 2 : ins.opcode == kDp3 ? 3 : ins.opcode == kDp4 ? 4 : 0;
        for (unsigned k = first; k < ins.count; ++k) {
            const Operand& o = ins.ops[k];
            if (o.type != kTemp || o.dims < 1) continue;
            if (dot) {
                for (unsigned i = 0; i < dot; ++i) work.push_back({at, o.index[0], o.sel[i]});
            } else {
                work.push_back({at, o.index[0], o.sel[w.c]});
            }
        }
    }
}

// The value instruction `at` writes (component c) as alpha (x k) - threshold: add alpha, -threshold; mad alpha, k,
// -threshold (alpha: the temps among the first two); min of such and other tests (each one an option: a test of the
// screen position or the instance's fade beside the alpha's)
struct Option {
    Constant threshold;
    std::vector<Pending> alpha;
};
void AlphaMinusThreshold(const Shader& s, int at, unsigned c, std::vector<Option>& options, int depth = 0) {
    const Instruction& e = s.code[at];
    Option o;
    if (e.opcode == kAdd && e.count >= 3) {
        const int k = IsConstant(e.ops[1]) ? 1 : IsConstant(e.ops[2]) ? 2 : 0;
        if (!k || e.ops[3 - k].type != kTemp || !ConstantOf(e.ops[k], c, o.threshold)) return;
        if (o.threshold.slot < 0) o.threshold.literal = -o.threshold.literal;
        else if (!e.ops[k].negate) return;
        o.alpha.push_back({at, e.ops[3 - k].index[0], e.ops[3 - k].sel[c]});
        options.push_back(o);
    } else if (e.opcode == kMad && e.count >= 4) {
        if (!ConstantOf(e.ops[3], c, o.threshold)) return;
        if (o.threshold.slot < 0) o.threshold.literal = -o.threshold.literal;
        else if (!e.ops[3].negate) return;
        for (int k = 1; k <= 2; ++k)
            if (e.ops[k].type == kTemp) o.alpha.push_back({at, e.ops[k].index[0], e.ops[k].sel[c]});
        if (!o.alpha.empty()) options.push_back(o);
    } else if (e.opcode == kMin && e.count >= 3 && depth < 4) {
        for (int k = 1; k <= 2; ++k) {
            if (e.ops[k].type != kTemp) continue;
            const unsigned kc = e.ops[k].sel[c];
            const int w = Writer(s, at, kTemp, e.ops[k].index[0], kc);
            if (w >= 0) AlphaMinusThreshold(s, w, kc, options, depth + 1);
        }
    }
}

}  // namespace

void AnalyzePixel(const void* ps, std::size_t psSize, Program& out, char (&semantic)[32], unsigned& semanticIndex,
                  unsigned (&components)[2]) {
    out = Program{};
    semantic[0] = 0;
    semanticIndex = 0;
    Shader s;
    if (!ps || !ReadShader(ps, psSize, s)) return;
    int discard = -1;
    for (int i = 0; i < static_cast<int>(s.code.size()) && discard < 0; ++i)
        if (s.code[i].opcode == kDiscard) discard = i;
    if (discard < 0) return;
    // the test: discard if a < b (lt, nonzero; ge, zero) or if a >= b (ge, nonzero; lt, zero)
    const Instruction& d = s.code[discard];
    if (!d.count || d.ops[0].type != kTemp) return Fail(out, "discard of a non-temp");
    const unsigned cc = d.ops[0].sel[0];
    const int test = Writer(s, discard, kTemp, d.ops[0].index[0], cc);
    if (test < 0) return Fail(out, "discard's test not found");
    const Instruction& t = s.code[test];
    if ((t.opcode != kLt && t.opcode != kGe) || t.count < 3) return Fail(out, "discard's test not lt / ge");
    const bool less = (t.opcode == kLt) == d.nonzero;   // discard if a < b (else if a >= b)
    const Operand& a = t.ops[1];
    const Operand& b = t.ops[2];
    // the options: the alpha (temps, their components, multiplied) and the threshold (a constant)
    std::vector<Option> options;
    if (less && a.type == kTemp && b.type == kImm32 && b.imm[cc] == 0.0f) {   // a = alpha - threshold
        const int at = Writer(s, test, kTemp, a.index[0], a.sel[cc]);
        if (at >= 0) AlphaMinusThreshold(s, at, a.sel[cc], options);
        if (options.empty()) return Fail(out, "alpha - threshold not followed");
    } else if (less && a.type == kTemp && IsConstant(b)) {
        Option o;
        ConstantOf(b, cc, o.threshold);
        o.alpha.push_back({test, a.index[0], a.sel[cc]});
        options.push_back(o);
    } else if (!less && b.type == kTemp && IsConstant(a)) {   // discard if threshold >= alpha
        Option o;
        ConstantOf(a, cc, o.threshold);
        o.alpha.push_back({test, b.index[0], b.sel[cc]});
        options.push_back(o);
    } else {
        return Fail(out, "discard's test not followed");
    }
    // the samples each option's alpha comes from; the first at an interpolated TEXCOORD (others: a depth fade, a
    // screen mask)
    bool anySample = false;
    for (const Option& option : options) {
        std::vector<std::pair<int, unsigned>> samples;
        for (const Pending& p : option.alpha) SliceSamples(s, p.before, p.reg, p.c, samples);
        anySample |= !samples.empty();
        out.threshold = option.threshold;
        for (const auto& sample : samples) {
            const Instruction& ins = s.code[sample.first];
            if (ins.count < 3 || ins.ops[2].type != kResource || ins.ops[2].relative) continue;
            unsigned reg[2], comp[2];
            Constant tiling[2], offset[2];
            if (!PixelUv(s, sample.first, 0, reg[0], comp[0], tiling[0], offset[0]) ||
                !PixelUv(s, sample.first, 1, reg[1], comp[1], tiling[1], offset[1]) || reg[0] != reg[1])
                continue;
            const Element* e0 = ElementAt(s.inputs, reg[0], comp[0]);
            const Element* e1 = ElementAt(s.inputs, reg[1], comp[1]);
            if (!e0 || e0 != e1 || _stricmp(e0->name, "TEXCOORD") != 0) continue;
            out.texture = static_cast<std::uint8_t>(ins.ops[2].index[0]);
            out.channel = static_cast<std::uint8_t>(ins.ops[2].sel[sample.second]);
            for (int j = 0; j < 2; ++j) {
                out.psTiling[j] = tiling[j];
                out.psOffset[j] = offset[j];
            }
            std::snprintf(semantic, sizeof(semantic), "%s", e0->name);
            semanticIndex = e0->index;
            const unsigned first = FirstComponent(e0->mask);
            components[0] = comp[0] - first;
            components[1] = comp[1] - first;
            out.result = kFollowed;
            return;
        }
    }
    out.threshold = Constant{-1, 0, 0.5f};
    Fail(out, anySample ? "no texture at a TEXCOORD" : "no texture in the alpha");
}

bool AnalyzeVertex(const void* vs, std::size_t vsSize, const char* semantic, unsigned semanticIndex,
                   const unsigned (&components)[2], Program& out) {
    Shader s;
    if (!vs || !ReadShader(vs, vsSize, s)) {
        Fail(out, "vertex shader not read");
        return false;
    }
    const Element* o = nullptr;
    for (const Element& e : s.outputs)
        if (!_stricmp(e.name, semantic) && e.index == semanticIndex) o = &e;
    if (!o) {
        Fail(out, "UV not a vertex shader output");
        return false;
    }
    const unsigned first = FirstComponent(o->mask);
    const int end = static_cast<int>(s.code.size());
    for (int j = 0; j < 2; ++j) {
        const unsigned c = first + components[j];
        int w = Writer(s, end, kOutput, o->reg, c);
        unsigned wc = c;
        // through temps by movs (at most two)
        for (int hop = 0; hop < 2 && w >= 0 && s.code[w].opcode == kMov && s.code[w].count >= 2 &&
                          s.code[w].ops[1].type == kTemp;
             ++hop) {
            const Operand& src = s.code[w].ops[1];
            const unsigned sc = src.sel[wc];
            w = Writer(s, w, kTemp, src.index[0], sc);
            wc = sc;
        }
        unsigned reg = 0, comp = 0;
        if (w < 0 || !LinearStep(s.code[w], wc, kInput, reg, comp, out.vsScale[j], out.vsOffset[j])) {
            Fail(out, "UV's vertex shader step not followed");
            return false;
        }
        const Element* in = ElementAt(s.inputs, reg, comp);
        if (!in || _stricmp(in->name, "TEXCOORD") != 0) {
            Fail(out, "UV not from a TEXCOORD input");
            return false;
        }
        if (j == 1 && in->index != out.texcoord) {
            Fail(out, "u and v from two inputs");
            return false;
        }
        out.texcoord = static_cast<std::uint8_t>(in->index);
        out.component[j] = static_cast<std::uint8_t>(comp - FirstComponent(in->mask));
    }
    return true;
}

void Analyze(const void* vs, std::size_t vsSize, const void* ps, std::size_t psSize, Program& out) {
    char semantic[32];
    unsigned semanticIndex = 0, components[2] = {0, 1};
    AnalyzePixel(ps, psSize, out, semantic, semanticIndex, components);
    if (out.result != kFollowed) return;
    if (!AnalyzeVertex(vs, vsSize, semantic, semanticIndex, components, out)) out.result = kUnfollowed;
}

}  // namespace RtCutout
