#pragma once

// Strategies as shared libraries.
//
// A plugin is one .cpp that defines a Strategy subclass and ends with
//     MARKET_PLUGIN(MyStrategy)
// Build it with `make plugin SRC=plugins/my_strategy.cpp`, which produces
// build/plugins/my_strategy.so, and run it with
//     ./build/market_engine --plugin build/plugins/my_strategy.so:k=v,k=v
//
// The host never dlcloses a plugin: the strategy object's vtable lives in the
// library and must outlive the object. Everything a plugin needs is header-only
// (Strategy, AgentContext, Params, RollingSMA), so it has no host symbols to
// resolve.

#include "strategies.h"
#include "strategy.h"

#define MARKET_PLUGIN_API_VERSION 1

extern "C" {
typedef int (*market_plugin_api_version_fn)();
typedef Strategy* (*market_plugin_create_fn)(const Params*);
}

#define MARKET_PLUGIN(ClassName)                                                          \
    extern "C" int market_plugin_api_version() { return MARKET_PLUGIN_API_VERSION; }      \
    extern "C" Strategy* market_plugin_create(const Params* params) {                     \
        return new ClassName(*params);                                                    \
    }
