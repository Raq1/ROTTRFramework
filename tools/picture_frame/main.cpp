// ROTTRPictureFrame: screenshots in the style of the game's own pictures (Chapter Replay's), for the Cutscene Viewer.
//
// Drop images (JPEG or PNG, any size; or folders of them) on ROTTRPictureFrame.exe. Each is cropped to 2:1 from its
// middle, scaled to 2048x1024 (Lanczos) and given frame.png's look (picture_frame.h: the alpha the frame's, the colour
// times the frame's grey), and saved as framed\<name>.png beside it. Put the result in the cutscene images folder,
// <game>\ROTTRFramework\cutscenes\images, as the cutscene's image (or <image name>.png beside its .jpg: the newest is
// used). The framework does the same to screenshots without alpha by itself; this is for seeing the result, touching
// it up, or a frame of your own.
//
// frame.png: beside this exe, or <this exe's folder>\ROTTRFramework\cutscenes\frame.png (the exe in the game's folder).
// Options: --size WxH (the output's size; default 2048x1024), --frame <png>, --out <folder> (default: framed\ beside
// each input), --no-frame (only cropped and scaled).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb/stb_image.h"

#include "miniz/miniz.h"
#include "picture_frame.h"

namespace {
namespace fs = std::filesystem;

bool ReadFile(const fs::path& path, std::vector<unsigned char>& bytes) {
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return false;
    bytes.clear();
    unsigned char buffer[1 << 16];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(file);
    return !bytes.empty();
}

bool Decode(const fs::path& path, PictureFrame::Image& image) {
    std::vector<unsigned char> bytes;
    if (!ReadFile(path, bytes) || bytes.size() > (256u << 20)) return false;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    if (!pixels) return false;
    image.width = w;
    image.height = h;
    image.rgba.assign(pixels, pixels + std::size_t(w) * h * 4);
    stbi_image_free(pixels);
    return w > 0 && h > 0;
}

bool WritePng(const fs::path& path, const PictureFrame::Image& image) {
    std::size_t size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(image.rgba.data(), image.width, image.height, 4, &size,
                                                            MZ_DEFAULT_LEVEL, MZ_FALSE);
    if (!png) return false;
    FILE* file = _wfopen(path.c_str(), L"wb");
    bool ok = file && std::fwrite(png, 1, size, file) == size;
    if (file) ok = std::fclose(file) == 0 && ok;
    mz_free(png);
    return ok;
}

bool IsImage(const fs::path& path) {
    std::wstring ext = path.extension().wstring();
    for (wchar_t& c : ext) c = wchar_t(std::towlower(c));
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg";
}

fs::path ExeFolder() {
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    return fs::path(std::wstring(buffer, n)).parent_path();
}

fs::path FindFrame() {
    const fs::path folder = ExeFolder();
    for (const fs::path& candidate : {folder / L"frame.png", folder / L"ROTTRFramework" / L"cutscenes" / L"frame.png"}) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

// (started by dropping files on it: the console is ours alone, and closes when we end)
void PauseIfOwnConsole() {
    DWORD processes[2] = {};
    if (GetConsoleProcessList(processes, 2) <= 1) {
        std::wprintf(L"\nPress Enter to close.");
        std::fflush(stdout);
        (void)std::getwchar();
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    int width = 2048, height = 1024;
    bool useFrame = true;
    fs::path framePath, outFolder;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::swscanf(argv[++i], L"%dx%d", &w, &h) == 2 && w >= 16 && h >= 16 && w <= 8192 && h <= 8192) {
                width = w;
                height = h;
            } else {
                std::fwprintf(stderr, L"--size wants WxH (16 to 8192), e.g. 2048x1024\n");
                return 2;
            }
        } else if (arg == L"--frame" && i + 1 < argc) {
            framePath = argv[++i];
        } else if (arg == L"--out" && i + 1 < argc) {
            outFolder = argv[++i];
        } else if (arg == L"--no-frame") {
            useFrame = false;
        } else {
            inputs.push_back(arg);
        }
    }
    std::wprintf(L"ROTTRPictureFrame: screenshots in the style of the game's Chapter Replay pictures\n\n");
    if (inputs.empty()) {
        std::wprintf(L"Drop JPEG or PNG screenshots (or folders of them) on this exe. Each is cropped to 2:1 from its\n"
                     L"middle, scaled to %dx%d and given frame.png's edges, then saved as framed\\<name>.png beside it.\n"
                     L"Put the result in <game>\\ROTTRFramework\\cutscenes\\images as the cutscene's image.\n\n"
                     L"Options: --size WxH, --frame <png>, --out <folder>, --no-frame\n", width, height);
        PauseIfOwnConsole();
        return 1;
    }
    PictureFrame::Image frame;
    if (useFrame) {
        if (framePath.empty()) framePath = FindFrame();
        if (framePath.empty() || !Decode(framePath, frame)) {
            std::fwprintf(stderr, L"frame.png not found or not readable (beside this exe, or ROTTRFramework\\cutscenes\\"
                                  L"frame.png under its folder; or --frame <png>)\n");
            PauseIfOwnConsole();
            return 1;
        }
        frame = PictureFrame::Resize(frame, width, height);
        std::wprintf(L"frame: %ls\n\n", framePath.c_str());
    }
    std::vector<fs::path> files;
    for (const fs::path& input : inputs) {
        std::error_code ec;
        if (fs::is_directory(input, ec)) {
            for (const auto& entry : fs::directory_iterator(input, ec))
                if (entry.is_regular_file(ec) && IsImage(entry.path())) files.push_back(entry.path());
        } else {
            files.push_back(input);
        }
    }
    int made = 0, failed = 0;
    for (const fs::path& file : files) {
        PictureFrame::Image picture;
        if (!Decode(file, picture)) {
            std::wprintf(L"  %ls: not a JPEG or PNG it can read\n", file.filename().c_str());
            ++failed;
            continue;
        }
        const int sourceWidth = picture.width, sourceHeight = picture.height;
        for (std::size_t i = 3; i < picture.rgba.size(); i += 4) picture.rgba[i] = 255;   // (a screenshot: opaque)
        picture = PictureFrame::Fit(picture, width, height);
        if (useFrame) PictureFrame::ApplyFrame(picture, frame);
        const fs::path folder = outFolder.empty() ? file.parent_path() / L"framed" : outFolder;
        std::error_code ec;
        fs::create_directories(folder, ec);
        const fs::path out = folder / (file.stem().wstring() + L".png");
        if (WritePng(out, picture)) {
            std::wprintf(L"  %ls (%dx%d) -> %ls\n", file.filename().c_str(), sourceWidth, sourceHeight, out.c_str());
            ++made;
        } else {
            std::wprintf(L"  %ls: could not write %ls\n", file.filename().c_str(), out.c_str());
            ++failed;
        }
    }
    std::wprintf(L"\n%d made%ls\n", made, failed ? (L", " + std::to_wstring(failed) + L" failed").c_str() : L"");
    PauseIfOwnConsole();
    return failed ? 1 : 0;
}
