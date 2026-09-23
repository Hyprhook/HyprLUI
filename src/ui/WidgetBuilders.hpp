#pragma once
//
// WidgetBuilders.hpp
//
// Recursively converts a tagged Lua widget-spec table into a real CWidget
// subtree - the implementation behind hyprlui.window{}'s tree argument.
// One builder function per widget type (its own unique fields only) plus
// a shared common-tail applier (base CWidget properties every type
// accepts - padding, opacity, debug flags, interactive state, animation
// overrides, fill). See docs/api.md for the full per-type Lua field
// reference.

struct lua_State;

#include "Widget.hpp"

#include <functional>
#include <unordered_set>
#include <vector>

namespace HyprLUI::Lua {

    // `autoId` synthesizes ids for widgets that don't set one explicitly.
    // `bindings` collects one closure per hyprlui.Bind()-tagged field
    // found - the caller (luaWindow()) attaches them to the finished
    // CCanvas once it exists. `seenIds` is a per-window set checked
    // against every resolved id - a duplicate is a hard error.
    // `inheritedDebug` is this widget's ancestor-resolved `debug` state
    // (mirrors resolveDebugSpec()'s cascade, computed once here instead
    // of per-frame) - used for construction-time-only diagnostics.
    PWidget buildWidget(lua_State* L, int idx, int& autoId, std::vector<std::function<void()>>& bindings, std::unordered_set<std::string>& seenIds, bool inheritedDebug = false);

} // namespace HyprLUI::Lua
