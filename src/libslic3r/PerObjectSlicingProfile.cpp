#include "PerObjectSlicingProfile.hpp"

#include "Model.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

void apply_print_preset_to_model_object(ModelObject& model_object, const DynamicPrintConfig& print_preset_config, bool ignore_nonexistent)
{
    model_object.config.apply(print_preset_config, ignore_nonexistent);
}

bool apply_named_print_preset_to_model_object(PresetBundle& bundle, ModelObject& model_object, const std::string& preset_name, std::string& err)
{
    if (preset_name.empty()) {
        err = "empty preset name";
        return false;
    }
    const Preset* p = bundle.prints.find_preset(preset_name, false, true);
    if (!p) {
        err = "print preset not found: " + preset_name;
        return false;
    }
    apply_print_preset_to_model_object(model_object, p->config, true);
    return true;
}

} // namespace Slic3r
