#pragma once

#include "strategies.h"
#include "strategy.h"

#include <memory>
#include <string>

// dlopen a strategy plugin built against include/plugin.h and construct its
// strategy. Returns nullptr and fills `err` on failure. The library handle is
// kept open for the life of the process.
std::unique_ptr<Strategy> load_plugin(const std::string& path, const Params& params, std::string& err);
