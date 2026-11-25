#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// The Cutscene Viewer's pictures in the style of the game's own (Chapter Replay's level pictures, e.g. sovietbase:
// 512x256 DXT5 whose alpha is a brushed, spray-speckled rectangle with a drip, and whose colour is the clean picture
// darkened toward that edge, about rgb * (a/255)^2). A frame image holds that look: its alpha is the picture's alpha, its
// colour (grey) what the picture's colour is multiplied by (cutscenes\frame.png, 2048x1024: the sample's alpha scaled
// up 4x, Lanczos, with a little grain; grey = 255 * (a/255)^2). Used by cutscene_viewer.cpp: the screenshots in
// cutscenes\images framed when they have no alpha of their own.
namespace PictureFrame {
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;   // 8 bits per channel, sRGB colour, straight alpha
};

inline bool Opaque(const Image& image) {
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        if (image.rgba[i] != 255) return false;
    return true;
}

// The middle of the image with the shape aw:ah (the most of it that has that shape).
inline Image CropToAspect(const Image& src, int aw, int ah) {
    if (src.width <= 0 || src.height <= 0 || aw <= 0 || ah <= 0) return src;
    int cw = src.width, ch = src.height;
    if (std::int64_t(src.width) * ah > std::int64_t(src.height) * aw)
        cw = std::max(1, int((std::int64_t(src.height) * aw + ah / 2) / ah));
    else
        ch = std::max(1, int((std::int64_t(src.width) * ah + aw / 2) / aw));
    if (cw == src.width && ch == src.height) return src;
    const int cx = (src.width - cw) / 2, cy = (src.height - ch) / 2;
    Image out;
    out.width = cw;
    out.height = ch;
    out.rgba.resize(std::size_t(cw) * ch * 4);
    for (int y = 0; y < ch; ++y)
        std::copy_n(&src.rgba[(std::size_t(cy + y) * src.width + cx) * 4], std::size_t(cw) * 4,
                    &out.rgba[std::size_t(y) * cw * 4]);
    return out;
}

namespace detail {
struct Taps {
    int first = 0;
    std::vector<float> weights;
};

inline double Lanczos3(double x) {
    x = std::fabs(x);
    if (x < 1e-8) return 1.0;
    if (x >= 3.0) return 0.0;
    constexpr double kPi = 3.14159265358979323846;
    const double px = kPi * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

// Each output sample's source samples and weights (Lanczos-3, widened when shrinking; cut at the edges and the rest
// made to sum to 1, as PIL does).
inline std::vector<Taps> Weights(int srcSize, int dstSize) {
    std::vector<Taps> taps(std::size_t(std::max(0, dstSize)));
    const double scale = double(dstSize) / double(srcSize);
    const double stretch = std::max(1.0, 1.0 / scale), support = 3.0 * stretch;
    for (int d = 0; d < dstSize; ++d) {
        const double center = (d + 0.5) / scale;
        const int lo = std::max(0, int(std::floor(center - support))), hi = std::min(srcSize - 1, int(std::ceil(center + support)));
        Taps& t = taps[std::size_t(d)];
        t.first = lo;
        double sum = 0.0;
        for (int s = lo; s <= hi; ++s) {
            const double w = Lanczos3((s + 0.5 - center) / stretch);
            t.weights.push_back(float(w));
            sum += w;
        }
        if (sum != 0.0)
            for (float& w : t.weights) w = float(w / sum);
    }
    return taps;
}

inline std::uint8_t ToByte(float v) {
    return std::uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
}

inline float SrgbToLinear(std::uint8_t v) {
    static const std::vector<float> table = [] {
        std::vector<float> t(256);
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[std::size_t(i)] = float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table[v];
}

inline std::uint8_t LinearToSrgb(float v) {
    static const std::vector<std::uint8_t> table = [] {
        std::vector<std::uint8_t> t(4096);
        for (int i = 0; i < 4096; ++i) {
            const double c = i / 4095.0;
            const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
            t[std::size_t(i)] = std::uint8_t(std::clamp(s * 255.0 + 0.5, 0.0, 255.0));
        }
        return t;
    }();
    return table[std::size_t(std::clamp(int(v * 4095.0f + 0.5f), 0, 4095))];
}
}  // namespace detail

// Scaled to w x h (Lanczos-3, each channel on its own, on the stored sRGB values as image tools do).
inline Image Resize(const Image& src, int w, int h) {
    if (src.width == w && src.height == h) return src;
    Image out;
    if (src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0) return out;
    const auto across = detail::Weights(src.width, w), down = detail::Weights(src.height, h);
    std::vector<float> mid(std::size_t(w) * src.height * 4);   // (across first: src.height rows of w)
    for (int y = 0; y < src.height; ++y) {
        const std::uint8_t* row = &src.rgba[std::size_t(y) * src.width * 4];
        float* dst = &mid[std::size_t(y) * w * 4];
        for (int x = 0; x < w; ++x) {
            const detail::Taps& t = across[std::size_t(x)];
            float sum[4] = {0, 0, 0, 0};
            for (std::size_t k = 0; k < t.weights.size(); ++k) {
                const std::uint8_t* p = row + (std::size_t(t.first) + k) * 4;
                const float wk = t.weights[k];
                for (int c = 0; c < 4; ++c) sum[c] += wk * float(p[c]);
            }
            for (int c = 0; c < 4; ++c) dst[x * 4 + c] = sum[c];
        }
    }
    out.width = w;
    out.height = h;
    out.rgba.resize(std::size_t(w) * h * 4);
    std::vector<float> sum(std::size_t(w) * 4);
    for (int y = 0; y < h; ++y) {
        const detail::Taps& t = down[std::size_t(y)];
        std::fill(sum.begin(), sum.end(), 0.0f);
        for (std::size_t k = 0; k < t.weights.size(); ++k) {
            const float* row = &mid[(std::size_t(t.first) + k) * w * 4];
            const float wk = t.weights[k];
            for (std::size_t i = 0; i < sum.size(); ++i) sum[i] += wk * row[i];
        }
        std::uint8_t* dst = &out.rgba[std::size_t(y) * w * 4];
        for (std::size_t i = 0; i < sum.size(); ++i) dst[i] = detail::ToByte(sum[i]);
    }
    return out;
}

// Cropped from its middle to w:h, then scaled to w x h.
inline Image Fit(const Image& src, int w, int h) {
    return Resize(CropToAspect(src, w, h), w, h);
}

// Scaled to fit in w x h whole (its shape kept), in the middle, transparent around (the Model Viewer's grid pictures).
inline Image Contain(const Image& src, int w, int h) {
    Image out;
    if (src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0) return out;
    int sw = w, sh = int((std::int64_t(src.height) * w + src.width / 2) / src.width);
    if (sh > h) sh = h, sw = int((std::int64_t(src.width) * h + src.height / 2) / src.height);
    sw = std::clamp(sw, 1, w), sh = std::clamp(sh, 1, h);
    const Image scaled = Resize(src, sw, sh);
    out.width = w;
    out.height = h;
    out.rgba.assign(std::size_t(w) * h * 4, 0);
    const int x0 = (w - sw) / 2, y0 = (h - sh) / 2;
    for (int y = 0; y < sh; ++y)
        std::copy_n(&scaled.rgba[std::size_t(y) * sw * 4], std::size_t(sw) * 4, &out.rgba[(std::size_t(y0 + y) * w + x0) * 4]);
    return out;
}

// The frame's look on the picture (the frame scaled to the picture first when their sizes differ): the colour times
// the frame's (grey: 255 keeps it), the alpha the frame's.
inline void ApplyFrame(Image& image, const Image& frame) {
    if (image.width <= 0 || image.height <= 0 || frame.width <= 0 || frame.height <= 0) return;
    const Image scaled = frame.width == image.width && frame.height == image.height ? Image{} : Resize(frame, image.width, image.height);
    const Image& f = scaled.width ? scaled : frame;
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        for (int c = 0; c < 3; ++c)
            image.rgba[i + std::size_t(c)] = std::uint8_t((unsigned(image.rgba[i + std::size_t(c)]) * f.rgba[i + std::size_t(c)] + 127) / 255);
        image.rgba[i + 3] = f.rgba[i + 3];
    }
}

// Each level half the one before (the last both sides still at least 1), the colour averaged as light (sRGB), the alpha
// as it is.
inline std::vector<Image> MipChain(const Image& top, int levels) {
    std::vector<Image> chain;
    if (top.width <= 0 || top.height <= 0 || levels <= 0) return chain;
    chain.push_back(top);
    while (int(chain.size()) < levels) {
        const Image& a = chain.back();
        if (a.width < 2 && a.height < 2) break;
        Image b;
        b.width = std::max(1, a.width / 2);
        b.height = std::max(1, a.height / 2);
        b.rgba.resize(std::size_t(b.width) * b.height * 4);
        for (int y = 0; y < b.height; ++y)
            for (int x = 0; x < b.width; ++x) {
                const int x0 = std::min(x * 2, a.width - 1), x1 = std::min(x * 2 + 1, a.width - 1);
                const int y0 = std::min(y * 2, a.height - 1), y1 = std::min(y * 2 + 1, a.height - 1);
                const std::uint8_t* p[4] = {&a.rgba[(std::size_t(y0) * a.width + x0) * 4], &a.rgba[(std::size_t(y0) * a.width + x1) * 4],
                                            &a.rgba[(std::size_t(y1) * a.width + x0) * 4], &a.rgba[(std::size_t(y1) * a.width + x1) * 4]};
                std::uint8_t* d = &b.rgba[(std::size_t(y) * b.width + x) * 4];
                for (int c = 0; c < 3; ++c)
                    d[c] = detail::LinearToSrgb((detail::SrgbToLinear(p[0][c]) + detail::SrgbToLinear(p[1][c]) +
                                                 detail::SrgbToLinear(p[2][c]) + detail::SrgbToLinear(p[3][c])) * 0.25f);
                d[3] = std::uint8_t((unsigned(p[0][3]) + p[1][3] + p[2][3] + p[3][3] + 2) / 4);
            }
        chain.push_back(std::move(b));
    }
    return chain;
}
}  // namespace PictureFrame
