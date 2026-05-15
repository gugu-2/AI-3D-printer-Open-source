#ifndef _TOPOLOGY_OPTIMIZED_TREE_SUPPORT_HPP_
#define _TOPOLOGY_OPTIMIZED_TREE_SUPPORT_HPP_

#include "TreeSupport.hpp"
#include <Eigen/Dense>

// Load analysis for topology optimization
struct LoadAnalysis {
    double total_load;                          // Total weight on node (cumulative)
    Eigen::Vector2d load_direction;             // Directional load vector (normalized)
    double stress_ratio;                        // Stress concentration (0-1)
    int critical_level;                         // 0=not critical, 1=medium, 2=high
    double efficiency_factor;                   // 0-1, how critical this node is
};

// Topology optimization statistics
struct TopologyMetrics {
    double total_support_volume_mm3;
    double original_support_volume_mm3;
    double material_reduction_percent;
    double average_stress_score;
    int high_stress_nodes;
    int optimized_node_count;
};

class TopologyOptimizedTreeSupport : public TreeSupport {
public:
    TopologyOptimizedTreeSupport(PrintObject& print_object, const SlicingParameters& slicing_params);
    ~TopologyOptimizedTreeSupport() override = default;

    // Main generation method - overrides TreeSupport::generate()
    void generate() override;

    // Get optimization metrics
    TopologyMetrics get_metrics() const { return metrics_; }

private:
    // Topology optimization pipeline
    void analyze_load_distribution();
    void optimize_node_topology(int optimization_iterations = 3);
    void optimize_node_positions();
    void optimize_node_radii();
    void merge_redundant_nodes();

    // Helper functions for load analysis
    LoadAnalysis compute_node_load(const SupportNode& node, int layer_index) const;
    double compute_overhang_weight(const ExPolygon& overhang) const;
    Eigen::Vector2d compute_load_direction(const ExPolygon& overhang) const;
    double compute_stress_concentration(const SupportNode& node, int layer_index) const;

    // Override node radius calculation with topology awareness
    double calc_topology_aware_radius(const SupportNode& node, double base_radius) const;

    // Calculate stress-based efficiency
    double compute_efficiency_factor(const LoadAnalysis& load) const;

    // Cull low-efficiency nodes
    void cull_low_efficiency_nodes();

    // Metrics tracking
    TopologyMetrics metrics_;
    std::map<const SupportNode*, LoadAnalysis> node_loads_;

    // Configuration
    double material_efficiency_target_;  // 0.0-1.0
    int optimization_iterations_;
    double stress_threshold_;            // Above this, node is "high stress"
};

#endif // _TOPOLOGY_OPTIMIZED_TREE_SUPPORT_HPP
