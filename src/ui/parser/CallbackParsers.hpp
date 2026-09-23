#pragma once
//
// CallbackParsers.hpp
//
// Reads a Lua function field (onClick, onKey, onChange, ...) off a
// widget's spec table and wraps it into a std::function that invokes it
// via lua_pcall, logging (not propagating) any error - these fire from
// input-handling callbacks with no pcall of their own. Each returns an
// empty (falsy) std::function if the field is absent.

struct lua_State;

#include <cstdint>
#include <functional>
#include <string>

namespace HyprLUI::Lua {

    std::function<void()> fieldZeroArgFn(lua_State* L, int idx, const char* fieldName);

    // `onKey(keysym, pressed)` - keysym is an already-resolved xkb_keysym_t.
    std::function<void(uint32_t, bool)> fieldOnKey(lua_State* L, int idx);

    // `onChange(text)` - fires only on a real edit, not a programmatic set.
    std::function<void(const std::string&)> fieldOnChange(lua_State* L, int idx);

    // A Checkbox's `onChange(checked)` - fires only on a real click.
    std::function<void(bool)> fieldOnChangeBool(lua_State* L, int idx);

    // `onScroll(delta, vertical)` - delta is the raw axis-event value,
    // forwarded as-is, unnormalized.
    std::function<void(double, bool)> fieldOnScroll(lua_State* L, int idx);

} // namespace HyprLUI::Lua
