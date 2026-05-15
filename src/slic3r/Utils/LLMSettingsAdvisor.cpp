#include "LLMSettingsAdvisor.hpp"
#include <iostream>
#include <curl/curl.h>
#include <sstream>

// Helper function for CURL responses
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

LLMSettingsAdvisor::LLMSettingsAdvisor() : api_key_("") {
}

LLMSettingsAdvisor::~LLMSettingsAdvisor() {
}

std::optional<LLMSettingsAdvisor::Suggestion> LLMSettingsAdvisor::suggest_settings(
    const std::string& user_intent,
    const DynamicPrintConfig& current_config) {

    if (!is_api_configured()) {
        last_error_ = "API key not configured";
        return std::nullopt;
    }

    if (user_intent.empty()) {
        last_error_ = "Empty user intent";
        return std::nullopt;
    }

    // Build prompts
    std::string system_prompt = build_system_prompt();
    std::string user_prompt = build_user_prompt(user_intent, current_config);

    // Call Claude API
    std::optional<json> response = call_claude_api(user_prompt);
    if (!response) {
        last_error_ = "API call failed or returned invalid response";
        return std::nullopt;
    }

    // Parse response
    Suggestion suggestion = parse_response(*response);

    // Validate suggestion
    if (!validate_suggestion(suggestion)) {
        last_error_ = "Suggestion validation failed - some settings are out of range";
        // Don't return nullopt - the suggestion might still be useful with clamped values
    }

    return suggestion;
}

std::optional<json> LLMSettingsAdvisor::call_claude_api(const std::string& prompt) {
    try {
        CURL* curl = curl_easy_init();
        if (!curl) {
            last_error_ = "Failed to initialize CURL";
            return std::nullopt;
        }

        // Prepare request body
        json request;
        request["model"] = "claude-3-5-sonnet-20241022";
        request["max_tokens"] = 1024;

        json messages = json::array();
        json user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = prompt;
        messages.push_back(user_msg);

        request["messages"] = messages;

        std::string request_body = request.dump();
        std::string response_body;

        // Set CURL options
        curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER,
            curl_slist_append(nullptr, "Content-Type: application/json"));

        std::string auth_header = "x-api-key: " + api_key_;
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, auth_header.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        // Perform request
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            last_error_ = "CURL request failed: " + std::string(curl_easy_strerror(res));
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return std::nullopt;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Parse response
        auto response_json = json::parse(response_body);
        if (!response_json.contains("content") || response_json["content"].empty()) {
            last_error_ = "Invalid API response format";
            return std::nullopt;
        }

        std::string response_text = response_json["content"][0]["text"];

        // Extract JSON from response (Claude may wrap it in markdown)
        size_t json_start = response_text.find('{');
        size_t json_end = response_text.rfind('}');
        if (json_start != std::string::npos && json_end != std::string::npos) {
            std::string json_str = response_text.substr(json_start, json_end - json_start + 1);
            return json::parse(json_str);
        }

        return std::nullopt;
    } catch (const std::exception& e) {
        last_error_ = std::string("Exception: ") + e.what();
        return std::nullopt;
    }
}

std::string LLMSettingsAdvisor::build_system_prompt() const {
    return R"(You are an expert 3D printing settings advisor for QuantumForge, a professional slicing software.
Your role is to interpret user intent in natural language and translate it into concrete slicer settings adjustments.

Key principles:
1. Layer Height (mm): 0.1-0.4. Lower = better detail, slower. Higher = faster, less detail.
2. Wall Thickness (mm): 0.4-2.0. Higher = stronger parts.
3. Infill Density (%): 0% = hollow, 50% = balanced, 100% = solid. Higher = stronger but slower.
4. Print Speed (mm/s): 30-200. Higher = faster but lower quality.
5. Nozzle Temperature (°C): Material-dependent, typically 190-230 for PLA.
6. Bed Temperature (°C): Material-dependent, typically 20-80 for PLA.
7. Tree Support: true/false. Enable for complex overhangs.
8. Support Density (%): 10-50. Higher = more support (more material).

When the user expresses intent like "strong and fast", interpret as:
- Strong: Increase wall_thickness and infill_density
- Fast: Increase layer_height and print_speed

Always respond with valid JSON in this format:
{
  "reasoning": "explanation of your logic",
  "settings": {
    "layer_height": 0.2,
    "wall_thickness": 1.2,
    ...
  },
  "confidence": 0.85,
  "explanation": "detailed explanation for the user"
})";
}

std::string LLMSettingsAdvisor::build_user_prompt(
    const std::string& intent,
    const DynamicPrintConfig& current_config) const {
    std::ostringstream oss;
    oss << "Current settings:\n";
    oss << "- Layer Height: 0.2mm\n";
    oss << "- Wall Thickness: 1.2mm\n";
    oss << "- Infill Density: 20%\n";
    oss << "- Print Speed: 100mm/s\n";
    oss << "\nUser intent: " << intent << "\n";
    oss << "\nProvide optimized settings for this intent.";
    return oss.str();
}

LLMSettingsAdvisor::Suggestion LLMSettingsAdvisor::parse_response(const json& response) {
    Suggestion suggestion;
    suggestion.success = false;

    try {
        if (response.contains("reasoning")) {
            suggestion.reasoning = response["reasoning"].get<std::string>();
        }
        if (response.contains("confidence")) {
            suggestion.confidence = response["confidence"].get<double>();
        }
        if (response.contains("explanation")) {
            suggestion.explanation = response["explanation"].get<std::string>();
        }
        if (response.contains("settings") && response["settings"].is_object()) {
            for (const auto& [key, value] : response["settings"].items()) {
                if (value.is_number()) {
                    suggestion.settings[key] = value.get<double>();
                }
            }
        }

        suggestion.success = !suggestion.settings.empty();
    } catch (const std::exception& e) {
        last_error_ = "Failed to parse response: " + std::string(e.what());
    }

    return suggestion;
}

bool LLMSettingsAdvisor::validate_suggestion(Suggestion& suggestion) {
    if (!settings_ranges_) {
        settings_ranges_ = std::make_unique<std::map<std::string, std::pair<double, double>>>();
        (*settings_ranges_)["layer_height"] = {0.1, 0.4};
        (*settings_ranges_)["wall_thickness"] = {0.4, 2.0};
        (*settings_ranges_)["infill_density"] = {0, 100};
        (*settings_ranges_)["print_speed"] = {30, 200};
        (*settings_ranges_)["nozzle_temperature"] = {150, 250};
        (*settings_ranges_)["bed_temperature"] = {20, 100};
        (*settings_ranges_)["support_density"] = {10, 100};
    }

    bool all_valid = true;
    for (auto& [key, value] : suggestion.settings) {
        auto it = settings_ranges_->find(key);
        if (it != settings_ranges_->end()) {
            double min_val = it->second.first;
            double max_val = it->second.second;
            if (value < min_val || value > max_val) {
                // Clamp value
                value = std::max(min_val, std::min(value, max_val));
                all_valid = false;
            }
        }
    }

    return all_valid;
}

void LLMSettingsAdvisor::apply_suggestion_to_config(const Suggestion& suggestion, DynamicPrintConfig& config) {
    // This would be called by the UI to apply the suggestion
    // For now, the caller handles the actual config modification
}

std::string LLMSettingsAdvisor::build_system_prompt_impl() const {
    return build_system_prompt();
}

std::map<std::string, std::pair<double, double>> LLMSettingsAdvisor::get_setting_range_map() const {
    return {
        {"layer_height", {0.1, 0.4}},
        {"wall_thickness", {0.4, 2.0}},
        {"infill_density", {0, 100}},
        {"print_speed", {30, 200}},
        {"nozzle_temperature", {150, 250}},
        {"bed_temperature", {20, 100}},
        {"support_density", {10, 100}}
    };
}
