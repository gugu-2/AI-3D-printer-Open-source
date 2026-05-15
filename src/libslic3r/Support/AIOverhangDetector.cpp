#include "AIOverhangDetector.hpp"

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../TriangleMesh.hpp"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace Slic3r {
namespace AI {

namespace {

static constexpr uint32_t kForestMagic   = 0x52464f31u; // 'RF1'
static constexpr uint32_t kForestVersion = 2u;

inline void append_u32(std::vector<uint8_t>& buf, uint32_t v)
{
    buf.push_back(uint8_t(v & 0xffu));
    buf.push_back(uint8_t((v >> 8) & 0xffu));
    buf.push_back(uint8_t((v >> 16) & 0xffu));
    buf.push_back(uint8_t((v >> 24) & 0xffu));
}

inline void append_f32(std::vector<uint8_t>& buf, float v)
{
    static_assert(sizeof(float) == 4, "float size");
    uint32_t u;
    std::memcpy(&u, &v, 4);
    append_u32(buf, u);
}

inline bool read_u32(const uint8_t* data, size_t size, size_t& off, uint32_t& out)
{
    if (off + 4 > size)
        return false;
    out = uint32_t(data[off]) | (uint32_t(data[off + 1]) << 8) | (uint32_t(data[off + 2]) << 16) | (uint32_t(data[off + 3]) << 24);
    off += 4;
    return true;
}

inline bool read_i32(const uint8_t* data, size_t size, size_t& off, int32_t& out)
{
    uint32_t u;
    if (!read_u32(data, size, off, u))
        return false;
    out = int32_t(u);
    return true;
}

inline bool read_f32(const uint8_t* data, size_t size, size_t& off, float& out)
{
    uint32_t u;
    if (!read_u32(data, size, off, u))
        return false;
    std::memcpy(&out, &u, 4);
    return true;
}

inline float expolygon_perimeter_scaled(const ExPolygon& ex)
{
    double L = ex.contour.length();
    for (const Polygon& h : ex.holes)
        L += h.length();
    return float(L);
}

// Heuristic post-adjustment inspired by SupportSpotsGenerator::Params (bridge_distance, sharp features).
static float refine_probability_support_spots(float p, const FeatureVec& f)
{
    constexpr float kBridgeMm = 16.f; // SupportSpotsGenerator::Params::bridge_distance
    float           q         = p;
    if (f[11] > 0.5f)
        q = std::max(q, 0.58f);
    if (f[10] >= 0.75f * kBridgeMm)
        q += 0.12f;
    if (f[9] < 0.12f && f[3] > 40.f)
        q += 0.08f;
    if (f[8] < 0.08f && f[0] > 2.f)
        q += 0.06f;
    return std::clamp(q, 0.f, 1.f);
}

static indexed_triangle_set mesh_world_from_object(const PrintObject& po)
{
    TriangleMesh tm = po.model_object()->mesh();
    tm.transform(po.trafo_centered(), false);
    return tm.its;
}

static bool expolygon_touches_sharp_tail(const ExPolygon& region, const Layer* layer)
{
    if (!layer)
        return false;
    for (const ExPolygon& st : layer->sharp_tails) {
        if (!intersection_ex(region, st).empty())
            return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// DecisionTree
// ---------------------------------------------------------------------------

float DecisionTree::gini(const std::vector<int>& labels)
{
    if (labels.empty())
        return 0.f;
    int n1 = 0;
    for (int v : labels)
        n1 += v;
    float p = float(n1) / float(labels.size());
    return 1.f - p * p - (1.f - p) * (1.f - p);
}

bool DecisionTree::best_split(const std::vector<TrainingSample>& samples,
                                const std::vector<int>&            indices,
                                const std::vector<int>&            feature_subset,
                                int&                               best_feat,
                                float&                             best_thresh,
                                float&                             best_gain)
{
    std::vector<int> labels;
    labels.reserve(indices.size());
    for (int idx : indices)
        labels.push_back(samples[idx].label);
    const float parent_gini = gini(labels);
    best_gain = -1.f;

    for (int feat : feature_subset) {
        std::vector<std::pair<float, int>> pairs;
        pairs.reserve(indices.size());
        for (int idx : indices)
            pairs.emplace_back(samples[idx].features[size_t(feat)], samples[idx].label);
        std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        for (size_t s = 0; s + 1 < pairs.size(); ++s) {
            if (pairs[s].first == pairs[s + 1].first)
                continue;
            const float thr = 0.5f * (pairs[s].first + pairs[s + 1].first);
            std::vector<int> left_l, right_l;
            left_l.reserve(s + 1);
            right_l.reserve(pairs.size() - s - 1);
            for (size_t i = 0; i <= s; ++i)
                left_l.push_back(pairs[i].second);
            for (size_t i = s + 1; i < pairs.size(); ++i)
                right_l.push_back(pairs[i].second);
            if (left_l.empty() || right_l.empty())
                continue;
            const float n  = float(labels.size());
            const float gl = gini(left_l);
            const float gr = gini(right_l);
            const float gain = parent_gini - (float(left_l.size()) / n) * gl - (float(right_l.size()) / n) * gr;
            if (gain > best_gain + 1e-7f) {
                best_gain   = gain;
                best_feat   = feat;
                best_thresh = thr;
            }
        }
    }
    return best_gain > 1e-7f;
}

int DecisionTree::build_node(const std::vector<TrainingSample>& samples,
                               const std::vector<int>&            indices,
                               int                                depth,
                               int                                max_depth,
                               int                                min_samples_split,
                               std::mt19937&                      rng,
                               int                                max_features)
{
    float sum = 0.f;
    for (int idx : indices)
        sum += float(samples[idx].label);
    const float mean_l = indices.empty() ? 0.f : sum / float(indices.size());

    if (indices.size() < size_t(min_samples_split) || depth >= max_depth) {
        TreeNode leaf;
        leaf.is_leaf    = true;
        leaf.leaf_value = mean_l;
        m_nodes.push_back(leaf);
        return int(m_nodes.size() - 1);
    }

    bool all_same = true;
    for (size_t i = 1; i < indices.size(); ++i) {
        if (samples[indices[i]].label != samples[indices[0]].label) {
            all_same = false;
            break;
        }
    }
    if (all_same) {
        TreeNode leaf;
        leaf.is_leaf    = true;
        leaf.leaf_value = mean_l;
        m_nodes.push_back(leaf);
        return int(m_nodes.size() - 1);
    }

    std::vector<int> feat_idx(FEATURE_DIM);
    std::iota(feat_idx.begin(), feat_idx.end(), 0);
    std::shuffle(feat_idx.begin(), feat_idx.end(), rng);
    if (max_features < int(feat_idx.size()))
        feat_idx.resize(size_t(max_features));

    int   bf = -1;
    float thr = 0.f, gain = 0.f;
    if (!best_split(samples, indices, feat_idx, bf, thr, gain)) {
        TreeNode leaf;
        leaf.is_leaf    = true;
        leaf.leaf_value = mean_l;
        m_nodes.push_back(leaf);
        return int(m_nodes.size() - 1);
    }

    std::vector<int> left_i, right_i;
    for (int idx : indices) {
        if (samples[idx].features[size_t(bf)] <= thr)
            left_i.push_back(idx);
        else
            right_i.push_back(idx);
    }
    if (left_i.empty() || right_i.empty()) {
        TreeNode leaf;
        leaf.is_leaf    = true;
        leaf.leaf_value = mean_l;
        m_nodes.push_back(leaf);
        return int(m_nodes.size() - 1);
    }

    const int left  = build_node(samples, left_i, depth + 1, max_depth, min_samples_split, rng, max_features);
    const int right = build_node(samples, right_i, depth + 1, max_depth, min_samples_split, rng, max_features);

    TreeNode node;
    node.is_leaf     = false;
    node.feature_idx = bf;
    node.threshold   = thr;
    node.left_child  = left;
    node.right_child = right;
    m_nodes.push_back(node);
    return int(m_nodes.size() - 1);
}

void DecisionTree::fit(const std::vector<TrainingSample>& samples,
                         const std::vector<int>&            indices,
                         int                                max_depth,
                         int                                min_samples_split,
                         std::mt19937&                      rng,
                         int                                max_features)
{
    m_nodes.clear();
    if (indices.empty()) {
        TreeNode leaf;
        leaf.is_leaf    = true;
        leaf.leaf_value = 0.5f;
        m_nodes.push_back(leaf);
        return;
    }
    build_node(samples, indices, 0, max_depth, min_samples_split, rng, max_features);
}

float DecisionTree::predict(const FeatureVec& x) const
{
    if (m_nodes.empty())
        return 0.5f;
    int idx = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        const TreeNode& n = m_nodes[size_t(idx)];
        if (n.is_leaf)
            return n.leaf_value;
        const int f = n.feature_idx;
        if (f < 0 || f >= FEATURE_DIM)
            return n.leaf_value;
        idx = (x[size_t(f)] <= n.threshold) ? n.left_child : n.right_child;
        if (idx < 0 || idx >= int(m_nodes.size()))
            return 0.5f;
    }
    return 0.5f;
}

void DecisionTree::serialize(std::vector<uint8_t>& buf) const
{
    append_u32(buf, uint32_t(m_nodes.size()));
    for (const TreeNode& n : m_nodes) {
        append_u32(buf, uint32_t(int32_t(n.feature_idx)));
        append_f32(buf, n.threshold);
        append_u32(buf, uint32_t(n.left_child));
        append_u32(buf, uint32_t(n.right_child));
        append_f32(buf, n.leaf_value);
        append_u32(buf, n.is_leaf ? 1u : 0u);
    }
}

bool DecisionTree::deserialize(const uint8_t* data, size_t size, size_t& off)
{
    uint32_t nn = 0;
    if (!read_u32(data, size, off, nn))
        return false;
    m_nodes.clear();
    m_nodes.resize(size_t(nn));
    for (size_t i = 0; i < size_t(nn); ++i) {
        int32_t fi = 0, lc = 0, rc = 0;
        float   th = 0.f, lv = 0.f;
        uint32_t is_leaf_u = 0;
        if (!read_i32(data, size, off, fi) || !read_f32(data, size, off, th) || !read_i32(data, size, off, lc) ||
            !read_i32(data, size, off, rc) || !read_f32(data, size, off, lv) || !read_u32(data, size, off, is_leaf_u))
            return false;
        TreeNode& n     = m_nodes[i];
        n.feature_idx   = int(fi);
        n.threshold     = th;
        n.left_child    = int(lc);
        n.right_child   = int(rc);
        n.leaf_value    = lv;
        n.is_leaf       = (is_leaf_u != 0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// RandomForest
// ---------------------------------------------------------------------------

void RandomForest::fit(const std::vector<TrainingSample>& samples,
                       int                                n_trees,
                       int                                max_depth,
                       int                                min_samples,
                       unsigned                           seed)
{
    m_trees.clear();
    if (samples.empty())
        return;
    std::mt19937 rng(seed);
    const int    max_feat = std::max(1, int(std::lround(std::sqrt(double(FEATURE_DIM)))));
    m_trees.reserve(size_t(n_trees));
    for (int t = 0; t < n_trees; ++t) {
        std::uniform_int_distribution<size_t> dist(0, samples.size() - 1);
        std::vector<int>                      boot;
        boot.reserve(samples.size());
        for (size_t i = 0; i < samples.size(); ++i)
            boot.push_back(int(dist(rng)));
        DecisionTree tree;
        tree.fit(samples, boot, max_depth, min_samples, rng, max_feat);
        m_trees.push_back(std::move(tree));
    }
}

float RandomForest::predict_proba(const FeatureVec& x) const
{
    if (m_trees.empty())
        return 0.5f;
    double s = 0.;
    for (const DecisionTree& tr : m_trees)
        s += double(tr.predict(x));
    return float(s / double(m_trees.size()));
}

int RandomForest::predict(const FeatureVec& x, float threshold) const
{
    return predict_proba(x) >= threshold ? 1 : 0;
}

std::vector<uint8_t> RandomForest::serialize() const
{
    std::vector<uint8_t> buf;
    append_u32(buf, kForestMagic);
    append_u32(buf, kForestVersion);
    append_u32(buf, uint32_t(m_trees.size()));
    for (const DecisionTree& tr : m_trees) {
        std::vector<uint8_t> part;
        tr.serialize(part);
        append_u32(buf, uint32_t(part.size()));
        buf.insert(buf.end(), part.begin(), part.end());
    }
    return buf;
}

bool RandomForest::deserialize(const std::vector<uint8_t>& data)
{
    size_t off = 0;
    uint32_t magic = 0, ver = 0, nt = 0;
    if (!read_u32(data.data(), data.size(), off, magic) || magic != kForestMagic || !read_u32(data.data(), data.size(), off, ver) ||
        ver != kForestVersion || !read_u32(data.data(), data.size(), off, nt))
        return false;
    m_trees.clear();
    m_trees.resize(size_t(nt));
    for (size_t ti = 0; ti < size_t(nt); ++ti) {
        uint32_t psz = 0;
        if (!read_u32(data.data(), data.size(), off, psz) || off + psz > data.size())
            return false;
        size_t sub = 0;
        if (!m_trees[ti].deserialize(data.data() + off, psz, sub) || sub != psz)
            return false;
        off += psz;
    }
    return off == data.size();
}

std::array<float, FEATURE_DIM> RandomForest::feature_importances() const
{
    std::array<float, FEATURE_DIM> cnt{};
    for (const DecisionTree& tr : m_trees) {
        for (const TreeNode& n : tr.nodes()) {
            if (!n.is_leaf && n.feature_idx >= 0 && n.feature_idx < FEATURE_DIM)
                cnt[size_t(n.feature_idx)] += 1.f;
        }
    }
    float sum = 0.f;
    for (float c : cnt)
        sum += c;
    if (sum > 0.f) {
        for (float& c : cnt)
            c /= sum;
    }
    return cnt;
}

// ---------------------------------------------------------------------------
// OverhangFeatureExtractor
// ---------------------------------------------------------------------------

void OverhangFeatureExtractor::compute_overhang_angles(const ExPolygon&            region,
                                                        const indexed_triangle_set& its,
                                                        float                       layer_z_bottom_mm,
                                                        float                       layer_z_top_mm,
                                                        float&                      mean_angle_deg,
                                                        float&                      max_angle_deg)
{
    mean_angle_deg = 0.f;
    max_angle_deg  = 0.f;
    if (its.indices.empty())
        return;

    const BoundingBox bb = get_extents(region);
    const double      margin = scale_(0.5);
    const float       slab_lo = layer_z_bottom_mm - 0.15f;
    const float       slab_hi = layer_z_top_mm + 0.15f;

    std::vector<float> angles;
    angles.reserve(std::min(size_t(4096), its.indices.size()));

    for (const Vec3i32& tri : its.indices) {
        const Vec3f& a = its.vertices[size_t(tri(0))];
        const Vec3f& b = its.vertices[size_t(tri(1))];
        const Vec3f& c = its.vertices[size_t(tri(2))];
        const Vec3f  ctr = (a + b + c) * (1.f / 3.f);
        if (ctr.z() < slab_lo || ctr.z() > slab_hi)
            continue;
        const Point p(scale_(double(ctr.x())), scale_(double(ctr.y())));
        if (p.x() < bb.min.x() - coord_t(margin) || p.x() > bb.max.x() + coord_t(margin) || p.y() < bb.min.y() - coord_t(margin) ||
            p.y() > bb.max.y() + coord_t(margin))
            continue;
        if (!region.contains(p))
            continue;

        Vec3f n = its_face_normal(its, tri);
        const float len = n.norm();
        if (len < 1e-20f)
            continue;
        n /= len;
        const float w = std::clamp(-n.z(), 0.f, 1.f);
        const float ang_deg = float(std::acos(double(w)) * 180.0 / PI);
        if (n.z() > -0.02f)
            continue;
        angles.push_back(ang_deg);
    }

    if (angles.empty())
        return;
    max_angle_deg = *std::max_element(angles.begin(), angles.end());
    double acc = 0.;
    for (float v : angles)
        acc += double(v);
    mean_angle_deg = float(acc / double(angles.size()));
}

float OverhangFeatureExtractor::estimate_bridge_span(const ExPolygon& region)
{
    const BoundingBox bb = get_extents(region);
    const double      dx = double(bb.max.x() - bb.min.x()) * SCALING_FACTOR;
    const double      dy = double(bb.max.y() - bb.min.y()) * SCALING_FACTOR;
    return float(std::max(dx, dy));
}

float OverhangFeatureExtractor::lower_support_fraction(const ExPolygon& region, const Layer* lower_layer)
{
    if (!lower_layer)
        return 0.f;
    const ExPolygons inter = intersection_ex(region, lower_layer->lslices);
    double           supported = 0.;
    for (const ExPolygon& e : inter)
        supported += e.area();
    const double tot = region.area();
    if (tot <= 0.)
        return 0.f;
    return float(std::clamp(supported / tot, 0., 1.));
}

FeatureVec OverhangFeatureExtractor::extract(const ExPolygon&            overhang_region,
                                               const Layer*                current_layer,
                                               const Layer*                lower_layer,
                                               const indexed_triangle_set& its,
                                               double                      layer_height_mm,
                                               bool                        is_sharp_tail)
{
    FeatureVec f{};
    const double area_scaled = overhang_region.area();
    const float  area_mm2    = float(double(area_scaled) * SCALING_FACTOR * SCALING_FACTOR);
    const float  perim_mm    = float(double(expolygon_perimeter_scaled(overhang_region)) * SCALING_FACTOR);

    float mean_ang = 0.f, max_ang = 0.f;
    const float zb = current_layer ? float(current_layer->bottom_z()) : -1.e30f;
    const float zt = current_layer ? float(current_layer->print_z) : 1.e30f;
    compute_overhang_angles(overhang_region, its, zb, zt, mean_ang, max_ang);

    const float lower_sup = lower_support_fraction(overhang_region, lower_layer);
    const float support_ratio = float(std::clamp(1.0 - double(lower_sup), 0., 1.));

    BoundingBox bb = get_extents(overhang_region);
    const double dx = double(std::max(bb.max.x() - bb.min.x(), coord_t(1))) * SCALING_FACTOR;
    const double dy = double(std::max(bb.max.y() - bb.min.y(), coord_t(1))) * SCALING_FACTOR;
    const float  aspect = float(dx / dy);

    const float compact = (perim_mm > 1e-4f) ? float((4.f * float(PI) * area_mm2) / (perim_mm * perim_mm)) : 0.f;

    f[0]  = area_mm2;
    f[1]  = perim_mm;
    f[2]  = support_ratio;
    f[3]  = mean_ang;
    f[4]  = max_ang;
    f[5]  = float(layer_height_mm);
    f[6]  = current_layer ? float(current_layer->print_z) : 0.f;
    f[7]  = aspect;
    f[8]  = compact;
    f[9]  = lower_sup;
    f[10] = estimate_bridge_span(overhang_region);
    f[11] = (is_sharp_tail ? 1.f : 0.f);
    return f;
}

std::vector<FeatureVec> OverhangFeatureExtractor::extract_layer(const ExPolygons&             overhang_regions,
                                                                const Layer*                  current_layer,
                                                                const Layer*                  lower_layer,
                                                                const indexed_triangle_set&   its,
                                                                double                        layer_height_mm)
{
    std::vector<FeatureVec> out;
    out.reserve(overhang_regions.size());
    for (const ExPolygon& ex : overhang_regions) {
        const bool sharp = expolygon_touches_sharp_tail(ex, current_layer);
        out.push_back(extract(ex, current_layer, lower_layer, its, layer_height_mm, sharp));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SyntheticTrainingDataGenerator
// ---------------------------------------------------------------------------

std::vector<TrainingSample> SyntheticTrainingDataGenerator::generate(int n_samples, float support_angle, unsigned seed)
{
    std::mt19937                      rng(seed);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    std::vector<TrainingSample>      out;
    out.reserve(size_t(n_samples));

    for (int i = 0; i < n_samples; ++i) {
        TrainingSample s;
        s.features[0]  = std::exp(U(rng) * std::log(5000.f + 1.f)) - 1.f + 0.05f; // area mm2
        s.features[1]  = 4.f * std::sqrt(std::max(s.features[0], 0.01f)) * (0.8f + 0.4f * U(rng));
        s.features[9]  = U(rng);
        s.features[2]  = std::clamp(1.f - s.features[9], 0.f, 1.f);
        s.features[3]  = U(rng) * 70.f;
        s.features[4]  = std::max(s.features[3], U(rng) * 90.f);
        s.features[5]  = 0.08f + U(rng) * 0.32f;
        s.features[6]  = U(rng) * 200.f;
        s.features[7]  = 0.2f + U(rng) * 5.f;
        s.features[8]  = U(rng);
        s.features[10] = U(rng) * 40.f;
        s.features[11] = U(rng) < 0.08f ? 1.f : 0.f;

        const bool angle_rule = (s.features[3] > support_angle - 8.f) || (s.features[4] > support_angle + 5.f);
        const bool tail_rule  = (s.features[11] > 0.5f) && (s.features[0] > 1.f);
        const bool bridge_rule = s.features[10] > 14.f && s.features[9] < 0.2f;
        int        label       = (angle_rule || tail_rule || bridge_rule) ? 1 : 0;
        if (U(rng) < 0.04f)
            label = 1 - label;
        s.label = label;
        out.push_back(std::move(s));
    }
    return out;
}

// ---------------------------------------------------------------------------
// AIOverhangDetector
// ---------------------------------------------------------------------------

AIOverhangDetector::AIOverhangDetector(const Config& cfg) : m_config(cfg) {}

static std::vector<uint8_t> build_default_pretrained_blob()
{
    // Small forest for fast startup and unit tests; call train() for a larger model.
    std::vector<TrainingSample> samples = SyntheticTrainingDataGenerator::generate(600, 45.f, 31415);
    RandomForest                rf;
    rf.fit(samples, 10, 6, 4, 271828);
    return rf.serialize();
}

void AIOverhangDetector::init()
{
    if (m_config.use_pretrained_model) {
        static const std::vector<uint8_t> blob = build_default_pretrained_blob();
        if (!m_forest.deserialize(blob) && m_config.verbose) {
            // fall through to train
        }
    }
    if (!m_forest.is_trained()) {
        train(std::max(2000, m_config.n_trees * 40), 424242);
    }
}

bool AIOverhangDetector::load_pretrained_weights()
{
    static const std::vector<uint8_t> blob = build_default_pretrained_blob();
    return m_forest.deserialize(blob);
}

void AIOverhangDetector::train(int n_samples, unsigned seed)
{
    std::vector<TrainingSample> samples = SyntheticTrainingDataGenerator::generate(n_samples, m_config.classic_angle_deg, seed);
    m_forest.fit(samples, m_config.n_trees, m_config.max_depth, MIN_SAMPLES_SPLIT, seed ^ 0x9e3779b9u);
}

std::vector<uint8_t> AIOverhangDetector::serialize_model() const { return m_forest.serialize(); }

bool AIOverhangDetector::load_model(const std::vector<uint8_t>& data) { return m_forest.deserialize(data); }

ExPolygons AIOverhangDetector::classic_detect_layer(const Layer* layer, const Layer* lower_layer, float angle_deg) const
{
    if (!layer || !lower_layer)
        return {};
    const double layer_h = std::max(layer->height, EPSILON);
    const double thresh   = Geometry::deg2rad(std::clamp(angle_deg, 1.f, 89.f));
    const double overlap  = std::tan(Geometry::deg2rad(90.) - thresh) * layer_h;
    const float  off      = float(std::max(overlap, double(SCALING_FACTOR)));
    ExPolygons   lower_sup = offset_ex(lower_layer->lslices, scale_(off));
    return diff_ex(layer->lslices, lower_sup);
}

ExPolygons AIOverhangDetector::merge_hybrid(const std::vector<OverhangPrediction>& ai_preds,
                                             const ExPolygons&                      classic_regions,
                                             float                                  classic_weight) const
{
    ExPolygons ai_polys;
    for (const OverhangPrediction& p : ai_preds) {
        if (p.needs_support)
            ai_polys.push_back(p.region);
    }
    if (classic_weight <= 1e-6f)
        return ai_polys;
    if (classic_weight >= 1.f - 1e-6f)
        return union_ex(ai_polys, classic_regions);
    const float margin_mm = std::clamp(classic_weight * 8.f, 0.25f, 8.f);
    ExPolygons  grown     = offset_ex(ai_polys, scale_(double(margin_mm)));
    ExPolygons  classic_trim = intersection_ex(classic_regions, grown);
    return union_ex(ai_polys, classic_trim);
}

std::vector<OverhangPrediction> AIOverhangDetector::detect_layer(const Layer*                layer,
                                                                 const Layer*                lower_layer,
                                                                 const indexed_triangle_set& its) const
{
    std::vector<OverhangPrediction> preds;
    if (!layer || !m_forest.is_trained())
        return preds;

    ExPolygons candidates = !layer->loverhangs.empty() ? layer->loverhangs : classic_detect_layer(layer, lower_layer, m_config.classic_angle_deg);
    if (candidates.empty())
        return preds;

    const double lh = std::max(layer->height, EPSILON);
    preds.reserve(candidates.size());

    int lidx = int(layer->id());
    for (const ExPolygon& ex : candidates) {
        const bool sharp = expolygon_touches_sharp_tail(ex, layer);
        FeatureVec feat = OverhangFeatureExtractor::extract(ex, layer, lower_layer, its, lh, sharp);
        float      prob = m_forest.predict_proba(feat);
        prob              = refine_probability_support_spots(prob, feat);
        const bool need   = prob >= m_config.probability_threshold;
        OverhangPrediction p;
        p.region         = ex;
        p.probability    = prob;
        p.needs_support  = need;
        p.layer_idx      = lidx;
        p.z_mm           = float(layer->print_z);
        p.features       = feat;
        preds.push_back(std::move(p));
    }
    return preds;
}

OverhangPredictions AIOverhangDetector::detect(const PrintObject& object) const
{
    OverhangPredictions all;
    if (!m_forest.is_trained())
        return all;
    const indexed_triangle_set its = mesh_world_from_object(object);
    for (size_t i = 0; i < object.layer_count(); ++i) {
        const Layer* layer = object.get_layer(int(i));
        const Layer* lower = (i > 0) ? object.get_layer(int(i - 1)) : nullptr;
        auto         layer_preds = detect_layer(layer, lower, its);
        all.insert(all.end(), layer_preds.begin(), layer_preds.end());
    }
    return all;
}

ExPolygons AIOverhangDetector::get_support_regions(const PrintObject& object) const
{
    ExPolygons out;
    if (!m_forest.is_trained())
        return out;
    const indexed_triangle_set its = mesh_world_from_object(object);
    for (size_t i = 0; i < object.layer_count(); ++i) {
        const Layer* layer = object.get_layer(int(i));
        const Layer* lower = (i > 0) ? object.get_layer(int(i - 1)) : nullptr;
        auto         preds = detect_layer(layer, lower, its);
        if (m_config.hybrid_mode && lower) {
            ExPolygons classic = classic_detect_layer(layer, lower, m_config.classic_angle_deg);
            ExPolygons merged  = merge_hybrid(preds, classic, m_config.hybrid_classic_weight);
            append(out, merged);
        } else {
            for (const OverhangPrediction& p : preds)
                if (p.needs_support)
                    out.push_back(p.region);
        }
    }
    return out;
}

std::string AIOverhangDetector::feature_importance_report() const
{
    auto        imp = m_forest.feature_importances();
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4);
    for (int i = 0; i < FEATURE_DIM; ++i)
        oss << feature_name(i) << "=" << imp[size_t(i)] << (i + 1 < FEATURE_DIM ? ", " : "");
    return oss.str();
}

} // namespace AI
} // namespace Slic3r
