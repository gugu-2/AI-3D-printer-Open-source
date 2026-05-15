#include <catch2/catch_all.hpp>

#include "libslic3r/PrintFailureImageDetector.hpp"

using namespace Slic3r;

static std::vector<uint8_t> solid_rgb(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> out(size_t(w * h * 3));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t o = size_t((y * w + x) * 3);
            out[o]     = r;
            out[o + 1] = g;
            out[o + 2] = b;
        }
    return out;
}

TEST_CASE("PrintFailureImageHeuristic stable frames stay low", "[PrintFailureImage]")
{
    PrintFailureImageHeuristic det(96);
    std::vector<uint8_t>       buf = solid_rgb(64, 64, 40, 50, 60);
    Rgb8View                   v{ buf.data(), 64, 64, 64 * 3 };
    REQUIRE(det.analyze(v) == Approx(0.f).margin(0.01f));
    REQUIRE(det.analyze(v) == Approx(0.f).margin(0.01f));
    REQUIRE(det.analyze(v) == Approx(0.f).margin(0.01f));
}

TEST_CASE("PrintFailureImageHeuristic noise spike raises score", "[PrintFailureImage]")
{
    PrintFailureImageHeuristic det(96);
    std::vector<uint8_t>       calm = solid_rgb(48, 48, 120, 120, 120);
    Rgb8View                   v1{ calm.data(), 48, 48, 48 * 3 };
    (void)det.analyze(v1);
    std::vector<uint8_t> chaos = solid_rgb(48, 48, 0, 0, 0);
    for (size_t i = 0; i < chaos.size(); ++i)
        chaos[i] = uint8_t((i * 37 + 11) % 256);
    Rgb8View v2{ chaos.data(), 48, 48, 48 * 3 };
    const float s = det.analyze(v2);
    REQUIRE(s > 0.35f);
}
