#include "CallbackParsers.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <hyprland/src/debug/log/Logger.hpp>

#include <memory>

namespace HyprLUI::Lua {

    namespace {
        // Wraps a LUA_REGISTRYINDEX reference so the Lua function it
        // points to is released exactly once, when the last copy of the
        // owning std::function is destroyed. Held via shared_ptr since
        // std::function requires its target to be copyable, which a bare
        // move-only RAII guard wouldn't be.
        struct SLuaFnRef {
            lua_State* L   = nullptr;
            int        ref = LUA_NOREF;

            ~SLuaFnRef() {
                if (L && ref != LUA_NOREF)
                    luaL_unref(L, LUA_REGISTRYINDEX, ref);
            }
        };
    } // namespace

    std::function<void()> fieldZeroArgFn(lua_State* L, int idx, const char* fieldName) {
        lua_getfield(L, idx, fieldName);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops the function value
        // Constructed in place, NOT via
        // make_shared<SLuaFnRef>(SLuaFnRef{L, ref}) - that form builds a
        // temporary first and copies it in, whose destructor unrefs the
        // slot immediately, before the callback is ever invoked.
        auto fnRef = std::make_shared<SLuaFnRef>(L, ref);

        return [L, fnRef, fieldName]() {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in {} handler: {}", fieldName, err ? err : "<error object is not a string>");
                lua_pop(L, 1);
            }
        };
    }

    std::function<void(uint32_t, bool)> fieldOnKey(lua_State* L, int idx) {
        lua_getfield(L, idx, "onKey");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
        auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

        return [L, fnRef](uint32_t keysym, bool pressed) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
            lua_pushinteger(L, keysym);
            lua_pushboolean(L, pressed);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in onKey handler: {}", err ? err : "<error object is not a string>");
                lua_pop(L, 1);
            }
        };
    }

    std::function<void(const std::string&)> fieldOnChange(lua_State* L, int idx) {
        lua_getfield(L, idx, "onChange");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
        auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

        return [L, fnRef](const std::string& text) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
            lua_pushlstring(L, text.data(), text.size());
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in onChange handler: {}", err ? err : "<error object is not a string>");
                lua_pop(L, 1);
            }
        };
    }

    std::function<void(bool)> fieldOnChangeBool(lua_State* L, int idx) {
        lua_getfield(L, idx, "onChange");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
        auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

        return [L, fnRef](bool checked) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
            lua_pushboolean(L, checked);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in onChange handler: {}", err ? err : "<error object is not a string>");
                lua_pop(L, 1);
            }
        };
    }

    std::function<void(double, bool)> fieldOnScroll(lua_State* L, int idx) {
        lua_getfield(L, idx, "onScroll");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
        auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

        return [L, fnRef](double delta, bool vertical) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
            lua_pushnumber(L, delta);
            lua_pushboolean(L, vertical);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in onScroll handler: {}", err ? err : "<error object is not a string>");
                lua_pop(L, 1);
            }
        };
    }

    std::function<void(EMouseButton)> fieldOnClick(lua_State* L, int idx) {
        lua_getfield(L, idx, "onClick");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return {};
        }

        const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
        auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

        return [L, fnRef](EMouseButton button) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
            switch (button) {
                case EMouseButton::Left: lua_pushstring(L, "left"); break;
                case EMouseButton::Right: lua_pushstring(L, "right"); break;
                case EMouseButton::Middle: lua_pushstring(L, "middle"); break;
            }
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in onClick handler: {}", err ? err : "<error object is not a string>");
                lua_pop(L, 1);
            }
        };
    }

} // namespace HyprLUI::Lua
