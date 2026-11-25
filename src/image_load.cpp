#include "image_load.h"

#include <windows.h>

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb/stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb/stb_dxt.h"
#define BCDEC_IMPLEMENTATION
#include "bcdec/bcdec.h"

namespace ImageLoad {
bool DecodeFile(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height) {
    rgba.clear();
    width = height = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::vector<unsigned char> bytes;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20);
    if (ok) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(file);
    if (!ok) return false;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!pixels) return false;
    if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
        rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
        width = w;
        height = h;
    }
    stbi_image_free(pixels);
    return !rgba.empty();
}

std::size_t Bc3Size(int width, int height) {
    return static_cast<std::size_t>((width + 3) / 4) * static_cast<std::size_t>((height + 3) / 4) * 16;
}

void CompressBc3(const unsigned char* rgba, int width, int height, unsigned char* out) {
    unsigned char block[64];
    for (int by = 0; by < height; by += 4)
        for (int bx = 0; bx < width; bx += 4) {
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {   // (a part block repeats its last row and column)
                    const int sx = bx + x < width ? bx + x : width - 1, sy = by + y < height ? by + y : height - 1;
                    std::memcpy(block + (y * 4 + x) * 4, rgba + (static_cast<std::size_t>(sy) * width + sx) * 4, 4);
                }
            stb_compress_dxt_block(out, block, 1, STB_DXT_HIGHQUAL);
            out += 16;
        }
}

bool DecodeTexture(std::uint32_t format, const unsigned char* data, std::size_t size, int width, int height,
                   std::vector<unsigned char>& rgba) {
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
    rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    const std::size_t pitch = static_cast<std::size_t>(width) * 4;
    int blockBytes = 0;
    switch (format) {
        case 71: case 72: blockBytes = 8; break;              // BC1
        case 74: case 75: case 77: case 78: case 98: case 99: blockBytes = 16; break;   // BC2, BC3, BC7
        case 28: case 29: case 87: case 91: {                  // RGBA8 / BGRA8
            const std::size_t need = static_cast<std::size_t>(width) * height * 4;
            if (size < need) return false;
            std::memcpy(rgba.data(), data, need);
            if (format == 87 || format == 91)
                for (std::size_t i = 0; i < need; i += 4) std::swap(rgba[i], rgba[i + 2]);
            return true;
        }
        default: return false;
    }
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (size < static_cast<std::size_t>(bw) * bh * blockBytes) return false;
    unsigned char block[64];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const unsigned char* src = data + (static_cast<std::size_t>(by) * bw + bx) * blockBytes;
            switch (format) {
                case 71: case 72: bcdec_bc1(src, block, 16); break;
                case 74: case 75: bcdec_bc2(src, block, 16); break;
                case 77: case 78: bcdec_bc3(src, block, 16); break;
                default: bcdec_bc7(src, block, 16); break;
            }
            for (int y = 0; y < 4 && by * 4 + y < height; ++y)
                for (int x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::memcpy(rgba.data() + static_cast<std::size_t>(by * 4 + y) * pitch + static_cast<std::size_t>(bx * 4 + x) * 4,
                                block + (y * 4 + x) * 4, 4);
        }
    return true;
}
}  // namespace ImageLoad
