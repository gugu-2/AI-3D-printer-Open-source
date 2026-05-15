#pragma once
// ============================================================================
// AssemblyValidator.hpp  —  Print-in-place assembly validator
// ============================================================================
// Detects moving parts in a model (hinges, gears, snap-fits, living hinges)
// and validates that:
//   1. Clearance gaps between mating surfaces are within printable tolerances.
//   2. The orientation is optimal for printing without supports.
//   3. Layer lines are aligned to maximise joint strength.
//
// Detection strategy
// ──────────────────
// The validator analyses the mesh topology and geometry to identify:
//
//   • Thin-wall pairs: two nearly-parallel surfaces separated by a small gap
//     (< clearance_threshold_mm).  These are likely mating surfaces of a
//     hinge or slider.
//
//   • Cylindrical features: surfaces whose normals form a cone around a
//     common axis.  These are likely pin joints, axles, or gear bores.
//
//   • Snap-fit hooks: thin cantilever features with a bulge at the tip.
//     Detected by finding thin extrusions that widen at the free end.
//
//   • Living hinges: very thin (< 1 mm) flat regions connecting two larger
//     bodies.  Detected by finding narrow necks in the cross-section.
//
// For each detected feature the validator reports:
//   • Feature type and location.
//   • Current clearance gap (mm).
//   • Recommended clearance gap for the selected filament.
//   • Whether the current orientation is optimal.
//   • Suggested orientation (rotation axis + angle) if not optimal.
//   • Estimated printability score [0, 1].
// ============================================================================

#ifndef slic3r_AssemblyValidator_hpp_
#define slic3r_AssemblyValidator_hpp_

#include "TriangleMesh.hpp"
#include "Point.hpp"
#include "ExPolygon.hpp"

#include <string>
#include <vector>
#include <array>

namespace Slic3r {

class ModelObject;
class PrintObject;

namespace Assembly {

// ============================================================================
// Feature types
// ============================================================================

enum class FeatureType {
    ThinWallPair,    // two parallel surfaces with a small gap (hinge/slider)
    CylindricalJoint,// pin joint, axle, or gear bore
    SnapFitHook,     // cantilever snap-fit
    LivingHinge,     // thin flexible neck
    GearMesh,        // two gear-like cylindrical features in proximity
    Unknown
};

inline const char* feature_type_name(FeatureType t) {
    switch (t) {
    case FeatureType::ThinWallPair:     return "Thin-wall pair (hinge/slider)";
    case FeatureType::CylindricalJoint: return "Cylindrical joint (pin/axle)";
    case FeatureType::SnapFitHook:      return "Snap-fit hook";
    case FeatureType::LivingHinge:      return "Living hinge";
    case FeatureType::GearMesh:         return "Gear mesh";
    default:                            return "Unknown";
    }
}

// ============================================================================
// Orientation suggestion
// ============================================================================

struct OrientationSuggestion {
    Vec3d  rotation_axis;   // unit vector
    double rotation_deg;    // degrees to rotate
    double score_before;    // printability score before rotation [0,1]
    double score_after;     // estimated printability score after rotation [0,1]
    std::string reason;
};

// ============================================================================
// Detected feature
// ============================================================================

struct AssemblyFeature {
    FeatureType type;
    Vec3d       centroid;           // world-space centroid of the feature
    double      current_gap_mm;     // measured clearance gap
    double      recommended_gap_mm; // recommended gap for the filament
    bool        gap_ok;             // current_gap_mm >= recommended_gap_mm
    bool        orientation_ok;     // current orientation is printable
    double      printability_score; // [0, 1]; 1 = perfect
    std::string description;
    OrientationSuggestion orientation_suggestion; // only valid if !orientation_ok
    std::vector<size_t> face_indices; // mesh faces belonging to this feature
};

// ============================================================================
// Validation result
// ============================================================================

struct ValidationResult {
    std::vector<AssemblyFeature> features;
    bool   all_ok;
    double overall_score; // mean printability score
    std::string summary;
};

// ============================================================================
// Configuration
// ============================================================================

struct ValidatorConfig {
    double clearance_threshold_mm  = 0.5;  // gap smaller than this triggers a warning
    double recommended_gap_pla_mm  = 0.3;  // recommended gap for PLA
    double recommended_gap_petg_mm = 0.35; // recommended gap for PETG
    double recommended_gap_abs_mm  = 0.4;  // recommended gap for ABS
    double thin_wall_threshold_mm  = 1.2;  // wall thinner than this is a "thin wall"
    double living_hinge_max_mm     = 0.8;  // neck thinner than this is a living hinge
    double cylinder_angle_tol_deg  = 12.0; // tolerance for cylindrical feature detection
    std::string filament_type      = "PLA";
    bool   suggest_orientation     = true;
    bool   verbose                 = false;
};

// ============================================================================
// Validator
// ============================================================================

class AssemblyValidator {
public:
    explicit AssemblyValidator(const ValidatorConfig& cfg = ValidatorConfig{});

    // Validate a single mesh (model object mesh in world coordinates).
    ValidationResult validate(const indexed_triangle_set& its) const;

    // Validate a PrintObject (uses the transformed mesh).
    ValidationResult validate(const PrintObject& object) const;

    // Validate a ModelObject.
    ValidationResult validate(const ModelObject& model_object) const;

    const ValidatorConfig& config() const { return m_cfg; }

private:
    ValidatorConfig m_cfg;

    // Feature detection passes
    std::vector<AssemblyFeature> detect_thin_wall_pairs(const indexed_triangle_set& its) const;
    std::vector<AssemblyFeature> detect_cylindrical_joints(const indexed_triangle_set& its) const;
    std::vector<AssemblyFeature> detect_snap_fits(const indexed_triangle_set& its) const;
    std::vector<AssemblyFeature> detect_living_hinges(const indexed_triangle_set& its) const;

    // Orientation analysis
    OrientationSuggestion suggest_orientation(const AssemblyFeature& feat,
                                               const indexed_triangle_set& its) const;

    // Recommended gap for the configured filament
    double recommended_gap() const;

    // Printability score for a feature given its orientation
    double printability_score(const AssemblyFeature& feat,
                               const indexed_triangle_set& its) const;
};

} // namespace Assembly
} // namespace Slic3r

#endif // slic3r_AssemblyValidator_hpp_
