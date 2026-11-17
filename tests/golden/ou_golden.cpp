// ou-golden: image checks for the render regression ctests (tests/golden/render_tests.sh, docs/RENDERING.md "Testing").
//   ou-golden compare A B MAX_MEAN [MIN_PSNR]   mean |channel diff| (/255) <= MAX_MEAN and PSNR >= MIN_PSNR -> exit 0
//   ou-golden darker OFF ON MIN MAX              mean luminance drop OFF -> ON (/255) in [MIN, MAX], and the darkened pixels
//                                                outweigh the brightened ones 4:1 (ambient occlusion only removes light)
//   ou-golden bless IN OUT.png                   re-encode IN as an 8-bit RGB PNG (golden re-bless)
// A / B / IN are binary PPM (P6, maxval 255, as ou-game --screenshot writes) or 8-bit RGB / RGBA PNG (stored goldens).
// Exit 1 = check failed or sizes differ, 2 = usage / I/O error.
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool loadPpm(const std::vector<uint8_t>& data, Image& img) {
    std::string header(data.begin(), data.begin() + std::min<size_t>(data.size(), 64));
    char magic[3] = {};
    int maxval = 0, used = 0;
    if (std::sscanf(header.c_str(), "%2s %d %d %d%n", magic, &img.w, &img.h, &maxval, &used) != 4 || std::strcmp(magic, "P6") ||
        maxval != 255 || img.w <= 0 || img.h <= 0)
        return false;
    const size_t start = size_t(used) + 1, n = size_t(img.w) * img.h * 3; // one whitespace byte after maxval
    if (data.size() < start + n) return false;
    img.rgb.assign(data.begin() + std::ptrdiff_t(start), data.begin() + std::ptrdiff_t(start + n));
    return true;
}

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

bool loadPng(const std::vector<uint8_t>& data, Image& img) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() < 8 || std::memcmp(data.data(), sig, 8)) return false;
    std::vector<uint8_t> idat;
    int depth = 0, colour = 0, interlace = 0;
    for (size_t p = 8; p + 12 <= data.size();) {
        const uint32_t len = be32(&data[p]);
        if (p + 12 + len > data.size()) return false;
        const uint8_t* body = &data[p + 8];
        if (!std::memcmp(&data[p + 4], "IHDR", 4) && len >= 13) {
            img.w = int(be32(body)), img.h = int(be32(body + 4)), depth = body[8], colour = body[9], interlace = body[12];
        } else if (!std::memcmp(&data[p + 4], "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(&data[p + 4], "IEND", 4)) {
            break;
        }
        p += 12 + len;
    }
    if (img.w <= 0 || img.h <= 0 || depth != 8 || (colour != 2 && colour != 6) || interlace) return false;
    const size_t bpp = colour == 6 ? 4 : 3, stride = size_t(img.w) * bpp;
    std::vector<uint8_t> raw((stride + 1) * img.h);
    uLongf rawLen = uLongf(raw.size());
    if (uncompress(raw.data(), &rawLen, idat.data(), uLong(idat.size())) != Z_OK || rawLen != raw.size()) return false;
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    img.rgb.resize(size_t(img.w) * img.h * 3);
    for (int y = 0; y < img.h; ++y) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* in = &raw[y * (stride + 1) + 1];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= bpp ? cur[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
            int pred = 0;
            switch (filter) {
            case 0: pred = 0; break;
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: return false;
            }
            cur[x] = uint8_t(in[x] + pred);
        }
        for (int x = 0; x < img.w; ++x)
            std::memcpy(&img.rgb[(size_t(y) * img.w + x) * 3], &cur[x * bpp], 3);
        std::swap(prev, cur);
    }
    return true;
}

bool load(const char* path, Image& img) {
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> data{std::istreambuf_iterator<char>(f), {}};
    if (loadPng(data, img) || loadPpm(data, img)) return true;
    std::fprintf(stderr, "ou-golden: cannot read %s (P6 PPM or 8-bit RGB/RGBA PNG)\n", path);
    return false;
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    const uint32_t len = uint32_t(body.size());
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(len >> s));
    const size_t typeAt = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t crc = uint32_t(crc32(0, &out[typeAt], uInt(4 + body.size())));
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(crc >> s));
}

bool savePng(const char* path, const Image& img) {
    // Paeth filter on every row (smallest for rendered frames), zlib level 9.
    const size_t stride = size_t(img.w) * 3;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * img.h);
    for (int y = 0; y < img.h; ++y) {
        raw.push_back(4);
        const uint8_t* row = &img.rgb[y * stride];
        const uint8_t* up = y ? row - stride : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= 3 ? row[x - 3] : 0, b = up ? up[x] : 0, c = up && x >= 3 ? up[x - 3] : 0;
            raw.push_back(uint8_t(row[x] - paeth(a, b, c)));
        }
    }
    std::vector<uint8_t> z(compressBound(uLong(raw.size())));
    uLongf zLen = uLongf(z.size());
    if (compress2(z.data(), &zLen, raw.data(), uLong(raw.size()), 9) != Z_OK) return false;
    z.resize(zLen);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr(13, 0);
    for (int i = 0; i < 4; ++i) ihdr[i] = uint8_t(uint32_t(img.w) >> (24 - 8 * i)), ihdr[4 + i] = uint8_t(uint32_t(img.h) >> (24 - 8 * i));
    ihdr[8] = 8, ihdr[9] = 2; // 8-bit RGB, deflate, no filter method extensions, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    return bool(f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size())));
}

bool sameSize(const Image& a, const Image& b) {
    if (a.w == b.w && a.h == b.h) return true;
    std::fprintf(stderr, "ou-golden: size mismatch %dx%d vs %dx%d\n", a.w, a.h, b.w, b.h);
    return false;
}

double luma(const uint8_t* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

} // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    Image a, b;
    if (cmd == "bless" && argc == 4) {
        if (!load(argv[2], a)) return 2;
        if (!savePng(argv[3], a)) { std::fprintf(stderr, "ou-golden: cannot write %s\n", argv[3]); return 2; }
        std::printf("blessed %s (%dx%d)\n", argv[3], a.w, a.h);
        return 0;
    }
    if (cmd == "compare" && (argc == 5 || argc == 6)) {
        if (!load(argv[2], a) || !load(argv[3], b)) return 2;
        if (!sameSize(a, b)) return 1;
        const double maxMean = std::atof(argv[4]), minPsnr = argc == 6 ? std::atof(argv[5]) : 0.0;
        double sumAbs = 0, sumSq = 0;
        for (size_t i = 0; i < a.rgb.size(); ++i) {
            const int d = std::abs(int(a.rgb[i]) - int(b.rgb[i]));
            sumAbs += d, sumSq += double(d) * d;
        }
        const double n = double(a.rgb.size()), mean = sumAbs / n, mse = sumSq / n;
        const double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
        const bool ok = mean <= maxMean && psnr >= minPsnr;
        std::printf("%dx%d mean_abs %.3f PSNR %.2f dB | limits mean <= %.3f, PSNR >= %.1f -> %s\n", a.w, a.h, mean, psnr, maxMean,
                    minPsnr, ok ? "OK" : "DIFFERENT");
        return ok ? 0 : 1;
    }
    if (cmd == "darker" && argc == 6) {
        if (!load(argv[2], a) || !load(argv[3], b)) return 2;
        if (!sameSize(a, b)) return 1;
        const double lo = std::atof(argv[4]), hi = std::atof(argv[5]);
        double sum = 0, darker = 0, brighter = 0;
        for (size_t i = 0; i < a.rgb.size(); i += 3) {
            const double d = luma(&a.rgb[i]) - luma(&b.rgb[i]);
            sum += d;
            (d > 0 ? darker : brighter) += std::abs(d);
        }
        const double n = double(a.rgb.size() / 3), drop = sum / n;
        const bool ok = drop >= lo && drop <= hi && darker >= 4.0 * brighter;
        std::printf("%dx%d luminance drop %.4f (darkened %.4f, brightened %.4f) | limits [%.3f, %.3f], darkened >= 4x brightened -> %s\n",
                    a.w, a.h, drop, darker / n, brighter / n, lo, hi, ok ? "OK" : "OUT OF BOUNDS");
        return ok ? 0 : 1;
    }
    std::fprintf(stderr, "usage: ou-golden compare A B MAX_MEAN [MIN_PSNR] | darker OFF ON MIN MAX | bless IN OUT.png\n");
    return 2;
}
