#include "Watcher.hpp"
#include "../ui/UIManager.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

// Same extern "C" requirement as LuaBridge.cpp - see the comment there for
// why (this Lua build's headers don't self-guard with extern "C").
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <chrono>

namespace HyprLUI {

    CWatcherManager& CWatcherManager::get() {
        static CWatcherManager instance;
        return instance;
    }

    CWatcherManager::~CWatcherManager() {
        clear();
    }

    namespace {
        // Calls the Lua function at LUA_REGISTRYINDEX ref `fnRef` with no
        // arguments and coerces its single return value to a string the
        // same way Lua's own tostring()/`..` would (luaL_tolstring handles
        // numbers/booleans/strings sensibly, and honors a __tostring
        // metamethod if the return value has one). Errors are caught and
        // logged, returning `fallback` instead of propagating - see
        // Watcher.hpp's doc comment on notify() for why this can't rely on
        // a caller-side pcall.
        std::string callWatcherFn(lua_State* L, int fnRef, const std::string& fallback) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);

            if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
                const char* err = lua_tostring(L, -1);
                Log::logger->log(Log::ERR, "[hyprlui] error in watcher function: {}", err ? err : "<error object is not a string>");
                lua_pop(L, 1);
                return fallback;
            }

            const char* str    = luaL_tolstring(L, -1, nullptr);
            std::string result = str ? str : fallback;
            lua_pop(L, 2); // luaL_tolstring's own pushed string, plus the original return value
            return result;
        }
    } // namespace

    void CWatcherManager::registerWatcher(lua_State* L, const std::string& name, int fnRef, std::optional<int> intervalMs) {
        if (m_watchers.contains(name)) {
            luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
            luaL_error(L, "hyprlui.watch: a watcher named '%s' already exists", name.c_str());
            return; // unreachable
        }

        SWatcher watcher;
        watcher.L     = L;
        watcher.fnRef = fnRef;
        watcher.value = callWatcherFn(L, fnRef, "");

        if (intervalMs) {
            const int ms  = *intervalMs;
            watcher.timer = makeShared<CEventLoopTimer>(
                std::chrono::milliseconds(ms),
                [name, ms](SP<CEventLoopTimer> self, void* data) {
                    // Re-arm before doing the actual work - same reasoning
                    // as Hyprland's own hl.timer(): avoids the re-arm
                    // clobbering anything a nested call might have done.
                    self->updateTimeout(std::chrono::milliseconds(ms));
                    CWatcherManager::get().notify(name);
                },
                nullptr);

            if (g_pEventLoopManager)
                g_pEventLoopManager->addTimer(watcher.timer);
        }

        m_watchers.emplace(name, std::move(watcher));
    }

    bool CWatcherManager::notify(const std::string& name) {
        auto it = m_watchers.find(name);
        if (it == m_watchers.end())
            return false;

        const std::string newValue = callWatcherFn(it->second.L, it->second.fnRef, it->second.value);
        if (newValue != it->second.value) {
            it->second.value = newValue;
            CUIManager::get().damageAll();
        }

        return true;
    }

    std::string CWatcherManager::currentValue(const std::string& name, const std::string& def) const {
        auto it = m_watchers.find(name);
        return it == m_watchers.end() ? def : it->second.value;
    }

    bool CWatcherManager::hasWatcher(const std::string& name) const {
        return m_watchers.contains(name);
    }

    void CWatcherManager::clear() {
        for (auto& [name, watcher] : m_watchers) {
            if (watcher.timer)
                watcher.timer->cancel();
            if (watcher.L)
                luaL_unref(watcher.L, LUA_REGISTRYINDEX, watcher.fnRef);
        }
        m_watchers.clear();
    }

} // namespace HyprLUI
