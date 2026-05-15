#ifndef _LLM_SETTINGS_ADVISOR_HPP_
#define _LLM_SETTINGS_ADVISOR_HPP_

#include <string>
#include <map>
#include <optional>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class DynamicPrintConfig;

class LLMSettingsAdvisor {
public:
    struct Suggestion {
        std::map<std::string, double> settings;  // config_key -> value
        std::string reasoning;
        double confidence;
        std::string explanation;
        bool success;
    };

    LLMSettingsAdvisor();
    ~LLMSettingsAdvisor();

    // Get suggestions based on natural language intent
    // Returns empty optional on API failure or invalid input
    std::optional<Suggestion> suggest_settings(
        const std::string& user_intent,
        const DynamicPrintConfig& current_config
    );

    // Validate suggestions against config constraints
    bool validate_suggestion(Suggestion& suggestion);

    // Apply suggestion to config (caller is responsible for applying)
    void apply_suggestion_to_config(const Suggestion& suggestion, DynamicPrintConfig& config);

    // Set API key for Anthropic Claude
    void set_api_key(const std::string& api_key) { api_key_ = api_key; }

    // Get current API status
    bool is_api_configured() const { return !api_key_.empty(); }

    // Get last error message
    std::string get_last_error() const { return last_error_; }

private:
    // Call Claude API for suggestions
    std::optional<json> call_claude_api(const std::string& prompt);

    // Build system prompt for Claude
    std::string build_system_prompt() const;

    // Build user prompt from intent and current config
    std::string build_user_prompt(const std::string& intent, const DynamicPrintConfig& config) const;

    // Parse Claude response to Suggestion struct
    Suggestion parse_response(const json& response);

    // Map QuantumForge config keys to user-friendly names
    std::map<std::string, std::string> get_setting_name_map() const;

    // Get valid range for a setting
    std::pair<double, double> get_setting_range(const std::string& setting_name) const;

    std::string api_key_;
    mutable std::string last_error_;

    // Caching for settings metadata
    mutable std::unique_ptr<std::map<std::string, std::pair<double, double>>> settings_ranges_;
};

#endif // _LLM_SETTINGS_ADVISOR_HPP
