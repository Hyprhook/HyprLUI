-- Verbose logging - makes plugin notifications/console output easier to
-- follow while testing against a nested/dev Hyprland instance.
hl.config({
	debug = {
		disable_logs = false,
		enable_stdout_logs = true,
		colored_stdout_logs = true,
	},
})

-- Top-level exec_cmd calls fire before the IPC socket exists, so autostart
-- (and anything using hyprctl, like plugin loading) has to wait for the
-- "hyprland.start" event - same pattern as Hyprland's own example config.
-- `hyprctl plugin load` needs an absolute path; this file is a throwaway
-- dev config for this repo checkout, so hardcoding it here is fine.
hl.on("hyprland.start", function()
	hl.exec_cmd("hyprctl plugin load /home/moritzgleissner/dev/HyprLUI/HyprLUI.so")
	hl.exec_cmd("kitty")
end)

-- ALT + T: open a terminal
hl.bind("ALT + T", hl.dsp.exec_cmd("kitty"), { description = "Open Terminal" })
-- ALT + Q: close the focused window
hl.bind("ALT + Q", hl.dsp.window.close(), { description = "Close window" })

-- Plugin registers its Lua functions under lowercase "hyprlui" (see
-- HyprlandAPI::addLuaFunction(handle, "hyprlui", ...) in src/main.cpp).
-- This only means anything on a config *reload* after the plugin has
-- already loaded - on first boot it runs before the hyprland.start hook
-- above has had a chance to load it.
if hl.plugin.hyprlui ~= nil then
	hl.notification.create({ text = "'hyprlui' is loaded.", timeout = 3000 })
else
	hl.notification.create({ text = "'hyprlui' is NOT loaded.", timeout = 3000 })
end

----------------------------
---- HYPRLUI PLUGIN TEST ----
----------------------------
-- Exercises hl.plugin.hyprlui.* directly: a Stack root with an absolutely
-- positioned Box behind a Column of Text (so the panel genuinely sits
-- behind the labels - unlike the nesting-only illustration in
-- LuaBridge.hpp's doc comment), plus binds to mutate/toggle/tear it down.
-- Every call is wrapped in pcall since these are real C++ luaL_error
-- sites (e.g. window{} errors if the name's already in use) - see
-- src/ui/LuaBridge.cpp.

local HYPRLUI_WINDOW = "hyprlui_test"
local hyprluiClicks = 0
local hyprluiVisible = true

local function hyprluiWarn(label, err)
	hl.notification.create({ text = label .. " failed: " .. tostring(err), timeout = 3000 })
end

-- ALT + SHIFT + H: create the hyprlui_test window (Stack + Box + Column of Text)
hl.bind("ALT + SHIFT + H", function()
	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_WINDOW,
			x = 200,
			y = 200,
			hl.plugin.hyprlui.Stack({
				id = "root",
				hl.plugin.hyprlui.Box({ id = "bg", x = 0, y = 0, w = 320, h = 100, color = 0xcc111111, rounding = 8 }),
				hl.plugin.hyprlui.Column({
					id = "text",
					x = 16,
					y = 16,
					gap = 6,
					hl.plugin.hyprlui.Text({ id = "title", text = "HyprLUI test window", size = 18 }),
					hl.plugin.hyprlui.Text({
						id = "counter",
						text = "clicks: 0",
						size = 14,
						color = { r = 0.8, g = 0.8, b = 0.8, a = 1.0 },
					}),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window", err)
	end
end, { description = "HyprLUI: create test window" })

-- ALT + SHIFT + J: increment the counter and set_text() it onto the "counter" widget
hl.bind("ALT + SHIFT + J", function()
	hyprluiClicks = hyprluiClicks + 1
	local ok, err = pcall(hl.plugin.hyprlui.set_text, HYPRLUI_WINDOW, "counter", "clicks: " .. hyprluiClicks)
	if not ok then
		hyprluiWarn("hyprlui.set_text", err)
	end
end, { description = "HyprLUI: bump the test window's counter text" })

-- ALT + SHIFT + K: remove_widget() the "counter" text, leaving title + background
hl.bind("ALT + SHIFT + K", function()
	local ok, err = pcall(hl.plugin.hyprlui.remove_widget, HYPRLUI_WINDOW, "counter")
	if not ok then
		hyprluiWarn("hyprlui.remove_widget", err)
	end
end, { description = "HyprLUI: remove the counter text widget" })

-- ALT + SHIFT + V: toggle the whole test window's visibility via set_canvas_visible()
hl.bind("ALT + SHIFT + V", function()
	hyprluiVisible = not hyprluiVisible
	local ok, err = pcall(hl.plugin.hyprlui.set_canvas_visible, HYPRLUI_WINDOW, hyprluiVisible)
	if not ok then
		hyprluiWarn("hyprlui.set_canvas_visible", err)
	end
end, { description = "HyprLUI: toggle test window visibility" })

-- ALT + SHIFT + R: remove_canvas() - tear down the whole test window
hl.bind("ALT + SHIFT + R", function()
	local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_WINDOW)
	if not ok then
		hyprluiWarn("hyprlui.remove_canvas", err)
	end
end, { description = "HyprLUI: remove the test window" })

-- ALT + SHIFT + A: create/toggle an anchored window (Phase 2) - top-right
-- corner of the focused monitor's usable box (excludes existing bars/
-- panels for free, via logicalBoxMinusReserved()), 10px in from both
-- edges. `monitor` is omitted on purpose - that's what means "the focused
-- monitor" now (there's no "current" keyword, see LuaBridge.hpp). Toggles:
-- removes it if it already exists.
local HYPRLUI_ANCHOR_WINDOW = "hyprlui_anchor_test"
local hyprluiAnchorWindowOpen = false
hl.bind("ALT + SHIFT + A", function()
	if hyprluiAnchorWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_ANCHOR_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiAnchorWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_ANCHOR_WINDOW,
			anchor = "top-right",
			monitor = "test",
			x = 10,
			y = 10,
			hl.plugin.hyprlui.Box({
				id = "root",
				w = 220,
				h = 60,
				color = 0xcc224488,
				rounding = 8,
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (anchored)", err)
	else
		hyprluiAnchorWindowOpen = true
	end
end, { description = "HyprLUI: toggle an anchored top-right test window" })

-- ALT + SHIFT + C: deliberately malformed call, NOT wrapped in pcall - this
-- is the actual crash test. Box{ id = "bad_box" } is missing its required
-- w/h fields, so buildWidget() hits requireFieldNumber() -> luaL_error()
-- three C++ stack frames deep (luaWindow -> buildWidget(root) ->
-- buildWidget(bad_box)), with the outer frames' locals (std::string id/x/y,
-- the already-constructed CStackWidget shared_ptr, etc.) still alive on the
-- stack when it fires. luaL_error() -> lua_error() longjmps past all of
-- them - their destructors never run (UB for non-trivial dtors, but no
-- C++ exception machinery is involved, so it doesn't corrupt state; see
-- DESIGN.md / the luaL_error discussion for why). Expected: Hyprland's own
-- protected call around bind dispatch catches this and logs an error -
-- the compositor and the plugin should both keep running. If this crashes
-- Hyprland instead, that's a real bug in our luaL_error usage or in how
-- addLuaFunction callbacks get invoked - not something to silently work
-- around with a pcall here.
hl.bind("ALT + SHIFT + C", function()
	hl.plugin.hyprlui.window({
		name = "hyprlui_crash_test",
		hl.plugin.hyprlui.Stack({
			id = "root",
			hl.plugin.hyprlui.Box({ id = "bad_box" }),
		}),
	})
end, { description = "HyprLUI: malformed call, unprotected (crash/error-handling test)" })
