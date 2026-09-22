#pragma once
//
// NativeServices.hpp
//
// The native-services layer: two generic Lua primitives - run a command,
// open a raw socket - everything higher-level is meant to be built in
// pure Lua on top. Runtime/ephemeral resources tied to the
// currently-running script - clear() is wired into both config.preReload
// and PLUGIN_EXIT, unlike CPersistenceStore's deliberately-not-cleared-
// on-reload lifecycle.
//
// Async I/O is polling-based (a short repeating CEventLoopTimer + plain
// non-blocking read()), not CEventLoopManager::doOnReadable() - that API
// has a confirmed gap where a HANGUP reported together with READABLE
// (the common "trailing data, writer already gone" case) skips the
// registered callback entirely, which is exactly the moment a command
// finishes or a peer disconnects. See DESIGN.md's Completed history.
//
// CFileDescriptor::getFlags()/setFlags() are F_GETFD/F_SETFD (the
// close-on-exec flag), NOT F_GETFL/F_SETFL (where O_NONBLOCK lives) -
// setting O_NONBLOCK here goes through raw fcntl() calls instead.

#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

#include <string>
#include <unordered_map>

// This Lua build's headers don't self-guard with extern "C". lauxlib.h
// (not just lua.h) is needed here for LUA_NOREF.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

class CEventLoopTimer;

namespace HyprLUI {

    class CNativeServices {
      public:
        static CNativeServices& get();

        // hyprlui.run_cmd(cmd, callback): runs `cmd` via `/bin/sh -c`,
        // asynchronously, one-shot. `fnRef` is a LUA_REGISTRYINDEX
        // reference to the callback, owned by this class. Once the
        // command's stdout closes, calls `callback(output)` with
        // everything captured. No exit code is reported - Hyprland sets
        // SA_NOCLDWAIT globally, so the kernel auto-reaps every child
        // before we could ever waitpid() it. On a spawn/pipe failure,
        // logs a warning and still invokes `callback("")` - the callback
        // always fires exactly once.
        void runCmd(lua_State* L, const std::string& cmd, int fnRef);

        // hyprlui.open_socket(path, callback): connects a Unix domain
        // socket to `path` and calls `callback(sock)` once connected,
        // where `sock` is a Lua table wrapping the connection
        // (:read(callback)/:write(data)/:close()). `callback(nil)` on a
        // connect failure (logged, not luaL_error - a possibly-absent
        // service isn't a config-authoring mistake). connect() itself is
        // a blocking call, deliberately - realistically instant for a
        // local Unix domain socket; all read/write after that is
        // non-blocking.
        void openSocket(lua_State* L, const std::string& path, int fnRef);

        // Whether `id` still names an open socket - checked before
        // arming a read, so :read() on an already-closed socket gets an
        // immediate callback(nil) instead of a dangling registration.
        bool hasSocket(int id) const;

        // Arms a poll for `id`, calling back with exactly one read()'s
        // worth of data once readable, or `nil` if the peer has closed.
        void armSocketRead(int id, int fnRef);

        // Best-effort, single write() call, no partial-write retry.
        // Returns false (and logs a warning) if `id` doesn't name an
        // open socket or the write() call itself failed.
        bool writeSocket(int id, const std::string& data);

        // Cancels any pending :read() wait and closes the underlying fd.
        // Safe to call more than once.
        void closeSocket(int id);

        // Cancels every in-flight poll timer, releases every callback
        // ref, best-effort SIGTERMs any still-running command, and closes
        // every open socket. Wired into BOTH config.preReload and
        // PLUGIN_EXIT - these are ephemeral, not meant to survive a
        // reload.
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

        // Commands/sockets are looked up fresh by id on every timer tick
        // rather than captured by raw pointer in the timer's closure - a
        // cancelled CEventLoopTimer may still be invoked once more before
        // it's actually purged, which would dangling-pointer-deref an
        // entry clear() already destroyed. An id lookup just returns
        // early on a miss instead.
        void                                     finishCommand(int id);
        void                                     pollCommand(int id);

        std::unordered_map<int, SRunningCommand> m_commands;
        std::unordered_map<int, SSocket>         m_sockets;
        int                                      m_nextCommandId = 0;
        int                                      m_nextSocketId  = 0;
    };

} // namespace HyprLUI
