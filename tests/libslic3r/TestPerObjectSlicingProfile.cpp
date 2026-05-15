#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PerObjectSlicingProfile.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("apply_print_preset_to_model_object merges keys", "[PerObjectProfile]")
{
    Model        model;
    ModelObject* mo = model.add_object();
    REQUIRE(mo != nullptr);

    DynamicPrintConfig preset;
    preset.set("layer_height", 0.37);
    preset.set("sparse_infill_density", 22.0);

    apply_print_preset_to_model_object(*mo, preset, true);
    REQUIRE(mo->config.has("layer_height"));
    REQUIRE(mo->config.opt_float("layer_height") == Approx(0.37));
    REQUIRE(mo->config.has("sparse_infill_density"));
    REQUIRE(mo->config.opt_float("sparse_infill_density") == Approx(22.0));
}
