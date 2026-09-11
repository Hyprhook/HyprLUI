local M = {}
-- which-key.lua
--
-- A real-world HyprLUI demo: a which-key-style popup for Hyprland,
-- replacing an eww-based implementation the user already had working
-- (see ~/.setup/modules/services/eww/ + ~/.setup/scripts/bin/which-key
-- for the reference this was built against). Same core mechanism, same
-- Catppuccin palette, rendered by HyprLUI instead of eww.
--
-- MECHANISM (confirmed from Hyprland 0.56.0 source, not guessed):
--   - Hyprland's own `hl.on("keybinds.submap", function(submap) ... end)`
--     fires on every active-submap change. `submap == ""` means "back to
--     global" - there is no separate "closed" event, this IS it.
--   - Hyprland's Lua API has no bind-enumeration or JSON support at all
--     (confirmed by grepping src/config/lua/ - zero matches for
--     json/cjson, zero matches for a get_binds-style function). So the
--     only way to learn what's bound in the active submap is to shell
--     out to `hyprctl binds -j` (via hyprlui.run_cmd(), Phase 12) and
--     decode the JSON ourselves - see jsonDecode() below.
--   - Submaps/binds are defined via `hl.bind(key, dispatcher, opts)` +
--     `hl.define_submap(name, resetKey, fn)` (any hl.bind() called
--     inside `fn` automatically inherits that submap - confirmed in
--     LuaBindingsToplevel.cpp). `hl.dsp.submap("")` is the escape-to-
--     global dispatcher, matching classic `submap = reset` syntax.
--
-- DELIBERATE DEVIATIONS from the eww reference (flagged during planning,
-- not accidental):
--   - The escape/"back" bind is SHOWN as a normal entry here, not hidden
--     - the reference script hides it by checking `.arg contains
--     "reset"`, but the Lua-idiomatic escape is `hl.dsp.submap("")`
--     (empty arg, not the string "reset"), so that filter wouldn't even
--     match here. Showing "esc - back" seems like better UX anyway
--     (real which-key.nvim shows <esc> too).
--   - Flat rounded background, no border - CRectNode/Box has no stroke
--     concept yet (see DESIGN.md Phase 19, tracked as a follow-up, not
--     blocking this demo).
--   - No show-delay timer (which-key.nvim has one) - doesn't apply here,
--     we're purely reacting to an already-instant Hyprland-native event,
--     not debouncing our own keystrokes.

local WHICH_KEY_WINDOW = "hyprlui_which_key"

-- Tracks the MOST RECENTLY requested submap so the async run_cmd()
-- callback below can tell whether it's still relevant by the time it
-- actually fires - without this, rapidly tapping through several
-- submaps could show stale content if an older callback resolves after
-- a newer submap change already superseded it.
local activeSubmap = nil

local function hyprluiWarn(label, err)
	hl.notification.create({ text = label .. " failed: " .. tostring(err), timeout = 3000 })
end

--------------------------------------------------
---- minimal JSON decoder ----
--------------------------------------------------
-- Hyprland's Lua sandbox has no json.decode (confirmed) - this is a
-- plain recursive-descent parser, just enough for hyprctl's own output
-- (objects/arrays/strings/numbers/booleans/null). Not a shared module -
-- no second demo needs it yet; factor out if one does.
local function jsonDecode(str)
	local pos = 1
	local parseValue

	local function skipWhitespace()
		while pos <= #str do
			local c = str:sub(pos, pos)
			if c == " " or c == "\t" or c == "\n" or c == "\r" then
				pos = pos + 1
			else
				break
			end
		end
	end

	local function parseString()
		pos = pos + 1 -- opening quote
		local start = pos
		local buf = {}
		while pos <= #str do
			local c = str:sub(pos, pos)
			if c == '"' then
				table.insert(buf, str:sub(start, pos - 1))
				pos = pos + 1
				return table.concat(buf)
			elseif c == "\\" then
				table.insert(buf, str:sub(start, pos - 1))
				local nextC = str:sub(pos + 1, pos + 1)
				local escapes =
					{ ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
				if escapes[nextC] then
					table.insert(buf, escapes[nextC])
					pos = pos + 2
				elseif nextC == "u" then
					-- Minimal \uXXXX handling - only the plain-ASCII range
					-- decodes to a real character, anything above falls
					-- back to "?" rather than implementing UTF-8 encoding
					-- for a field (keybind descriptions) that's ASCII in
					-- practice.
					local hex = str:sub(pos + 2, pos + 5)
					local codepoint = tonumber(hex, 16) or 63
					table.insert(buf, codepoint < 128 and string.char(codepoint) or "?")
					pos = pos + 6
				else
					table.insert(buf, nextC)
					pos = pos + 2
				end
				start = pos
			else
				pos = pos + 1
			end
		end
		error("jsonDecode: unterminated string")
	end

	local function parseNumber()
		local start = pos
		while pos <= #str and str:sub(pos, pos):match("[%d%.%-%+eE]") do
			pos = pos + 1
		end
		return tonumber(str:sub(start, pos - 1))
	end

	local function parseArray()
		pos = pos + 1
		skipWhitespace()
		local arr = {}
		if str:sub(pos, pos) == "]" then
			pos = pos + 1
			return arr
		end
		while true do
			skipWhitespace()
			table.insert(arr, parseValue())
			skipWhitespace()
			local c = str:sub(pos, pos)
			if c == "," then
				pos = pos + 1
			elseif c == "]" then
				pos = pos + 1
				break
			else
				error("jsonDecode: expected , or ] in array")
			end
		end
		return arr
	end

	local function parseObject()
		pos = pos + 1
		skipWhitespace()
		local obj = {}
		if str:sub(pos, pos) == "}" then
			pos = pos + 1
			return obj
		end
		while true do
			skipWhitespace()
			if str:sub(pos, pos) ~= '"' then
				error("jsonDecode: expected string key in object")
			end
			local key = parseString()
			skipWhitespace()
			if str:sub(pos, pos) ~= ":" then
				error("jsonDecode: expected ':' in object")
			end
			pos = pos + 1
			skipWhitespace()
			obj[key] = parseValue()
			skipWhitespace()
			local c = str:sub(pos, pos)
			if c == "," then
				pos = pos + 1
			elseif c == "}" then
				pos = pos + 1
				break
			else
				error("jsonDecode: expected , or } in object")
			end
		end
		return obj
	end

	parseValue = function()
		skipWhitespace()
		local c = str:sub(pos, pos)
		if c == '"' then
			return parseString()
		elseif c == "{" then
			return parseObject()
		elseif c == "[" then
			return parseArray()
		elseif c == "t" and str:sub(pos, pos + 3) == "true" then
			pos = pos + 4
			return true
		elseif c == "f" and str:sub(pos, pos + 4) == "false" then
			pos = pos + 5
			return false
		elseif c == "n" and str:sub(pos, pos + 3) == "null" then
			pos = pos + 4
			return nil
		else
			return parseNumber()
		end
	end

	skipWhitespace()
	return parseValue()
end

--------------------------------------------------
---- bind grouping (mirrors the eww reference's jq pipeline) ----
--------------------------------------------------

-- Filters to the active submap's own binds, splits into "leads to
-- another submap" vs "plain command" (sorted by key within each group,
-- commands first - same order the reference script produced), then
-- merges them into one list. Nothing is excluded here - see this file's
-- own header comment on why the escape/"back" bind is shown, not hidden.
local function prepareBinds(allBinds, submap)
	local filtered = {}
	for _, b in ipairs(allBinds) do
		if b.submap == submap then
			table.insert(filtered, b)
		end
	end

	local submapGroup, commandGroup = {}, {}
	for _, b in ipairs(filtered) do
		if b.dispatcher and b.dispatcher:find("submap") then
			table.insert(submapGroup, b)
		else
			table.insert(commandGroup, b)
		end
	end

	local function byKey(a, c)
		return (a.key or "") < (c.key or "")
	end
	table.sort(submapGroup, byKey)
	table.sort(commandGroup, byKey)

	local merged = {}
	for _, b in ipairs(commandGroup) do
		table.insert(merged, b)
	end
	for _, b in ipairs(submapGroup) do
		table.insert(merged, b)
	end
	return merged
end

-- Same column math as the reference script: chunk size (NOT column
-- count - the variable name in that script was misleading) is
-- max(4, ceil(total/3)); the resulting number of chunks is however many
-- that produces. Each chunk becomes one visual column.
local function buildColumns(merged)
	local total = #merged
	if total == 0 then
		return {}
	end
	local chunkSize = math.max(4, math.ceil(total / 3))
	local columns = {}
	local i = 1
	while i <= total do
		local chunk = {}
		for j = i, math.min(i + chunkSize - 1, total) do
			table.insert(chunk, merged[j])
		end
		table.insert(columns, chunk)
		i = i + chunkSize
	end
	return columns
end

-- Hyprland's own modifier bitmask convention (standard XKB modifier
-- bits, confirmed against Hyprland's usage elsewhere - NOT the eww
-- reference script's own modmask handling, which only special-cased
-- modmask==1/64/65 and got the bit values wrong for anything else, e.g.
-- it treated 64 as ctrl when 64 is actually super). Real submaps (see
-- this file's own test submaps below) commonly bind "SHIFT + H"-style
-- combos, not just bare keys - without this, the popup would show "H"
-- with no indication it needs a held modifier at all.
local MOD_BITS = { { 1, "shift" }, { 4, "ctrl" }, { 8, "alt" }, { 64, "super" } }

local function modLabel(modmask)
	if not modmask or modmask == 0 then
		return ""
	end
	local parts = {}
	for _, pair in ipairs(MOD_BITS) do
		if modmask & pair[1] ~= 0 then
			table.insert(parts, pair[2])
		end
	end
	if #parts == 0 then
		return ""
	end
	return table.concat(parts, "+") .. "+"
end

-- The focused monitor's pixel width, for a full-width bar matching the
-- eww reference's `:width "100%"` - confirmed hl.get_monitors() exists
-- and returns rich Monitor objects (.width/.focused/etc, see
-- LuaMonitor.cpp) natively, no HyprLUI-side feature needed for this.
local function focusedMonitorWidth()
	local ok, monitors = pcall(hl.get_monitors)
	if not ok or not monitors then
		return 1920
	end
	for _, m in ipairs(monitors) do
		if m.focused then
			return m.width
		end
	end
	return monitors[1] and monitors[1].width or 1920
end

--------------------------------------------------
---- rendering ----
--------------------------------------------------

-- Single customization block for this demo. `yOffset` is a window-level
-- setting (the popup's distance from the anchored screen edge), not a
-- widget prop, so it can't live inside the component itself - it's kept
-- here anyway so every user-facing knob this demo exposes (per the
-- request this was built against: y anchor offset, theme colors, font +
-- size, in/out animation) has exactly one place to edit. Everything else
-- here is passed straight through to the WhichKeyPopup component below
-- (defaults: Catppuccin Mocha, same palette as the eww reference's
-- eww.scss - base #1e1e2e, mauve #cba6f7, blue #89b4fa, peach #fac6a7).
local CONFIG = {
	yOffset = 20,
	bgColor = 0xff1e1e2e,
	keyColor = 0xff89b4fa,
	descColor = 0xfffac6a7,
	descSubmapColor = 0xffcba6f7,
	font = "sans",
	size = 13,
	padding = 10,
	animationIn = { speed = 3, bezier = "default", style = "slide bottom" },
	animationOut = { speed = 3, bezier = "default", style = "slide bottom" },
}

-- Registered via hyprlui.defineComponent() (Phase 9) instead of building
-- the Stack/Box/Row tree inline in buildAndShow() - this is the demo's
-- only real widget-tree shape, so turning it into a named, reusable
-- component is mostly a test of the mechanism itself (see the task this
-- was built for), but it also means the popup's entire *appearance* is
-- now just a props table, decoupled from the submap/JSON-grouping logic
-- that produces `columns`. Schema defaults reuse CONFIG's own values -
-- still only one literal source for each, even though buildAndShow()
-- below passes CONFIG's fields through explicitly rather than relying on
-- them (making every user-facing knob visible at the call site).
--
-- Guarded the same way the Phase 9 example in this repo's own
-- hyprlandd.lua was: defineComponent() runs at top-level module-load
-- time, which happens BEFORE the plugin has finished loading on first
-- boot (hl.plugin.hyprlui is still nil then) - main.cpp's PLUGIN_INIT
-- reloads the config right after registering the plugin's functions, so
-- this re-runs a second time with hl.plugin.hyprlui populated. Unguarded
-- would self-correct too, just with a spurious error logged in between.
if hl.plugin.hyprlui ~= nil then
	hl.plugin.hyprlui.defineComponent("WhichKeyPopup", {
		props = {
			columns = { required = true }, -- this submap's chunked bind entries (buildColumns()'s output) - no sensible default, always supplied fresh per rebuild
			bgColor = { default = CONFIG.bgColor },
			keyColor = { default = CONFIG.keyColor },
			descColor = { default = CONFIG.descColor },
			descSubmapColor = { default = CONFIG.descSubmapColor },
			font = { default = CONFIG.font },
			size = { default = CONFIG.size },
			padding = { default = CONFIG.padding },
			animationIn = { default = CONFIG.animationIn },
			animationOut = { default = CONFIG.animationOut },
		},
		render = function(props)
			-- Right-aligned keys via a separate keys-Column (align="end")
			-- beside a descriptions-Column (align="start") per
			-- column-group - the structure this demo ORIGINALLY used. A
			-- prior revision of this file worked around a row-drift
			-- symptom here by switching to monospace + manually
			-- left-padded key strings in a single shared Row per entry
			-- instead - that was a demo-level band-aid for what turned
			-- out to be an engine-level bug (CTextNode's measured height
			-- varying per STRING CONTENT, not just per font/size - see
			-- gfx::naturalLineHeight()'s own doc comment, gfx.hpp).  Now
			-- that CTextNode's height is standardized there, this
			-- simpler two-column structure works correctly again -
			-- reverted to it deliberately, both because it's simpler (no
			-- manual padding math, no forced monospace font) and because
			-- it doubles as a live check that the actual engine fix
			-- holds.
			local columnsRow = hl.plugin.hyprlui.Row({ id = "columns", gap = 50, padding = props.padding })
			for i, col in ipairs(props.columns) do
				local keysColumn = hl.plugin.hyprlui.Column({ id = "keys_" .. i, align = "end", gap = 6 })
				local descColumn = hl.plugin.hyprlui.Column({ id = "descs_" .. i, align = "start", gap = 6 })
				for _, entry in ipairs(col) do
					local isSubmapEntry = entry.dispatcher and entry.dispatcher:find("submap")
					local keyLabel = modLabel(entry.modmask) .. (entry.key or "?")
					table.insert(
						keysColumn,
						hl.plugin.hyprlui.Text({
							text = keyLabel,
							size = props.size,
							font = props.font,
							color = props.keyColor,
						})
					)
					table.insert(
						descColumn,
						hl.plugin.hyprlui.Text({
							text = entry.description or "",
							size = props.size,
							font = props.font,
							color = isSubmapEntry and props.descSubmapColor or props.descColor,
						})
					)
				end
				table.insert(columnsRow, hl.plugin.hyprlui.Row({ id = "col_" .. i, gap = 20, keysColumn, descColumn }))
			end

			return hl.plugin.hyprlui.Stack({
				-- debug = true,
				id = "root",
				fill = true, -- stretches to match the canvas's forced full-monitor width (Phase 15's root-fills-canvas)
				animationIn = props.animationIn,
				animationOut = props.animationOut,
				hl.plugin.hyprlui.Box({ id = "bg", w = 1, h = 1, fill = true, color = props.bgColor, rounding = 8 }),
				columnsRow,
			})
		end,
	})
end

local function closePopup()
	pcall(hl.plugin.hyprlui.remove_canvas, WHICH_KEY_WINDOW)
end

-- Builds the popup's root widget from this submap's chunked bind entries
-- (buildAndShow()'s `columns`) - the ONLY thing buildAndShow() itself
-- needs back, so a caller wanting to render this completely differently
-- (a different registered component, a different Component() key/opts, a
-- hand-built widget tree with no Component() at all) only has to replace
-- this one function, not fork buildAndShow(). Overridable via M.setup()
-- below; defaults to this demo's own WhichKeyPopup component with
-- CONFIG's values.
local buildPopup = function(columns)
	return hl.plugin.hyprlui.Component("WhichKeyPopup", {
		columns = columns,
		bgColor = CONFIG.bgColor,
		keyColor = CONFIG.keyColor,
		descColor = CONFIG.descColor,
		descSubmapColor = CONFIG.descSubmapColor,
		font = CONFIG.font,
		size = CONFIG.size,
		padding = CONFIG.padding,
		animationIn = CONFIG.animationIn,
		animationOut = CONFIG.animationOut,
	}, { key = "popup" })
end

-- Call any time after require()'ing this module - order against
-- M.test_binds()/the module's own always-active hl.on("keybinds.submap",
-- ...) hook doesn't matter, since buildPopup/CONFIG.yOffset are only ever
-- READ once a submap change actually fires a rebuild, never at require()
-- time. Re-call this on every config reload alongside the require() that
-- brings the module back in, same as M.test_binds() - the whole module
-- (including CONFIG/buildPopup's defaults) re-evaluates from scratch
-- every reload, so an override made before the last reload is gone.
function M.setup(opts)
	opts = opts or {}
	if opts.buildPopup then
		buildPopup = opts.buildPopup
	end
	if opts.yOffset then
		CONFIG.yOffset = opts.yOffset
	end
end

local function buildAndShow(submap, allBinds)
	-- Stale async response from a submap we've since left - drop it (see
	-- `activeSubmap`'s own doc comment above).
	if submap ~= activeSubmap then
		return
	end

	local merged = prepareBinds(allBinds, submap)
	if #merged == 0 then
		return
	end
	local columns = buildColumns(merged)

	closePopup() -- destroy any still-open popup from the previous submap before rebuilding - content shape differs per submap, so mutating in place isn't practical
	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = WHICH_KEY_WINDOW,
			anchor = "bottom",
			w = focusedMonitorWidth(),
			x = 0,
			y = CONFIG.yOffset,
			buildPopup(columns),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (which-key)", err)
	end
end

hl.on("keybinds.submap", function(submap)
	activeSubmap = submap

	if submap == "" then
		closePopup()
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.run_cmd("hyprctl binds -j", function(output)
			local decodeOk, decoded = pcall(jsonDecode, output)
			if not decodeOk then
				hyprluiWarn("which-key json decode", decoded)
				return
			end
			buildAndShow(submap, decoded)
		end)
	end)
	if not ok then
		hyprluiWarn("hyprlui.run_cmd", err)
	end
end)

--------------------------------------------------
---- self-contained test submaps ----
--------------------------------------------------
-- hyprlandd.lua is this repo's own throwaway dev config, not the user's
-- real dotfiles - these exist purely so the demo is testable standalone.
-- Shape is loosely modeled on a real which-key-style hyprland.lua setup
-- (a wide top-level hub of "+group" entries, several of which cross-link
-- directly into EACH OTHER rather than only back up to the hub) instead
-- of just one flat toy menu, specifically to exercise more of the
-- popup's own rendering edge cases:
--   - a submap where EVERY entry leads to another submap (all-mauve
--     descriptions, "demo" itself)
--   - two submaps that link directly to each other (demo_focus <->
--     demo_move), not just back to a shared parent - tests jumping
--     between SIBLING submaps without passing through global
--   - a large flat (for-loop generated) list of plain entries
--     (demo_workspaces, 10 of them) - tests column-chunking with more
--     entries than fit in one column
--   - modifier-combo binds ("SHIFT + H", not just a bare key) - tests
--     modLabel() actually showing the held modifier, not just the key
--   - a real notification for every leaf action instead of exec_cmd'ing
--     a script that doesn't exist in this repo (fine per the user - "not
--     always have the right execution", the point is exercising the
--     popup, not actually rebinding volume/focus for real)
local function notify(text)
	return function()
		hl.notification.create({ text = text, timeout = 2000 })
	end
end

function M.test_binds()
	hl.bind("ALT + SHIFT + Space", hl.dsp.submap("demo"), { description = "open which-key demo menu" })

	hl.define_submap("demo", "escape", function()
		hl.bind("T", hl.dsp.exec_cmd("kitty"), { description = "open terminal" })
		hl.bind("Q", hl.dsp.window.close(), { description = "close window" })
		hl.bind("escape", hl.dsp.submap(""), { description = "back" })

		hl.bind("N", hl.dsp.submap("demo_nested"), { description = "+nested" })
		hl.bind("F", hl.dsp.submap("demo_focus"), { description = "+focus" })
		hl.bind("M", hl.dsp.submap("demo_move"), { description = "+move" })
		hl.bind("W", hl.dsp.submap("demo_workspaces"), { description = "+workspaces" })
		hl.bind("V", hl.dsp.submap("demo_volume"), { description = "+volume" })
	end)

	hl.define_submap("demo_nested", "escape", function()
		hl.bind("A", notify("nested action a"), { description = "nested action a" })
		hl.bind("B", notify("nested action b"), { description = "nested action b" })
		hl.bind("escape", hl.dsp.submap("demo"), { description = "back" })
	end)

	-- Reciprocal submaps - each links directly to the other (F/M), not just
	-- back to "demo" - see this section's own header comment for why.
	hl.define_submap("demo_focus", "escape", function()
		hl.bind("escape", hl.dsp.submap("demo"), { description = "back" })
		hl.bind("M", hl.dsp.submap("demo_move"), { description = "+move" })
		hl.bind("H", notify("focus left"), { description = "focus left" })
		hl.bind("L", notify("focus right"), { description = "focus right" })
		hl.bind("K", notify("focus up"), { description = "focus up" })
		hl.bind("J", notify("focus down"), { description = "focus down" })
		hl.bind("SHIFT + H", notify("focus workspace left"), { description = "focus workspace left" })
		hl.bind("SHIFT + L", notify("focus workspace right"), { description = "focus workspace right" })
	end)

	hl.define_submap("demo_move", "escape", function()
		hl.bind("escape", hl.dsp.submap("demo"), { description = "back" })
		hl.bind("F", hl.dsp.submap("demo_focus"), { description = "+focus" })
		hl.bind("H", notify("move left"), { description = "move left" })
		hl.bind("L", notify("move right"), { description = "move right" })
		hl.bind("SHIFT + H", notify("move to workspace left"), { description = "move to workspace left" })
		hl.bind("SHIFT + L", notify("move to workspace right"), { description = "move to workspace right" })
	end)

	-- 10 for-loop-generated plain entries - tests column-chunking with more
	-- rows than fit in one column (chunkSize = max(4, ceil(10/3)) = 4, so
	-- this splits into 3 columns of 4/4/2).
	hl.define_submap("demo_workspaces", "escape", function()
		hl.bind("escape", hl.dsp.submap("demo"), { description = "back" })
		for i = 1, 10 do
			local key = i % 10
			hl.bind("" .. key, notify("focus workspace " .. i), { description = "focus workspace " .. i })
		end
	end)

	hl.define_submap("demo_volume", "escape", function()
		hl.bind("escape", hl.dsp.submap("demo"), { description = "back" })
		hl.bind("K", notify("volume up"), { description = "increase volume" })
		hl.bind("J", notify("volume down"), { description = "decrease volume" })
		hl.bind("M", notify("mic toggled"), { description = "toggle mic" })
	end)
end

return M
