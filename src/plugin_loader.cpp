#include "plugin_loader.h"

#include "plugin.h"

#include <dlfcn.h>
#include <vector>

namespace {
std::vector<void*>& handles() {
    static std::vector<void*> h;
    return h;
}
} // namespace

std::unique_ptr<Strategy> load_plugin(const std::string& path, const Params& params, std::string& err) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        err = e ? e : "dlopen failed";
        return nullptr;
    }
    auto version = reinterpret_cast<market_plugin_api_version_fn>(dlsym(h, "market_plugin_api_version"));
    auto create = reinterpret_cast<market_plugin_create_fn>(dlsym(h, "market_plugin_create"));
    if (!version || !create) {
        err = path + ": missing market_plugin_* symbols (end the file with MARKET_PLUGIN(ClassName))";
        dlclose(h);
        return nullptr;
    }
    if (version() != MARKET_PLUGIN_API_VERSION) {
        err = path + ": plugin API version " + std::to_string(version()) + ", host expects " +
              std::to_string(MARKET_PLUGIN_API_VERSION);
        dlclose(h);
        return nullptr;
    }
    handles().push_back(h); // intentionally never closed
    Strategy* s = create(&params);
    if (!s) {
        err = path + ": market_plugin_create returned null";
        return nullptr;
    }
    return std::unique_ptr<Strategy>(s);
}
