#include <catch2/catch_all.hpp>

#include "libslic3r/Support/AIOverhangDetector.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using namespace Slic3r::AI;

TEST_CASE("SyntheticTrainingDataGenerator produces labelled samples", "[AIOverhang]")
{
    auto samples = SyntheticTrainingDataGenerator::generate(200, 45.f, 7);
    REQUIRE(samples.size() == 200);
    int n0 = 0, n1 = 0;
    for (const auto& s : samples) {
        REQUIRE((s.label == 0 || s.label == 1));
        n0 += (s.label == 0);
        n1 += (s.label == 1);
    }
    REQUIRE(n0 > 0);
    REQUIRE(n1 > 0);
}

TEST_CASE("RandomForest fit predict and serialize roundtrip", "[AIOverhang]")
{
    auto                       samples = SyntheticTrainingDataGenerator::generate(800, 40.f, 11);
    RandomForest               rf;
    rf.fit(samples, 12, 6, 4, 99);
    REQUIRE(rf.is_trained());
    REQUIRE(rf.n_trees() == 12);

    float sum = 0.f;
    for (const auto& s : samples)
        sum += rf.predict_proba(s.features);
    REQUIRE(sum / float(samples.size()) > 0.f);
    REQUIRE(sum / float(samples.size()) < 1.f);

    std::vector<uint8_t> buf = rf.serialize();
    RandomForest           rf2;
    REQUIRE(rf2.deserialize(buf));
    REQUIRE(rf2.n_trees() == rf.n_trees());
    for (const auto& s : samples) {
        REQUIRE(rf2.predict_proba(s.features) == Approx(rf.predict_proba(s.features)).margin(1e-5f));
    }
}

TEST_CASE("OverhangFeatureExtractor on simple cube mesh", "[AIOverhang]")
{
    indexed_triangle_set its = its_make_cube(10., 10., 10.);
    ExPolygon            region;
    region.contour.points = {
        Point(scale_(1.), scale_(1.)),
        Point(scale_(9.), scale_(1.)),
        Point(scale_(9.), scale_(9.)),
        Point(scale_(1.), scale_(9.)),
    };

    FeatureVec f = OverhangFeatureExtractor::extract(region, nullptr, nullptr, its, 0.2, false);
    REQUIRE(f[0] > 0.f);
    REQUIRE(f[1] > 0.f);
    REQUIRE(f[5] == Approx(0.2f));
}

TEST_CASE("AIOverhangDetector init and forest ready", "[AIOverhang]")
{
    AIOverhangDetector::Config cfg;
    cfg.use_pretrained_model = true;
    cfg.verbose              = false;
    AIOverhangDetector         det(cfg);
    det.init();
    REQUIRE(det.is_ready());
    REQUIRE_FALSE(det.feature_importance_report().empty());
}
