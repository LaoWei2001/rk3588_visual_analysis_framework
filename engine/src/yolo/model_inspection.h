#pragma once

#include <string>

// Independent probe process: no video pipeline, inference, or display initialization.
int inspect_model_cli(const std::string &path);
