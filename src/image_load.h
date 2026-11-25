#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// JPEG / PNG files to RGBA8 pixels (stb_image; the Cutscene Viewer's screenshots), and RGBA8 to BC3 (DXT5, stb_dxt;
// the viewer's picture textures).
namespace ImageLoad {
bool DecodeFile(const std::wstring& path, std::vector<unsigned char>& rgba, int& width, int& height);
std::size_t Bc3Size(int width, int height);   // 16 bytes a 4x4 block (a part block counts whole)
void CompressBc3(const unsigned char* rgba, int width, int height, unsigned char* out);   // out: Bc3Size bytes
// A texture's first level to RGBA8 (bcdec): DXGI formats BC1, BC2, BC3, BC7 (sRGB or not), RGBA8, BGRA8. false: another
// format, or too little data.
bool DecodeTexture(std::uint32_t format, const unsigned char* data, std::size_t size, int width, int height,
                   std::vector<unsigned char>& rgba);
}
