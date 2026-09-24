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

	bgColor = 0xff1e1e2e,
	titleColor = 0xffcdd6f4,
	bodyColor = 0xffa6adc8,
	font = "sans",
	titleSize = 13,
	bodySize = 12,
	rounding = 8,
	borderWidth = 2,

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

local function warn(label, err)
	hl.notification.create({ text = label .. " failed: " .. tostring(err), timeout = 3000 })
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

-- Card layout: a Stack with the Box (background/border/click target)
-- and the two Text labels as SIBLINGS, not nested inside the Box - Box
-- never renders children (only Button/Input do, see docs/api.md's own
-- note on this, found the hard way building demos/task-checks.lua).
local function addCard(n)
	local title = n.appName ~= "" and (n.appName .. ": " .. n.summary) or n.summary
	local id = n.id
	local maxW = CONFIG.cardWidth - 24

	local ok, err = pcall(function()
		hl.plugin.hyprlui.add_widget(
			WINDOW_NAME,
			"root",
			hl.plugin.hyprlui.Stack({
				id = "card_" .. id,
				w = CONFIG.cardWidth,
				h = 64,
				animationIn = CONFIG.animationIn,
				animationOut = CONFIG.animationOut,
				-- Lets THIS card slide smoothly into a new slot when a
				-- sibling above it is dismissed, instead of snapping -
				-- animationOut (above, on the card actually being
				-- removed) stopping its own layout space immediately is
				-- what makes that reflow start at the same time as the
				-- dismissed card's own fade-out, not after it.
				animationLayout = CONFIG.animationLayout,
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
				}),
				hl.plugin.hyprlui.Text({
					id = "summary_" .. id,
					x = 12,
					y = 8,
					text = title,
					maxW = maxW,
					size = CONFIG.titleSize,
					font = CONFIG.font,
					color = CONFIG.titleColor,
				}),
				hl.plugin.hyprlui.Text({
					id = "body_" .. id,
					x = 12,
					y = 30,
					text = n.body,
					maxW = maxW,
					size = CONFIG.bodySize,
					font = CONFIG.font,
					color = CONFIG.bodyColor,
				}),
			})
		)
	end)
	if not ok then
		warn("hyprlui.add_widget (notification-manager)", err)
		return
	end
	liveIds[id] = true
end

-- Idempotent - a click-dismiss racing an already-fired auto-dismiss
-- timer (or vice versa) just no-ops the second call, not an error.
function M.dismiss(id)
	if not liveIds[id] then
		return
	end
	liveIds[id] = nil
	hl.plugin.hyprlui.remove_widget(WINDOW_NAME, "card_" .. id)
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

	addCard({
		id = event.id,
		appName = event.appName or "",
		summary = event.summary or "",
		body = event.body or "",
		color = style.color,
	})

	if dwellMs then
		hl.timer(function()
			M.dismiss(event.id)
		end, { timeout = dwellMs, type = "oneshot" })
	end
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
-- open_socket()'s sock:read(callback) delivers one read()'s worth of
-- data per call, not necessarily one line - a JSON line from the daemon
-- can split across reads or arrive batched with others, so this buffers
-- and splits on "\n" itself rather than assuming line-sized chunks.
local function startReadLoop(sock)
	local buffer = ""

	local function onData(chunk)
		if not chunk then
			warn("hyprlui notification-manager", "daemon connection closed")
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
		"bgColor",
		"titleColor",
		"bodyColor",
		"font",
		"titleSize",
		"bodySize",
		"rounding",
		"borderWidth",
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
	if hl.plugin.hyprlui ~= nil then
		hl.plugin.hyprlui.open_socket(CONFIG.socketPath, function(sock)
			if not sock then
				warn(
					"hyprlui notification-manager",
					"could not connect to " .. CONFIG.socketPath .. " - is notification-daemon.service running?"
				)
				return
			end
			startReadLoop(sock)
		end)
	end
end

return M
