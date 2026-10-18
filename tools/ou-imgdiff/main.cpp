// ou-imgdiff: compare two binary PPM (P6, maxval 255) images, e.g. GL vs Vulkan screenshots.
// Prints mean absolute channel error, share of pixels with any channel off by > 16, and PSNR.
// Exit 0 if mean <= MAX_MEAN (default 4.0), 1 if larger or sizes differ, 2 on I/O errors.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Ppm {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
};

bool load(const char* path, Ppm& img) {
    std::ifstream f(path, std::ios::binary);
    std::string magic;
    int maxval = 0;
    if (!(f >> magic >> img.w >> img.h >> maxval) || magic != "P6" || maxval != 255 || img.w <= 0 || img.h <= 0)
        return false;
    f.get(); // single whitespace after the header
    img.rgb.resize(size_t(img.w) * img.h * 3);
    return bool(f.read(reinterpret_cast<char*>(img.rgb.data()), std::streamsize(img.rgb.size())));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ou-imgdiff a.ppm b.ppm [MAX_MEAN]\n");
        return 2;
    }
    Ppm a, b;
    if (!load(argv[1], a) || !load(argv[2], b)) {
        std::fprintf(stderr, "ou-imgdiff: cannot read P6 PPM %s or %s\n", argv[1], argv[2]);
        return 2;
    }
    if (a.w != b.w || a.h != b.h) {
        std::fprintf(stderr, "ou-imgdiff: size mismatch %dx%d vs %dx%d\n", a.w, a.h, b.w, b.h);
        return 1;
    }
    const double maxMean = argc > 3 ? std::atof(argv[3]) : 4.0;
    double sumAbs = 0, sumSq = 0;
    size_t bad = 0;
    for (size_t p = 0; p < a.rgb.size(); p += 3) {
        int worst = 0;
        for (size_t c = 0; c < 3; ++c) {
            int d = std::abs(int(a.rgb[p + c]) - int(b.rgb[p + c]));
            sumAbs += d;
            sumSq += double(d) * d;
            worst = std::max(worst, d);
        }
        bad += worst > 16;
    }
    const double n = double(a.rgb.size()), mean = sumAbs / n, mse = sumSq / n;
    const double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : INFINITY;
    std::printf("%dx%d mean_abs %.3f | pixels >16 off %.3f%% | PSNR %.2f dB | limit %.2f -> %s\n", a.w, a.h, mean,
                100.0 * double(bad) / (n / 3), psnr, maxMean, mean <= maxMean ? "OK" : "DIFFERENT");
    return mean <= maxMean ? 0 : 1;
}
