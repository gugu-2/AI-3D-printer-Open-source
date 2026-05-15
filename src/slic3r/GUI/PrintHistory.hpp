#pragma once
// PrintHistory.hpp
// Tracks every completed/failed print job with metadata.
// Stores records in a JSON file under the user data directory.
// No external DB dependency — pure stdlib + nlohmann/json (already in deps).

#ifndef slic3r_PrintHistory_hpp_
#define slic3r_PrintHistory_hpp_

#include <string>
#include <vector>
#include <ctime>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// Data model
// ─────────────────────────────────────────────────────────────────────────────

enum class PrintOutcome {
    Success,
    Failed,
    Cancelled,
    Unknown
};

struct PrintRecord {
    std::string  id;               // UUID-like: timestamp + random suffix
    std::time_t  start_time  = 0;
    std::time_t  end_time    = 0;
    std::string  project_name;
    std::string  printer_name;
    std::string  filament_type;
    std::string  filament_color;
    double       filament_used_g  = 0.0;  // grams
    double       filament_cost    = 0.0;  // user currency
    double       print_time_sec   = 0.0;
    double       layer_height_mm  = 0.0;
    int          total_layers     = 0;
    PrintOutcome outcome          = PrintOutcome::Unknown;
    std::string  notes;

    // Derived helpers
    double duration_minutes() const;
    std::string outcome_str() const;
    std::string start_time_str() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Manager (singleton)
// ─────────────────────────────────────────────────────────────────────────────

class PrintHistoryManager {
public:
    static PrintHistoryManager& instance();

    // Lifecycle
    void init(const boost::filesystem::path& data_dir);
    void save();

    // CRUD
    void add_record(PrintRecord rec);
    void update_outcome(const std::string& id, PrintOutcome outcome, std::time_t end_time);
    void delete_record(const std::string& id);
    void clear_all();

    const std::vector<PrintRecord>& records() const { return m_records; }

    // Analytics helpers
    double total_filament_used_g()  const;
    double total_filament_cost()    const;
    double total_print_hours()      const;
    int    success_count()          const;
    int    failure_count()          const;
    double success_rate_pct()       const;

    // Start a new in-progress record; returns its id
    std::string begin_print(const std::string& project_name,
                            const std::string& printer_name,
                            const std::string& filament_type,
                            const std::string& filament_color,
                            double             filament_used_g,
                            double             filament_cost,
                            double             print_time_sec,
                            double             layer_height_mm,
                            int                total_layers);

private:
    PrintHistoryManager() = default;
    ~PrintHistoryManager() = default;
    PrintHistoryManager(const PrintHistoryManager&) = delete;
    PrintHistoryManager& operator=(const PrintHistoryManager&) = delete;

    void load();
    std::string generate_id() const;

    boost::filesystem::path   m_path;
    std::vector<PrintRecord>  m_records;
    bool                      m_initialized = false;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrintHistory_hpp_
