#pragma once
// ============================================================================
// FillNoise.hpp  —  Generative noise-based infill patterns
// ============================================================================
// Provides three procedural infill patterns that replace static grids with
// mathematically generated geometry:
//
//   FillPerlin        — Perlin-noise iso-contour infill.  Traces the zero-
//                       crossing of a 2-D Perlin field at the given density,
//                       producing organic, flowing lines.
//
//   FillReactionDiffusion — Gray-Scott reaction-diffusion infill.  Simulates
//                       a chemical reaction on a 2-D grid and traces the
//                       resulting Turing-pattern iso-contours.  Produces
//                       spots, stripes, and labyrinthine structures depending
//                       on the feed/kill parameters.
//
//   FillVoronoiNoise  — Worley (cellular) noise infill.  Traces the Voronoi
//                       cell boundaries of a random point set, producing a
//                       foam-like structure with excellent isotropy.
//
// All three classes inherit from FillBase and plug into the existing infill
// pipeline.  They are registered in FillBase.cpp alongside the existing
// patterns.
//
// Design notes
// ─────────────
// • No external library is required.  Perlin and Worley noise are implemented
//   from scratch using only <cmath> and <random>.
// • The reaction-diffusion solver uses a simple explicit Euler step on a
//   fixed-resolution grid; it is fast enough for interactive use.
// • All patterns respect the bounding polygon of the region and clip output
//   polylines to the fill area using the existing Clipper infrastructure.
// • Thread-safe: each Fill object is independent; no shared mutable state.
// ============================================================================

#ifndef slic3r_FillNoise_hpp_
#define slic3r_FillNoise_hpp_

#include "FillBase.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "../Point.hpp"

#include <array>
#include <vector>
#include <cmath>
#include <random>
#include <cstdint>

namespace Slic3r {

// ============================================================================
// Shared noise utilities
// ============================================================================

namespace NoiseUtil {

// Classic improved Perlin noise (Ken Perlin, 2002).
// Returns a value in approximately [-1, 1].
float perlin2(float x, float y, uint32_t seed = 0);

// Fractal Brownian Motion: sum of octaves of Perlin noise.
float fbm2(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed = 0);

// Worley (cellular) noise — returns distance to nearest feature point.
// Returns value in [0, 1].
float worley2(float x, float y, float freq, uint32_t seed = 0);

// Iso-contour extraction: march over a grid and collect line segments where
// the scalar field crosses zero.  Returns a list of polylines (in scaled
// integer coordinates).
Polylines iso_contours(const std::function<float(float, float)>& field,
                       const BoundingBox&                         bbox_scaled,
                       float                                      grid_step_mm,
                       float                                      iso_value = 0.f);

} // namespace NoiseUtil

// ============================================================================
// FillPerlin
// ============================================================================

class FillPerlin : public FillBase {
public:
    ~FillPerlin() override = default;

    FillBase* clone() const override { return new FillPerlin(*this); }

    // Infill parameters exposed to the user
    int   octaves    = 4;
    float lacunarity = 2.0f;
    float gain       = 0.5f;
    float frequency  = 1.5f;   // base spatial frequency (cycles per mm)
    float iso_step   = 0.35f;  // spacing between iso-contours (normalised units)

protected:
    void _fill_surface_single(const FillParams&  params,
                               unsigned int       thickness_layers,
                               const std::pair<float, Point>& direction,
                               ExPolygon          expolygon,
                               Polylines&         polylines_out) override;
};

// ============================================================================
// FillReactionDiffusion
// ============================================================================

class FillReactionDiffusion : public FillBase {
public:
    ~FillReactionDiffusion() override = default;

    FillBase* clone() const override { return new FillReactionDiffusion(*this); }

    // Gray-Scott parameters — change these to get different patterns:
    //   feed=0.055, kill=0.062  → spots
    //   feed=0.035, kill=0.065  → stripes
    //   feed=0.025, kill=0.060  → labyrinth
    float feed_rate  = 0.055f;
    float kill_rate  = 0.062f;
    int   iterations = 3000;
    int   grid_res   = 128;    // simulation grid resolution (NxN)

protected:
    void _fill_surface_single(const FillParams&  params,
                               unsigned int       thickness_layers,
                               const std::pair<float, Point>& direction,
                               ExPolygon          expolygon,
                               Polylines&         polylines_out) override;

private:
    // Run the Gray-Scott simulation and return the V-field as a flat array.
    std::vector<float> run_simulation(int N, float f, float k, int steps, uint32_t seed) const;
};

// ============================================================================
// FillVoronoiNoise
// ============================================================================

class FillVoronoiNoise : public FillBase {
public:
    ~FillVoronoiNoise() override = default;

    FillBase* clone() const override { return new FillVoronoiNoise(*this); }

    float frequency = 2.0f;   // Voronoi cell density (cells per mm)
    float threshold = 0.15f;  // iso-value for cell-boundary tracing

protected:
    void _fill_surface_single(const FillParams&  params,
                               unsigned int       thickness_layers,
                               const std::pair<float, Point>& direction,
                               ExPolygon          expolygon,
                               Polylines&         polylines_out) override;
};

} // namespace Slic3r

#endif // slic3r_FillNoise_hpp_
