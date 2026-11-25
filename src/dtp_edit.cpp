#include "dtp_edit.h"
#include "game_build.h"
#include "log.h"
#include "memory_probe.h"
#include <Windows.h>
#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// DTPDataSection (TR2 PDB; the retail functions do the same): s_dtpDataInstances[0x113000] holds a header per loaded
// DTP id (+0 u16 m_refCount, +6 bit 0 m_isStreaming) and the data follows the 0x10-byte header. GetBasePointer (retail
// 0x343100) = table[id] + 0x10 when m_refCount != 0; GetPointer (retail 0x3435F0) = the same while not streaming.
namespace DtpEdit {
namespace {
constexpr std::uintptr_t kRvaGetBasePointer = 0x00343100ull;
constexpr std::uintptr_t kRvaTableDisp = kRvaGetBasePointer + 13;  // lea rcx, [rip + disp32]: the disp32
constexpr std::uintptr_t kRvaAfterLea = kRvaGetBasePointer + 17;
// DTPDataSection::GetSize(this unused, id): the resolve heap's size of the DTP's block - 0x10 (0: not loaded)
constexpr std::uintptr_t kRvaGetSize = 0x00343C10ull;
constexpr std::uint32_t kMaxDtpId = 0x113000;
constexpr std::size_t kHeaderSize = 0x10;
constexpr std::size_t kFlagsOffset = 0x6;
constexpr std::uint32_t kSizeSlack = 32;  // a heap block may be a little larger than the data it holds (alignment)
constexpr ULONGLONG kTickMs = 500;
constexpr int kMaxLogLines = 400;

constexpr std::uint8_t kHeadSig[] = {
    0x81, 0xFA, 0x00, 0x30, 0x11, 0x00,  // cmp edx, 113000h
    0x73, 0x1D,                          // jae -> null
    0x8B, 0xC2,                          // mov eax, edx
    0x48, 0x8D, 0x0D,                    // lea rcx, [s_dtpDataInstances]
};
constexpr std::uint8_t kTailSig[] = {
    0x48, 0x8B, 0x04, 0xC1,  // mov rax, [rcx+rax*8]
    0x48, 0x85, 0xC0,        // test rax, rax
    0x74, 0x0B,              // je -> null
    0x66, 0x83, 0x38, 0x00,  // cmp word ptr [rax], 0     (m_refCount)
    0x74, 0x05,              // je -> null
    0x48, 0x83, 0xC0, 0x10,  // add rax, 10h              (the data)
    0xC3,
};
constexpr std::uint8_t kGetSizeHead[] = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 28h
    0x81, 0xFA, 0x00, 0x30, 0x11, 0x00,  // cmp edx, 113000h
    0x73, 0x27,                          // jae -> 0
    0x8B, 0xC2,                          // mov eax, edx
    0x48, 0x8D, 0x15,                    // lea rdx, [s_dtpDataInstances]   (disp32 at +0x11)
};
constexpr std::uint8_t kGetSizeMid[] = {
    0x48, 0x8B, 0x14, 0xC2,  // mov rdx, [rdx+rax*8]
    0x48, 0x85, 0xD2,        // test rdx, rdx
    0x74, 0x15,              // je -> 0
    0x48, 0x8B, 0x0D,        // mov rcx, [cdc::s_resolveHeap]   (disp32 at +0x21: `this` is never read)
};
constexpr std::uint8_t kGetSizeTail[] = {
    0x48, 0x8B, 0x01,  // mov rax, [rcx]
    0xFF, 0x50, 0x48,  // call [rax+48h]               (the heap's block size)
    0x83, 0xE8, 0x10,  // sub eax, 10h
};
using GetSizeFn = unsigned(__fastcall*)(void*, unsigned);

struct Key {
    std::uint32_t dtp, offset;
    std::uint8_t bit, bits;
    bool operator<(const Key& o) const { return std::tie(dtp, offset, bit, bits) < std::tie(o.dtp, o.offset, o.bit, o.bits); }
};

struct State {
    Field field;
    std::map<int, std::uint64_t> values;  // owner -> value
    std::vector<int> order;                // owners, the oldest first
    std::vector<std::uint64_t> expect;     // the game's own values that may be overwritten (empty: any)
    std::uint32_t fileSize = 0;            // the DTP's body size in the files (0: unknown)
    std::uint8_t* data = nullptr;          // the DTP data the original value was read from
    std::uint64_t original = 0;
    bool haveOriginal = false;
};

bool g_available = false;
std::uintptr_t g_base = 0;
std::uint8_t* const* g_table = nullptr;
GetSizeFn g_getSize = nullptr;         // null: sizes are not checked (the signature did not match)
std::string g_status = "not started";
std::map<Key, State> g_states;
ULONGLONG g_lastTickMs = 0;
int g_logLines = 0;
std::atomic<int> g_editorCount{0}, g_buildCount{0}, g_refused{0}, g_writes{0};

Key KeyOf(const Field& f) { return Key{f.dtp, f.offset, f.bit, f.bits}; }

std::uint64_t Mask(const Field& f) {
    if (f.bits) return f.bits >= 64 ? ~0ull : ((1ull << f.bits) - 1);
    return f.size >= 8 ? ~0ull : ((1ull << (f.size * 8)) - 1);
}

std::string Describe(const Field& f) {
    char buf[96];
    if (f.bits)
        std::snprintf(buf, sizeof(buf), "DTP %u +0x%X bits %u..%u", f.dtp, f.offset, f.bit, f.bit + f.bits - 1);
    else if (f.kind == 'b')
        std::snprintf(buf, sizeof(buf), "DTP %u +0x%X bool", f.dtp, f.offset);
    else
        std::snprintf(buf, sizeof(buf), "DTP %u +0x%X %c%u", f.dtp, f.offset, f.kind, f.size * 8);
    return buf;
}

// VirtualQuery costs about a millisecond on the game's large heap regions (it walks the region's pages): with one or two
// per value, 80 Suite edits made every 500 ms tick take 95 ms (Modding Suite 2.0.25 log); with the answers kept for one
// tick, a worn outfit's 41 voice values still took 40 ms every tick (0.9.337 log). v0.9.338: MemoryProbe (a guarded read
// to read; VirtualQuery kept a second for the heap to write). CopyIn/CopyOut catch a fault anyway.
bool Accessible(const void* address, std::size_t size, bool write) {
    return write ? MemoryProbe::Writable(address, size) : MemoryProbe::Readable(address, size);
}

// SEH only in the next three (no objects with destructors).
std::uint8_t* DataOf(std::uint32_t dtp) {
    if (!g_table || dtp == 0 || dtp >= kMaxDtpId) return nullptr;
    __try {
        std::uint8_t* header = g_table[dtp];
        if (!header) return nullptr;
        if (*reinterpret_cast<const std::uint16_t*>(header) == 0) return nullptr;  // not in use
        if (header[kFlagsOffset] & 1) return nullptr;                               // still streaming in
        return header + kHeaderSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

unsigned SizeRaw(std::uint32_t dtp) {
    __try {
        return g_getSize(nullptr, dtp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The size of a loaded DTP's data (0: unknown or not loaded). The game's GetSize asks the resolve heap (a virtual call
// on the heap object) and costs milliseconds while the game streams: the editor's get_values asked it once per value
// (up to 3000 for an enemy) and stalled the game thread for 2-55 s. Asked once per loaded DTP now (game thread only):
// a DTP reloaded elsewhere has another data pointer and is asked again.
// v0.9.290: a size of 0 (the heap does not know the block) is kept too: before, those DTPs asked GetSize again for
// every value (an enemy's 3000 values took 28 s in the v0.9.289 log).
struct SizeEntry {
    const std::uint8_t* data = nullptr;
    unsigned size = 0;
};
std::map<std::uint32_t, SizeEntry> g_sizes;

unsigned SizeOf(std::uint32_t dtp) {
    if (!g_getSize || dtp == 0 || dtp >= kMaxDtpId) return 0;
    const std::uint8_t* data = DataOf(dtp);
    if (!data) return 0;
    const auto found = g_sizes.find(dtp);
    if (found != g_sizes.end() && found->second.data == data) return found->second.size;
    const unsigned size = SizeRaw(dtp);
    if (g_sizes.size() > 20000) g_sizes.clear();   // many levels later: start over
    g_sizes[dtp] = SizeEntry{data, size};
    return size;
}

bool CopyIn(const std::uint8_t* p, std::size_t size, std::uint64_t& out) {
    __try {
        std::uint64_t v = 0;
        std::memcpy(&v, p, size);
        out = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyOut(std::uint8_t* p, std::size_t size, std::uint64_t v) {
    __try {
        std::memcpy(p, &v, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadRaw(std::uint8_t* data, const Field& f, std::uint64_t& value) {
    const std::uint8_t* p = data + f.offset;
    std::uint64_t v = 0;
    if (!Accessible(p, f.size, false) || !CopyIn(p, f.size, v)) return false;
    value = f.bits ? (v >> f.bit) & Mask(f) : v & Mask(f);
    return true;
}

bool WriteRaw(std::uint8_t* data, const Field& f, std::uint64_t value) {
    std::uint8_t* p = data + f.offset;
    if (!Accessible(p, f.size, true)) return false;
    std::uint64_t v = value & Mask(f);
    if (f.bits) {
        std::uint64_t old = 0;
        if (!CopyIn(p, f.size, old)) return false;
        v = (old & ~(Mask(f) << f.bit)) | (v << f.bit);
    }
    if (!CopyOut(p, f.size, v)) return false;
    g_writes.fetch_add(1);
    return true;
}

std::string Format(const Field& f, std::uint64_t v) {
    char buf[64];
    if (f.kind == 'b') return v ? "true" : "false";
    if (f.bits || f.kind == 'u') {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    } else if (f.kind == 'f') {
        if (f.size == 4) {
            const auto bits = static_cast<std::uint32_t>(v);
            float x;
            std::memcpy(&x, &bits, 4);
            std::snprintf(buf, sizeof(buf), "%.9g", x);
        } else {
            double x;
            std::memcpy(&x, &v, 8);
            std::snprintf(buf, sizeof(buf), "%.17g", x);
        }
    } else {
        const int shift = 64 - f.size * 8;
        const long long x = static_cast<long long>(v << shift) >> shift;
        std::snprintf(buf, sizeof(buf), "%lld", x);
    }
    return buf;
}

std::string Trimmed(const std::string& text) {
    std::size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    return text.substr(a, b - a);
}

bool Parse(const Field& f, const std::string& input, std::uint64_t& out, std::string& error) {
    std::string text = Trimmed(input);
    if (text.empty()) {
        error = "no value";
        return false;
    }
    if (f.kind == 'b') {
        if (text == "true" || text == "1") out = 1;
        else if (text == "false" || text == "0") out = 0;
        else {
            error = "not true / false: " + text;
            return false;
        }
        return true;
    }
    char* end = nullptr;
    errno = 0;
    if (f.kind == 'f') {
        std::replace(text.begin(), text.end(), ',', '.');
        const double d = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end || !std::isfinite(d)) {
            error = "not a number: " + text;
            return false;
        }
        if (f.size == 4) {
            if (std::fabs(d) > 3.4e38) {
                error = "too large for a float";
                return false;
            }
            const auto x = static_cast<float>(d);
            std::uint32_t bits;
            std::memcpy(&bits, &x, 4);
            out = bits;
        } else {
            std::memcpy(&out, &d, 8);
        }
        return true;
    }
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (f.kind == 'i' && !f.bits) {
        const long long x = std::strtoll(text.c_str(), &end, hex ? 16 : 10);
        if (end == text.c_str() || *end || errno == ERANGE) {
            error = "not a whole number: " + text;
            return false;
        }
        const int n = f.size * 8;
        if (n < 64) {
            const long long lo = -(1ll << (n - 1)), hi = (1ll << (n - 1)) - 1;
            if (x < lo || x > hi) {
                error = "out of range (" + std::to_string(lo) + " to " + std::to_string(hi) + ")";
                return false;
            }
        }
        out = static_cast<std::uint64_t>(x) & Mask(f);
        return true;
    }
    // unsigned integers and bitfields
    if (text[0] == '-') {
        if (text == "-1" && !f.bits) {
            out = Mask(f);  // an unsigned "none"
            return true;
        }
        error = "a negative value for an unsigned field";
        return false;
    }
    const unsigned long long x = std::strtoull(text.c_str(), &end, hex ? 16 : 10);
    if (end == text.c_str() || *end || errno == ERANGE) {
        error = "not a whole number: " + text;
        return false;
    }
    if (x > Mask(f)) {
        error = "out of range (0 to " + std::to_string(Mask(f)) + ")";
        return false;
    }
    out = x;
    return true;
}

bool KindOf(const std::string& type, char& kind, std::uint8_t& size) {
    static const struct {
        const char* name;
        char kind;
        std::uint8_t size;
    } kTypes[] = {{"f32", 'f', 4}, {"f64", 'f', 8}, {"i8", 'i', 1},  {"u8", 'u', 1},  {"bool", 'b', 1}, {"i16", 'i', 2},
                  {"u16", 'u', 2}, {"i32", 'i', 4}, {"u32", 'u', 4}, {"i64", 'i', 8}, {"u64", 'u', 8}};
    for (const auto& t : kTypes) {
        if (type == t.name) {
            kind = t.kind;
            size = t.size;
            return true;
        }
    }
    return false;
}

bool Effective(const State& s, std::uint64_t& value) {
    const auto editor = s.values.find(0);
    if (editor != s.values.end()) {
        value = editor->second;
        return true;
    }
    for (auto it = s.order.rbegin(); it != s.order.rend(); ++it) {
        const auto v = s.values.find(*it);
        if (v != s.values.end()) {
            value = v->second;
            return true;
        }
    }
    return false;
}

void UpdateCounts() {
    int editor = 0, builds = 0;
    for (const auto& [key, s] : g_states) {
        if (s.values.count(0)) ++editor;
        if (s.values.size() > s.values.count(0)) ++builds;
    }
    g_editorCount.store(editor);
    g_buildCount.store(builds);
}

// Writes the value that wins; reads the game's own value first when the DTP (re)loaded.
SetResult Apply(State& s, std::string* error) {
    std::uint8_t* data = DataOf(s.field.dtp);
    if (!data) {
        s.data = nullptr;
        return SetResult::Pending;
    }
    std::uint64_t want = 0;
    if (!Effective(s, want)) return SetResult::Pending;
    if (data != s.data) {
        // the DTP the game loaded must be the files' one: its size first, then the value
        if (const unsigned size = SizeOf(s.field.dtp)) {
            if (static_cast<std::uint64_t>(s.field.offset) + s.field.size > size) {
                if (error) *error = "the game's data ends before this value (" + std::to_string(size) + " bytes): another version of it";
                g_refused.fetch_add(1);
                return SetResult::Refused;
            }
            if (s.fileSize && (size < s.fileSize || size > s.fileSize + kSizeSlack)) {
                if (error)
                    *error = "the game's data has " + std::to_string(size) + " bytes, the files' " + std::to_string(s.fileSize) +
                             ": another version of it (another mod's?)";
                g_refused.fetch_add(1);
                return SetResult::Refused;
            }
        }
        std::uint64_t own = 0;
        if (!ReadRaw(data, s.field, own)) {
            if (error) *error = "the game's data could not be read";
            return SetResult::Refused;
        }
        if (!s.expect.empty() && own != want && std::find(s.expect.begin(), s.expect.end(), own) == s.expect.end()) {
            if (error) *error = "the game has " + Format(s.field, own) + " there, not the value of the files (another mod's data?)";
            g_refused.fetch_add(1);
            return SetResult::Refused;
        }
        s.data = data;
        s.original = own;
        s.haveOriginal = true;
    }
    std::uint64_t now = 0;
    if (!ReadRaw(data, s.field, now)) {
        if (error) *error = "the game's data could not be read";
        return SetResult::Refused;
    }
    if (now != want && !WriteRaw(data, s.field, want)) {
        if (error) *error = "the game's data could not be written";
        return SetResult::Refused;
    }
    return SetResult::Applied;
}

// The game's own value back, when the data it was read from is still there (false: not loaded now, or loaded anew,
// so the game has its own value anyway).
bool Restore(State& s) {
    if (!s.haveOriginal || !s.data) return false;
    std::uint8_t* data = DataOf(s.field.dtp);
    if (data != s.data) return false;
    std::uint64_t now = 0;
    if (ReadRaw(data, s.field, now) && now != s.original) WriteRaw(data, s.field, s.original);
    return true;
}

void Released(std::map<Key, State>::iterator it, int owner) {
    State& s = it->second;
    s.order.erase(std::remove(s.order.begin(), s.order.end(), owner), s.order.end());
    if (s.values.empty()) {
        const bool restored = Restore(s);
        // (the worn outfit's voice settings: VoiceEdit writes one line for all of them)
        if (owner <= kSuiteOwner && g_logLines++ < kMaxLogLines) {
            if (restored)
                Log::Detailf("DtpEdit: %s back to the game's value %s", Describe(s.field).c_str(), Format(s.field, s.original).c_str());
            else
                Log::Detailf("DtpEdit: %s released (not loaded now: the game reads its own value when it loads)", Describe(s.field).c_str());
        }
        g_states.erase(it);
        return;
    }
    Apply(s, nullptr);
}
}  // namespace

bool Initialize() {
    if (g_available) return true;
    if (!GameBuild::IsSupportedRetail()) {
        g_status = "unsupported game build";
        return false;
    }
    g_base = GameBuild::Base();
    if (!GameBuild::VerifyBytes(kRvaGetBasePointer, kHeadSig, sizeof(kHeadSig)) ||
        !GameBuild::VerifyBytes(kRvaAfterLea, kTailSig, sizeof(kTailSig))) {
        g_status = "DTPDataSection::GetBasePointer does not match this game build";
        Log::Write("DtpEdit: DTPDataSection::GetBasePointer signature mismatch: game data edits disabled");
        return false;
    }
    std::int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(g_base + kRvaTableDisp), sizeof(disp));
    g_table = reinterpret_cast<std::uint8_t* const*>(g_base + kRvaAfterLea + static_cast<std::intptr_t>(disp));
    // GetSize for the size check: the same table, and `this` is never read (called with null)
    std::int32_t sizeDisp = 0;
    if (GameBuild::VerifyBytes(kRvaGetSize, kGetSizeHead, sizeof(kGetSizeHead)) &&
        GameBuild::VerifyBytes(kRvaGetSize + 0x15, kGetSizeMid, sizeof(kGetSizeMid)) &&
        GameBuild::VerifyBytes(kRvaGetSize + 0x25, kGetSizeTail, sizeof(kGetSizeTail))) {
        std::memcpy(&sizeDisp, reinterpret_cast<const void*>(g_base + kRvaGetSize + 0x11), sizeof(sizeDisp));
        if (g_base + kRvaGetSize + 0x15 + static_cast<std::intptr_t>(sizeDisp) == reinterpret_cast<std::uintptr_t>(g_table))
            g_getSize = reinterpret_cast<GetSizeFn>(g_base + kRvaGetSize);
    }
    g_available = true;
    g_status = "ready";
    Log::Detailf("DtpEdit: ready (DTP table at ROTTR.exe+0x%llX, size check %s)",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_table) - g_base),
                g_getSize ? "on" : "off: DTPDataSection::GetSize does not match");
    return true;
}

bool Available() { return g_available; }

const char* Status() { return g_status.c_str(); }

bool ParseField(std::uint32_t dtp, std::uint32_t offset, const std::string& type, Field& out, std::string& error) {
    Field f;
    f.dtp = dtp;
    f.offset = offset;
    if (dtp == 0 || dtp >= kMaxDtpId) {
        error = "no such DTP id";
        return false;
    }
    if (offset > 0x4000000) {
        error = "offset out of range";
        return false;
    }
    if (type.rfind("E:", 0) == 0) {
        f.kind = 'i';
        f.size = 4;
    } else if (type.rfind("bf:", 0) == 0) {
        const std::size_t a = type.find(':', 3);
        const std::size_t b = a == std::string::npos ? a : type.find(':', a + 1);
        if (b == std::string::npos || !KindOf(type.substr(3, a - 3), f.kind, f.size) || f.kind == 'f') {
            error = "not an editable bitfield: " + type;
            return false;
        }
        const int bit = std::atoi(type.c_str() + a + 1), count = std::atoi(type.c_str() + b + 1);
        if (bit < 0 || count <= 0 || bit + count > f.size * 8) {
            error = "bad bitfield: " + type;
            return false;
        }
        f.bit = static_cast<std::uint8_t>(bit);
        f.bits = static_cast<std::uint8_t>(count);
    } else if (!KindOf(type, f.kind, f.size)) {
        error = "not an editable type: " + type;
        return false;
    }
    out = f;
    return true;
}

bool Loaded(std::uint32_t dtp) { return g_available && DataOf(dtp) != nullptr; }

void* DataPointer(std::uint32_t dtp) { return g_available ? DataOf(dtp) : nullptr; }

namespace {
// The data of a DTP in use (streaming or not), or null. SEH only.
const std::uint8_t* ResidentData(std::uint32_t dtp) {
    __try {
        const std::uint8_t* header = g_table[dtp];
        if (!header || *reinterpret_cast<const std::uint16_t*>(header) == 0) return nullptr;
        return header + kHeaderSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
}  // namespace

std::uint32_t FindContaining(const void* address, const void** dataOut) {
    if (dataOut) *dataOut = nullptr;
    if (!g_available || !address) return 0;
    const auto* a = static_cast<const std::uint8_t*>(address);
    std::uint32_t best = 0;
    const std::uint8_t* bestData = nullptr;
    for (std::uint32_t id = 1; id < kMaxDtpId; ++id) {
        const std::uint8_t* data = ResidentData(id);
        if (data && data <= a && data > bestData) {
            best = id;
            bestData = data;
            if (data == a) break;
        }
    }
    if (!best) return 0;
    if (bestData != a) {
        const unsigned size = SizeOf(best);
        if (size && a >= bestData + size + kSizeSlack) return 0;  // past its data (a size of 0: not known, taken as in it)
    }
    if (dataOut) *dataOut = bestData;
    return best;
}

bool StillResident(std::uint32_t dtp, const void* data) {
    return g_available && dtp && dtp < kMaxDtpId && data && ResidentData(dtp) == data;
}

bool Read(const Field& field, std::string& value) {
    if (!g_available) return false;
    std::uint8_t* data = DataOf(field.dtp);
    std::uint64_t raw = 0;
    if (!data) return false;
    const unsigned size = SizeOf(field.dtp);
    if (size && static_cast<std::uint64_t>(field.offset) + field.size > size) return false;  // past the game's data
    if (!ReadRaw(data, field, raw)) return false;
    value = Format(field, raw);
    return true;
}

SetResult SetOverride(int owner, const Field& field, const std::string& value, const std::vector<std::string>& expect,
                      std::uint32_t fileSize, std::string& error) {
    if (!g_available) {
        error = "game data edits are unavailable: " + g_status;
        return SetResult::Refused;
    }
    std::uint64_t raw = 0;
    if (!Parse(field, value, raw, error)) return SetResult::Refused;
    const Key key = KeyOf(field);
    auto it = g_states.find(key);
    if (it != g_states.end() && (it->second.field.kind != field.kind || it->second.field.size != field.size)) {
        Restore(it->second);  // the same place read as another type: that override goes
        g_states.erase(it);
        it = g_states.end();
    }
    if (it == g_states.end()) {
        it = g_states.emplace(key, State{}).first;
        it->second.field = field;
    }
    State& s = it->second;
    if (fileSize) s.fileSize = fileSize;
    for (const std::string& e : expect) {
        std::uint64_t x = 0;
        std::string ignored;
        if (Parse(field, e, x, ignored) && std::find(s.expect.begin(), s.expect.end(), x) == s.expect.end()) s.expect.push_back(x);
    }
    s.values[owner] = raw;
    s.order.erase(std::remove(s.order.begin(), s.order.end(), owner), s.order.end());
    s.order.push_back(owner);
    const SetResult result = Apply(s, &error);
    if (result == SetResult::Refused) {
        if (g_logLines++ < kMaxLogLines)
            Log::Writef("DtpEdit: %s = %s refused: %s", Describe(field).c_str(), value.c_str(), error.c_str());
        g_states.erase(key);
    } else if (owner <= kSuiteOwner && g_logLines++ < kMaxLogLines) {   // (voice settings: VoiceEdit's one line)
        Log::Detailf("DtpEdit: %s = %s by %s%s%s", Describe(field).c_str(), Format(field, raw).c_str(),
                    owner == kSuiteOwner ? "the Modding Suite"
                                         : owner > kSuiteOwner ? "the worn outfit's voice settings" : "another owner",
                    result == SetResult::Pending ? " (not loaded now: written when it loads)" : " (game value ",
                    result == SetResult::Pending ? "" : (Format(field, s.original) + ")").c_str());
    }
    UpdateCounts();
    return result;
}

void ClearOverride(int owner, const Field& field) {
    const auto it = g_states.find(KeyOf(field));
    if (it == g_states.end() || !it->second.values.erase(owner)) return;
    Released(it, owner);
    UpdateCounts();
}

void ClearOwner(int owner) {
    for (auto it = g_states.begin(); it != g_states.end();) {
        auto next = std::next(it);
        if (it->second.values.erase(owner)) Released(it, owner);
        it = next;
    }
    UpdateCounts();
}

bool OriginalValue(const Field& field, std::string& value) {
    const auto it = g_states.find(KeyOf(field));
    if (it == g_states.end() || !it->second.haveOriginal) return false;
    value = Format(it->second.field, it->second.original);
    return true;
}

int OverrideCount(int owner) {
    int count = 0;
    for (const auto& [key, s] : g_states)
        if (owner < 0 || s.values.count(owner)) ++count;
    return count;
}

void TickGameThread() {
    if (!g_available || g_states.empty()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastTickMs < kTickMs) return;
    g_lastTickMs = now;
    bool changed = false;
    for (auto it = g_states.begin(); it != g_states.end();) {
        std::string error;
        if (Apply(it->second, &error) == SetResult::Refused) {
            if (g_logLines++ < kMaxLogLines)
                Log::Writef("DtpEdit: %s: %s: the edit is dropped", Describe(it->second.field).c_str(), error.c_str());
            it = g_states.erase(it);
            changed = true;
            continue;
        }
        ++it;
    }
    if (changed) UpdateCounts();
}

}  // namespace DtpEdit
