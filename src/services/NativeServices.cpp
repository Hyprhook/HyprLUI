#include "NativeServices.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

extern "C" {
#include <lauxlib.h>
}

#include <spawn.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#include <cerrno>

extern char** environ;

using namespace Hyprutils::OS;

namespace HyprLUI {

    // How often the poll timers below check readiness - see
    // NativeServices.hpp's file-level comment for why this polls instead
    // of using CEventLoopManager::doOnReadable(). 16ms keeps a command's
    // output or a socket message feeling instant for UI purposes without
    // being a busy-loop.
    static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(16);

    namespace {
        // CFileDescriptor::getFlags()/setFlags() are F_GETFD/F_SETFD (the
        // close-on-exec flag) - NOT the file status flags O_NONBLOCK
        // lives under (F_GETFL/F_SETFL). Set it via raw fcntl() instead.
        void setNonBlocking(int fd) {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags != -1)
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }

        // Invokes a LUA_REGISTRYINDEX-ref'd callback with a single string
        // (or nil, if `str` isn't given) argument, logs+pops on error, and
        // always releases the ref - the shared tail of every "call back
        // into Lua exactly once" path below (run_cmd's success/failure
        // paths, open_socket's connect-failure path).
        void invokeAndRelease(lua_State* L, int fnRef, const std::string* str) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
            if (str)
                lua_pushlstring(L, str->data(), str->size());
            else
                lua_pushnil(L);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                Log::logger->log(Log::ERR, "[hyprlui] error in native-services callback: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
        }

        // Both `id` upvalues below are set via lua_pushcclosure() at the
        // table-construction site in pushSocketWrapper() - `sock:read()`/
        // `:write()`/`:close()`'s Lua `:` sugar passes `sock` itself as
        // arg 1, which these ignore entirely (the actual id lives in the
        // closure's upvalue, same pattern as luaPersistentGet/Set in
        // LuaBridge.cpp, just an integer id instead of a string key).
        int luaSocketRead(lua_State* L) {
            const int id = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
            luaL_checktype(L, 2, LUA_TFUNCTION);

            auto& services = CNativeServices::get();
            if (!services.hasSocket(id)) {
                lua_pushvalue(L, 2);
                lua_pushnil(L);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    Log::logger->log(Log::ERR, "[hyprlui] error in socket:read() callback: {}", lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
                return 0;
            }

            lua_pushvalue(L, 2);
            const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
            services.armSocketRead(id, fnRef);
            return 0;
        }

        int luaSocketWrite(lua_State* L) {
            const int   id   = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
            size_t      len  = 0;
            const char* data = luaL_checklstring(L, 2, &len);
            CNativeServices::get().writeSocket(id, std::string(data, len));
            return 0;
        }

        int luaSocketClose(lua_State* L) {
            const int id = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
            CNativeServices::get().closeSocket(id);
            return 0;
        }

        void pushSocketWrapper(lua_State* L, int id) {
            lua_newtable(L);
            const int tblIdx = lua_gettop(L);

            lua_pushinteger(L, id);
            lua_pushcclosure(L, luaSocketRead, 1);
            lua_setfield(L, tblIdx, "read");

            lua_pushinteger(L, id);
            lua_pushcclosure(L, luaSocketWrite, 1);
            lua_setfield(L, tblIdx, "write");

            lua_pushinteger(L, id);
            lua_pushcclosure(L, luaSocketClose, 1);
            lua_setfield(L, tblIdx, "close");
        }
    } // namespace

    CNativeServices& CNativeServices::get() {
        static CNativeServices instance;
        return instance;
    }

    void CNativeServices::runCmd(lua_State* L, const std::string& cmd, int fnRef) {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            Log::logger->log(Log::WARN, "[hyprlui] run_cmd('{}'): pipe() failed: {}", cmd, strerror(errno));
            const std::string empty;
            invokeAndRelease(L, fnRef, &empty);
            return;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        char* argv[] = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>(cmd.c_str()), nullptr};

        pid_t pid = -1;
        int   rc  = posix_spawn(&pid, "/bin/sh", &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);

        close(pipefd[1]);

        if (rc != 0) {
            close(pipefd[0]);
            Log::logger->log(Log::WARN, "[hyprlui] run_cmd('{}'): posix_spawn() failed: {}", cmd, strerror(rc));
            const std::string empty;
            invokeAndRelease(L, fnRef, &empty);
            return;
        }

        setNonBlocking(pipefd[0]);

        const int id       = ++m_nextCommandId;
        auto&     cmdState = m_commands[id];
        cmdState.L         = L;
        cmdState.fnRef     = fnRef;
        cmdState.pid       = pid;
        cmdState.fd        = CFileDescriptor(pipefd[0]);

        cmdState.timer = makeShared<CEventLoopTimer>(POLL_INTERVAL, [this, id](SP<CEventLoopTimer> self, void* data) { pollCommand(id); }, nullptr);

        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(cmdState.timer);
    }

    void CNativeServices::pollCommand(int id) {
        auto it = m_commands.find(id);
        if (it == m_commands.end())
            return; // cleared already (e.g. by a config reload) - nothing to do

        SRunningCommand& cmd = it->second;
        char             buf[4096];
        bool             done = false;

        while (true) {
            const ssize_t n = read(cmd.fd.get(), buf, sizeof(buf));
            if (n > 0) {
                cmd.output.append(buf, n);
                continue;
            }
            if (n == 0) {
                done = true; // EOF
                break;
            }
            // n < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;   // nothing more right now, keep polling
            done = true; // real read error - treat as finished with whatever we have
            break;
        }

        if (!done) {
            if (cmd.timer)
                cmd.timer->updateTimeout(POLL_INTERVAL);
            return;
        }

        finishCommand(id);
    }

    void CNativeServices::finishCommand(int id) {
        auto it = m_commands.find(id);
        if (it == m_commands.end())
            return;

        SRunningCommand& cmd = it->second;
        if (cmd.timer)
            cmd.timer->cancel();

        invokeAndRelease(cmd.L, cmd.fnRef, &cmd.output);

        m_commands.erase(it);
    }

    void CNativeServices::openSocket(lua_State* L, const std::string& path, int fnRef) {
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            Log::logger->log(Log::WARN, "[hyprlui] open_socket('{}'): socket() failed: {}", path, strerror(errno));
            invokeAndRelease(L, fnRef, nullptr);
            return;
        }

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        // Blocking connect() - deliberate, see NativeServices.hpp's doc
        // comment on openSocket(): realistically instant for a local
        // Unix domain socket, unlike a network connect() (which Phase 12
        // explicitly excludes - Unix domain sockets only).
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            Log::logger->log(Log::WARN, "[hyprlui] open_socket('{}'): connect() failed: {}", path, strerror(errno));
            close(fd);
            invokeAndRelease(L, fnRef, nullptr);
            return;
        }

        setNonBlocking(fd);

        const int id = ++m_nextSocketId;
        SSocket   sock;
        sock.L  = L;
        sock.fd = CFileDescriptor(fd);
        m_sockets.emplace(id, std::move(sock));

        lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef);
        pushSocketWrapper(L, id);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            Log::logger->log(Log::ERR, "[hyprlui] error in open_socket callback: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        luaL_unref(L, LUA_REGISTRYINDEX, fnRef);
    }

    // Called only once CNativeServices::get().hasSocket(id) has already
    // been confirmed true by the caller (luaSocketRead) - `id` is
    // trusted to exist here.
    void CNativeServices::armSocketRead(int id, int fnRef) {
        SSocket& sock = m_sockets.at(id);

        // Only one pending :read() at a time per socket - a second call
        // before the first resolves replaces it (releasing the old ref),
        // matching "last call wins" rather than queuing.
        if (sock.pendingReadFnRef != LUA_NOREF)
            luaL_unref(sock.L, LUA_REGISTRYINDEX, sock.pendingReadFnRef);
        sock.pendingReadFnRef = fnRef;

        if (!sock.readTimer) {
            sock.readTimer = makeShared<CEventLoopTimer>(
                POLL_INTERVAL,
                [this, id](SP<CEventLoopTimer> self, void* data) {
                    auto it2 = m_sockets.find(id);
                    if (it2 == m_sockets.end())
                        return; // closed by some other path already

                    SSocket& s = it2->second;
                    if (s.pendingReadFnRef == LUA_NOREF)
                        return; // nothing waiting right now; armSocketRead() rearms us when it is

                    char          buf[4096];
                    const ssize_t n = read(s.fd.get(), buf, sizeof(buf));

                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        self->updateTimeout(POLL_INTERVAL); // still nothing - keep waiting
                        return;
                    }

                    const int  fn      = s.pendingReadFnRef;
                    lua_State* L       = s.L;
                    s.pendingReadFnRef = LUA_NOREF;

                    lua_rawgeti(L, LUA_REGISTRYINDEX, fn);
                    if (n > 0)
                        lua_pushlstring(L, buf, n);
                    else
                        lua_pushnil(L); // EOF (n == 0) or a real read error - either way, done
                    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                        Log::logger->log(Log::ERR, "[hyprlui] error in socket:read() callback: {}", lua_tostring(L, -1));
                        lua_pop(L, 1);
                    }
                    luaL_unref(L, LUA_REGISTRYINDEX, fn);

                    if (n <= 0)
                        closeSocket(id); // peer closed or errored - tear down; `s` is dangling after this
                },
                nullptr);

            if (g_pEventLoopManager)
                g_pEventLoopManager->addTimer(sock.readTimer);
        } else {
            sock.readTimer->updateTimeout(POLL_INTERVAL);
        }
    }

    bool CNativeServices::hasSocket(int id) const {
        return m_sockets.contains(id);
    }

    bool CNativeServices::writeSocket(int id, const std::string& data) {
        auto it = m_sockets.find(id);
        if (it == m_sockets.end()) {
            Log::logger->log(Log::WARN, "[hyprlui] socket:write(): socket is closed");
            return false;
        }

        const ssize_t n = write(it->second.fd.get(), data.data(), data.size());
        if (n < 0) {
            Log::logger->log(Log::WARN, "[hyprlui] socket:write() failed: {}", strerror(errno));
            return false;
        }
        return true;
    }

    void CNativeServices::closeSocket(int id) {
        auto it = m_sockets.find(id);
        if (it == m_sockets.end())
            return;

        SSocket& sock = it->second;
        if (sock.readTimer)
            sock.readTimer->cancel();
        if (sock.pendingReadFnRef != LUA_NOREF)
            luaL_unref(sock.L, LUA_REGISTRYINDEX, sock.pendingReadFnRef);

        m_sockets.erase(it); // ~CFileDescriptor closes the fd
    }

    void CNativeServices::clear() {
        for (auto& [id, cmd] : m_commands) {
            if (cmd.timer)
                cmd.timer->cancel();
            if (cmd.pid > 0)
                kill(cmd.pid, SIGTERM); // best-effort; SA_NOCLDWAIT means we never reap it ourselves either way
            luaL_unref(cmd.L, LUA_REGISTRYINDEX, cmd.fnRef);
        }
        m_commands.clear();

        for (auto& [id, sock] : m_sockets) {
            if (sock.readTimer)
                sock.readTimer->cancel();
            if (sock.pendingReadFnRef != LUA_NOREF)
                luaL_unref(sock.L, LUA_REGISTRYINDEX, sock.pendingReadFnRef);
        }
        m_sockets.clear(); // ~CFileDescriptor closes every fd
    }

} // namespace HyprLUI
