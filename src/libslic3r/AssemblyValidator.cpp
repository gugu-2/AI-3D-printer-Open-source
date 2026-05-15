// ============================================================================
// AssemblyValidator.cpp  —  Print-in-place assembly validator
// ============================================================================
#include "AssemblyValidator.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMesh.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {
namespace Assembly {

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Compute face normals for all triangles
std::vector<Vec3f> compute_face_normals(const indexed_triangle_set& its)
{
    std::vector<Vec3f> normals;
    normals.reserve(its.indices.size());
    for (const Vec3i32& tri : its.indices)
        normals.push_back(its_face_normal(its, tri));
    return normals;
}

// Compute face centroids
std::vector<Vec3f> compute_face_centroids(const indexed_triangle_set& its)
{
    std::vector<Vec3f> centroids;
    centroids.reserve(its.indices.size());
    for (const Vec3i32& tri : its.indices) {
        const Vec3f& a = its.vertices[size_t(tri(0))];
        const Vec3f& b = its.vertices[size_t(tri(1))];
        const Vec3f& c = its.vertices[size_t(tri(2))];
        centroids.push_back((a + b + c) * (1.f / 3.f));
    }
    return centroids;
}

// Compute face areas
std::vector<float> compute_face_areas(const indexed_triangle_set& its)
{
    std::vector<float> areas;
    areas.reserve(its.indices.size());
    for (const Vec3i32& tri : its.indices) {
        const Vec3f& a = its.vertices[size_t(tri(0))];
        const Vec3f& b = its.vertices[size_t(tri(1))];
        const Vec3f& c = its.vertices[size_t(tri(2))];
        areas.push_back(0.5f * (b - a).cross(c - a).norm());
    }
    return areas;
}

// Cluster faces by normal direction (within angle_tol_deg)
std::vector<std::vector<size_t>> cluster_by_normal(const std::vector<Vec3f>& normals,
                                                     float                     angle_tol_deg)
{
    const float cos_tol = std::cos(float(angle_tol_deg * PI / 180.0));
    std::vector<bool>                 assigned(normals.size(), false);
    std::vector<std::vector<size_t>>  clusters;

    for (size_t i = 0; i < normals.size(); ++i) {
        if (assigned[i])
            continue;
        std::vector<size_t> cluster = {i};
        assigned[i] = true;
        for (size_t j = i + 1; j < normals.size(); ++j) {
            if (assigned[j])
                continue;
            const float dot = std::abs(normals[i].dot(normals[j]));
            if (dot >= cos_tol) {
                cluster.push_back(j);
                assigned[j] = true;
            }
        }
        if (cluster.size() >= 3)
            clusters.push_back(std::move(cluster));
    }
    return clusters;
}

// Compute centroid of a set of face centroids (weighted by area)
Vec3f weighted_centroid(const std::vector<size_t>&  indices,
                         const std::vector<Vec3f>&   centroids,
                         const std::vector<float>&   areas)
{
    Vec3f  sum  = Vec3f::Zero();
    float  wsum = 0.f;
    for (size_t i : indices) {
        sum  += centroids[i] * areas[i];
        wsum += areas[i];
    }
    return wsum > 1e-20f ? (sum / wsum) : Vec3f::Zero();
}

// Estimate the minimum distance between two sets of face centroids
float min_distance_between_clusters(const std::vector<size_t>&  a,
                                     const std::vector<size_t>&  b,
                                     const std::vector<Vec3f>&   centroids)
{
    float min_d = 1e30f;
    for (size_t i : a) {
        for (size_t j : b) {
            const float d = (centroids[i] - centroids[j]).norm();
            if (d < min_d)
                min_d = d;
        }
    }
    return min_d;
}

} // namespace

// ============================================================================
// AssemblyValidator
// ============================================================================

AssemblyValidator::AssemblyValidator(const ValidatorConfig& cfg) : m_cfg(cfg) {}

double AssemblyValidator::recommended_gap() const
{
    std::string ft = m_cfg.filament_type;
    std::transform(ft.begin(), ft.end(), ft.begin(), ::toupper);
    if (ft.find("PETG") != std::string::npos)
        return m_cfg.recommended_gap_petg_mm;
    if (ft.find("ABS") != std::string::npos || ft.find("ASA") != std::string::npos)
        return m_cfg.recommended_gap_abs_mm;
    return m_cfg.recommended_gap_pla_mm;
}

// ── Thin-wall pair detection ─────────────────────────────────────────────────

std::vector<AssemblyFeature> AssemblyValidator::detect_thin_wall_pairs(
    const indexed_triangle_set& its) const
{
    std::vector<AssemblyFeature> features;
    if (its.indices.empty())
        return features;

    const auto normals   = compute_face_normals(its);
    const auto centroids = compute_face_centroids(its);
    const auto areas     = compute_face_areas(its);

    // Cluster faces by normal direction
    auto clusters = cluster_by_normal(normals, float(m_cfg.cylinder_angle_tol_deg));

    // Find pairs of clusters with anti-parallel normals and small separation
    for (size_t ci = 0; ci < clusters.size(); ++ci) {
        for (size_t cj = ci + 1; cj < clusters.size(); ++cj) {
            const Vec3f ni = normals[clusters[ci][0]];
            const Vec3f nj = normals[clusters[cj][0]];
            const float dot = ni.dot(nj);
            // Anti-parallel: dot ≈ -1
            if (dot > -0.85f)
                continue;

            const float gap = min_distance_between_clusters(clusters[ci], clusters[cj], centroids);
            if (gap > float(m_cfg.clearance_threshold_mm * 3.0))
                continue;

            const Vec3f ctr_i = weighted_centroid(clusters[ci], centroids, areas);
            const Vec3f ctr_j = weighted_centroid(clusters[cj], centroids, areas);
            const Vec3f ctr   = (ctr_i + ctr_j) * 0.5f;

            AssemblyFeature feat;
            feat.type              = FeatureType::ThinWallPair;
            feat.centroid          = ctr.cast<double>();
            feat.current_gap_mm    = double(gap);
            feat.recommended_gap_mm = recommended_gap();
            feat.gap_ok            = feat.current_gap_mm >= feat.recommended_gap_mm;
            feat.face_indices.insert(feat.face_indices.end(), clusters[ci].begin(), clusters[ci].end());
            feat.face_indices.insert(feat.face_indices.end(), clusters[cj].begin(), clusters[cj].end());

            std::ostringstream desc;
            desc << "Thin-wall pair at (" << std::fixed << std::setprecision(1)
                 << ctr.x() << "," << ctr.y() << "," << ctr.z() << ") mm. "
                 << "Gap=" << std::setprecision(3) << gap << " mm, "
                 << "recommended=" << feat.recommended_gap_mm << " mm.";
            if (!feat.gap_ok)
                desc << " WARNING: gap too small — increase by "
                     << std::setprecision(3) << (feat.recommended_gap_mm - feat.current_gap_mm) << " mm.";
            feat.description = desc.str();
            features.push_back(std::move(feat));
        }
    }
    return features;
}

// ── Cylindrical joint detection ──────────────────────────────────────────────

std::vector<AssemblyFeature> AssemblyValidator::detect_cylindrical_joints(
    const indexed_triangle_set& its) const
{
    std::vector<AssemblyFeature> features;
    if (its.indices.empty())
        return features;

    const auto normals   = compute_face_normals(its);
    const auto centroids = compute_face_centroids(its);
    const auto areas     = compute_face_areas(its);

    // A cylindrical surface has normals that all lie in a plane perpendicular
    // to the cylinder axis.  We detect this by finding groups of faces whose
    // normals have a common perpendicular direction (the axis).
    // Simplified: find faces whose normals have near-zero Z component (vertical
    // cylinders) or near-zero X/Y component (horizontal cylinders).

    for (int axis = 0; axis < 3; ++axis) {
        std::vector<size_t> cyl_faces;
        for (size_t i = 0; i < normals.size(); ++i) {
            const float n_axis = std::abs(normals[i](axis));
            if (n_axis < 0.15f) // normal nearly perpendicular to axis
                cyl_faces.push_back(i);
        }
        if (cyl_faces.size() < 8)
            continue;

        // Compute centroid and bounding radius
        Vec3f ctr = Vec3f::Zero();
        float w   = 0.f;
        for (size_t i : cyl_faces) {
            ctr += centroids[i] * areas[i];
            w   += areas[i];
        }
        if (w < 1e-20f)
            continue;
        ctr /= w;

        float max_r = 0.f, min_r = 1e30f;
        for (size_t i : cyl_faces) {
            Vec3f d = centroids[i] - ctr;
            d(axis) = 0.f;
            const float r = d.norm();
            max_r = std::max(max_r, r);
            min_r = std::min(min_r, r);
        }
        const float wall_t = max_r - min_r;
        if (wall_t > float(m_cfg.thin_wall_threshold_mm * 3.f))
            continue; // not a thin cylinder wall

        AssemblyFeature feat;
        feat.type              = FeatureType::CylindricalJoint;
        feat.centroid          = ctr.cast<double>();
        feat.current_gap_mm    = double(wall_t);
        feat.recommended_gap_mm = recommended_gap();
        feat.gap_ok            = feat.current_gap_mm >= feat.recommended_gap_mm;
        feat.face_indices      = cyl_faces;

        static const char* axis_names[3] = {"X", "Y", "Z"};
        std::ostringstream desc;
        desc << "Cylindrical joint (axis=" << axis_names[axis] << ") at ("
             << std::fixed << std::setprecision(1)
             << ctr.x() << "," << ctr.y() << "," << ctr.z() << ") mm. "
             << "Wall thickness=" << std::setprecision(3) << wall_t << " mm.";
        feat.description = desc.str();
        features.push_back(std::move(feat));
    }
    return features;
}

// ── Snap-fit detection ───────────────────────────────────────────────────────

std::vector<AssemblyFeature> AssemblyValidator::detect_snap_fits(
    const indexed_triangle_set& its) const
{
    std::vector<AssemblyFeature> features;
    if (its.indices.empty())
        return features;

    const auto normals   = compute_face_normals(its);
    const auto centroids = compute_face_centroids(its);
    const auto areas     = compute_face_areas(its);

    // Snap-fits have a thin cantilever with a bulge at the tip.
    // Heuristic: find faces with large overhang angle (normal.z < -0.5)
    // that are near faces with a reversed normal (the hook face).
    std::vector<size_t> overhang_faces;
    for (size_t i = 0; i < normals.size(); ++i) {
        if (normals[i].z() < -0.5f && areas[i] < 5.f)
            overhang_faces.push_back(i);
    }
    if (overhang_faces.size() < 3)
        return features;

    // Cluster nearby overhang faces
    std::vector<bool> used(overhang_faces.size(), false);
    for (size_t ci = 0; ci < overhang_faces.size(); ++ci) {
        if (used[ci])
            continue;
        std::vector<size_t> cluster = {overhang_faces[ci]};
        used[ci] = true;
        for (size_t cj = ci + 1; cj < overhang_faces.size(); ++cj) {
            if (used[cj])
                continue;
            const float d = (centroids[overhang_faces[ci]] - centroids[overhang_faces[cj]]).norm();
            if (d < float(m_cfg.thin_wall_threshold_mm * 2.f)) {
                cluster.push_back(overhang_faces[cj]);
                used[cj] = true;
            }
        }
        if (cluster.size() < 3)
            continue;

        const Vec3f ctr = weighted_centroid(cluster, centroids, areas);
        AssemblyFeature feat;
        feat.type              = FeatureType::SnapFitHook;
        feat.centroid          = ctr.cast<double>();
        feat.current_gap_mm    = 0.0; // gap not directly measurable here
        feat.recommended_gap_mm = recommended_gap();
        feat.gap_ok            = true; // assume OK unless gap measured
        feat.face_indices      = cluster;

        std::ostringstream desc;
        desc << "Snap-fit hook at (" << std::fixed << std::setprecision(1)
             << ctr.x() << "," << ctr.y() << "," << ctr.z() << ") mm. "
             << "Ensure layer lines run along the cantilever for maximum strength.";
        feat.description = desc.str();
        features.push_back(std::move(feat));
    }
    return features;
}

// ── Living hinge detection ───────────────────────────────────────────────────

std::vector<AssemblyFeature> AssemblyValidator::detect_living_hinges(
    const indexed_triangle_set& its) const
{
    std::vector<AssemblyFeature> features;
    if (its.indices.empty())
        return features;

    const auto normals   = compute_face_normals(its);
    const auto centroids = compute_face_centroids(its);
    const auto areas     = compute_face_areas(its);

    // Living hinges are very thin flat regions.
    // Detect by finding faces with near-horizontal normals (|n.z| > 0.9)
    // and very small area, surrounded by larger faces.
    std::vector<size_t> flat_small;
    for (size_t i = 0; i < normals.size(); ++i) {
        if (std::abs(normals[i].z()) > 0.9f && areas[i] < float(m_cfg.living_hinge_max_mm * m_cfg.living_hinge_max_mm))
            flat_small.push_back(i);
    }
    if (flat_small.size() < 2)
        return features;

    // Cluster
    std::vector<bool> used(flat_small.size(), false);
    for (size_t ci = 0; ci < flat_small.size(); ++ci) {
        if (used[ci])
            continue;
        std::vector<size_t> cluster = {flat_small[ci]};
        used[ci] = true;
        for (size_t cj = ci + 1; cj < flat_small.size(); ++cj) {
            if (used[cj])
                continue;
            const float d = (centroids[flat_small[ci]] - centroids[flat_small[cj]]).norm();
            if (d < float(m_cfg.living_hinge_max_mm * 3.f)) {
                cluster.push_back(flat_small[cj]);
                used[cj] = true;
            }
        }
        if (cluster.size() < 2)
            continue;

        const Vec3f ctr = weighted_centroid(cluster, centroids, areas);
        AssemblyFeature feat;
        feat.type              = FeatureType::LivingHinge;
        feat.centroid          = ctr.cast<double>();
        feat.current_gap_mm    = 0.0;
        feat.recommended_gap_mm = 0.0;
        feat.gap_ok            = true;
        feat.face_indices      = cluster;

        std::ostringstream desc;
        desc << "Living hinge at (" << std::fixed << std::setprecision(1)
             << ctr.x() << "," << ctr.y() << "," << ctr.z() << ") mm. "
             << "Print with layer lines perpendicular to the hinge axis for flexibility. "
             << "Use PETG or TPU for best results.";
        feat.description = desc.str();
        features.push_back(std::move(feat));
    }
    return features;
}

// ── Orientation suggestion ───────────────────────────────────────────────────

OrientationSuggestion AssemblyValidator::suggest_orientation(
    const AssemblyFeature& feat, const indexed_triangle_set& its) const
{
    OrientationSuggestion sug;
    sug.score_before = printability_score(feat, its);
    sug.score_after  = sug.score_before;

    if (feat.type == FeatureType::ThinWallPair || feat.type == FeatureType::LivingHinge) {
        // Suggest rotating so the mating surfaces are vertical (parallel to Z)
        const Vec3d ctr = feat.centroid;
        sug.rotation_axis = Vec3d(0, 0, 1);
        sug.rotation_deg  = 0.0;
        sug.score_after   = std::min(1.0, sug.score_before + 0.2);
        sug.reason = "Orient mating surfaces vertically to avoid support material in the gap.";
    } else if (feat.type == FeatureType::SnapFitHook) {
        sug.rotation_axis = Vec3d(1, 0, 0);
        sug.rotation_deg  = 90.0;
        sug.score_after   = std::min(1.0, sug.score_before + 0.15);
        sug.reason = "Orient snap-fit cantilever horizontally so layer lines run along its length.";
    } else if (feat.type == FeatureType::CylindricalJoint) {
        sug.rotation_axis = Vec3d(0, 1, 0);
        sug.rotation_deg  = 0.0;
        sug.score_after   = sug.score_before;
        sug.reason = "Cylindrical joints print best with the axis vertical.";
    }
    return sug;
}

double AssemblyValidator::printability_score(const AssemblyFeature& feat,
                                              const indexed_triangle_set& /*its*/) const
{
    double score = 1.0;
    if (!feat.gap_ok) {
        const double deficit = feat.recommended_gap_mm - feat.current_gap_mm;
        score -= std::min(0.5, deficit / feat.recommended_gap_mm);
    }
    if (feat.type == FeatureType::SnapFitHook)
        score -= 0.1; // snap-fits are inherently harder to print
    if (feat.type == FeatureType::LivingHinge)
        score -= 0.05;
    return std::clamp(score, 0.0, 1.0);
}

// ── Public API ───────────────────────────────────────────────────────────────

ValidationResult AssemblyValidator::validate(const indexed_triangle_set& its) const
{
    ValidationResult result;
    result.all_ok = true;

    auto thin_walls = detect_thin_wall_pairs(its);
    auto cylinders  = detect_cylindrical_joints(its);
    auto snaps      = detect_snap_fits(its);
    auto hinges     = detect_living_hinges(its);

    for (auto& f : thin_walls) result.features.push_back(std::move(f));
    for (auto& f : cylinders)  result.features.push_back(std::move(f));
    for (auto& f : snaps)      result.features.push_back(std::move(f));
    for (auto& f : hinges)     result.features.push_back(std::move(f));

    double score_sum = 0.0;
    for (auto& feat : result.features) {
        feat.printability_score = printability_score(feat, its);
        feat.orientation_ok     = feat.printability_score >= 0.7;
        if (!feat.orientation_ok && m_cfg.suggest_orientation)
            feat.orientation_suggestion = suggest_orientation(feat, its);
        if (!feat.gap_ok || !feat.orientation_ok)
            result.all_ok = false;
        score_sum += feat.printability_score;
    }
    result.overall_score = result.features.empty() ? 1.0 : score_sum / double(result.features.size());

    std::ostringstream oss;
    oss << "Assembly validation: " << result.features.size() << " feature(s) detected. "
        << "Overall printability score: " << std::fixed << std::setprecision(2) << result.overall_score << ". "
        << (result.all_ok ? "All OK." : "Issues found — see feature list.");
    result.summary = oss.str();
    return result;
}

ValidationResult AssemblyValidator::validate(const PrintObject& object) const
{
    TriangleMesh tm = object.model_object()->mesh();
    tm.transform(object.trafo_centered(), false);
    return validate(tm.its);
}

ValidationResult AssemblyValidator::validate(const ModelObject& model_object) const
{
    return validate(model_object.mesh().its);
}

} // namespace Assembly
} // namespace Slic3r
