#include "hair_layout.h"

#include "dxbc_eight_weight_rewriter.h"
#include "game_build.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace HairLayout {
namespace {
    // Generated from rottr_hair.py v12's sites, checked against retail ROTTR.exe (27,582,000 bytes): each dword
    // (an instruction's immediate, or dcl_indexableTemp's count) and its value in each layout: stock, PH64, PH128, PH256.
    struct ShaderDword { std::uint32_t rva; std::uint32_t blob; std::uint32_t value[4]; bool tempArray; };
    constexpr ShaderDword kShaderDwords[] = {
        {0x031315CC, 0x0313036C, {5, 6, 7, 8}, false},   // descriptor count
        {0x031315D4, 0x0313036C, {22, 21, 22, 21}, false},   // descriptor count
        {0x03135378, 0x031344AC, {5, 6, 7, 8}, false},   // descriptor count
        {0x03135380, 0x031344AC, {22, 21, 22, 21}, false},   // descriptor count
        {0x03137640, 0x03136AAC, {5, 6, 7, 8}, false},   // descriptor count
        {0x03137648, 0x03136AAC, {22, 21, 22, 21}, false},   // descriptor count
        {0x03139ADC, 0x03138DAC, {5, 6, 7, 8}, false},   // descriptor count
        {0x03139AE4, 0x03138DAC, {22, 21, 22, 21}, false},   // descriptor count
        {0x0313B51C, 0x0313A4BC, {5, 6, 7, 8}, false},   // descriptor count
        {0x0313B524, 0x0313A4BC, {22, 21, 22, 21}, false},   // descriptor count
        {0x0313E894, 0x0313D76C, {5, 6, 7, 8}, false},   // descriptor count
        {0x0313E89C, 0x0313D76C, {22, 21, 22, 21}, false},   // descriptor count
        {0x031461C0, 0x03145888, {5, 6, 7, 8}, false},   // descriptor count
        {0x031461C8, 0x03145888, {22, 21, 22, 21}, false},   // descriptor count
        {0x0314807C, 0x03147698, {5, 6, 7, 8}, false},   // descriptor count
        {0x03148084, 0x03147698, {22, 21, 22, 21}, false},   // descriptor count
        {0x03110058, 0x0310F380, {5, 6, 7, 8}, false},   // descriptor count
        {0x03110060, 0x0310F380, {22, 21, 22, 21}, false},   // descriptor count
        {0x03112564, 0x03111614, {5, 6, 7, 8}, false},   // descriptor count
        {0x0311256C, 0x03111614, {22, 21, 22, 21}, false},   // descriptor count
        {0x0312CB60, 0x0312AFE0, {5, 6, 7, 8}, false},   // descriptor count
        {0x0312CB68, 0x0312AFE0, {22, 21, 22, 21}, false},   // descriptor count
        {0x031353C0, 0x031344AC, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03137688, 0x03136AAC, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03139B08, 0x03138DAC, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x0313E8E8, 0x0313D76C, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03146208, 0x03145888, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x0314630C, 0x03145888, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x0314634C, 0x03145888, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x031480C4, 0x03147698, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x031481C8, 0x03147698, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03148208, 0x03147698, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03110024, 0x0310F380, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03112530, 0x03111614, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x0312CB04, 0x0312AFE0, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03113918, 0x03111614, {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, false},   // first-vertex masks
        {0x0311391C, 0x03111614, {0, 0, 0, 0}, false},   // first-vertex masks
        {0x03113920, 0x03111614, {0x3FFFFF, 0x1FFFFF, 0x3FFFFF, 0x1FFFFF}, false},   // first-vertex masks
        {0x03113924, 0x03111614, {0, 0, 0, 0}, false},   // first-vertex masks
        {0x031315B8, 0x0313036C, {27, 27, 29, 29}, false},   // group index
        {0x03135364, 0x031344AC, {27, 27, 29, 29}, false},   // group index
        {0x03139AC8, 0x03138DAC, {27, 27, 29, 29}, false},   // group index
        {0x0313B508, 0x0313A4BC, {27, 27, 29, 29}, false},   // group index
        {0x0313E880, 0x0313D76C, {27, 27, 29, 29}, false},   // group index
        {0x03146224, 0x03145888, {27, 27, 29, 29}, false},   // group index
        {0x031480E0, 0x03147698, {27, 27, 29, 29}, false},   // group index
        {0x03110084, 0x0310F380, {27, 27, 29, 29}, false},   // group index
        {0x03112590, 0x03111614, {27, 27, 29, 29}, false},   // group index
        {0x0312CB8C, 0x0312AFE0, {27, 27, 29, 29}, false},   // group index
        {0x03146828, 0x03145888, {11, 10, 9, 8}, false},   // binding word
        {0x03148654, 0x03147698, {11, 10, 9, 8}, false},   // binding word
        {0x03148978, 0x03147698, {11, 10, 9, 8}, false},   // binding word
        {0x03113BA8, 0x03111614, {11, 10, 9, 8}, false},   // binding word
        {0x0314683C, 0x03145888, {5, 6, 7, 8}, false},   // binding word
        {0x03146844, 0x03145888, {6, 4, 2, 0}, false},   // binding word
        {0x0314898C, 0x03147698, {5, 6, 7, 8}, false},   // binding word
        {0x03148994, 0x03147698, {6, 4, 2, 0}, false},   // binding word
        {0x03113C48, 0x03111614, {5, 6, 7, 8}, false},   // binding word
        {0x03113C50, 0x03111614, {6, 4, 2, 0}, false},   // binding word
        {0x03146884, 0x03145888, {63, 15, 3, 0}, false},   // binding word
        {0x031489D4, 0x03147698, {63, 15, 3, 0}, false},   // binding word
        {0x03113C74, 0x03111614, {63, 15, 3, 0}, false},   // binding word
        {0x031468B4, 0x03145888, {0x3C820821, 0x3D888889, 0x3EAAAAAB, 0x0}, false},   // binding word
        {0x03148A04, 0x03147698, {0x3C820821, 0x3D888889, 0x3EAAAAAB, 0x0}, false},   // binding word
        {0x03113CA4, 0x03111614, {0x3C820821, 0x3D888889, 0x3EAAAAAB, 0x0}, false},   // binding word
        {0x031468D4, 0x03145888, {0x3C820821, 0x3D888889, 0x3EAAAAAB, 0x0}, false},   // binding word
        {0x031468DC, 0x03145888, {0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000}, false},   // binding word
        {0x03148A24, 0x03147698, {0x3C820821, 0x3D888889, 0x3EAAAAAB, 0x0}, false},   // binding word
        {0x03148A2C, 0x03147698, {0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000}, false},   // binding word
        {0x031352B8, 0x031344AC, {16, 64, 128, 256}, true},   // sim temp arrays
        {0x031352C8, 0x031344AC, {16, 64, 128, 256}, true},   // sim temp arrays
    };
    // the shaders those are in: where (RVA) and their size (their checksum made again once changed)
    struct ShaderBlob { std::uint32_t rva; std::uint32_t size; };
    constexpr ShaderBlob kShaderBlobs[] = {
        {0x0310F380, 8836},
        {0x03111614, 10244},
        {0x0312AFE0, 10432},
        {0x0313036C, 16664},
        {0x031344AC, 9692},
        {0x03136AAC, 8924},
        {0x03138DAC, 5872},
        {0x0313A4BC, 7484},
        {0x0313D76C, 15264},
        {0x03145888, 7680},
        {0x03147698, 8512},
    };
    // the CPU's strand-descriptor accessors (group index, vertex count, first vertex): their bytes in each layout
    constexpr std::uint8_t kGIStock[] = {0x8B, 0x01, 0xC1, 0xE8, 0x1B, 0xC3};
    constexpr std::uint8_t kGIPH64[] = {0x8B, 0x01, 0xC1, 0xE8, 0x1B, 0xC3};
    constexpr std::uint8_t kGIPH128[] = {0x8B, 0x01, 0xC1, 0xE8, 0x1D, 0xC3};
    constexpr std::uint8_t kGIPH256[] = {0x8B, 0x01, 0xC1, 0xE8, 0x1D, 0xC3};
    constexpr std::uint32_t kRvaGI = 0x00AED5B0;
    constexpr std::uint8_t kNVStock[] = {0x8B, 0x01, 0xC1, 0xE8, 0x16, 0x83, 0xE0, 0x1F, 0xFF, 0xC0, 0xC3};
    constexpr std::uint8_t kNVPH64[] = {0x8B, 0x01, 0xC1, 0xE8, 0x15, 0x83, 0xE0, 0x3F, 0xFF, 0xC0, 0xC3};
    constexpr std::uint8_t kNVPH128[] = {0x8B, 0x01, 0xC1, 0xE8, 0x16, 0x83, 0xE0, 0x7F, 0xFF, 0xC0, 0xC3};
    constexpr std::uint8_t kNVPH256[] = {0x8B, 0x01, 0xC1, 0xE8, 0x15, 0x0F, 0xB6, 0xC0, 0xFF, 0xC0, 0xC3};
    constexpr std::uint32_t kRvaNV = 0x00AED5C0;
    constexpr std::uint8_t kFVIStock[] = {0x8B, 0x01, 0x25, 0xFF, 0xFF, 0x3F, 0x00, 0xC3};
    constexpr std::uint8_t kFVIPH64[] = {0x8B, 0x01, 0x25, 0xFF, 0xFF, 0x1F, 0x00, 0xC3};
    constexpr std::uint8_t kFVIPH128[] = {0x8B, 0x01, 0x25, 0xFF, 0xFF, 0x3F, 0x00, 0xC3};
    constexpr std::uint8_t kFVIPH256[] = {0x8B, 0x01, 0x25, 0xFF, 0xFF, 0x1F, 0x00, 0xC3};
    constexpr std::uint32_t kRvaFVI = 0x00AED5D0;
    constexpr std::uint32_t kRvaV8CallNv = 0x00AEF14C;
    constexpr std::uint32_t kRvaV8CallFvi = 0x00AEF156;
    constexpr std::uint32_t kRvaV8VersionCheck = 0x00AED14B;
    constexpr std::uint32_t kRvaV8Cave = 0x00D00ABE;

    constexpr const std::uint8_t* kCpu[3][4] = {{kGIStock, kGIPH64, kGIPH128, kGIPH256},
                                                {kNVStock, kNVPH64, kNVPH128, kNVPH256},
                                                {kFVIStock, kFVIPH64, kFVIPH128, kFVIPH256}};
    constexpr std::size_t kCpuSize[3] = {sizeof(kGIStock), sizeof(kNVStock), sizeof(kFVIStock)};
    constexpr std::uint32_t kCpuRva[3] = {kRvaGI, kRvaNV, kRvaFVI};
    constexpr const char* kCpuName[3] = {"group index", "vertex count", "first vertex"};
    constexpr std::uint32_t kTempArray[4] = {16, 64, 128, 256};
    constexpr std::uint32_t kPoints[4] = {32, 64, 128, 256};   // a strand's most points
    constexpr std::size_t kV8CaveSize = 80;
    constexpr std::uint8_t kV8VersionPatched[] = {0x83, 0x79, 0x04, 0x16, 0x73, 0xE5};

    // The hair's rendering context (retail 0x276140: made with its compute shaders right after the render device, at
    // 0x3FC15D) is kept at 0x1A53E20: 0x27615B mov [rip+0x17DDCBE], rax. Set: its shaders are made already.
    constexpr std::uint32_t kRvaHairContextStore = 0x0027615B, kRvaHairContext = 0x01A53E20;
    constexpr std::uint8_t kHairContextStoreSig[] = {0x48, 0x89, 0x05, 0xBE, 0xDC, 0x7D, 0x01};

    int g_current = -1;
    char g_status[256] = "Not read";

    std::uint8_t* At(std::uint32_t rva) { return reinterpret_cast<std::uint8_t*>(GameBuild::Base() + rva); }
    std::uint32_t Dword(std::uint32_t rva) {
        std::uint32_t v;
        std::memcpy(&v, At(rva), 4);
        return v;
    }

    // The layouts the bytes in memory are (a bit each), the CPU accessors' and the shaders' together; 0: mixed / unknown.
    unsigned Matching() {
        unsigned layouts = (1u << Count) - 1u;
        for (int c = 0; c < 3; ++c) {
            unsigned these = 0;
            for (int l = 0; l < Count; ++l)
                if (std::memcmp(At(kCpuRva[c]), kCpu[c][l], kCpuSize[c]) == 0) these |= 1u << l;
            layouts &= these;
        }
        for (const ShaderDword& d : kShaderDwords) {
            if (d.tempArray) continue;   // (its count may be another: rottr_hair.py --temp-array)
            unsigned these = 0;
            const std::uint32_t v = Dword(d.rva);
            for (int l = 0; l < Count; ++l)
                if (v == d.value[l]) these |= 1u << l;
            layouts &= these;
        }
        return layouts;
    }
    int Single(unsigned layouts) {
        for (int l = Count - 1; l >= 0; --l)   // (PH256 matches some PH64 / PH128 bytes: the widest that matches all)
            if (layouts == (1u << l)) return l;
        return -1;
    }

    // An older rottr_hair.py's (v8) changes: calls led past the accessors, a version check, a code cave.
    std::string V8Leftovers() {
        std::string found;
        auto target = [](std::uint32_t site) {
            std::int32_t rel;
            std::memcpy(&rel, At(site + 1), 4);
            return std::uint32_t(std::int64_t(site) + 5 + rel);
        };
        if (*At(kRvaV8CallNv) == 0xE8 && target(kRvaV8CallNv) != kRvaNV) found += "call->nv ";
        if (*At(kRvaV8CallFvi) == 0xE8 && target(kRvaV8CallFvi) != kRvaFVI) found += "call->fvi ";
        if (std::memcmp(At(kRvaV8VersionCheck), kV8VersionPatched, sizeof(kV8VersionPatched)) == 0) found += "version-check ";
        for (std::size_t i = 0; i < kV8CaveSize; ++i)
            if (At(kRvaV8Cave)[i]) {
                found += "code-cave ";
                break;
            }
        return found;
    }

    bool BlobsIntact(std::string& why) {
        for (const ShaderBlob& b : kShaderBlobs) {
            std::uint32_t size;
            std::memcpy(&size, At(b.rva + 24), 4);
            if (std::memcmp(At(b.rva), "DXBC", 4) != 0 || size != b.size) {
                char text[96];
                std::snprintf(text, sizeof(text), "the shader at ROTTR.exe+0x%X is not the one expected", unsigned(b.rva));
                why = text;
                return false;
            }
        }
        return true;
    }
}

const char* Name(int layout) {
    static const char* const names[] = {"Stock", "PH64", "PH128", "PH256"};
    return layout >= 0 && layout < Count ? names[layout] : "mixed";
}

int Parse(const char* text) {
    if (!text) return -1;
    std::string s;
    for (const char* p = text; *p; ++p)
        if (!std::isspace(static_cast<unsigned char>(*p))) s.push_back(char(std::toupper(static_cast<unsigned char>(*p))));
    if (s == "STOCK") return Stock;
    if (s == "PH64" || s == "64") return PH64;
    if (s == "PH128" || s == "128") return PH128;
    if (s == "PH256" || s == "256") return PH256;
    return -1;
}

int Current() { return g_current; }
const char* Status() { return g_status; }

void ApplyAtStartup(const char* wanted, int tempArray) {
    const int target = Parse(wanted);
    std::string why;
    if (!BlobsIntact(why)) {
        std::snprintf(g_status, sizeof(g_status), "Not available: %s", why.c_str());
        Log::Writef("HairLayout: %s; the strand layout is left as it is", why.c_str());
        return;
    }
    const int before = Single(Matching());
    g_current = before;
    const std::string v8 = V8Leftovers();
    if (target < 0) {   // (off: the exe's own, told)
        if (wanted && wanted[0] && _stricmp(wanted, "off") != 0)
            Log::Writef("HairLayout: [Hair] StrandLayout=%s is not Stock, PH64, PH128 or PH256: the layout is left as it is", wanted);
        std::snprintf(g_status, sizeof(g_status), "Off: the game's %s layout%s", Name(before),
                      before > Stock ? " (ROTTR.exe patched)" : "");
        if (before != Stock) Log::Writef("HairLayout: ROTTR.exe's strand layout: %s%s", Name(before), v8.empty() ? "" : " (with v8 leftovers)");
        return;
    }
    if (!v8.empty()) {
        std::snprintf(g_status, sizeof(g_status), "Not applied: ROTTR.exe has an older rottr_hair.py's (v8) changes");
        Log::Writef("HairLayout: ROTTR.exe has an older rottr_hair.py's (v8) changes (%s): %s not applied (revert the exe "
                    "with rottr_hair.py revert first)", v8.c_str(), Name(target));
        return;
    }
    if (before < 0) {
        std::snprintf(g_status, sizeof(g_status), "Not applied: ROTTR.exe's hair code is partly patched (mixed layouts)");
        Log::Writef("HairLayout: ROTTR.exe's strand accessors and shaders are not all in one layout: %s not applied",
                    Name(target));
        return;
    }
    std::uint32_t array = kTempArray[target];
    if (target >= PH128 && tempArray) array = std::uint32_t(std::clamp(tempArray, 16, 256));
    bool arraysAsWanted = true;
    for (const ShaderDword& d : kShaderDwords)
        if (d.tempArray && Dword(d.rva) != array) arraysAsWanted = false;
    if (before == target && arraysAsWanted) {
        std::snprintf(g_status, sizeof(g_status), "%s (ROTTR.exe already is)", Name(target));
        Log::Writef("HairLayout: %s wanted; ROTTR.exe already has it", Name(target));
        return;
    }
    // (too late: the game made the hair's shaders from the bytes as they were)
    if (!GameBuild::VerifyBytes(kRvaHairContextStore, kHairContextStoreSig, sizeof(kHairContextStoreSig))) {
        std::snprintf(g_status, sizeof(g_status), "Not applied: the game's hair setup is not the one expected");
        Log::Write("HairLayout: the hair context's store at ROTTR.exe+0x27615B is not as expected: the layout is left as it is");
        return;
    }
    if (*reinterpret_cast<void* const*>(At(kRvaHairContext))) {
        std::snprintf(g_status, sizeof(g_status), "Not applied: the game had made its hair shaders already");
        Log::Writef("HairLayout: the game made its hair shaders before the framework started: %s not applied", Name(target));
        return;
    }

    // the CPU's accessors
    for (int c = 0; c < 3; ++c) {
        DWORD old = 0, ignored = 0;
        if (!VirtualProtect(At(kCpuRva[c]), kCpuSize[c], PAGE_EXECUTE_READWRITE, &old)) {
            std::snprintf(g_status, sizeof(g_status), "Not applied: the %s accessor could not be written", kCpuName[c]);
            Log::Writef("HairLayout: VirtualProtect failed on the %s accessor (%lu)", kCpuName[c], GetLastError());
            return;   // (the ones before written: Current says mixed; nothing was made with them yet)
        }
        std::memcpy(At(kCpuRva[c]), kCpu[c][target], kCpuSize[c]);
        VirtualProtect(At(kCpuRva[c]), kCpuSize[c], old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), At(kCpuRva[c]), kCpuSize[c]);
    }
    // the shaders' immediates, then their checksums
    const std::uint32_t first = kShaderBlobs[0].rva;
    std::uint32_t end = first;
    for (const ShaderBlob& b : kShaderBlobs) end = std::max(end, b.rva + b.size);
    DWORD old = 0, ignored = 0;
    if (!VirtualProtect(At(first), end - first, PAGE_READWRITE, &old)) {
        std::snprintf(g_status, sizeof(g_status), "Not applied: the hair shaders could not be written");
        Log::Writef("HairLayout: VirtualProtect failed on the hair shaders (%lu)", GetLastError());
        return;
    }
    int written = 0, sums = 0;
    for (const ShaderDword& d : kShaderDwords) {
        const std::uint32_t v = d.tempArray ? array : d.value[target];
        std::memcpy(At(d.rva), &v, 4);
        ++written;
    }
    bool sumsOk = true;
    for (const ShaderBlob& b : kShaderBlobs) {
        std::uint8_t sum[16];
        if (!DxbcEightWeight::Checksum(At(b.rva), b.size, sum)) {
            sumsOk = false;
            continue;
        }
        std::memcpy(At(b.rva + 4), sum, 16);
        ++sums;
    }
    VirtualProtect(At(first), end - first, old, &ignored);
    g_current = Single(Matching());
    if (*reinterpret_cast<void* const*>(At(kRvaHairContext)))   // (made while the bytes were being written)
        Log::Write("HairLayout: the game made its hair shaders while the layout was being written: restart the game if "
                   "the hair looks wrong");
    std::snprintf(g_status, sizeof(g_status), "%s (applied at startup%s)%s", Name(target),
                  target >= PH128 && array != kTempArray[target] ? ", smaller simulation arrays" : "",
                  g_current == target && sumsOk ? "" : ": check the log");
    Log::Writef("HairLayout: strand layout %s applied in memory (was %s): the 3 descriptor accessors, %d shader values, %d "
                "checksums%s; strands up to %u points (simulation arrays %u)%s", Name(target), Name(before), written, sums,
                sumsOk ? "" : " (some could not be computed)", kPoints[target], array,
                g_current == target ? "" : "; the bytes read back are not that layout");
}
}
