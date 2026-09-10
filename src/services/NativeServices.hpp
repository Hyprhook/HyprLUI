#pragma once
//
// NativeServices.hpp
//
// Phase 12 (DESIGN.md): the native-services layer. Exposes exactly two
// generic Lua primitives - run a command, and open a raw socket - so
// everything higher-level (polling `pactl`, talking to a PipeWire/D-Bus
// proxy over its socket, etc.) can be built in pure Lua on top, the same
// "small native surface, rest is Lua" philosophy as every other phase.
//
// These are runtime/ephemeral resources tied to the currently-running
// script - the OPPOSITE lifecycle from Phase 11's CPersistenceStore:
// clear() here IS wired into resetAllState() (config.preReload) as well
// as PLUGIN_EXIT, matching every other manager in this codebase. A
// reload gets a fresh script, so any command/socket the old script was
// waiting on is meaningless to keep around.
//
// Async I/O model - verified against Hyprland's own event-loop source
// before picking this, not assumed: CEventLoopManager::doOnReadable()
// looks like the obvious primitive (see Watcher.hpp/.cpp for this
// project's established internal-event-loop-reliance precedent), but its
// wayland-side handler (handleWaiterFD() in EventLoopManager.cpp) checks
// `mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)` FIRST and, if set, drops
// the waiter via onFdReadableFail() WITHOUT ever invoking the registered
// callback. On Linux, a pipe or a stream socket whose peer has closed
// commonly reports HANGUP together with READABLE in the very same
// readiness notification (the "there's trailing data AND the writer is
// gone" case) - meaning doOnReadable's callback can simply never fire
// for the exact moment a command finishes or a peer disconnects, the
// most important moment for both of Phase 12's primitives. Rather than
// depend on an internal API with a confirmed gap for the case this code
// actually needs, both primitives below poll on a short repeating
// CEventLoopTimer (the same primitive Watcher.cpp already uses for
// interval-based watchers) and drive reads via a plain non-blocking
// read() - EAGAIN means "keep polling", 0 means EOF, a positive count is
// data, any other errno is a real error - slightly higher latency (one
// poll interval, see NativeServices.cpp for the exact value) in exchange
// for actually being correct for the close/EOF case.
//
// Note also that CFileDescriptor::getFlags()/setFlags() are F_GETFD/
// F_SETFD (the close-on-exec fd flag) - NOT F_GETFL/F_SETFL (the file
// status flags O_NONBLOCK lives under). Setting O_NONBLOCK here goes
// through raw fcntl() calls, not through CFileDescriptor's own flag
// methods.

#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

#include <string>
#include <unordered_map>

// Same extern "C" requirement as LuaBridge.cpp/Watcher.cpp - see the
// comment in Watcher.cpp for why this Lua build's headers don't
// self-guard with extern "C". lauxlib.h (not just lua.h) is needed here
// for LUA_NOREF.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

class CEventLoopTimer;

namespace HyprLUI {

    class CNativeServices {
      public:
        static CNativeServices& get();

        // hyprlui.run_cmd(cmd, callback): runs `cmd` via `/bin/sh -c`
        // (matching hl.exec_cmd's own shell-string convention - see
        // DESIGN.md's Phase 12 decision log), asynchronously, one-shot
        // (no streaming/repeat). `fnRef` is a LUA_REGISTRYINDEX reference
        // to the callback (caller creates it via luaL_ref, this class
        // owns releasing it). Once the command's stdout closes, calls
        // `callback(output)` with everything captured. No exit code is
        // reported: Hyprland's own process sets SA_NOCLDWAIT on SIGCHLD
        // globally (verified in Hyprland's main.cpp,
        // reapZombieChildrenAutomatically()) so the kernel auto-reaps
        // every child - including ours - before we could ever waitpid()
        // it ourselves to retrieve a status. On a spawn/pipe failure,
        // logs a warning (Log::WARN) and still invokes `callback("")` -
        // the callback is guaranteed to fire exactly once either way, no
        // separate ok/error signal to check.
        void runCmd(lua_State* L, const std::string& cmd, int fnRef);

        // hyprlui.open_socket(path, callback): connects a Unix domain
        // socket to `path` (Unix domain only - see DESIGN.md's Phase 12
        // decision log) and calls `callback(sock)` once connected, where
        // `sock` is a Lua table wrapping the connection
        // (:read(callback)/:write(data)/:close(), see pushSocketWrapper
        // in NativeServices.cpp). `callback(nil)` on a connect failure
        // (logged as a warning; failing to connect to a possibly-absent
        // service isn't a config-authoring mistake, so this doesn't
        // luaL_error). connect() itself is a blocking call here,
        // deliberately - for a LOCAL Unix domain socket (no DNS, no
        // network round-trip) this is realistically instant; all ongoing
        // read/write after that point is non-blocking.
        void openSocket(lua_State* L, const std::string& path, int fnRef);

        // Whether `id` still names an open socket - checked by
        // luaSocketRead() before ever calling armSocketRead(), so a
        // script calling :read() on an already-closed socket gets an
        // immediate callback(nil) instead of a dangling registration.
        bool hasSocket(int id) const;

        // A :read(callback) call on an open socket (see luaSocketRead in
        // NativeServices.cpp) - arms a poll for `id` and calls back with
        // exactly one read()'s worth of data once readable (a plain Lua
        // string), or with `nil` if the peer has closed (distinct from a
        // legitimate empty read, which cannot happen for a stream socket
        // reported readable - see the file-level comment above).
        void armSocketRead(int id, int fnRef);

        // A :write(data) call - best-effort, single write() call, no
        // partial-write retry/buffering (matches this project's "expose
        // the raw primitive, let Lua build convention on top" stance).
        // Returns false (and logs a warning) if `id` doesn't name an open
        // socket or the write() call itself failed.
        bool writeSocket(int id, const std::string& data);

        // A :close(callback) call - cancels any pending :read() wait,
        // releases its callback ref if one was pending, and closes the
        // underlying fd. Safe to call more than once (a no-op after the
        // first).
        void closeSocket(int id);

        // Cancels every in-flight command/socket poll timer, releases
        // every Lua callback ref, best-effort SIGTERMs any still-running
        // command process, and closes every open socket fd. Wired into
        // BOTH resetAllState() (config.preReload) and PLUGIN_EXIT -
        // unlike Phase 11's CPersistenceStore, these are ephemeral
        // runtime resources scoped to the currently-running script, not
        // meant to survive a reload.
        void clear();

      private:
        CNativeServices()  = default;
        ~CNativeServices() = default;

        struct SRunningCommand {
            lua_State*                     L     = nullptr;
            int                            fnRef = LUA_NOREF;
            pid_t                          pid   = -1;
            Hyprutils::OS::CFileDescriptor fd;
            SP<CEventLoopTimer>            timer;
            std::string                    output;
        };

        struct SSocket {
            lua_State*                     L = nullptr;
            Hyprutils::OS::CFileDescriptor fd;
            SP<CEventLoopTimer>            readTimer;
            int                            pendingReadFnRef = LUA_NOREF;
        };

        // Both commands and sockets are stored keyed by a stable integer
        // id, looked up fresh by id on every timer tick, rather than a
        // raw pointer captured into the timer's closure - a cancelled
        // CEventLoopTimer may still be referenced by
        // CEventLoopManager's own timer list for a little while after
        // cancel() (SP<> shared ownership), so a closure that captured a
        // raw pointer into an SRunningCommand/SSocket destroyed by
        // clear() in the meantime would be a dangling-pointer call if the
        // manager ever invoked it again before purging it. An id lookup
        // that just returns early on a miss is the same defense
        // Watcher.cpp's own timers use (by watcher name, not a pointer).
        void                                     finishCommand(int id);
        void                                     pollCommand(int id);

        std::unordered_map<int, SRunningCommand> m_commands;
        std::unordered_map<int, SSocket>         m_sockets;
        int                                      m_nextCommandId = 0;
        int                                      m_nextSocketId  = 0;
    };

} // namespace HyprLUI
