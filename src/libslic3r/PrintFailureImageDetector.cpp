#include "PrintFailureImageDetector.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Slic3r {

PrintFailureImageHeuristic::PrintFailureImageHeuristic(int downsample_max_side)
    : m_max_side(std::max(32, downsample_max_side))
{}

void PrintFailureImageHeuristic::reset()
{
    m_prev_gray.clear();
    m_small_w = m_small_h = 0;
}

static void downscale_gray(const Rgb8View& in, int max_side, std::vector<uint8_t>& out_gray, int& ow, int& oh)
{
    if (!in.data || in.width <= 0 || in.height <= 0 || in.stride_bytes < in.width * 3) {
        out_gray.clear();
        ow = oh = 0;
        return;
    }
    const int max_dim = std::max(in.width, in.height);
    const int scale   = std::max(1, (max_dim + max_side - 1) / max_side);
    ow                = std::max(1, in.width / scale);
    oh                = std::max(1, in.height / scale);
    out_gray.resize(size_t(ow) * size_t(oh));
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            const int sx = std::min(in.width - 1, x * scale);
            const int sy = std::min(in.height - 1, y * scale);
            const uint8_t* p = in.data + sy * in.stride_bytes + sx * 3;
            const int      r = p[0], g = p[1], b = p[2];
            const int      Y = (77 * r + 150 * g + 29 * b + 128) >> 8;
            out_gray[size_t(y * ow + x)] = uint8_t(std::clamp(Y, 0, 255));
        }
    }
}

float PrintFailureImageHeuristic::analyze(const Rgb8View& frame)
{
    std::vector<uint8_t> cur;
    int                  cw = 0, ch = 0;
    downscale_gray(frame, m_max_side, cur, cw, ch);
    if (cur.empty())
        return 0.f;

    if (m_prev_gray.empty() || cw != m_small_w || ch != m_small_h) {
        m_prev_gray = std::move(cur);
        m_small_w   = cw;
        m_small_h   = ch;
        return 0.f;
    }

    const size_t n = cur.size();
    double       mean_diff = 0.;
    int          high      = 0;
    for (size_t i = 0; i < n; ++i) {
        const int d = std::abs(int(cur[i]) - int(m_prev_gray[i]));
        mean_diff += d;
        if (d > 40)
            ++high;
    }
    mean_diff /= double(n);
    const float frac_high = float(double(high) / double(n));

    m_prev_gray = std::move(cur);

    const float score_diff  = float(std::clamp((mean_diff - 6.0) / 28.0, 0., 1.));
    const float score_burst = float(std::clamp((frac_high - 0.06f) / 0.38f, 0.f, 1.f));
    return std::max(0.f, std::min(1.f, 0.45f * score_diff + 0.55f * score_burst));
}

} // namespace Slic3r
