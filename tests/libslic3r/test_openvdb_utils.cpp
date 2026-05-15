#include <catch2/catch.hpp>
#include "libslic3r/OpenVDBUtils.hpp"
#include <openvdb/openvdb.h>

TEST_CASE("Test apply_gradient_to_grid functionality", "[OpenVDBUtils]") {
    openvdb::initialize();
    
    // Create a mock float grid
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
    openvdb::FloatGrid::Accessor accessor = grid->getAccessor();
    
    // Fill a small region with active voxels
    for (int x = -5; x <= 5; ++x) {
        for (int y = -5; y <= 5; ++y) {
            for (int z = -5; z <= 5; ++z) {
                accessor.setValue(openvdb::Coord(x, y, z), 1.0f);
            }
        }
    }
    
    // The vector we want to project the gradient onto
    Slic3r::Vec3d dir_vector(0.0, 0.0, 1.0);
    
    // Apply our gradient function
    Slic3r::apply_gradient_to_grid(*grid, dir_vector);
    
    // Verify that the voxels have been modified based on their Z coordinate
    bool has_variation = false;
    float min_val = 1.0f;
    float max_val = 0.0f;
    
    for (openvdb::FloatGrid::ValueOnCIter iter = grid->cbeginValueOn(); iter; ++iter) {
        float val = *iter;
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
    }
    
    // The function std::abs(std::sin(projection / 10.0)) should produce values between 0 and 1
    // For z in [-5, 5], projection is in [-5, 5]
    // sin(-0.5) is ~ -0.479, abs is 0.479
    // sin(0) is 0
    // So min_val should be 0.0 and max_val should be ~0.479
    
    REQUIRE(min_val >= 0.0f);
    REQUIRE(max_val > 0.0f);
    REQUIRE(max_val <= 1.0f);
    
    // Check specific coordinates
    REQUIRE(accessor.getValue(openvdb::Coord(0, 0, 0)) == Approx(0.0f).margin(0.001f));
}
