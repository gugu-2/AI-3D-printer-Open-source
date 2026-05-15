#pragma once
// Helpers to apply a full *print* preset onto a single ModelObject’s overrides
// (ModelObject::config), so each plate object can carry its own speed / layer /
// infill bundle without maintaining a separate project file.

#include <string>

namespace Slic3r {

class DynamicPrintConfig;
class ModelObject;

/// Merge every option present in `print_preset_config` into `model_object.config`
/// using the same DynamicPrintConfig definition as the rest of the slicer.
/// Unknown / printer-only keys are skipped when `ignore_nonexistent` is true.
void apply_print_preset_to_model_object(ModelObject& model_object, const DynamicPrintConfig& print_preset_config, bool ignore_nonexistent = true);

/// Convenience: load a named print preset from the active bundle and apply it.
/// On success returns true; on failure sets `err` and returns false.
class PresetBundle;
bool apply_named_print_preset_to_model_object(PresetBundle& bundle, ModelObject& model_object, const std::string& preset_name, std::string& err);

} // namespace Slic3r
