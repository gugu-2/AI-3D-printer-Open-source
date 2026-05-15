// ============================================================================
// FillNoise.cpp  —  Generative noise-based infill patterns
// ============================================================================
#include "FillNoise.hpp"
#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Surface.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace Slic3r {

// ============================================================================
// NoiseUtil implementation
// ============================================================================

namespace NoiseUtil {

// ── Permutation table (Perlin 2002) ─────────────────────────────────────────

static uint8_t perm_base[256] = {
    151,160,137, 91, 90, 15,131, 13,201, 95, 96, 53,194,233,  7,225,
    140, 36,103, 30, 69,142,  8, 99, 37,240, 21, 10, 23,190,  6,148,
    247,120,234, 75,  0, 26,197, 62, 94,252,219,203,117, 35, 11, 32,
     57,177, 33, 88,237,149, 56, 87,174, 20,125,136,171,168, 68,175,
     74,165, 71,134,139, 48, 27,166, 77,146,158,231, 83,111,229,122,
     60,211,133,230,220,105, 92, 41, 55, 46,245, 40,244,102,143, 54,
     65, 25, 63,161,  1,216, 80, 73,209, 76,132,187,208, 89, 18,169,
    200,196,135,130,116,188,159, 86,164,100,109,198,173,186,  3, 64,
     52,217,226,250,124,123,  5,202, 38,147,118,126,255, 82, 85,212,
    207,206, 59,227, 47, 16, 58, 17,182,189, 28, 42,223,183,170,213,
    119,248,152,  2, 44,154,163, 70,221,153,101,155,167, 43,172,  9,
    129, 22, 39,253, 19, 98,108,110, 79,113,224,232,178,185,112,104,
    218,246, 97,228,251, 34,242,193,238,210,144, 12,191,179,162,241,
     81, 51,145,235,249, 14,239,107, 49,192,214, 31,181,199,106,157,
    184, 84,204,176,115,121, 50, 45,127,  4,150,254,138,236,205, 93,
    222,114, 67, 29, 24, 72,243,141,128,195, 78, 66,215, 61,156,180
};

static uint8_t make_perm(int i, uint32_t seed)
{
    // XOR-shift the base permutation with the seed for variety
    return perm_base[i & 255] ^ uint8_t((seed >> (i & 7)) & 0xffu);
}

static float grad2(int hash, float x, float y)
{
    const int h = hash & 7;
    const float u = h < 4 ? x : y;
    const float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
static float lerp(float a, float b, float t) { return a + t * (b - a); }

float perlin2(float x, float y, uint32_t seed)
{
    const int X = int(std::floor(x)) & 255;
    const int Y = int(std::floor(y)) & 255;
    x -= std::floor(x);
    y -= std::floor(y);
    const float u = fade(x), v = fade(y);
    const int A  = (make_perm(X,     seed) + Y) & 255;
    const int B  = (make_perm(X + 1, seed) + Y) & 255;
    const int AA = make_perm(A,     seed);
    const int AB = make_perm(A + 1, seed);
    const int BA = make_perm(B,     seed);
    const int BB = make_perm(B + 1, seed);
    return lerp(lerp(grad2(AA, x,     y    ), grad2(BA, x - 1.f, y    ), u),
                lerp(grad2(AB, x,     y - 1.f), grad2(BB, x - 1.f, y - 1.f), u), v);
}

float fbm2(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed)
{
    float val = 0.f, amp = 0.5f, freq = 1.f;
    for (int i = 0; i < octaves; ++i) {
        val  += amp * perlin2(x * freq, y * freq, seed + uint32_t(i * 7919));
        freq *= lacunarity;
        amp  *= gain;
    }
    return val;
}

float worley2(float x, float y, float freq, uint32_t seed)
{
    x *= freq;
    y *= freq;
    const int xi = int(std::floor(x));
    const int yi = int(std::floor(y));
    float min_d2 = 1e30f;
    for (int dx = -2; dx <= 2; ++dx) {
        for (int dy = -2; dy <= 2; ++dy) {
            const uint32_t h = uint32_t((xi + dx) * 1619 + (yi + dy) * 31337) ^ seed;
            const float px = float(xi + dx) + float((h * 2654435761u) & 0xffffu) / 65536.f;
            const float py = float(yi + dy) + float((h * 2246822519u) & 0xffffu) / 65536.f;
            const float d2 = (x - px) * (x - px) + (y - py) * (y - py);
            if (d2 < min_d2)
                min_d2 = d2;
        }
    }
    return std::sqrt(std::max(min_d2, 0.f));
}

// ── Marching squares iso-contour extraction ──────────────────────────────────

Polylines iso_contours(const std::function<float(float, float)>& field,
                       const BoundingBox&                         bbox_scaled,
                       float                                      grid_step_mm,
                       float                                      iso_value)
{
    const double step = scale_(double(grid_step_mm));
    const int    nx   = std::max(2, int(std::ceil(double(bbox_scaled.max.x() - bbox_scaled.min.x()) / step)) + 1);
    const int    ny   = std::max(2, int(std::ceil(double(bbox_scaled.max.y() - bbox_scaled.min.y()) / step)) + 1);

    // Sample the field on a grid
    std::vector<float> grid(size_t(nx) * size_t(ny));
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            const float wx = float(unscale<double>(bbox_scaled.min.x()) + ix * double(grid_step_mm));
            const float wy = float(unscale<double>(bbox_scaled.min.y()) + iy * double(grid_step_mm));
            grid[size_t(iy) * size_t(nx) + size_t(ix)] = field(wx, wy) - iso_value;
        }
    }

    // Marching squares — collect line segments
    // Edge interpolation helper
    auto interp = [&](int ix0, int iy0, int ix1, int iy1) -> Point {
        const float v0 = grid[size_t(iy0) * size_t(nx) + size_t(ix0)];
        const float v1 = grid[size_t(iy1) * size_t(nx) + size_t(ix1)];
        const float t  = (std::abs(v1 - v0) < 1e-12f) ? 0.5f : std::clamp(-v0 / (v1 - v0), 0.f, 1.f);
        const double x = unscale<double>(bbox_scaled.min.x()) + (ix0 + t * float(ix1 - ix0)) * double(grid_step_mm);
        const double y = unscale<double>(bbox_scaled.min.y()) + (iy0 + t * float(iy1 - iy0)) * double(grid_step_mm);
        return Point(scale_(x), scale_(y));
    };

    // Collect raw segments
    using Seg = std::pair<Point, Point>;
    std::vector<Seg> segs;
    segs.reserve(size_t(nx * ny / 4));

    for (int iy = 0; iy + 1 < ny; ++iy) {
        for (int ix = 0; ix + 1 < nx; ++ix) {
            const float v00 = grid[size_t(iy    ) * size_t(nx) + size_t(ix    )];
            const float v10 = grid[size_t(iy    ) * size_t(nx) + size_t(ix + 1)];
            const float v01 = grid[size_t(iy + 1) * size_t(nx) + size_t(ix    )];
            const float v11 = grid[size_t(iy + 1) * size_t(nx) + size_t(ix + 1)];
            const int   idx = (v00 < 0.f ? 1 : 0) | (v10 < 0.f ? 2 : 0) | (v11 < 0.f ? 4 : 0) | (v01 < 0.f ? 8 : 0);
            if (idx == 0 || idx == 15)
                continue;
            // Lookup table: pairs of edges (0=bottom,1=right,2=top,3=left)
            static const int8_t lut[16][4] = {
                {-1,-1,-1,-1}, {0,3,-1,-1}, {0,1,-1,-1}, {1,3,-1,-1},
                {1,2,-1,-1},   {0,1,2,3},   {0,2,-1,-1}, {2,3,-1,-1},
                {2,3,-1,-1},   {0,2,-1,-1}, {0,3,1,2},   {1,2,-1,-1},
                {1,3,-1,-1},   {0,1,-1,-1}, {0,3,-1,-1}, {-1,-1,-1,-1}
            };
            auto edge_pt = [&](int e) -> Point {
                switch (e) {
                case 0: return interp(ix, iy,     ix + 1, iy    );
                case 1: return interp(ix + 1, iy, ix + 1, iy + 1);
                case 2: return interp(ix, iy + 1, ix + 1, iy + 1);
                case 3: return interp(ix, iy,     ix,     iy + 1);
                default: return Point(0, 0);
                }
            };
            const int8_t* row = lut[idx];
            if (row[0] >= 0 && row[1] >= 0)
                segs.emplace_back(edge_pt(row[0]), edge_pt(row[1]));
            if (row[2] >= 0 && row[3] >= 0)
                segs.emplace_back(edge_pt(row[2]), edge_pt(row[3]));
        }
    }

    // Chain segments into polylines using a simple endpoint hash
    Polylines out;
    if (segs.empty())
        return out;

    // Build adjacency: endpoint → segment index
    const coord_t snap = coord_t(scale_(double(grid_step_mm) * 0.6));
    auto snap_pt = [&](const Point& p) -> Point {
        return Point((p.x() / snap) * snap, (p.y() / snap) * snap);
    };

    std::unordered_map<int64_t, std::vector<size_t>> ep_map;
    ep_map.reserve(segs.size() * 2);
    auto key = [](const Point& p) -> int64_t {
        return (int64_t(uint32_t(p.x())) << 32) | uint32_t(p.y());
    };
    for (size_t i = 0; i < segs.size(); ++i) {
        ep_map[key(snap_pt(segs[i].first))].push_back(i);
        ep_map[key(snap_pt(segs[i].second))].push_back(i);
    }

    std::vector<bool> used(segs.size(), false);
    for (size_t start = 0; start < segs.size(); ++start) {
        if (used[start])
            continue;
        Polyline pl;
        pl.points.push_back(segs[start].first);
        pl.points.push_back(segs[start].second);
        used[start] = true;
        // Extend forward
        for (;;) {
            const Point& tail = pl.points.back();
            const auto   it   = ep_map.find(key(snap_pt(tail)));
            if (it == ep_map.end())
                break;
            bool extended = false;
            for (size_t si : it->second) {
                if (used[si])
                    continue;
                used[si] = true;
                if (snap_pt(segs[si].first) == snap_pt(tail))
                    pl.points.push_back(segs[si].second);
                else
                    pl.points.push_back(segs[si].first);
                extended = true;
                break;
            }
            if (!extended)
                break;
        }
        if (pl.points.size() >= 2)
            out.push_back(std::move(pl));
    }
    return out;
}

} // namespace NoiseUtil

// ============================================================================
// FillPerlin
// ============================================================================

void FillPerlin::_fill_surface_single(const FillParams&              params,
                                       unsigned int                   /*thickness_layers*/,
                                       const std::pair<float, Point>& /*direction*/,
                                       ExPolygon                      expolygon,
                                       Polylines&                     polylines_out)
{
    const BoundingBox bb = get_extents(expolygon);
    if (bb.size().x() <= 0 || bb.size().y() <= 0)
        return;

    const float density = std::clamp(params.density, 0.05f, 1.f);
    const float spacing_mm = float(this->spacing) * SCALING_FACTOR;
    const float grid_step  = std::max(0.15f, spacing_mm * 0.5f);

    // Number of iso-contours to trace: more density → more contours
    const int n_iso = std::max(1, int(density * 8.f));
    const uint32_t seed = uint32_t(std::hash<float>{}(float(bb.min.x()) + float(bb.min.y())));

    Polylines raw;
    for (int k = 0; k < n_iso; ++k) {
        const float iso_val = -0.8f + float(k) * (1.6f / float(std::max(n_iso - 1, 1)));
        auto field = [&](float wx, float wy) -> float {
            return NoiseUtil::fbm2(wx * frequency, wy * frequency, octaves, lacunarity, gain, seed) - iso_val;
        };
        Polylines contours = NoiseUtil::iso_contours(field, bb, grid_step, 0.f);
        append(raw, std::move(contours));
    }

    // Clip to fill region
    Polylines clipped = intersection_pl(raw, expolygon);
    append(polylines_out, std::move(clipped));
}

// ============================================================================
// FillReactionDiffusion
// ============================================================================

std::vector<float> FillReactionDiffusion::run_simulation(int N, float f, float k, int steps, uint32_t seed) const
{
    const int sz = N * N;
    std::vector<float> U(size_t(sz), 1.f);
    std::vector<float> V(size_t(sz), 0.f);

    // Seed with small random perturbations in the centre
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.f, 0.05f);
    const int cx = N / 2, cy = N / 2, r = N / 8;
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            if (x >= 0 && x < N && y >= 0 && y < N) {
                U[size_t(y * N + x)] = 0.5f + dist(rng);
                V[size_t(y * N + x)] = 0.25f + dist(rng);
            }
        }
    }

    std::vector<float> nU(size_t(sz)), nV(size_t(sz));
    const float Du = 0.2097f, Dv = 0.1050f, dt = 1.f;

    for (int step = 0; step < steps; ++step) {
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const int xm = (x - 1 + N) % N, xp = (x + 1) % N;
                const int ym = (y - 1 + N) % N, yp = (y + 1) % N;
                const float u = U[size_t(y * N + x)];
                const float v = V[size_t(y * N + x)];
                const float lap_u = U[size_t(y * N + xm)] + U[size_t(y * N + xp)] +
                                    U[size_t(ym * N + x)] + U[size_t(yp * N + x)] - 4.f * u;
                const float lap_v = V[size_t(y * N + xm)] + V[size_t(y * N + xp)] +
                                    V[size_t(ym * N + x)] + V[size_t(yp * N + x)] - 4.f * v;
                const float uvv = u * v * v;
                nU[size_t(y * N + x)] = std::clamp(u + dt * (Du * lap_u - uvv + f * (1.f - u)), 0.f, 1.f);
                nV[size_t(y * N + x)] = std::clamp(v + dt * (Dv * lap_v + uvv - (f + k) * v), 0.f, 1.f);
            }
        }
        std::swap(U, nU);
        std::swap(V, nV);
    }
    return V; // return the V-field (activator)
}

void FillReactionDiffusion::_fill_surface_single(const FillParams&              params,
                                                  unsigned int                   /*thickness_layers*/,
                                                  const std::pair<float, Point>& /*direction*/,
                                                  ExPolygon                      expolygon,
                                                  Polylines&                     polylines_out)
{
    const BoundingBox bb = get_extents(expolygon);
    if (bb.size().x() <= 0 || bb.size().y() <= 0)
        return;

    const int N = std::clamp(grid_res, 32, 256);
    const uint32_t seed = uint32_t(std::hash<float>{}(float(bb.min.x()) * 1.3f + float(bb.min.y()) * 0.7f));
    std::vector<float> V = run_simulation(N, feed_rate, kill_rate, iterations, seed);

    // Build a field function that bilinearly interpolates the V-grid
    const double bbw = double(bb.max.x() - bb.min.x());
    const double bbh = double(bb.max.y() - bb.min.y());
    auto field = [&](float wx, float wy) -> float {
        const double px = (scale_(double(wx)) - double(bb.min.x())) / bbw * double(N - 1);
        const double py = (scale_(double(wy)) - double(bb.min.y())) / bbh * double(N - 1);
        const int    ix = std::clamp(int(px), 0, N - 2);
        const int    iy = std::clamp(int(py), 0, N - 2);
        const float  tx = float(px - double(ix));
        const float  ty = float(py - double(iy));
        const float  v00 = V[size_t(iy * N + ix)];
        const float  v10 = V[size_t(iy * N + ix + 1)];
        const float  v01 = V[size_t((iy + 1) * N + ix)];
        const float  v11 = V[size_t((iy + 1) * N + ix + 1)];
        return (v00 * (1.f - tx) + v10 * tx) * (1.f - ty) + (v01 * (1.f - tx) + v11 * tx) * ty;
    };

    const float density = std::clamp(params.density, 0.05f, 1.f);
    const float spacing_mm = float(this->spacing) * SCALING_FACTOR;
    const float grid_step  = std::max(0.15f, spacing_mm * 0.5f);
    const int   n_iso      = std::max(1, int(density * 5.f));

    Polylines raw;
    for (int k = 0; k < n_iso; ++k) {
        const float iso_val = 0.1f + float(k) * (0.6f / float(std::max(n_iso, 1)));
        auto shifted = [&](float wx, float wy) { return field(wx, wy) - iso_val; };
        Polylines contours = NoiseUtil::iso_contours(shifted, bb, grid_step, 0.f);
        append(raw, std::move(contours));
    }

    Polylines clipped = intersection_pl(raw, expolygon);
    append(polylines_out, std::move(clipped));
}

// ============================================================================
// FillVoronoiNoise
// ============================================================================

void FillVoronoiNoise::_fill_surface_single(const FillParams&              params,
                                             unsigned int                   /*thickness_layers*/,
                                             const std::pair<float, Point>& /*direction*/,
                                             ExPolygon                      expolygon,
                                             Polylines&                     polylines_out)
{
    const BoundingBox bb = get_extents(expolygon);
    if (bb.size().x() <= 0 || bb.size().y() <= 0)
        return;

    const float density    = std::clamp(params.density, 0.05f, 1.f);
    const float spacing_mm = float(this->spacing) * SCALING_FACTOR;
    const float grid_step  = std::max(0.15f, spacing_mm * 0.5f);
    const uint32_t seed    = uint32_t(std::hash<float>{}(float(bb.min.x()) * 2.1f + float(bb.min.y()) * 1.7f));

    const int n_iso = std::max(1, int(density * 6.f));

    Polylines raw;
    for (int k = 0; k < n_iso; ++k) {
        const float iso_val = threshold + float(k) * (0.5f / float(std::max(n_iso, 1)));
        auto field = [&](float wx, float wy) -> float {
            return NoiseUtil::worley2(wx, wy, frequency, seed) - iso_val;
        };
        Polylines contours = NoiseUtil::iso_contours(field, bb, grid_step, 0.f);
        append(raw, std::move(contours));
    }

    Polylines clipped = intersection_pl(raw, expolygon);
    append(polylines_out, std::move(clipped));
}

} // namespace Slic3r
