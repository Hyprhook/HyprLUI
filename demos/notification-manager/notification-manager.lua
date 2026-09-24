local M = {}
-- notification-manager.lua
--
-- HyprLUI-side consumer for demos/notification-manager/daemon/
-- notification-daemon.lua - connects to its Unix socket
-- ($XDG_RUNTIME_DIR/hyprlui-notifications.sock, override via
-- HYPRLUI_NOTIFY_SOCKET) as a client (hyprlui.open_socket() is
-- client-only - see docs/api.md) and renders each relayed Notify() as a
-- toast card stacked top-right, auto-dismissing after a per-urgency
-- timeout (or on click).
--
-- Each card is added/removed via hyprlui.add_widget()/remove_widget()
-- (TASKS.md's own task 1) - a persistent widget tree the window keeps
-- across notifications, not a remove_canvas+window() rebuild-from-
-- scratch every time (the old approach - see git history). That's also
-- what makes animationIn/animationOut/animationLayout (task 2) on each
-- card actually fire: a freshly-rebuilt-every-frame widget never had a
-- "just became visible"/"just moved" transition to animate in the first
-- place.
--
-- Customization follows demos/which-key.lua's own CONFIG/M.setup(opts)
-- convention - see its own header comment for why (a single place to
-- edit, re-evaluated fresh from scratch on every config reload).

-- require("./demos/jsondecode") resolves relative to the MAIN CONFIG's
-- own directory, not this file's - breaks outside HyprLUI's own dev
-- hyprlandd.lua (confirmed live). Locate it relative to THIS file's own
-- path instead, via debug.getinfo - see demos/which-key.lua's identical
-- block.
local scriptDir = debug.getinfo(1, "S").source:match("^@(.*/)")
package.path = scriptDir .. "../?.lua;" .. package.path
local jsonDecode = require("jsondecode").decode

local WINDOW_NAME = "hyprlui_notification_stack"

-- Single customization block for this demo - see demos/which-key.lua's
-- own CONFIG for the same convention. `animationIn`/`animationOut`/
-- `animationLayout` default to nil (instant, no animation at all -
-- matching CWidget's own "field not set = disabled" default) rather
-- than shipping some default transition - see M.setup()'s own note on
-- this if you want the bouncy stress-test version back (that's how the
-- reflow-ghosting bug in TASKS.md's task 2 actually got caught).
local CONFIG = {
	socketPath = os.getenv("HYPRLUI_NOTIFY_SOCKET")
		or (os.getenv("XDG_RUNTIME_DIR") or "/tmp") .. "/hyprlui-notifications.sock",

	anchor = "top-right",
	xOffset = 16,
	yOffset = 16,

	cardWidth = 320,
	gap = 8,
	iconSize = 32,
	iconGap = 8,

	bgColor = 0xff1e1e2e,
	titleColor = 0xffcdd6f4,
	bodyColor = 0xffa6adc8,
	font = "sans",
	titleSize = 13,
	bodySize = 12,
	rounding = 8,
	borderWidth = 2,

	-- action buttons (Notify's `actions` array) - wrapped into rows of
	-- actionsPerRow (more than that per row and each button gets too
	-- narrow to read), only reserved on cards that actually have any.
	actionsPerRow = 2,
	actionRowHeight = 28,
	actionGap = 6,
	actionButtonColor = 0xff313244,
	actionTextColor = 0xffcdd6f4,
	actionTextSize = 12,

	-- urgency (spec: 0=low, 1=normal, 2=critical) -> accent color +
	-- default dwell time when the sender didn't request a specific
	-- expire_timeout. 5s/8s/forever mirrors the convention most status-
	-- bar toast stacks use. `nil` timeout = never auto-dismiss.
	urgency = {
		[0] = { color = 0xff6c7086, timeout = 5000 },
		[1] = { color = 0xff89b4fa, timeout = 8000 },
		[2] = { color = 0xfff38ba8, timeout = nil },
	},

	animationIn = nil,
	animationOut = nil,
	animationLayout = nil,
}

local liveIds = {} -- set of notification ids currently rendered as a card
local windowCreated = false
local currentSock = nil -- the live daemon connection, if any - set in startReadLoop(), cleared on disconnect (see "socket connection" section below)

local function warn(label, err)
	hl.notification.create({ text = label .. " failed: " .. tostring(err), timeout = 3000 })
end

-- Tells the daemon the user picked an action button - it relays this to
-- the ORIGINAL sending app as a real ActionInvoked D-Bus signal (see
-- decodeActionMessage()/sendActionInvoked() in the daemon). Silently
-- no-ops if the socket isn't currently connected, matching this file's
-- own "outages self-heal, don't block on them" stance elsewhere (see
-- connect()'s own doc comment) rather than erroring - the button click
-- itself still dismisses the card either way (see addCard()).
local function sendAction(id, key)
	if not currentSock then
		return
	end
	currentSock:write(string.format('{"type":"action","id":%d,"key":%q}\n', id, key))
end

--------------------------------------------------
---- icon resolution ----
--------------------------------------------------
-- app_icon (freedesktop Notifications spec) is either an absolute path
-- (handled directly, no lookup needed) or a bare icon-theme name like
-- "firefox"/"dialog-warning" - resolving THAT means walking the
-- installed icon themes ourselves; neither HyprLUI nor Hyprland has any
-- such facility built in. Deliberately NOT full XDG icon-theme-spec
-- compliant (no index.theme parsing, no theme inheritance, no
-- size/scale matching) - just a recursive filename search across every
-- installed theme (plus the flat /usr/share/pixmaps fallback the spec
-- itself carves out for legacy apps), first match wins. Good enough to
-- find most real icons; a demo, not a spec-compliant icon loader.
--
-- hints["image-data"] (TASKS.md task 4b) - a raw ARGB32 pixel buffer
-- some senders (Vesktop/Discord confirmed) use instead of app_icon -
-- is NOT handled by this function at all. It needs no path/theme-name
-- resolution (the daemon already hands over the decoded bytes directly)
-- so handleNotify() branches around resolveIcon() entirely for that
-- case - see its own comment.
local function iconSearchDirs()
	local home = os.getenv("HOME") or ""
	local dataHome = os.getenv("XDG_DATA_HOME") or (home .. "/.local/share")
	local dataDirs = os.getenv("XDG_DATA_DIRS") or "/usr/local/share:/usr/share"

	local dirs = { dataHome .. "/icons", home .. "/.icons" }
	for dir in dataDirs:gmatch("[^:]+") do
		table.insert(dirs, dir .. "/icons")
	end
	table.insert(dirs, "/usr/share/pixmaps")

	-- Single-quoted for the shell command resolveIcon() below builds -
	-- these come from env vars, not the (untrusted, comes off the
	-- session bus) icon name itself, but quoting defensively costs
	-- nothing and correctly handles a space in e.g. $HOME.
	for i, dir in ipairs(dirs) do
		dirs[i] = "'" .. dir:gsub("'", [['\'']]) .. "'"
	end
	return table.concat(dirs, " ")
end
local ICON_SEARCH_DIRS = iconSearchDirs()

-- Freedesktop icon names are conventionally reverse-DNS-like identifiers
-- - letters/digits/._- only. Doubles as the actual defense here: `name`
-- comes off the session bus (any local process can send a Notify() call)
-- and gets shell-interpolated into a `find` command below, so anything
-- outside this allowlist is rejected outright rather than escaped.
local ICON_NAME_PATTERN = "^[%w_.-]+$"

local iconCache = {} -- appIcon name -> resolved path, or "" for "looked up, not found"

-- Resolves `appIcon` to a usable Image `path` and calls cb(path|nil).
-- Async (shells out to `find` via hl.plugin.hyprlui.run_cmd - NativeServices'
-- own polling-based non-blocking I/O, not a blocking io.popen(), which
-- would stall the compositor while the search runs) except for the
-- already-a-path and already-cached cases, which call back immediately.
local function resolveIcon(appIcon, cb)
	if not appIcon or appIcon == "" then
		cb(nil)
		return
	end
	if appIcon:sub(1, 1) == "/" then
		cb(appIcon) -- already a path - Image{} fails gracefully (fill+border, no crash) if it turns out not to exist
		return
	end
	if iconCache[appIcon] ~= nil then
		cb(iconCache[appIcon] ~= "" and iconCache[appIcon] or nil)
		return
	end
	if not appIcon:match(ICON_NAME_PATTERN) or hl.plugin.hyprlui == nil then
		iconCache[appIcon] = "" -- cache the miss so a malformed/adversarial name isn't re-checked every notification
		cb(nil)
		return
	end

	local quoted = "'" .. appIcon .. "'" -- safe: ICON_NAME_PATTERN above already rejected anything but [%w_.-]
	local cmd = "find "
		.. ICON_SEARCH_DIRS
		.. " -iname "
		.. quoted
		.. ".svg -o -iname "
		.. quoted
		.. ".png 2>/dev/null | head -n1"
	hl.plugin.hyprlui.run_cmd(cmd, function(output)
		local path = (output or ""):gsub("%s+$", "")
		iconCache[appIcon] = path
		cb(path ~= "" and path or nil)
	end)
end

--------------------------------------------------
---- window/card lifecycle ----
--------------------------------------------------
-- The window itself is created once, empty, and never rebuilt - only
-- individual cards get added/removed from its root Column from here on.
local function ensureWindow()
	if windowCreated then
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = WINDOW_NAME,
			anchor = CONFIG.anchor,
			x = CONFIG.xOffset,
			y = CONFIG.yOffset,
			hl.plugin.hyprlui.Column({ id = "root", gap = CONFIG.gap }),
		})
	end)
	if not ok then
		warn("hyprlui.window (notification-manager)", err)
		return
	end
	windowCreated = true
end

-- Card layout: a Stack with the Box (background/border/click target),
-- optional icon Image, and the two Text labels as SIBLINGS, not nested
-- inside the Box - Box never renders children (only Button/Input do,
-- see docs/api.md's own note on this, found the hard way building
-- demos/task-checks.lua). The icon (n.iconPath from resolveIcon(), or
-- n.iconPixels straight off hints["image-data"] - handleNotify() only
-- ever sets one of the two) shifts the text column over when present
-- rather than overlapping it; omitted entirely when there is no icon,
-- keeping the original layout for that case.
local function addCard(n)
	local title = n.appName ~= "" and (n.appName .. ": " .. n.summary) or n.summary
	local id = n.id
	local hasIcon = n.iconPath ~= nil or n.iconPixels ~= nil
	local textX = hasIcon and (12 + CONFIG.iconSize + CONFIG.iconGap) or 12
	local maxW = CONFIG.cardWidth - textX - 12
	-- n.actions is Notify's own flat [key1, label1, key2, label2, ...]
	-- shape (freedesktop Notifications spec) - every entry rendered as a
	-- button uniformly, including a sender's "default" key if it sends
	-- one (some desktop notification servers treat "default" specially -
	-- invoked on a body click instead of shown as its own button - not
	-- done here, keeps every action key on equal footing rather than
	-- inventing a hidden special case nobody asked for).
	local hasActions = n.actions ~= nil and #n.actions > 0
	local numActionRows = hasActions and math.ceil((#n.actions / 2) / CONFIG.actionsPerRow) or 0
	local cardHeight = 64 + numActionRows * CONFIG.actionRowHeight + math.max(0, numActionRows - 1) * CONFIG.actionGap

	local ok, err = pcall(function()
		local spec = {
			id = "card_" .. id,
			w = CONFIG.cardWidth,
			h = cardHeight,
			animationIn = CONFIG.animationIn,
			animationOut = CONFIG.animationOut,
			-- Lets THIS card slide smoothly into a new slot when a
			-- sibling above it is dismissed, instead of snapping -
			-- animationOut (above, on the card actually being removed)
			-- stopping its own layout space immediately is what makes
			-- that reflow start at the same time as the dismissed
			-- card's own fade-out, not after it.
			animationLayout = CONFIG.animationLayout,
		}
		table.insert(
			spec,
			hl.plugin.hyprlui.Box({
				id = "bg_" .. id,
				fill = true,
				color = CONFIG.bgColor,
				rounding = CONFIG.rounding,
				borderColor = n.color,
				borderWidth = CONFIG.borderWidth,
				onClick = function()
					M.dismiss(id)
				end,
			})
		)
		if hasIcon then
			table.insert(
				spec,
				hl.plugin.hyprlui.Image({
					id = "icon_" .. id,
					x = 12,
					y = (64 - CONFIG.iconSize) / 2,
					w = CONFIG.iconSize,
					h = CONFIG.iconSize,
					path = n.iconPath,
					pixels = n.iconPixels,
				})
			)
		end
		table.insert(
			spec,
			hl.plugin.hyprlui.Text({
				id = "summary_" .. id,
				x = textX,
				y = 8,
				text = title,
				maxW = maxW,
				size = CONFIG.titleSize,
				font = CONFIG.font,
				color = CONFIG.titleColor,
			})
		)
		table.insert(
			spec,
			hl.plugin.hyprlui.Text({
				id = "body_" .. id,
				x = textX,
				y = 30,
				text = n.body,
				maxW = maxW,
				size = CONFIG.bodySize,
				font = CONFIG.font,
				color = CONFIG.bodyColor,
			})
		)
		if hasActions then
			-- Wrapped into rows of CONFIG.actionsPerRow rather than one
			-- long Row - past a couple of buttons, evenly dividing the
			-- card's own width among all of them at once makes each one
			-- too narrow to read. Each row's buttons only divide THAT
			-- row's width among themselves, so a leftover last row (e.g.
			-- 3 actions at actionsPerRow=2 -> a lone 3rd button) goes
			-- full-width instead of matching the earlier rows' narrower
			-- width for no real reason.
			local rowWidth = CONFIG.cardWidth - 24
			local actionIndex = 0
			for i = 1, #n.actions, 2 * CONFIG.actionsPerRow do
				local remainingPairs = (#n.actions - i + 1) / 2
				local rowActionCount = math.min(CONFIG.actionsPerRow, remainingPairs)
				local btnWidth = (rowWidth - (rowActionCount - 1) * CONFIG.actionGap) / rowActionCount
				local rowIndex = actionIndex / CONFIG.actionsPerRow
				local row = {
					id = "actions_" .. id .. "_" .. rowIndex,
					x = 12,
					y = 64 + rowIndex * (CONFIG.actionRowHeight + CONFIG.actionGap),
					gap = CONFIG.actionGap,
				}
				for j = i, math.min(i + 2 * CONFIG.actionsPerRow - 1, #n.actions), 2 do
					local key, label = n.actions[j], n.actions[j + 1]
					table.insert(
						row,
						hl.plugin.hyprlui.Button({
							id = "action_" .. id .. "_" .. j,
							w = btnWidth,
							h = CONFIG.actionRowHeight - 6,
							color = CONFIG.actionButtonColor,
							rounding = 4,
							onClick = function()
								sendAction(id, key)
								M.dismiss(id)
							end,
							hl.plugin.hyprlui.Text({
								x = 8,
								y = 4,
								text = label,
								size = CONFIG.actionTextSize,
								font = CONFIG.font,
								color = CONFIG.actionTextColor,
							}),
						})
					)
				end
				table.insert(spec, hl.plugin.hyprlui.Row(row))
				actionIndex = actionIndex + CONFIG.actionsPerRow
			end
		end

		hl.plugin.hyprlui.add_widget(WINDOW_NAME, "root", hl.plugin.hyprlui.Stack(spec))
	end)
	if not ok then
		warn("hyprlui.add_widget (notification-manager)", err)
		return
	end
	liveIds[id] = true
end

-- Idempotent - a click-dismiss racing an already-fired auto-dismiss
-- timer (or vice versa) just no-ops the second call, not an error.
--
-- Reached from async contexts (a hl.timer dwell callback, the daemon
-- socket's "closed" event, a card's onClick) that can still fire after
-- hl.plugin.hyprlui goes away mid-flight - e.g. the plugin binary itself
-- getting rebuilt/reloaded while a dwell timer is pending, not just the
-- Lua config reloading (which reuses the same still-live plugin). Same
-- guard + pcall discipline as every other hl.plugin.hyprlui call site in
-- this file/demos/which-key.lua - unguarded here would otherwise raise
-- straight out of a timer/socket callback with no pcall boundary above
-- it in the Lua call stack.
function M.dismiss(id)
	if not liveIds[id] then
		return
	end
	liveIds[id] = nil

	if hl.plugin.hyprlui == nil then
		return
	end

	local ok, err = pcall(hl.plugin.hyprlui.remove_widget, WINDOW_NAME, "card_" .. id)
	if not ok then
		warn("hyprlui.remove_widget (notification-manager)", err)
	end
end

--------------------------------------------------
---- daemon event handling ----
--------------------------------------------------
local function handleNotify(event)
	local urgency = (event.hints and event.hints.urgency) or 1
	local style = CONFIG.urgency[urgency] or CONFIG.urgency[1]

	local dwellMs
	if event.expireTimeout and event.expireTimeout > 0 then
		dwellMs = event.expireTimeout -- sender asked for a specific timeout
	elseif event.expireTimeout ~= 0 then
		dwellMs = style.timeout -- expireTimeout omitted/-1: server (us) decides, per urgency
	end
	-- expireTimeout == 0 means "never auto-dismiss" - dwellMs stays nil either way in that case

	ensureWindow()

	-- replaces_id: no in-place update mutator exists yet (DESIGN.md task
	-- 9) - swap the card out for a fresh one instead. Loses continuity
	-- for that one update (a visible out-then-in instead of a smooth
	-- content swap), acceptable until task 9 lands.
	if liveIds[event.id] then
		M.dismiss(event.id)
	end

	-- Actionable notifications don't auto-dismiss on the usual per-urgency
	-- schedule - a 5-8s default timeout isn't much time to read two
	-- buttons and decide, and most desktop notification systems (e.g.
	-- GNOME) treat this the same way. Still dismissible by clicking the
	-- card body or an action button itself (see addCard()).
	local hasActions = event.actions ~= nil and #event.actions > 0

	local function finish(iconPath, iconPixels)
		addCard({
			id = event.id,
			appName = event.appName or "",
			summary = event.summary or "",
			body = event.body or "",
			color = style.color,
			iconPath = iconPath,
			iconPixels = iconPixels,
			actions = event.actions,
		})

		if dwellMs and not hasActions then
			hl.timer(function()
				M.dismiss(event.id)
			end, { timeout = dwellMs, type = "oneshot" })
		end
	end

	-- hints["image-data"] (TASKS.md task 4b) - a raw ARGB32 pixel buffer
	-- some senders (Vesktop/Discord confirmed live via dbus-monitor) use
	-- INSTEAD of app_icon/image-path - already the actual bytes (the
	-- daemon's decodeImageData() forwards {width, height, rowstride,
	-- hasAlpha, channels, dataBase64}, matching Image{}'s own `pixels`
	-- field shape directly, no remapping needed), so this skips
	-- resolveIcon() entirely and takes priority when present - no
	-- path/theme-name to resolve in the first place.
	local imageData = event.hints
		and (event.hints["image-data"] or event.hints["icon_data"] or event.hints["image_data"])
	if imageData then
		finish(nil, imageData)
		return
	end

	-- app_icon (the positional field) is frequently left empty by real
	-- senders - confirmed live via dbus-monitor that this system's own
	-- notify-send (libnotify 0.8.8) puts `-i`'s value into
	-- hints["image-path"] instead, leaving app_icon "". Both are valid
	-- per the Notifications spec for a plain path/theme-name reference -
	-- try app_icon first since it's the canonical field, fall back to
	-- the hint.
	local appIcon = event.appIcon
	if not appIcon or appIcon == "" then
		appIcon = event.hints and event.hints["image-path"]
	end

	-- resolveIcon() is async (cached hits/already-a-path still call back
	-- immediately, but a fresh icon-theme-name lookup shells out) - the
	-- card only gets built once it resolves, so the dwell timer above
	-- starts from when the card actually appears, not from event
	-- arrival.
	resolveIcon(appIcon, function(iconPath)
		finish(iconPath, nil)
	end)
end

local function handleEvent(event)
	if event.type == "notify" then
		handleNotify(event)
	elseif event.type == "closed" then
		M.dismiss(event.id)
	end
end

--------------------------------------------------
---- socket connection ----
--------------------------------------------------
-- Neither a failed connect NOR a mid-session disconnect (daemon
-- restarted/crashed) recovered on its own before this - the read loop
-- just stopped, silently, until the next full Lua config reload happened
-- to call M.setup() again. `connect()` below is the retry loop for both
-- cases; `warnedDisconnected` limits the "can't reach the daemon"
-- notification to once per outage instead of once per retry (the daemon
-- being down for a while would otherwise spam a popup every
-- RECONNECT_DELAY_MS).
local RECONNECT_DELAY_MS = 3000
local warnedDisconnected = false

local function warnDisconnectedOnce(reason)
	if warnedDisconnected then
		return
	end
	warnedDisconnected = true
	warn("hyprlui notification-manager", reason .. " - retrying every " .. (RECONNECT_DELAY_MS / 1000) .. "s")
end

-- Forward-declared: startReadLoop()'s onData (below) needs to schedule a
-- reconnect through this on disconnect, and connect() (further below)
-- needs to hand its successful connection to startReadLoop() - by the
-- time either closure actually RUNS, `connect` has already been assigned
-- its function value, same as any other mutually-recursive local pair.
local connect

-- open_socket()'s sock:read(callback) delivers one read()'s worth of
-- data per call, not necessarily one line - a JSON line from the daemon
-- can split across reads or arrive batched with others, so this buffers
-- and splits on "\n" itself rather than assuming line-sized chunks.
local function startReadLoop(sock)
	local buffer = ""
	currentSock = sock

	local function onData(chunk)
		if not chunk then
			currentSock = nil
			warnDisconnectedOnce("daemon connection closed")
			hl.timer(connect, { timeout = RECONNECT_DELAY_MS, type = "oneshot" })
			return
		end

		buffer = buffer .. chunk
		while true do
			local nl = buffer:find("\n", 1, true)
			if not nl then
				break
			end
			local line = buffer:sub(1, nl - 1)
			buffer = buffer:sub(nl + 1)
			if #line > 0 then
				local ok, event = pcall(jsonDecode, line)
				if ok then
					handleEvent(event)
				end
			end
		end

		sock:read(onData)
	end

	sock:read(onData)
end

-- Same guard discipline as M.dismiss() (see its own doc comment) - the
-- retry timer armed below can still fire after hl.plugin.hyprlui goes
-- away mid-flight (the plugin binary itself getting rebuilt/reloaded
-- while a reconnect is pending), so this has to survive that on its own
-- rather than assuming the M.setup() that originally started it is still
-- the one in charge.
function connect()
	if hl.plugin.hyprlui == nil then
		return
	end
	hl.plugin.hyprlui.open_socket(CONFIG.socketPath, function(sock)
		if not sock then
			warnDisconnectedOnce(
				"could not connect to " .. CONFIG.socketPath .. " - is notification-daemon.service running?"
			)
			hl.timer(connect, { timeout = RECONNECT_DELAY_MS, type = "oneshot" })
			return
		end
		warnedDisconnected = false
		startReadLoop(sock)
	end)
end

-- Call any time after require()'ing this module - same convention
-- demos/which-key.lua's own M.setup(opts) uses. Re-call on every config
-- reload (the whole script re-runs then anyway, so CONFIG's own defaults
-- above already reset themselves - this just reconnects the socket
-- fresh, same as before).
--
-- Every CONFIG field is a plain scalar override EXCEPT `urgency`, which
-- merges per-level/per-field into the existing defaults instead of
-- replacing the whole table - `opts.urgency = { [2] = { color = ... } }`
-- only changes critical's color, low/normal and critical's own timeout
-- stay at their built-in defaults. `animationIn`/`animationOut`/
-- `animationLayout` are the one exception to "merge" - those replace
-- whole-table, matching animationIn/animationOut's own documented
-- behavior elsewhere ("self-contained, not a partial merge").
function M.setup(opts)
	opts = opts or {}

	for _, key in ipairs({
		"socketPath",
		"anchor",
		"xOffset",
		"yOffset",
		"cardWidth",
		"gap",
		"iconSize",
		"iconGap",
		"bgColor",
		"titleColor",
		"bodyColor",
		"font",
		"titleSize",
		"bodySize",
		"rounding",
		"borderWidth",
		"actionsPerRow",
		"actionRowHeight",
		"actionGap",
		"actionButtonColor",
		"actionTextColor",
		"actionTextSize",
		"animationIn",
		"animationOut",
		"animationLayout",
	}) do
		if opts[key] ~= nil then
			CONFIG[key] = opts[key]
		end
	end

	if opts.urgency then
		for level, fields in pairs(opts.urgency) do
			CONFIG.urgency[level] = CONFIG.urgency[level] or {}
			for field, value in pairs(fields) do
				CONFIG.urgency[level][field] = value
			end
		end
	end

	liveIds = {}
	windowCreated = false
	warnedDisconnected = false
	currentSock = nil
	connect()
end

return M
