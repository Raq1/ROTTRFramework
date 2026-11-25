// (v0.9.552) RtCutout's analysis over a folder of the game's shader libraries (an unpacked drm's ShaderLib):
// every pixel shader that discards, and every vertex shader with a TEXCOORD 0 output. cutout_test <folder>
#include "rt_cutout.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string Describe(const RtCutout::Constant& c) {
    char text[48];
    if (c.slot < 0) std::snprintf(text, sizeof(text), "%g", c.literal);
    else std::snprintf(text, sizeof(text), "cb%d[%u].%c", c.slot, c.at / 4, "xyzw"[c.at % 4]);
    return text;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::set<std::string> seen;
    std::map<std::string, int> pixelWhy, vertexWhy;
    std::map<std::string, std::string> pixelExample;
    int pixels = 0, followed = 0, vertices = 0, vertexFollowed = 0, shown = 0;
    for (const auto& entry : fs::directory_iterator(argv[1])) {
        std::ifstream f(entry.path(), std::ios::binary);
        std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        for (std::size_t i = 0; i + 32 <= d.size();) {
            const char* at = static_cast<const char*>(std::memchr(d.data() + i, 'D', d.size() - i));
            if (!at) break;
            i = at - d.data();
            if (i + 32 > d.size() || std::memcmp(at, "DXBC", 4) != 0) {
                ++i;
                continue;
            }
            std::uint32_t size = 0;
            std::memcpy(&size, at + 24, 4);
            if (size < 32 || i + size > d.size()) {
                ++i;
                continue;
            }
            const std::string blob(at, size);
            i += size;
            if (!seen.insert(blob).second) continue;
            // the shader's kind: SHEX / SHDR version token's program type (bits 16..31: 0 pixel, 1 vertex)
            const char* shex = nullptr;
            std::uint32_t chunks = 0;
            std::memcpy(&chunks, blob.data() + 28, 4);
            for (std::uint32_t k = 0; k < chunks && 32 + 4 * (k + 1) <= size; ++k) {
                std::uint32_t off = 0;
                std::memcpy(&off, blob.data() + 32 + 4 * k, 4);
                if (off + 12 <= size && (!std::memcmp(blob.data() + off, "SHEX", 4) || !std::memcmp(blob.data() + off, "SHDR", 4)))
                    shex = blob.data() + off + 8;
            }
            if (!shex) continue;
            std::uint32_t version = 0;
            std::memcpy(&version, shex, 4);
            const unsigned type = version >> 16;
            if (type == 0) {
                RtCutout::Program p;
                char semantic[32];
                unsigned index = 0, comps[2] = {0, 1};
                RtCutout::AnalyzePixel(blob.data(), size, p, semantic, index, comps);
                if (p.result == RtCutout::kNone) continue;
                ++pixels;
                if (p.result == RtCutout::kFollowed) {
                    ++followed;
                    if (shown++ < 12 || (argc > 2 && entry.path().filename().string().find(argv[2]) == 0))
                        std::printf("PS %s: t%u.%c at %s%u.%c%c x (%s, %s) + (%s, %s); discard below %s\n",
                                    entry.path().filename().string().c_str(), p.texture, "xyzw"[p.channel], semantic,
                                    index, "xyzw"[comps[0]], "xyzw"[comps[1]], Describe(p.psTiling[0]).c_str(),
                                    Describe(p.psTiling[1]).c_str(), Describe(p.psOffset[0]).c_str(),
                                    Describe(p.psOffset[1]).c_str(), Describe(p.threshold).c_str());
                } else {
                    if (pixelWhy[p.why]++ < 3) pixelExample[p.why] += entry.path().filename().string() + " ";
                }
            } else if (type == 1) {
                RtCutout::Program p;
                const unsigned comps[2] = {0, 1};
                p.result = RtCutout::kFollowed;
                // only those with a TEXCOORD 0 output
                if (blob.find("TEXCOORD") == std::string::npos) continue;
                ++vertices;
                if (RtCutout::AnalyzeVertex(blob.data(), size, "TEXCOORD", 0, comps, p)) {
                    ++vertexFollowed;
                    if (vertexFollowed <= 6)
                        std::printf("VS %s: TEXCOORD%u.%c%c x (%s, %s) + (%s, %s)\n", entry.path().filename().string().c_str(),
                                    p.texcoord, "xyzw"[p.component[0]], "xyzw"[p.component[1]], Describe(p.vsScale[0]).c_str(),
                                    Describe(p.vsScale[1]).c_str(), Describe(p.vsOffset[0]).c_str(),
                                    Describe(p.vsOffset[1]).c_str());
                } else {
                    ++vertexWhy[p.why];
                }
            }
        }
    }
    std::printf("pixel shaders that discard: %d, followed %d\n", pixels, followed);
    for (const auto& kv : pixelWhy)
        std::printf("  not followed: %s x%d (%s)\n", kv.first.c_str(), kv.second, pixelExample[kv.first].c_str());
    std::printf("vertex shaders with a TEXCOORD: %d, TEXCOORD0's UV followed %d\n", vertices, vertexFollowed);
    for (const auto& kv : vertexWhy) std::printf("  not followed: %s x%d\n", kv.first.c_str(), kv.second);
    return 0;
}
