-- notification-daemon.lua
--
-- Standalone Lua 5.1 process - NOT loaded into Hyprland's own Lua 5.5
-- config state. ldbus (the D-Bus binding this depends on) has no
-- upstream Lua 5.5 build - its own rockspec hard-excludes >= 5.5 - so
-- this has to run as a separate process under the system's plain `lua`
-- (5.1), not inside HyprLUI/Hyprland itself. Run via `nix develop`
-- (flake.nix's devShell already exports LUA_CPATH/LUA_PATH for ldbus/
-- luasocket) or with those set manually elsewhere:
--   lua demos/notification-manager/daemon/notification-daemon.lua
--
-- Registers as the REAL org.freedesktop.Notifications D-Bus service -
-- the same interface notify-send/libnotify/Chromium/etc. all call - and
-- relays every Notify() call as one JSON line over a Unix domain socket
-- at $XDG_RUNTIME_DIR/hyprlui-notifications.sock (override via
-- HYPRLUI_NOTIFY_SOCKET). HyprLUI's own hyprlui.open_socket() (client-
-- only, no listen/accept of its own - see docs/api.md) connects to it
-- from there; that consumer doesn't exist yet, this is the daemon half
-- only. Verified interactively against a live session bus (ldbus's own
-- README/example weren't fully trustworthy on method names - some
-- differed from what actually shipped) before writing this file, not
-- guessed from docs.
--
-- Claims the bus name with replace_existing=true, since testing this
-- means actually receiving real notify-send calls - this WILL steal
-- notification delivery from whatever's already running (mako/dunst/
-- swaync/...) for as long as this process is alive. Ctrl+C to give it
-- back; most daemons re-claim the name on their own once it's released.
-- Fine to run standalone/experimentally, just don't leave it as your
-- only notification handler.

local ldbus = require("ldbus")
local socket = require("socket")
local unix = require("socket.unix")

local SOCKET_PATH = os.getenv("HYPRLUI_NOTIFY_SOCKET") or (os.getenv("XDG_RUNTIME_DIR") or "/tmp") .. "/hyprlui-notifications.sock"

--------------------------------------------------
---- minimal JSON encoder ----
--------------------------------------------------
-- Lua 5.1 has no json.encode - just enough for this daemon's own flat
-- event shape (strings/numbers/booleans/arrays/objects), not a general-
-- purpose encoder. Mirrors demos/which-key.lua's own hand-rolled JSON
-- *decoder* in spirit, opposite direction.
local function jsonEncode(value)
	local t = type(value)
	if t == "string" then
		return string.format("%q", value)
	elseif t == "number" or t == "boolean" then
		return tostring(value)
	elseif t ~= "table" then
		return "null"
	end

	local isArray, n = true, 0
	for k in pairs(value) do
		n = n + 1
		if type(k) ~= "number" then
			isArray = false
		end
	end

	local parts = {}
	if isArray and n == #value then
		for i, v in ipairs(value) do
			parts[i] = jsonEncode(v)
		end
		return "[" .. table.concat(parts, ",") .. "]"
	end
	for k, v in pairs(value) do
		parts[#parts + 1] = string.format("%q", tostring(k)) .. ":" .. jsonEncode(v)
	end
	return "{" .. table.concat(parts, ",") .. "}"
end

--------------------------------------------------
---- D-Bus connection ----
--------------------------------------------------
local conn = assert(ldbus.bus.get("session"))
local owned, ownErr = ldbus.bus.request_name(conn, "org.freedesktop.Notifications", { replace_existing = true })
if not owned then
	io.stderr:write("[hyprlui-notify-daemon] failed to claim org.freedesktop.Notifications: " .. tostring(ownErr) .. "\n")
	os.exit(1)
end
print("[hyprlui-notify-daemon] owns org.freedesktop.Notifications (" .. owned .. ")")

local nextId = 1

-- Reads an "as" (array of string) argument at the iterator's current
-- position - actions, in Notify's case.
local function readStringArray(iter)
	local out = {}
	local sub = iter:recurse()
	while sub do
		out[#out + 1] = sub:get_basic()
		if not sub:next() then
			break
		end
	end
	return out
end

-- Reads an "a{sv}" (dict of string -> variant) argument - Notify's
-- hints. Each entry is itself a container (key, then value) - recurse()
-- twice: once into the array to get each dict_entry, once more into the
-- dict_entry itself. Only scalar variant values are read (get_basic());
-- a struct/array-typed hint value is skipped, not erroring the whole
-- notification out over one hint we don't understand.
local function readHints(iter)
	local hints = {}
	local dictIter = iter:recurse()
	while dictIter do
		local entry = dictIter:recurse()
		local key = entry:get_basic()
		entry:next()
		local variantIter = entry:recurse()
		local ok, value = pcall(function()
			return variantIter:get_basic()
		end)
		if ok then
			hints[key] = value
		end
		if not dictIter:next() then
			break
		end
	end
	return hints
end

-- Notify's full signature per the spec: susssasa{sv}i -> u
local function readNotify(msg)
	local iter = msg:iter_init()
	local appName = iter:get_basic()
	iter:next()
	local replacesId = iter:get_basic()
	iter:next()
	local appIcon = iter:get_basic()
	iter:next()
	local summary = iter:get_basic()
	iter:next()
	local body = iter:get_basic()
	iter:next()
	local actions = readStringArray(iter)
	iter:next()
	local hints = readHints(iter)
	iter:next()
	local expireTimeout = iter:get_basic()

	return {
		appName = appName,
		replacesId = replacesId,
		appIcon = appIcon,
		summary = summary,
		body = body,
		actions = actions,
		hints = hints,
		expireTimeout = expireTimeout,
	}
end

--------------------------------------------------
---- Unix socket server ----
--------------------------------------------------
-- HyprLUI-side consumers connect here (as clients - see docs/api.md's
-- open_socket()) and receive one JSON line per event: {type="notify",
-- ...} or {type="closed", id=..., reason=...}.
os.remove(SOCKET_PATH)
local server = assert(unix())
assert(server:bind(SOCKET_PATH))
assert(server:listen(8))
server:settimeout(0)
print("[hyprlui-notify-daemon] listening on " .. SOCKET_PATH)

local clients = {}

local function acceptPending()
	while true do
		local client = server:accept()
		if not client then
			break
		end
		client:settimeout(0)
		clients[#clients + 1] = client
		print("[hyprlui-notify-daemon] client connected (" .. #clients .. " total)")
	end
end

local function broadcast(event)
	local line = jsonEncode(event) .. "\n"
	for i = #clients, 1, -1 do
		local ok = clients[i]:send(line)
		if not ok then
			clients[i]:close()
			table.remove(clients, i)
		end
	end
end

--------------------------------------------------
---- reply helpers ----
--------------------------------------------------
local function replyUint32(msg, value)
	local reply = msg:new_method_return()
	local iter = reply:iter_init_append()
	iter:append_basic(value, ldbus.basic_types.uint32)
	conn:send(reply)
end

local function replyEmpty(msg)
	conn:send(msg:new_method_return())
end

local function replyStringArray(msg, values)
	local reply = msg:new_method_return()
	local iter = reply:iter_init_append()
	local sub = iter:open_container(ldbus.types.array, ldbus.basic_types.string)
	for _, v in ipairs(values) do
		sub:append_basic(v, ldbus.basic_types.string)
	end
	iter:close_container(sub)
	conn:send(reply)
end

local function replyServerInformation(msg)
	local reply = msg:new_method_return()
	local iter = reply:iter_init_append()
	iter:append_basic("hyprlui-notification-daemon", ldbus.basic_types.string)
	iter:append_basic("HyprLUI", ldbus.basic_types.string)
	iter:append_basic("0.1", ldbus.basic_types.string)
	iter:append_basic("1.2", ldbus.basic_types.string)
	conn:send(reply)
end

--------------------------------------------------
---- method dispatch ----
--------------------------------------------------
-- One lambda per org.freedesktop.Notifications method actually
-- supported. Capabilities are kept honest - only what's genuinely
-- forwarded ("body", "actions"), not a wishlist of things this daemon
-- doesn't do (no persistence/sound/markup support here).
local handlers = {
	Notify = function(msg)
		local n = readNotify(msg)
		local id = n.replacesId ~= 0 and n.replacesId or nextId
		if id == nextId then
			nextId = nextId + 1
		end
		replyUint32(msg, id)
		broadcast({
			type = "notify",
			id = id,
			appName = n.appName,
			appIcon = n.appIcon,
			summary = n.summary,
			body = n.body,
			actions = n.actions,
			hints = n.hints,
			expireTimeout = n.expireTimeout,
		})
	end,

	CloseNotification = function(msg)
		local iter = msg:iter_init()
		local id = iter:get_basic()
		replyEmpty(msg)

		-- reason 3: "closed by a call to CloseNotification" - see the
		-- spec's NotificationClosed signal reasons.
		local signal = ldbus.message.new_signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", "NotificationClosed")
		local sIter = signal:iter_init_append()
		sIter:append_basic(id, ldbus.basic_types.uint32)
		sIter:append_basic(3, ldbus.basic_types.uint32)
		conn:send(signal)

		broadcast({ type = "closed", id = id, reason = 3 })
	end,

	GetCapabilities = function(msg)
		replyStringArray(msg, { "body", "actions" })
	end,

	GetServerInformation = function(msg)
		replyServerInformation(msg)
	end,
}

--------------------------------------------------
---- main loop ----
--------------------------------------------------
-- Polls D-Bus (conn:read_write, non-blocking with a short timeout) and
-- the notification socket (non-blocking accept/broadcast) in the same
-- loop - a short sleep only when NEITHER had anything, so this doesn't
-- busy-spin the CPU while idle.
while true do
	conn:read_write(20)
	acceptPending()

	local msg = conn:pop_message()
	local hadWork = msg ~= nil

	if msg and msg:get_type() == "method_call" and msg:get_interface() == "org.freedesktop.Notifications" then
		local handler = handlers[msg:get_member()]
		if handler then
			local ok, err = pcall(handler, msg)
			if not ok then
				io.stderr:write("[hyprlui-notify-daemon] error handling " .. tostring(msg:get_member()) .. ": " .. tostring(err) .. "\n")
			end
		end
	end

	if not hadWork then
		socket.sleep(0.02)
	end
end
