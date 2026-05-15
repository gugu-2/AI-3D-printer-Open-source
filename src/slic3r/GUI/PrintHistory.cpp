// PrintHistory.cpp
// Implementation of PrintHistoryManager — JSON-backed print log.

#include "PrintHistory.hpp"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <random>
#include <ctime>

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/log/trivial.hpp>

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

// ─────────────────────────────────────────────────────────────────────────────
// PrintRecord helpers
// ─────────────────────────────────────────────────────────────────────────────

double PrintRecord::duration_minutes() const
{
    if (end_time > start_time)
        return static_cast<double>(end_time - start_time) / 60.0;
    return print_time_sec / 60.0;
}

std::string PrintRecord::outcome_str() const
{
    switch (outcome) {
    case PrintOutcome::Success:   return "Success";
    case PrintOutcome::Failed:    return "Failed";
    case PrintOutcome::Cancelled: return "Cancelled";
    default:                      return "Unknown";
    }
}

std::string PrintRecord::start_time_str() const
{
    if (start_time == 0) return "—";
    std::tm* tm_info = std::localtime(&start_time);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

PrintHistoryManager& PrintHistoryManager::instance()
{
    static PrintHistoryManager s_instance;
    return s_instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Init / Load / Save
// ─────────────────────────────────────────────────────────────────────────────

void PrintHistoryManager::init(const fs::path& data_dir)
{
    m_path = data_dir / "print_history.json";
    load();
    m_initialized = true;
}

void PrintHistoryManager::load()
{
    if (!fs::exists(m_path)) return;

    try {
        pt::ptree root;
        pt::read_json(m_path.string(), root);

        m_records.clear();
        for (const auto& item : root.get_child("records")) {
            const auto& v = item.second;
            PrintRecord r;
            r.id               = v.get<std::string>("id", "");
            r.start_time       = static_cast<std::time_t>(v.get<long long>("start_time", 0));
            r.end_time         = static_cast<std::time_t>(v.get<long long>("end_time", 0));
            r.project_name     = v.get<std::string>("project_name", "");
            r.printer_name     = v.get<std::string>("printer_name", "");
            r.filament_type    = v.get<std::string>("filament_type", "");
            r.filament_color   = v.get<std::string>("filament_color", "");
            r.filament_used_g  = v.get<double>("filament_used_g", 0.0);
            r.filament_cost    = v.get<double>("filament_cost", 0.0);
            r.print_time_sec   = v.get<double>("print_time_sec", 0.0);
            r.layer_height_mm  = v.get<double>("layer_height_mm", 0.0);
            r.total_layers     = v.get<int>("total_layers", 0);
            r.notes            = v.get<std::string>("notes", "");
            int oc             = v.get<int>("outcome", 3);
            r.outcome          = static_cast<PrintOutcome>(oc);
            m_records.push_back(std::move(r));
        }
        BOOST_LOG_TRIVIAL(info) << "PrintHistory: loaded " << m_records.size() << " records";
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "PrintHistory: failed to load: " << e.what();
    }
}

void PrintHistoryManager::save()
{
    try {
        pt::ptree root;
        pt::ptree records_node;

        for (const auto& r : m_records) {
            pt::ptree rec;
            rec.put("id",              r.id);
            rec.put("start_time",      static_cast<long long>(r.start_time));
            rec.put("end_time",        static_cast<long long>(r.end_time));
            rec.put("project_name",    r.project_name);
            rec.put("printer_name",    r.printer_name);
            rec.put("filament_type",   r.filament_type);
            rec.put("filament_color",  r.filament_color);
            rec.put("filament_used_g", r.filament_used_g);
            rec.put("filament_cost",   r.filament_cost);
            rec.put("print_time_sec",  r.print_time_sec);
            rec.put("layer_height_mm", r.layer_height_mm);
            rec.put("total_layers",    r.total_layers);
            rec.put("outcome",         static_cast<int>(r.outcome));
            rec.put("notes",           r.notes);
            records_node.push_back({"", rec});
        }
        root.add_child("records", records_node);

        // Ensure parent directory exists
        fs::create_directories(m_path.parent_path());
        pt::write_json(m_path.string(), root);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "PrintHistory: failed to save: " << e.what();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CRUD
// ─────────────────────────────────────────────────────────────────────────────

void PrintHistoryManager::add_record(PrintRecord rec)
{
    if (rec.id.empty()) rec.id = generate_id();
    m_records.push_back(std::move(rec));
    save();
}

void PrintHistoryManager::update_outcome(const std::string& id,
                                          PrintOutcome       outcome,
                                          std::time_t        end_time)
{
    for (auto& r : m_records) {
        if (r.id == id) {
            r.outcome  = outcome;
            r.end_time = end_time;
            break;
        }
    }
    save();
}

void PrintHistoryManager::delete_record(const std::string& id)
{
    m_records.erase(
        std::remove_if(m_records.begin(), m_records.end(),
                       [&id](const PrintRecord& r) { return r.id == id; }),
        m_records.end());
    save();
}

void PrintHistoryManager::clear_all()
{
    m_records.clear();
    save();
}

// ─────────────────────────────────────────────────────────────────────────────
// Analytics
// ─────────────────────────────────────────────────────────────────────────────

double PrintHistoryManager::total_filament_used_g() const
{
    double total = 0.0;
    for (const auto& r : m_records) total += r.filament_used_g;
    return total;
}

double PrintHistoryManager::total_filament_cost() const
{
    double total = 0.0;
    for (const auto& r : m_records) total += r.filament_cost;
    return total;
}

double PrintHistoryManager::total_print_hours() const
{
    double total = 0.0;
    for (const auto& r : m_records) total += r.duration_minutes();
    return total / 60.0;
}

int PrintHistoryManager::success_count() const
{
    int n = 0;
    for (const auto& r : m_records)
        if (r.outcome == PrintOutcome::Success) ++n;
    return n;
}

int PrintHistoryManager::failure_count() const
{
    int n = 0;
    for (const auto& r : m_records)
        if (r.outcome == PrintOutcome::Failed) ++n;
    return n;
}

double PrintHistoryManager::success_rate_pct() const
{
    if (m_records.empty()) return 0.0;
    int finished = 0;
    for (const auto& r : m_records)
        if (r.outcome == PrintOutcome::Success || r.outcome == PrintOutcome::Failed)
            ++finished;
    if (finished == 0) return 0.0;
    return 100.0 * success_count() / finished;
}

// ─────────────────────────────────────────────────────────────────────────────
// Begin print helper
// ─────────────────────────────────────────────────────────────────────────────

std::string PrintHistoryManager::begin_print(const std::string& project_name,
                                              const std::string& printer_name,
                                              const std::string& filament_type,
                                              const std::string& filament_color,
                                              double             filament_used_g,
                                              double             filament_cost,
                                              double             print_time_sec,
                                              double             layer_height_mm,
                                              int                total_layers)
{
    PrintRecord r;
    r.id              = generate_id();
    r.start_time      = std::time(nullptr);
    r.project_name    = project_name;
    r.printer_name    = printer_name;
    r.filament_type   = filament_type;
    r.filament_color  = filament_color;
    r.filament_used_g = filament_used_g;
    r.filament_cost   = filament_cost;
    r.print_time_sec  = print_time_sec;
    r.layer_height_mm = layer_height_mm;
    r.total_layers    = total_layers;
    r.outcome         = PrintOutcome::Unknown;

    std::string id = r.id;
    add_record(std::move(r));
    return id;
}

// ─────────────────────────────────────────────────────────────────────────────
// ID generation
// ─────────────────────────────────────────────────────────────────────────────

std::string PrintHistoryManager::generate_id() const
{
    std::time_t now = std::time(nullptr);
    std::mt19937 rng(static_cast<unsigned>(now));
    std::uniform_int_distribution<int> dist(1000, 9999);
    std::ostringstream oss;
    oss << now << "_" << dist(rng);
    return oss.str();
}

} // namespace GUI
} // namespace Slic3r
