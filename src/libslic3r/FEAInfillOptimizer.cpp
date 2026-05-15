// ============================================================================
// FEAInfillOptimizer.cpp  —  Fast FEA-guided per-region infill density
// ============================================================================
#include "FEAInfillOptimizer.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Print.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace Slic3r {
namespace FEA {

// ============================================================================
// FEAInfillOptimizer
// ============================================================================

FEAInfillOptimizer::FEAInfillOptimizer(const FEAConfig& cfg) : m_cfg(cfg) {}

// ── Occupancy mask ───────────────────────────────────────────────────────────

std::vector<bool> FEAInfillOptimizer::build_occupancy(const ExPolygons& slices,
                                                        const BoundingBox& bb,
                                                        int nx, int ny) const
{
    std::vector<bool> occ(size_t(nx) * size_t(ny), false);
    if (slices.empty() || nx <= 0 || ny <= 0)
        return occ;

    const double dx = double(bb.max.x() - bb.min.x()) / double(nx);
    const double dy = double(bb.max.y() - bb.min.y()) / double(ny);

    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            // Cell centre
            const Point ctr(
                bb.min.x() + coord_t((double(ix) + 0.5) * dx),
                bb.min.y() + coord_t((double(iy) + 0.5) * dy));
            for (const ExPolygon& ex : slices) {
                if (ex.contains(ctr)) {
                    occ[size_t(iy) * size_t(nx) + size_t(ix)] = true;
                    break;
                }
            }
        }
    }
    return occ;
}

// ── Conjugate-gradient solver ────────────────────────────────────────────────
// Sparse matrix stored as COO triplets; assembled into CSR on the fly.

bool FEAInfillOptimizer::cg_solve(const std::vector<std::array<float, 3>>& triplets,
                                   int                                       n,
                                   const std::vector<float>&                 b,
                                   std::vector<float>&                       x,
                                   int                                       max_iter,
                                   float                                     tol)
{
    // Build CSR from triplets (sum duplicates)
    std::vector<std::vector<std::pair<int, float>>> rows(size_t(n));
    for (const auto& t : triplets) {
        const int r = int(t[0]), c = int(t[1]);
        if (r < 0 || r >= n || c < 0 || c >= n)
            continue;
        rows[size_t(r)].emplace_back(c, t[2]);
    }
    // Merge duplicates
    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::pair<int, float>> merged;
        for (const auto& kv : row) {
            if (!merged.empty() && merged.back().first == kv.first)
                merged.back().second += kv.second;
            else
                merged.push_back(kv);
        }
        row = std::move(merged);
    }

    auto matvec = [&](const std::vector<float>& v, std::vector<float>& out) {
        std::fill(out.begin(), out.end(), 0.f);
        for (int r = 0; r < n; ++r)
            for (const auto& kv : rows[size_t(r)])
                out[size_t(r)] += kv.second * v[size_t(kv.first)];
    };

    x.assign(size_t(n), 0.f);
    std::vector<float> r_vec(b), p(b), Ap(size_t(n), 0.f);
    float rr = 0.f;
    for (float v : r_vec)
        rr += v * v;
    if (rr < tol * tol)
        return true;

    for (int iter = 0; iter < max_iter; ++iter) {
        matvec(p, Ap);
        float pAp = 0.f;
        for (int i = 0; i < n; ++i)
            pAp += p[size_t(i)] * Ap[size_t(i)];
        if (std::abs(pAp) < 1e-30f)
            break;
        const float alpha = rr / pAp;
        float rr_new = 0.f;
        for (int i = 0; i < n; ++i) {
            x[size_t(i)]     += alpha * p[size_t(i)];
            r_vec[size_t(i)] -= alpha * Ap[size_t(i)];
            rr_new += r_vec[size_t(i)] * r_vec[size_t(i)];
        }
        if (rr_new < tol * tol)
            return true;
        const float beta = rr_new / rr;
        rr = rr_new;
        for (int i = 0; i < n; ++i)
            p[size_t(i)] = r_vec[size_t(i)] + beta * p[size_t(i)];
    }
    return false;
}

// ── FEA stress solver ────────────────────────────────────────────────────────
// Uses a simplified plane-stress formulation with bilinear quad elements.
// Each occupied voxel is one quad element; empty voxels are skipped.
// DOFs: 2 per node (u, v displacement).

std::vector<float> FEAInfillOptimizer::solve_stress(const std::vector<bool>& occ,
                                                     int nx, int ny,
                                                     float cell_w_mm,
                                                     float cell_h_mm) const
{
    std::vector<float> stress(size_t(nx) * size_t(ny), 0.f);
    if (occ.empty())
        return stress;

    // Node numbering: (ix, iy) → node_id = iy*(nx+1) + ix
    const int n_nodes = (nx + 1) * (ny + 1);
    const int n_dof   = n_nodes * 2;

    // Plane-stress constitutive matrix D (3×3)
    const float E  = m_cfg.youngs_modulus;
    const float nu = m_cfg.poissons_ratio;
    const float c  = E / (1.f - nu * nu);
    // D = c * [[1, nu, 0], [nu, 1, 0], [0, 0, (1-nu)/2]]
    const float D00 = c, D01 = c * nu, D11 = c, D22 = c * (1.f - nu) * 0.5f;

    // Bilinear quad element stiffness (2×2 Gauss quadrature)
    // For a rectangle of size a×b, the element stiffness is assembled analytically.
    const float a = cell_w_mm * 0.5f; // half-width
    const float b = cell_h_mm * 0.5f; // half-height

    // Gauss points and weights for 2×2 quadrature
    static const float gp[2] = {-0.577350269f, 0.577350269f};
    static const float gw[2] = {1.f, 1.f};

    // Element DOF indices for a quad with nodes (0,0),(1,0),(1,1),(0,1)
    // Local node order: 0=(0,0), 1=(1,0), 2=(1,1), 3=(0,1)
    // Each node has 2 DOFs: u (x-disp) and v (y-disp)
    // ke is 8×8

    // Build global stiffness triplets and load vector
    std::vector<std::array<float, 3>> triplets;
    triplets.reserve(size_t(nx) * size_t(ny) * 64);
    std::vector<float> F(size_t(n_dof), 0.f);

    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            if (!occ[size_t(iy) * size_t(nx) + size_t(ix)])
                continue;

            // Global node indices for this element
            const int n0 = iy * (nx + 1) + ix;
            const int n1 = iy * (nx + 1) + ix + 1;
            const int n2 = (iy + 1) * (nx + 1) + ix + 1;
            const int n3 = (iy + 1) * (nx + 1) + ix;
            const int dofs[8] = {n0*2, n0*2+1, n1*2, n1*2+1, n2*2, n2*2+1, n3*2, n3*2+1};

            // Element stiffness ke[8][8] via 2×2 Gauss quadrature
            float ke[8][8] = {};
            for (int gi = 0; gi < 2; ++gi) {
                for (int gj = 0; gj < 2; ++gj) {
                    const float xi  = gp[gi], eta = gp[gj];
                    const float w   = gw[gi] * gw[gj];
                    // Shape function derivatives w.r.t. xi, eta
                    const float dN_dxi[4]  = {-(1.f-eta)*0.25f, (1.f-eta)*0.25f, (1.f+eta)*0.25f, -(1.f+eta)*0.25f};
                    const float dN_deta[4] = {-(1.f-xi)*0.25f, -(1.f+xi)*0.25f, (1.f+xi)*0.25f, (1.f-xi)*0.25f};
                    // Jacobian (rectangular element)
                    const float J11 = a, J22 = b; // J = diag(a, b)
                    const float detJ = J11 * J22;
                    // B matrix (3×8): strain-displacement
                    float B[3][8] = {};
                    for (int ni = 0; ni < 4; ++ni) {
                        const float dNdx = dN_dxi[ni] / J11;
                        const float dNdy = dN_deta[ni] / J22;
                        B[0][ni*2]   = dNdx;
                        B[1][ni*2+1] = dNdy;
                        B[2][ni*2]   = dNdy;
                        B[2][ni*2+1] = dNdx;
                    }
                    // ke += w * detJ * B^T * D * B
                    // D*B (3×8)
                    float DB[3][8] = {};
                    for (int r = 0; r < 8; ++r) {
                        DB[0][r] = D00 * B[0][r] + D01 * B[1][r];
                        DB[1][r] = D01 * B[0][r] + D11 * B[1][r];
                        DB[2][r] = D22 * B[2][r];
                    }
                    for (int r = 0; r < 8; ++r) {
                        for (int s = 0; s < 8; ++s) {
                            float val = 0.f;
                            for (int q = 0; q < 3; ++q)
                                val += B[q][r] * DB[q][s];
                            ke[r][s] += w * detJ * val;
                        }
                    }
                }
            }

            // Assemble into global triplets
            for (int r = 0; r < 8; ++r) {
                for (int s = 0; s < 8; ++s) {
                    if (std::abs(ke[r][s]) > 1e-20f)
                        triplets.push_back({float(dofs[r]), float(dofs[s]), ke[r][s]});
                }
            }

            // Body force (gravity + acceleration) distributed to nodes
            const float fx = m_cfg.body_force_x * cell_w_mm * cell_h_mm * 0.25f;
            const float fy = m_cfg.body_force_y * cell_w_mm * cell_h_mm * 0.25f;
            for (int ni = 0; ni < 4; ++ni) {
                F[size_t(dofs[ni*2])]   += fx;
                F[size_t(dofs[ni*2+1])] += fy;
            }
        }
    }

    // Apply Dirichlet BCs: fix bottom row (iy=0) in both DOFs
    for (int ix = 0; ix <= nx; ++ix) {
        const int n = ix; // iy=0
        for (int d = 0; d < 2; ++d) {
            const int dof = n * 2 + d;
            // Zero out row and column, set diagonal to 1, rhs to 0
            for (auto& t : triplets) {
                if (int(t[0]) == dof || int(t[1]) == dof)
                    t[2] = (int(t[0]) == dof && int(t[1]) == dof) ? 1.f : 0.f;
            }
            F[size_t(dof)] = 0.f;
        }
    }

    // Solve
    std::vector<float> U;
    cg_solve(triplets, n_dof, F, U, m_cfg.cg_max_iter, m_cfg.cg_tol);
    if (U.empty())
        return stress;

    // Compute von-Mises stress at element centroids
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            if (!occ[size_t(iy) * size_t(nx) + size_t(ix)])
                continue;
            const int n0 = iy * (nx + 1) + ix;
            const int n1 = iy * (nx + 1) + ix + 1;
            const int n2 = (iy + 1) * (nx + 1) + ix + 1;
            const int n3 = (iy + 1) * (nx + 1) + ix;
            const int dofs[8] = {n0*2, n0*2+1, n1*2, n1*2+1, n2*2, n2*2+1, n3*2, n3*2+1};

            // Evaluate B at centroid (xi=0, eta=0)
            const float dN_dxi[4]  = {-0.25f, 0.25f, 0.25f, -0.25f};
            const float dN_deta[4] = {-0.25f, -0.25f, 0.25f, 0.25f};
            float B[3][8] = {};
            for (int ni = 0; ni < 4; ++ni) {
                const float dNdx = dN_dxi[ni] / a;
                const float dNdy = dN_deta[ni] / b;
                B[0][ni*2]   = dNdx;
                B[1][ni*2+1] = dNdy;
                B[2][ni*2]   = dNdy;
                B[2][ni*2+1] = dNdx;
            }
            // Strain = B * u_e
            float eps[3] = {};
            for (int q = 0; q < 3; ++q)
                for (int r = 0; r < 8; ++r)
                    eps[q] += B[q][r] * U[size_t(dofs[r])];
            // Stress = D * eps
            const float sx  = D00 * eps[0] + D01 * eps[1];
            const float sy  = D01 * eps[0] + D11 * eps[1];
            const float txy = D22 * eps[2];
            // von-Mises
            const float vm = std::sqrt(std::max(0.f, sx*sx - sx*sy + sy*sy + 3.f*txy*txy));
            stress[size_t(iy) * size_t(nx) + size_t(ix)] = vm;
        }
    }
    return stress;
}

// ── Public API ───────────────────────────────────────────────────────────────

DensityMap FEAInfillOptimizer::compute_layer(const Layer& layer) const
{
    DensityMap map;
    if (layer.lslices.empty())
        return map;

    const BoundingBox bb = get_extents(layer.lslices);
    if (bb.size().x() <= 0 || bb.size().y() <= 0)
        return map;

    const int nx = m_cfg.grid_nx;
    const int ny = m_cfg.grid_ny;

    const float cell_w_mm = float(double(bb.max.x() - bb.min.x()) * SCALING_FACTOR / double(nx));
    const float cell_h_mm = float(double(bb.max.y() - bb.min.y()) * SCALING_FACTOR / double(ny));

    std::vector<bool>  occ    = build_occupancy(layer.lslices, bb, nx, ny);
    std::vector<float> stress = solve_stress(occ, nx, ny, cell_w_mm, cell_h_mm);

    // Normalise stress
    float max_s = 0.f;
    for (float s : stress)
        max_s = std::max(max_s, s);
    if (max_s < 1e-20f)
        max_s = 1.f;

    const double dx = double(bb.max.x() - bb.min.x()) / double(nx);
    const double dy = double(bb.max.y() - bb.min.y()) / double(ny);

    map.reserve(size_t(nx) * size_t(ny));
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            if (!occ[size_t(iy) * size_t(nx) + size_t(ix)])
                continue;
            const float s_norm = stress[size_t(iy) * size_t(nx) + size_t(ix)] / max_s;
            const float density = m_cfg.min_density +
                (m_cfg.max_density - m_cfg.min_density) *
                std::pow(std::clamp(s_norm, 0.f, 1.f), m_cfg.stress_exponent);

            DensityCell cell;
            cell.bbox = BoundingBox(
                Point(bb.min.x() + coord_t(double(ix) * dx),
                      bb.min.y() + coord_t(double(iy) * dy)),
                Point(bb.min.x() + coord_t(double(ix + 1) * dx),
                      bb.min.y() + coord_t(double(iy + 1) * dy)));
            cell.density = density;
            cell.stress  = s_norm;
            map.push_back(cell);
        }
    }
    return map;
}

std::vector<DensityMap> FEAInfillOptimizer::compute_object(const PrintObject& object) const
{
    std::vector<DensityMap> result;
    result.reserve(object.layer_count());
    for (size_t i = 0; i < object.layer_count(); ++i)
        result.push_back(compute_layer(*object.get_layer(int(i))));
    return result;
}

float FEAInfillOptimizer::density_at(const DensityMap& map, const Point& pt) const
{
    for (const DensityCell& cell : map) {
        if (cell.bbox.contains(pt))
            return cell.density;
    }
    return m_cfg.min_density;
}

std::string FEAInfillOptimizer::summary(const DensityMap& map) const
{
    if (map.empty())
        return "FEA: no cells";
    float min_d = 1.f, max_d = 0.f, sum_d = 0.f;
    float min_s = 1.f, max_s = 0.f;
    for (const DensityCell& c : map) {
        min_d = std::min(min_d, c.density);
        max_d = std::max(max_d, c.density);
        sum_d += c.density;
        min_s = std::min(min_s, c.stress);
        max_s = std::max(max_s, c.stress);
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << "FEA: cells=" << map.size()
        << " density=[" << min_d << "," << max_d << "] avg=" << (sum_d / float(map.size()))
        << " stress=[" << min_s << "," << max_s << "]";
    return oss.str();
}

} // namespace FEA
} // namespace Slic3r
