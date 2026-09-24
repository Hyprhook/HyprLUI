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

-- require("./demos/jsondecode") resolves relative to the MAIN CONFIG's
-- own directory, not this file's - breaks outside HyprLUI's own dev
-- hyprlandd.lua (confirmed live). Locate it relative to THIS file's own
-- path instead, via debug.getinfo - see demos/which-key.lua's identical
-- block.
local scriptDir = debug.getinfo(1, "S").source:match("^@(.*/)")
package.path = scriptDir .. "../?.lua;" .. package.path
local jsonDecode = require("jsondecode").decode

local WINDOW_NAME = "hyprlui_notification_stack"
local SOCKET_PATH = os.getenv("HYPRLUI_NOTIFY_SOCKET")
	or (os.getenv("XDG_RUNTIME_DIR") or "/tmp") .. "/hyprlui-notifications.sock"
local CARD_WIDTH = 320

-- urgency (spec: 0=low, 1=normal, 2=critical) -> accent color + default
-- dwell time when the sender didn't request a specific expire_timeout.
-- 5s/8s/forever mirrors the convention most status-bar toast stacks use.
local URGENCY = {
	[0] = { color = 0xff6c7086, timeout = 5000 },
	[1] = { color = 0xff89b4fa, timeout = 8000 },
	[2] = { color = 0xfff38ba8, timeout = nil },
}

local liveIds = {} -- set of notification ids currently rendered as a card
local windowCreated = false

-- Deliberately slower + a bouncy spring (real overshoot, not just a
-- smooth ease) instead of a quick plain fade - stress-tests the damage/
-- positioning machinery harder, since content spends longer near (and
-- past, on the overshoot) its final bounds instead of snapping through
-- it in a couple of frames. This is how the reflow-ghosting bug (see
-- TASKS.md task 2) actually got caught - keep it this obvious for now.
hl.curve("hyprlui_notification_bounce", { type = "spring", stiffness = 120, dampening = 8, mass = 1 })
local CARD_ANIM_IN = { speed = 6, spring = "hyprlui_notification_bounce", style = "slide right" }
local CARD_ANIM_OUT = { speed = 6, spring = "hyprlui_notification_bounce", style = "slide right" }
local CARD_ANIM_LAYOUT = { speed = 8, spring = "hyprlui_notification_bounce" }

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
			anchor = "top-right",
			x = 16,
			y = 16,
			hl.plugin.hyprlui.Column({ id = "root", gap = 8 }),
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

	local ok, err = pcall(function()
		hl.plugin.hyprlui.add_widget(
			WINDOW_NAME,
			"root",
			hl.plugin.hyprlui.Stack({
				id = "card_" .. id,
				w = CARD_WIDTH,
				h = 64,
				animationIn = CARD_ANIM_IN,
				animationOut = CARD_ANIM_OUT,
				-- Lets THIS card slide smoothly into a new slot when a
				-- sibling above it is dismissed, instead of snapping -
				-- animationOut (above, on the card actually being
				-- removed) stopping its own layout space immediately is
				-- what makes that reflow start at the same time as the
				-- dismissed card's own fade-out, not after it.
				animationLayout = CARD_ANIM_LAYOUT,
				hl.plugin.hyprlui.Box({
					id = "bg_" .. id,
					fill = true,
					color = 0xff1e1e2e,
					rounding = 8,
					borderColor = n.color,
					borderWidth = 2,
					onClick = function()
						M.dismiss(id)
					end,
				}),
				hl.plugin.hyprlui.Text({
					id = "summary_" .. id,
					x = 12,
					y = 8,
					text = title,
					maxW = CARD_WIDTH - 24,
					size = 13,
					color = 0xffcdd6f4,
				}),
				hl.plugin.hyprlui.Text({
					id = "body_" .. id,
					x = 12,
					y = 30,
					text = n.body,
					maxW = CARD_WIDTH - 24,
					size = 12,
					color = 0xffa6adc8,
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
	local style = URGENCY[urgency] or URGENCY[1]

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

-- Call once after require()'ing this module, same convention
-- demos/which-key.lua's own M.setup() uses - re-call on every config
-- reload (the whole script re-runs then anyway, so this reconnects
-- fresh each time rather than needing any reconnect-on-drop logic of
-- its own).
function M.setup()
	liveIds = {}
	windowCreated = false
	hl.plugin.hyprlui.open_socket(SOCKET_PATH, function(sock)
		if not sock then
			warn(
				"hyprlui notification-manager",
				"could not connect to " .. SOCKET_PATH .. " - is notification-daemon.service running?"
			)
			return
		end
		startReadLoop(sock)
	end)
end

return M
