#pragma once
#include <string>

// JSON/stdio adapter only; all model loading and decoder rules belong to the SDK.
int inspect_model_cli(const std::string &path, const std::string &model_type = "");
