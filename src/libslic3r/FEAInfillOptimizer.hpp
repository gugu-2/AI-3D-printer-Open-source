#pragma once
// ============================================================================
// FEAInfillOptimizer.hpp  —  Fast FEA-guided per-region infill density
// ============================================================================
// Runs a lightweight 2-D plane-stress finite element analysis on each layer
// slice to estimate where stress concentrates, then maps the stress field to
// a local infill density override.
//
// Algorithm
// ─────────
//  1. For each layer, build a coarse voxel grid over the bounding box.
//  2. Assemble a simplified stiffness matrix using constant-strain triangles
//     (CST elements) derived from the voxel grid.
//  3. Apply a uniform body-force load (gravity + print acceleration) and
//     solve the linear system with a conjugate-gradient solver.
//  4. Compute the von-Mises stress at each element centroid.
//  5. Normalise the stress field and map it to a density in [min_density,
//     max_density] using a configurable transfer function.
//  6. Return a DensityMap: a list of (ExPolygon, density) pairs that the
//     infill generator can use to override the global density setting.
//
// The solver is intentionally coarse (default 32×32 grid) so it runs in
// well under one second per layer on a modern CPU.  Accuracy is sufficient
// to identify stress concentrations at corners, thin walls, and overhangs.
//
// No external FEA library is required — the entire solver is self-contained.
// ============================================================================

#ifndef slic3r_FEAInfillOptimizer_hpp_
#define slic3r_FEAInfillOptimizer_hpp_

#include "ExPolygon.hpp"
#include "Point.hpp"

#include <array>
#include <vector>
#include <functional>
#include <string>

namespace Slic3r {

class Layer;
class PrintObject;

namespace FEA {

// ============================================================================
// Configuration
// ============================================================================

struct FEAConfig {
    int   grid_nx          = 32;    // voxel grid columns
    int   grid_ny          = 32;    // voxel grid rows
    int   cg_max_iter      = 500;   // conjugate-gradient max iterations
    float cg_tol           = 1e-5f; // conjugate-gradient tolerance
    float youngs_modulus   = 3500.f;// MPa — typical PLA
    float poissons_ratio   = 0.36f; // PLA
    float body_force_x     = 0.f;   // N/mm² — horizontal acceleration load
    float body_force_y     = -9.81e-3f; // N/mm² — gravity (downward)
    float min_density      = 0.08f; // minimum infill density (fraction)
    float max_density      = 0.95f; // maximum infill density (fraction)
    float stress_exponent  = 0.5f;  // gamma for density = stress^gamma mapping
    bool  verbose          = false;
};

// ============================================================================
// Density map entry
// ============================================================================

struct DensityCell {
    BoundingBox bbox;    // cell bounding box in scaled coordinates
    float       density; // infill density in [min_density, max_density]
    float       stress;  // normalised von-Mises stress in [0, 1]
};

using DensityMap = std::vector<DensityCell>;

// ============================================================================
// FEA solver
// ============================================================================

class FEAInfillOptimizer {
public:
    explicit FEAInfillOptimizer(const FEAConfig& cfg = FEAConfig{});

    // Compute density map for a single layer.
    // Returns one DensityCell per voxel that lies inside the layer geometry.
    DensityMap compute_layer(const Layer& layer) const;

    // Compute density maps for all layers of a PrintObject.
    std::vector<DensityMap> compute_object(const PrintObject& object) const;

    // Query the density at a specific point (in scaled coordinates).
    // Returns cfg.min_density if the point is outside all cells.
    float density_at(const DensityMap& map, const Point& pt) const;

    // Human-readable summary of the stress distribution.
    std::string summary(const DensityMap& map) const;

    const FEAConfig& config() const { return m_cfg; }

private:
    FEAConfig m_cfg;

    // Build the voxel occupancy mask for a layer.
    // Returns a flat bool array of size nx*ny; true = inside geometry.
    std::vector<bool> build_occupancy(const ExPolygons& slices,
                                      const BoundingBox& bb,
                                      int nx, int ny) const;

    // Assemble and solve the FEA system.
    // Returns von-Mises stress per voxel (flat array, size nx*ny).
    std::vector<float> solve_stress(const std::vector<bool>& occ,
                                    int nx, int ny,
                                    float cell_w_mm, float cell_h_mm) const;

    // Conjugate-gradient solver for Ax = b.
    // A is stored as a sparse list of (row, col, value) triplets.
    static bool cg_solve(const std::vector<std::array<float, 3>>& triplets,
                         int                                       n,
                         const std::vector<float>&                 b,
                         std::vector<float>&                       x,
                         int                                       max_iter,
                         float                                     tol);
};

} // namespace FEA
} // namespace Slic3r

#endif // slic3r_FEAInfillOptimizer_hpp_
