#pragma once
//
// ValueParsers.hpp
//
// Raw Lua-table-field-to-native-value resolution: the number/string/
// boolean reads every other parser and widget builder in this plugin is
// built on top of.

struct lua_State;

#include <optional>
#include <string>

namespace HyprLUI::Lua {

    // Reads a field off the table at `idx`, raising a Lua error
    // (luaL_error, never returns) on a type mismatch. Returns `def` if the
    // field is absent.
    double fieldNumber(lua_State* L, int idx, const char* key, double def);

    // Optional numeric field - nullopt (not a baked-in default) lets a
    // caller leave a widget's own constructor-chosen default alone when
    // the Lua spec doesn't mention the field.
    std::optional<double> optFixedField(lua_State* L, int idx, const char* key);

    double                requireFieldNumber(lua_State* L, int idx, const char* key, const char* fnName);
    std::string           requireFieldString(lua_State* L, int idx, const char* key, const char* fnName);
    std::string           optFieldString(lua_State* L, int idx, const char* key, const std::string& def);
    bool                  optFieldBool(lua_State* L, int idx, const char* key, bool def);

    // Tri-state boolean read - nullopt when absent, for fields where "the
    // caller didn't say anything" is itself a meaningful state (e.g. the
    // debug-overlay fields: nullopt means "inherit").
    std::optional<bool> optFieldBoolOpt(lua_State* L, int idx, const char* key);

    // If the table at `idx`.`key` is a hyprlui.Bind(name) marker table
    // ({__bind = name}), returns that watcher name. Otherwise (plain
    // string, missing field, etc.) returns nullopt so the caller falls
    // back to reading the field normally.
    std::optional<std::string> fieldBindName(lua_State* L, int idx, const char* key);

} // namespace HyprLUI::Lua
