#include "TopologyOptimizedTreeSupport.hpp"
#include <cmath>
#include <algorithm>

TopologyOptimizedTreeSupport::TopologyOptimizedTreeSupport(
    PrintObject& print_object,
    const SlicingParameters& slicing_params)
    : TreeSupport(print_object, slicing_params),
      material_efficiency_target_(0.8),
      optimization_iterations_(3),
      stress_threshold_(0.6) {
}

void TopologyOptimizedTreeSupport::generate() {
    // Run standard TreeSupport generation first
    TreeSupport::generate();

    // Then apply topology optimization
    analyze_load_distribution();
    optimize_node_topology(optimization_iterations_);
    cull_low_efficiency_nodes();

    // Recalculate toolpaths with optimized node configuration
    draw_circles();
    generate_toolpaths();

    // Compute final metrics
    for (const auto& layer : contact_override) {
        for (const auto& poly : layer.polygons) {
            metrics_.total_support_volume_mm3 += poly.area() * layer_height;
        }
    }
}

void TopologyOptimizedTreeSupport::analyze_load_distribution() {
    node_loads_.clear();

    // Analyze each contact point and compute its load
    for (size_t i = 0; i < contact_override.size(); ++i) {
        for (const auto& poly : contact_override[i].polygons) {
            // Estimate load from overhang area
            double overhang_area = static_cast<double>(poly.area()) * 0.000001; // Convert to mm²
            double weight = compute_overhang_weight(poly);

            // For each relevant node at this layer, compute load
            for (auto& node : m_support_tree) {
                if (node.distance_to_top == static_cast<int>(i)) {
                    LoadAnalysis load;
                    load.total_load = weight;
                    load.load_direction = compute_load_direction(poly);
                    load.stress_ratio = compute_stress_concentration(node, i);
                    load.critical_level = (load.stress_ratio > stress_threshold_) ? 2 : 1;
                    load.efficiency_factor = compute_efficiency_factor(load);

                    node_loads_[&node] = load;
                    metrics_.average_stress_score += load.stress_ratio;
                    if (load.critical_level == 2) {
                        metrics_.high_stress_nodes++;
                    }
                }
            }
        }
    }

    if (!m_support_tree.empty()) {
        metrics_.average_stress_score /= m_support_tree.size();
    }
}

void TopologyOptimizedTreeSupport::optimize_node_topology(int optimization_iterations) {
    for (int iter = 0; iter < optimization_iterations; ++iter) {
        optimize_node_positions();
        optimize_node_radii();
        merge_redundant_nodes();
    }
}

void TopologyOptimizedTreeSupport::optimize_node_positions() {
    // Move nodes to distribute load more evenly
    for (auto& node : m_support_tree) {
        if (node_loads_.find(&node) != node_loads_.end()) {
            const LoadAnalysis& load = node_loads_[&node];

            // High-stress nodes: try to shift toward build plate (shorter path)
            if (load.critical_level == 2) {
                // Move node slightly to reduce lateral stress
                double shift_factor = 0.1 * load.stress_ratio;
                node.position.x() = static_cast<int>(node.position.x() * (1.0 - shift_factor * 0.05));
                node.position.y() = static_cast<int>(node.position.y() * (1.0 - shift_factor * 0.05));
            }
        }
    }
}

void TopologyOptimizedTreeSupport::optimize_node_radii() {
    // Adjust radii based on load distribution
    for (auto& node : m_support_tree) {
        if (node_loads_.find(&node) != node_loads_.end()) {
            const LoadAnalysis& load = node_loads_[&node];

            // Calculate layers to top for radius calculation
            int layers_to_top = static_cast<int>(node.distance_to_top);
            double base_radius = node.radius;

            // Apply topology-aware radius adjustment
            node.radius = calc_topology_aware_radius(node, base_radius);

            // Ensure minimum radius
            node.radius = std::max(node.radius, minimum_radius);
        }
    }
}

void TopologyOptimizedTreeSupport::merge_redundant_nodes() {
    // Merge nearby nodes with low efficiency
    for (auto it1 = m_support_tree.begin(); it1 != m_support_tree.end(); ++it1) {
        for (auto it2 = std::next(it1); it2 != m_support_tree.end(); ++it2) {
            if (node_loads_.find(&(*it1)) != node_loads_.end() &&
                node_loads_.find(&(*it2)) != node_loads_.end()) {

                double distance = std::sqrt(
                    std::pow(it1->position.x() - it2->position.x(), 2) +
                    std::pow(it1->position.y() - it2->position.y(), 2)
                );

                // If nodes are close and both have low efficiency, merge
                if (distance < minimum_radius * 4 &&
                    node_loads_[&(*it1)].efficiency_factor < 0.3 &&
                    node_loads_[&(*it2)].efficiency_factor < 0.3) {

                    // Merge it2 into it1
                    it1->radius = std::max(it1->radius, it2->radius);
                    it2->merged_neighbours.push_back(*it1);
                }
            }
        }
    }
}

LoadAnalysis TopologyOptimizedTreeSupport::compute_node_load(const SupportNode& node, int layer_index) const {
    LoadAnalysis load;
    load.total_load = 0;
    load.stress_ratio = 0.5;  // Default neutral stress
    load.critical_level = 1;
    load.efficiency_factor = 0.7;
    return load;
}

double TopologyOptimizedTreeSupport::compute_overhang_weight(const ExPolygon& overhang) const {
    // Estimate weight based on area and layer height
    double area_mm2 = static_cast<double>(overhang.area()) * 0.000001;
    double weight_grams = area_mm2 * layer_height * material_density * 0.001;  // Rough estimate
    return weight_grams;
}

Eigen::Vector2d TopologyOptimizedTreeSupport::compute_load_direction(const ExPolygon& overhang) const {
    // Compute centroid and direction of load
    Point centroid = overhang.contour.centroid();
    Eigen::Vector2d direction(static_cast<double>(centroid.x()), static_cast<double>(centroid.y()));
    if (direction.norm() > 0) {
        direction.normalize();
    }
    return direction;
}

double TopologyOptimizedTreeSupport::compute_stress_concentration(const SupportNode& node, int layer_index) const {
    // Estimate stress based on distance to build plate and load
    double layers_remaining = static_cast<double>(node.distance_to_top);
    double stress = std::min(1.0, 0.3 + (layers_remaining / 100.0) * 0.7);
    return stress;
}

double TopologyOptimizedTreeSupport::calc_topology_aware_radius(
    const SupportNode& node,
    double base_radius) const {
    if (node_loads_.find(&node) == node_loads_.end()) {
        return base_radius;
    }

    const LoadAnalysis& load = node_loads_.at(&node);

    // Increase radius for high-stress nodes
    double stress_factor = 1.0 + load.stress_ratio * 0.3;  // 1.0 to 1.3x adjustment
    double efficiency_factor = 1.0 - (load.efficiency_factor * material_efficiency_target_);

    double adjusted_radius = base_radius * stress_factor * (1.0 + efficiency_factor * 0.2);
    return adjusted_radius;
}

double TopologyOptimizedTreeSupport::compute_efficiency_factor(const LoadAnalysis& load) const {
    // Lower stress = higher efficiency = more reduction opportunity
    return 1.0 - load.stress_ratio;
}

void TopologyOptimizedTreeSupport::cull_low_efficiency_nodes() {
    // Remove nodes with very low efficiency (support material waste)
    auto it = m_support_tree.begin();
    while (it != m_support_tree.end()) {
        if (node_loads_.find(&(*it)) != node_loads_.end() &&
            node_loads_[&(*it)].efficiency_factor < 0.2 &&
            !it->to_buildplate) {
            it = m_support_tree.erase(it);
        } else {
            ++it;
        }
    }
}
