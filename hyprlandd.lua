-- NOTE on reload: every config reload (including one triggered just by
-- fixing an unrelated Lua error) now closes every open HyprLUI window/
-- watcher/exclusive zone outright - the plugin clears its own state on
-- config.preReload, since a reload re-runs this whole script (resetting
-- the *ToggleWindowOpen-style locals below to false) without unloading
-- the plugin itself. So after a reload, the toggle binds below correctly
-- start from "closed" again and need pressing again to reopen - see
-- DESIGN.md's "General plugin-lifecycle bug" note for why this replaced
-- the old (buggy) behavior of windows silently surviving a reload as
-- unreachable orphans.

-- Verbose logging - makes plugin notifications/console output easier to
-- follow while testing against a nested/dev Hyprland instance.
hl.config({
	debug = {
		disable_logs = false,
		enable_stdout_logs = true,
		colored_stdout_logs = true,
	},
})

hl.monitor({
	output = "",
	mode = "preferred",
	position = "auto",
	scale = "1",
	mirror = "HDMI-A-1",
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

----------------------------------------------
---- HYPRLUI REACTIVITY TEST (Phase 3) ----
----------------------------------------------
-- "poll" ticks on its own every second once created, with no keybind
-- needed - the whole point of using a real event-loop timer instead of
-- piggybacking on render activity (see src/reactive/Watcher.hpp) is that
-- this keeps updating even on an otherwise-idle desktop. "manual" only
-- updates when explicitly notify()'d (ALT + SHIFT + M below) - watch()
-- alone doesn't imply polling.

local hyprluiPollCount = 0
local hyprluiManualCount = 0

-- ALT + SHIFT + B: register both watchers + create the Bind()-driven
-- window. Split from the HYPRLUI_WINDOW bind above since watch() has to
-- run before the window{} that Bind()s to it (see LuaBridge.hpp).
hl.bind("ALT + SHIFT + B", function()
	local ok, err = pcall(function()
		hl.plugin.hyprlui.watch("poll", function()
			hyprluiPollCount = hyprluiPollCount + 1
			return "poll: " .. hyprluiPollCount
		end, { interval = 1000 })

		hl.plugin.hyprlui.watch("manual", function()
			return "manual: " .. hyprluiManualCount
		end)

		-- A real-world use case rather than a synthetic counter: os is in
		-- Hyprland's Lua stdlib allowlist (ConfigManager.cpp), so this
		-- works out of the box. Also a good live check that the
		-- size-sync fix holds up on content that keeps changing width
		-- (single- vs double-digit hour/minute/second) every tick.
		hl.plugin.hyprlui.watch("clock", function()
			return os.date("%H:%M:%S")
		end, { interval = 1000 })

		hl.plugin.hyprlui.window({
			name = "hyprlui_reactive_test",
			anchor = "bottom-right",
			x = 10,
			y = 10,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 4,
				hl.plugin.hyprlui.Text({ id = "clock_label", text = hl.plugin.hyprlui.Bind("clock"), size = 14 }),
				hl.plugin.hyprlui.Text({ id = "poll_label", text = hl.plugin.hyprlui.Bind("poll"), size = 14 }),
				hl.plugin.hyprlui.Text({ id = "manual_label", text = hl.plugin.hyprlui.Bind("manual"), size = 14 }),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui reactive test setup", err)
	end
end, { description = "HyprLUI: create the reactive (watch/notify/Bind) test window" })

-- ALT + SHIFT + M: bump the "manual" watcher's counter and notify() it -
-- the bound text should update immediately, without waiting on any poll.
hl.bind("ALT + SHIFT + M", function()
	hyprluiManualCount = hyprluiManualCount + 1
	local ok, err = pcall(hl.plugin.hyprlui.notify, "manual")
	if not ok then
		hyprluiWarn("hyprlui.notify", err)
	end
end, { description = "HyprLUI: bump + notify() the manual reactive counter" })

----------------------------------------
---- HYPRLUI BUTTON TEST (Phase 4) ----
----------------------------------------
-- Left-click only, press+release must land on the same button (drag off
-- to cancel - see src/input/InputHook.cpp). Clicking anywhere else in
-- this window (its background Box, the counter label) does nothing and
-- passes through untouched to whatever's beneath - only the Button's own
-- bounds are clickable, hit-testing doesn't swallow the whole window.

local HYPRLUI_BUTTON_WINDOW = "hyprlui_button_test"
local hyprluiButtonWindowOpen = false
local hyprluiButtonClicks = 0

-- ALT + SHIFT + N: toggle a centered window with a real clickable button -
-- onClick bumps a counter and set_text()s it onto a sibling Text label,
-- exercising the input hook end to end (hit-test -> press -> release ->
-- click() -> Lua callback -> mutate -> set_text -> re-damage).
hl.bind("ALT + SHIFT + N", function()
	if hyprluiButtonWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_BUTTON_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiButtonWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_BUTTON_WINDOW,
			anchor = "center",
			hl.plugin.hyprlui.Stack({
				id = "root",
				hl.plugin.hyprlui.Box({ id = "bg", x = 0, y = 0, w = 220, h = 100, color = 0xcc111111, rounding = 8 }),
				hl.plugin.hyprlui.Text({ id = "clicks_label", x = 16, y = 12, text = "clicks: 0", size = 14 }),
				hl.plugin.hyprlui.Button({
					id = "btn",
					x = 16,
					y = 44,
					w = 188,
					h = 32,
					color = 0x333366,
					rounding = 6,
					onClick = function()
						hyprluiButtonClicks = hyprluiButtonClicks + 1
						local setOk, setErr = pcall(
							hl.plugin.hyprlui.set_text,
							HYPRLUI_BUTTON_WINDOW,
							"clicks_label",
							"clicks: " .. hyprluiButtonClicks
						)
						if not setOk then
							hyprluiWarn("hyprlui.set_text (onClick)", setErr)
						end
					end,
					hl.plugin.hyprlui.Text({ x = 12, y = 8, text = "Click me" }),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (button test)", err)
	else
		hyprluiButtonWindowOpen = true
	end
end, { description = "HyprLUI: toggle a clickable-button test window" })

--------------------------------------------------
---- HYPRLUI EXCLUSIVE ZONE TEST (Phase 5) ----
--------------------------------------------------
-- Reserves real screen-edge space - open a tiled window (ALT + T for a
-- terminal) while this is active and it should visibly avoid the
-- reserved region, same as a real bar/dock would. Toggle off and the
-- reservation goes away and tiled windows reclaim the space.

local HYPRLUI_EXCLUSIVE_WINDOW = "hyprlui_exclusive_test"
local hyprluiExclusiveWindowOpen = false

-- ALT + SHIFT + E: toggle a 32px-tall exclusive top bar.
hl.bind("ALT + SHIFT + E", function()
	if hyprluiExclusiveWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_EXCLUSIVE_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiExclusiveWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_EXCLUSIVE_WINDOW,
			anchor = "top",
			exclusive = "top",
			hl.plugin.hyprlui.Box({
				id = "bg",
				w = 400,
				h = 32,
				color = 0xff223344,
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (exclusive test)", err)
	else
		hyprluiExclusiveWindowOpen = true
	end
end, { description = "HyprLUI: toggle an exclusive top-bar test window" })

--------------------------------------------------
---- HYPRLUI INPUT/FOCUS TEST (Phase 6) ----
--------------------------------------------------
-- Click the box to focus it, then type - capturing keystrokes, showing
-- them, and Backspace-removing them are all built into Input{} itself
-- (see LuaBridge.hpp), nothing to implement here. Click anywhere else, or
-- press ALT + SHIFT + U, to blur (blur_widget()) - typed content persists
-- across blur, same as a real text field. A key that's actually bound to
-- a real Hyprland keybind (e.g. ALT + SHIFT + U itself) never reaches
-- this field at all, it just fires the keybind normally, exactly as if
-- the field weren't focused. ALT + SHIFT + Y calls focus_widget()
-- programmatically instead of clicking, to exercise that path too.

local HYPRLUI_INPUT_WINDOW = "hyprlui_input_test"
local hyprluiInputWindowOpen = false

hl.bind("ALT + SHIFT + I", function()
	if hyprluiInputWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_INPUT_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiInputWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_INPUT_WINDOW,
			anchor = "center",
			x = 0,
			y = -80,
			hl.plugin.hyprlui.Stack({
				id = "root",
				hl.plugin.hyprlui.Input({
					id = "field",
					x = 0,
					y = 0,
					w = 220,
					h = 60,
					color = 0x33222222,
					rounding = 8,
					-- Exercises get_input_text() - reads back whatever was
					-- typed once focus is lost, independent of onChange.
					onBlur = function()
						local ok, textOrErr = pcall(hl.plugin.hyprlui.get_input_text, HYPRLUI_INPUT_WINDOW, "field")
						if not ok then
							hyprluiWarn("hyprlui.get_input_text", textOrErr)
							return
						end
						hl.notification.create({ text = "input blurred with: '" .. textOrErr .. "'", timeout = 2000 })
					end,
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (input test)", err)
	else
		hyprluiInputWindowOpen = true
	end
end, { description = "HyprLUI: toggle a focusable Input test window" })

-- ALT + SHIFT + Y: focus the Input above programmatically (no click).
hl.bind("ALT + SHIFT + Y", function()
	if not hyprluiInputWindowOpen then
		return
	end
	local ok, err = pcall(hl.plugin.hyprlui.focus_widget, HYPRLUI_INPUT_WINDOW, "field")
	if not ok then
		hyprluiWarn("hyprlui.focus_widget", err)
	end
end, { description = "HyprLUI: focus the Input test widget programmatically" })

-- ALT + SHIFT + U: blur whatever currently has HyprLUI's keyboard focus.
hl.bind("ALT + SHIFT + U", function()
	local ok, err = pcall(hl.plugin.hyprlui.blur_widget)
	hl.notification.create({ text = "ratesnt", timeout = 2000 })
	if not ok then
		hyprluiWarn("hyprlui.blur_widget", err)
	end
end, { description = "HyprLUI: blur whichever Input currently has focus" })

--------------------------------------------------
---- HYPRLUI BASE WIDGET PROPERTIES TEST (Phase 7) ----
--------------------------------------------------
-- Exercises the shared CWidget-level properties every widget type now
-- has (see DESIGN.md Phase 7 / Widget.hpp): padding, margin, opacity,
-- zIndex, min/max sizing (+ the text-overflow default it forces), and a
-- runtime visibility toggle for a single widget (not the whole window).

local HYPRLUI_PROPS_WINDOW = "hyprlui_props_test"
local hyprluiPropsWindowOpen = false
local hyprluiLongTextVisible = true

-- ALT + SHIFT + P: toggle the properties test window.
hl.bind("ALT + SHIFT + P", function()
	if hyprluiPropsWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_PROPS_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiPropsWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_PROPS_WINDOW,
			anchor = "center",
			x = 0,
			y = 80,
			hl.plugin.hyprlui.Stack({
				id = "root",
				w = 320,
				h = 160,
				-- Debug overlay (post-Phase-7): `debug = true` here
				-- cascades to every descendant by default - see "row"
				-- below for turning that back off for one subtree, and
				-- "bg" for force-hiding one specific label category on a
				-- widget that would otherwise auto-show it (it's plenty
				-- big enough).
				debug = true,
				debugFontSize = 7,
				hl.plugin.hyprlui.Box({
					id = "bg",
					x = 0,
					y = 0,
					w = 320,
					h = 160,
					color = 0x22111111,
					rounding = 8,
					debugShow = { size = false },
				}),

				-- zIndex: "front" is added BEFORE "back" here (so plain
				-- document order would paint "back" on top), but "front"
				-- has the higher zIndex - it should still end up on top,
				-- proving this is a real reorder, not just insertion order.
				hl.plugin.hyprlui.Box({
					id = "front",
					x = 20,
					y = 16,
					w = 60,
					h = 40,
					color = 0xffcc4444,
					rounding = 6,
					zIndex = 2,
				}),
				hl.plugin.hyprlui.Box({
					id = "back",
					x = 40,
					y = 30,
					w = 60,
					h = 40,
					color = 0xff4477cc,
					rounding = 6,
					zIndex = 1,
				}),

				-- opacity: same color/size as "front" above, faded to 35% -
				-- and multiplied with the window's own (default, 1.0)
				-- opacity, so this is purely this Box's own value.
				hl.plugin.hyprlui.Box({
					id = "faded",
					x = 50,
					y = 10,
					w = 60,
					h = 40,
					color = 0xffcc4444,
					rounding = 6,
					opacity = 0.35,
					zIndex = 3,
				}),

				-- padding + margin + gap: a Row with its own container
				-- padding and a gap between children, where "r2" also
				-- carries its own extra left margin ON TOP of that gap
				-- (visibly wider spacing before it than between r1/r3).
				hl.plugin.hyprlui.Row({
					id = "row",
					x = 12,
					y = 70,
					gap = 8,
					padding = 8,
					-- Walls this subtree off from the root's `debug = true`
					-- - r1/r2/r3 below show no debug overlay even though
					-- everything else in the window does.
					debugCascade = false,
					hl.plugin.hyprlui.Box({ id = "r1", w = 24, h = 24, color = 0xff88cc88 }),
					hl.plugin.hyprlui.Box({ id = "r2", w = 24, h = 24, color = 0xff88cc88, margin = { left = 16 } }),
					hl.plugin.hyprlui.Box({ id = "r3", w = 24, h = 24, color = 0xff88cc88 }),
				}),

				-- min/max + text overflow: `maxW` here is narrower than the
				-- text's natural width, so it truncates with an ellipsis -
				-- the chosen v1 default, gotten from Hyprland's own Pango-
				-- based text renderer rather than reimplemented (see
				-- TextNode.cpp). ALT + SHIFT + O below toggles just this
				-- widget's visibility without touching anything else.
				hl.plugin.hyprlui.Text({
					id = "long",
					x = 12,
					y = 116,
					text = "This label is far too long to fit",
					maxW = 140,
					color = 0xffffffff,
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (props test)", err)
	else
		hyprluiPropsWindowOpen = true
		hyprluiLongTextVisible = true -- matches the widget's fresh default every recreation
	end
end, { description = "HyprLUI: toggle the base widget properties test window" })

-- ALT + SHIFT + O: toggle just the truncated "long" Text widget's
-- visibility via set_widget_visible() - the rest of the window (and that
-- widget's own state/id) stays alive and unaffected.
hl.bind("ALT + SHIFT + O", function()
	if not hyprluiPropsWindowOpen then
		return
	end
	local ok, err =
		pcall(hl.plugin.hyprlui.set_widget_visible, HYPRLUI_PROPS_WINDOW, "long", not hyprluiLongTextVisible)
	if not ok then
		hyprluiWarn("hyprlui.set_widget_visible", err)
		return
	end
	hyprluiLongTextVisible = not hyprluiLongTextVisible
end, { description = "HyprLUI: toggle the props test's truncated Text widget visible" })

--------------------------------------------------
---- HYPRLUI WIDGET CATALOG TEST (Phase 8) ----
--------------------------------------------------
-- Exercises the three widgets that round out the v1 catalog (DESIGN.md
-- Phase 8): Image (decodes a real file via Hyprgraphics::CImage - see
-- test-icon.png, a small placeholder icon committed alongside this test),
-- Divider (pure Lua sugar over a Box, no new C++ behavior), and Checkbox
-- (checked/unchecked, click-to-toggle, onChange fires the new state).
-- Same outer-Stack-plus-absolutely-positioned-Column pattern the Phase 1
-- test window uses so the background Box actually sits behind its
-- siblings (see LuaBridge.hpp's own note on this).

local HYPRLUI_CATALOG_WINDOW = "hyprlui_catalog_test"
local hyprluiCatalogWindowOpen = false

-- ALT + SHIFT + G: toggle the widget catalog test window.
hl.bind("ALT + SHIFT + G", function()
	if hyprluiCatalogWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_CATALOG_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiCatalogWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_CATALOG_WINDOW,
			anchor = "center",
			x = 0,
			y = -140,
			hl.plugin.hyprlui.Stack({
				id = "root",
				w = 240,
				h = 190,
				hl.plugin.hyprlui.Box({ id = "bg", x = 0, y = 0, w = 240, h = 190, color = 0x22111111, rounding = 8 }),
				hl.plugin.hyprlui.Column({
					id = "content",
					x = 12,
					y = 12,
					gap = 10,
					hl.plugin.hyprlui.Image({
						id = "icon",
						path = "/home/moritzgleissner/dev/HyprLUI/test-icon.png",
						w = 40,
						h = 40,
					}),
					hl.plugin.hyprlui.Divider({ id = "sep", length = 216 }),
					hl.plugin.hyprlui.Row({
						id = "cb_row",
						gap = 8,
						hl.plugin.hyprlui.Checkbox({
							id = "cb",
							w = 20,
							h = 20,
							color = 0x33333333,
							checkedColor = 0xff4499ff,
							rounding = 4,
							-- get_checkbox_checked() would work just as well
							-- here - onChange's own `checked` argument is
							-- used instead just to exercise that path too.
							onChange = function(checked)
								local ok, err = pcall(
									hl.plugin.hyprlui.set_text,
									HYPRLUI_CATALOG_WINDOW,
									"cb_label",
									checked and "checked" or "unchecked"
								)
								if not ok then
									hyprluiWarn("hyprlui.set_text", err)
								end
							end,
						}),
						hl.plugin.hyprlui.Text({ id = "cb_label", text = "unchecked", size = 14 }),
					}),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (catalog test)", err)
	else
		hyprluiCatalogWindowOpen = true
	end
end, { description = "HyprLUI: toggle the widget catalog (Image/Divider/Checkbox) test window" })

--------------------------------------------------
---- HYPRLUI COMPONENTS TEST (Phase 9) ----
--------------------------------------------------
-- Exercises hyprlui.defineComponent()/hyprlui.Component() (DESIGN.md
-- Phase 9): a single "LabeledButton" template registered once, then
-- instantiated five times in the same window - twice with an explicit
-- `key` (independently addressable afterwards, see the onClick below),
-- three times with no key at all (auto-generated "LabeledButton#N",
-- exercising the no-explicit-key path). Every instance reuses the exact
-- same internal ids ("btn"/"label") inside render() - id-rewriting is
-- what keeps all five from colliding with each other.

local HYPRLUI_COMPONENTS_WINDOW = "hyprlui_components_test"
local hyprluiComponentsWindowOpen = false
local hyprluiSaveClicks = 0

-- Unlike every hl.plugin.hyprlui.* call elsewhere in this file (always
-- inside a bind's callback, so it only ever actually runs once the user
-- presses a key - long after the plugin has loaded), defineComponent()
-- needs to run once at top level, during the script's own initial
-- evaluation. On first boot that evaluation happens BEFORE the plugin has
-- finished loading (see the hl.plugin.hyprlui ~= nil check near the top
-- of this file for the same gap) - hl.plugin.hyprlui is still nil then,
-- so an unguarded call here throws "attempt to index a nil value (field
-- 'hyprlui')". Harmless and self-correcting even unguarded (main.cpp's
-- PLUGIN_INIT calls HyprlandAPI::reloadConfig() right after registering
-- the plugin's functions, which re-runs this whole script a second time
-- with hl.plugin.hyprlui now populated) - but the same nil-check used
-- above avoids the spurious error being logged in the meantime.
if hl.plugin.hyprlui ~= nil then
	hl.plugin.hyprlui.defineComponent("LabeledButton", {
		props = {
			label = { required = true },
			color = { default = 0x33333333 },
			onClick = { required = false },
		},
		render = function(props)
			return hl.plugin.hyprlui.Button({
				id = "btn",
				w = 140,
				h = 28,
				color = props.color,
				rounding = 6,
				onClick = props.onClick,
				hl.plugin.hyprlui.Text({ id = "label", x = 12, y = 6, text = props.label, size = 13 }),
			})
		end,
	})
end

-- ALT + SHIFT + Z: toggle the components test window.
hl.bind("ALT + SHIFT + Z", function()
	if hyprluiComponentsWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_COMPONENTS_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiComponentsWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_COMPONENTS_WINDOW,
			anchor = "center",
			x = 0,
			y = 190,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				-- Explicit keys - each instance addressable afterwards.
				-- onClick is supplied by the CALLER (not hardcoded inside
				-- render()), since only the caller knows what key it
				-- chose - render() itself has no way to know its own
				-- instance's key.
				hl.plugin.hyprlui.Component("LabeledButton", {
					label = "Save",
					color = 0x33224488,
					onClick = function()
						hyprluiSaveClicks = hyprluiSaveClicks + 1
						local ok, err = pcall(
							hl.plugin.hyprlui.set_text,
							HYPRLUI_COMPONENTS_WINDOW,
							"save_btn::label",
							"Saved x" .. hyprluiSaveClicks
						)
						if not ok then
							hyprluiWarn("hyprlui.set_text", err)
						end
					end,
				}, { key = "save_btn" }),
				hl.plugin.hyprlui.Component("LabeledButton", { label = "Cancel" }, { key = "cancel_btn" }),
				-- No explicit key - auto-generated, fine for instances
				-- nothing outside needs to address individually.
				hl.plugin.hyprlui.Component("LabeledButton", { label = "Item 1" }),
				hl.plugin.hyprlui.Component("LabeledButton", { label = "Item 2" }),
				hl.plugin.hyprlui.Component("LabeledButton", { label = "Item 3" }),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (components test)", err)
	else
		hyprluiComponentsWindowOpen = true
	end
end, { description = "HyprLUI: toggle the components (Phase 9) test window" })

--------------------------------------------------
---- HYPRLUI INTERACTIVE STATE TEST (Phase 10) ----
--------------------------------------------------
-- Exercises disabled/hover/scroll (DESIGN.md Phase 10). Hovering the
-- "target" button swaps its color via hoverColor - built in, no Lua round
-- trip needed - AND fires onHoverStart/onHoverEnd to update a status
-- label (the escape hatch for anything beyond a flat color swap); a real
-- pointer cursor change over it happens automatically too, independent of
-- either. Scrolling over it fires onScroll. The checkbox toggles the
-- target's disabled state via set_widget_disabled() - once disabled, it's
-- click-through/unhoverable/unscrollable entirely (excluded from hit-
-- testing, same as a real disabled control) and shows disabledColor
-- instead.
--
-- "plain_click" is a bare Box (NOT a Button) made clickable purely by
-- giving it an onClick handler - onClick moved to being a generic
-- CWidget field in a same-session Phase 10 follow-up, so any widget can
-- become a click target this way, not just Button.

local HYPRLUI_INTERACTIVE_WINDOW = "hyprlui_interactive_test"
local hyprluiInteractiveWindowOpen = false
local hyprluiScrollCount = 0
local hyprluiPlainClicks = 0

-- ALT + SHIFT + X: toggle the interactive state test window.
hl.bind("ALT + SHIFT + X", function()
	if hyprluiInteractiveWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_INTERACTIVE_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiInteractiveWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_INTERACTIVE_WINDOW,
			anchor = "center",
			x = 0,
			y = -190,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				hl.plugin.hyprlui.Button({
					id = "target",
					w = 180,
					h = 36,
					color = 0x33333333,
					hoverColor = 0x3355aaff,
					disabledColor = 0x22111111,
					rounding = 6,
					onHoverStart = function()
						pcall(hl.plugin.hyprlui.set_text, HYPRLUI_INTERACTIVE_WINDOW, "status", "hovering")
					end,
					onHoverEnd = function()
						pcall(hl.plugin.hyprlui.set_text, HYPRLUI_INTERACTIVE_WINDOW, "status", "idle")
					end,
					onScroll = function(delta, vertical)
						hyprluiScrollCount = hyprluiScrollCount + 1
						pcall(
							hl.plugin.hyprlui.set_text,
							HYPRLUI_INTERACTIVE_WINDOW,
							"status",
							"scrolled x" .. hyprluiScrollCount
						)
					end,
					hl.plugin.hyprlui.Text({
						id = "target_label",
						x = 12,
						y = 10,
						text = "hover / scroll me",
						size = 13,
					}),
				}),
				hl.plugin.hyprlui.Box({
					id = "plain_click",
					w = 180,
					h = 28,
					color = 0x33447744,
					rounding = 6,
					onClick = function()
						hyprluiPlainClicks = hyprluiPlainClicks + 1
						pcall(
							hl.plugin.hyprlui.set_text,
							HYPRLUI_INTERACTIVE_WINDOW,
							"status",
							"plain box clicked x" .. hyprluiPlainClicks
						)
					end,
					hl.plugin.hyprlui.Text({
						id = "plain_click_label",
						x = 12,
						y = 7,
						text = "click me (plain Box)",
						size = 12,
					}),
				}),
				hl.plugin.hyprlui.Text({ id = "status", text = "idle", size = 12 }),
				hl.plugin.hyprlui.Row({
					id = "toggle_row",
					gap = 8,
					hl.plugin.hyprlui.Checkbox({
						id = "toggle",
						w = 18,
						h = 18,
						color = 0x33333333,
						checkedColor = 0xffcc4444,
						rounding = 3,
						onChange = function(checked)
							local ok, err = pcall(
								hl.plugin.hyprlui.set_widget_disabled,
								HYPRLUI_INTERACTIVE_WINDOW,
								"target",
								checked
							)
							if not ok then
								hyprluiWarn("hyprlui.set_widget_disabled", err)
							end
						end,
					}),
					hl.plugin.hyprlui.Text({ id = "toggle_label", text = "disable target", size = 12 }),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window (interactive test)", err)
	else
		hyprluiInteractiveWindowOpen = true
		hyprluiScrollCount = 0
		hyprluiPlainClicks = 0
	end
end, { description = "HyprLUI: toggle the interactive state (Phase 10) test window" })

--------------------------------------------------
---- HYPRLUI PERSISTENCE TEST (Phase 11) ----
--------------------------------------------------
-- Exercises hyprlui.persistent() (DESIGN.md Phase 11). Unlike every other
-- local in this file, the value behind "persistentClicks" survives a
-- config reload - the wrapper table itself is a fresh Lua table every
-- time this line re-runs, but :get()/:set() both read/write the SAME
-- native C++ store underneath, which isn't touched by a reload at all.
--
-- To actually see this: press ALT + SHIFT + S a few times (notice the
-- count), then reload the config (save this file, or `hyprctl reload`),
-- then press ALT + SHIFT + S again - the count keeps going instead of
-- resetting to 0, unlike hyprluiClicks/hyprluiScrollCount/every other
-- plain `local` counter in this file, which WOULD reset on that same
-- reload.

-- Same top-level-call gap as defineComponent() above (see its own
-- comment for the full explanation): on first boot this line runs BEFORE
-- the plugin has loaded, when hl.plugin.hyprlui is still nil, so it has
-- to be guarded the same way - harmless either way, since PLUGIN_INIT's
-- reloadConfig() re-runs this whole script again right after the plugin
-- registers its functions.
local persistentClicks = nil
if hl.plugin.hyprlui ~= nil then
	persistentClicks = hl.plugin.hyprlui.persistent("persistent_clicks_demo", 0)
end

-- ALT + SHIFT + S: bump the persistent counter and show its current value.
hl.bind("ALT + SHIFT + S", function()
	-- Guards against the same first-boot gap persistentClicks itself is
	-- guarded against above - shouldn't actually be reachable in practice
	-- (a bind only ever fires well after boot, by which point the
	-- PLUGIN_INIT-triggered reload has already re-run this file with the
	-- plugin loaded), but keeps this honest for the type checker too.
	if not persistentClicks then
		return
	end

	local ok, err = pcall(function()
		persistentClicks:set(persistentClicks:get() + 1)
		hl.notification.create({ text = "persistent count: " .. persistentClicks:get(), timeout = 2000 })
	end)
	if not ok then
		hyprluiWarn("hyprlui.persistent", err)
	end
end, { description = "HyprLUI: bump a persistent counter (survives a config reload)" })

--------------------------------------------------
---- HYPRLUI NATIVE SERVICES TEST (Phase 12) ----
--------------------------------------------------
-- Exercises hyprlui.run_cmd()/hyprlui.open_socket() (DESIGN.md Phase 12) -
-- the two generic native primitives everything else (polling `pactl`,
-- talking to a PipeWire/D-Bus proxy, etc.) is meant to be built on top of
-- in pure Lua.

-- ALT + SHIFT + F: run_cmd() a one-shot shell command asynchronously and
-- show its captured stdout once it finishes.
hl.bind("ALT + SHIFT + F", function()
	if hl.plugin.hyprlui == nil then
		return
	end
	local ok, err = pcall(function()
		hl.plugin.hyprlui.run_cmd("date", function(output)
			hl.notification.create({ text = "run_cmd: " .. output, timeout = 3000 })
		end)
	end)
	if not ok then
		hyprluiWarn("hyprlui.run_cmd", err)
	end
end, { description = "HyprLUI: run_cmd() a shell command asynchronously" })

-- ALT + SHIFT + D: open_socket() to Hyprland's own event stream
-- (.socket2.sock) and print whatever the first :read() call returns, then
-- close it - a status-bar widget could instead keep re-arming :read()
-- forever to react to workspace/window changes live.
hl.bind("ALT + SHIFT + D", function()
	if hl.plugin.hyprlui == nil then
		return
	end
	local his = os.getenv("HYPRLAND_INSTANCE_SIGNATURE")
	local runtimeDir = os.getenv("XDG_RUNTIME_DIR")
	if not his or not runtimeDir then
		hyprluiWarn("hyprlui.open_socket", "HYPRLAND_INSTANCE_SIGNATURE/XDG_RUNTIME_DIR not set")
		return
	end
	local path = runtimeDir .. "/hypr/" .. his .. "/.socket2.sock"

	local ok, err = pcall(function()
		hl.plugin.hyprlui.open_socket(path, function(sock)
			if not sock then
				hl.notification.create({ text = "open_socket: failed to connect", timeout = 3000 })
				return
			end
			sock:read(function(data)
				if data then
					hl.notification.create({ text = "socket2 event: " .. data, timeout = 3000 })
				else
					hl.notification.create({ text = "socket2: closed", timeout = 3000 })
				end
				sock:close()
			end)
		end)
	end)
	if not ok then
		hyprluiWarn("hyprlui.open_socket", err)
	end
end, { description = "HyprLUI: open_socket() Hyprland's own event stream, print one event" })

--------------------------------------------------
---- HYPRLUI FADE ANIMATION TEST (Phase 13) ----
--------------------------------------------------
-- Exercises hyprlui.animation() + the opt-in fade path set_widget_visible()/
-- set_canvas_visible()/window()/remove_widget()/remove_canvas() now take
-- (DESIGN.md Phase 13). Configuring "in"/"out" once up front, like this, is
-- the whole API surface - EVERY later visibility change for ANY widget or
-- window then fades instead of snapping/popping, with no per-call change
-- needed: ALT+SHIFT+T below (open/close the whole window) fades on its own
-- purely as a side effect of this config existing, same as ALT+SHIFT+W
-- (toggle one widget) and ALT+SHIFT+Q (remove one widget for good) below.
-- Same top-level-call gap as defineComponent()/persistent() above (see
-- their own comments) - guarded the same way.
if hl.plugin.hyprlui ~= nil then
	hl.plugin.hyprlui.animation({ leaf = "in", speed = 3, bezier = "default" })
	hl.plugin.hyprlui.animation({ leaf = "out", speed = 3, bezier = "default" })
end

local HYPRLUI_FADE_WINDOW = "hyprlui_fade_test"
local hyprluiFadeWindowOpen = false
local hyprluiFadeBoxVisible = true

-- ALT + SHIFT + T: toggle the fade test window.
hl.bind("ALT + SHIFT + T", function()
	if hyprluiFadeWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_FADE_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiFadeWindowOpen = false
		return
	end

	hyprluiFadeBoxVisible = true
	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_FADE_WINDOW,
			anchor = "center",
			x = 0,
			y = 60,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				hl.plugin.hyprlui.Text({ id = "label", text = "ALT+SHIFT+W to fade both boxes", size = 12 }),
				hl.plugin.hyprlui.Row({
					id = "boxes",
					gap = 8,
					hl.plugin.hyprlui.Box({ id = "fade_box", w = 160, h = 80, color = 0xffcc8833, rounding = 8 }),
					-- Per-widget override (Phase 13 follow-up) - noticeably
					-- slower (1.2s vs the global 0.3s) and a different
					-- bezier, so toggling both boxes together with the
					-- SAME keybind makes the override visibly obvious
					-- rather than needing a separate bind to prove it
					-- does anything.
					hl.plugin.hyprlui.Box({
						id = "fade_box_custom",
						w = 160,
						h = 80,
						color = 0xff3388cc,
						rounding = 8,
						animationIn = { speed = 12, bezier = "default" },
						animationOut = { speed = 12, bezier = "default" },
					}),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window", err)
		return
	end
	hyprluiFadeWindowOpen = true
end, { description = "HyprLUI: toggle the fade-animation test window" })

-- ALT + SHIFT + W: toggle both boxes' visibility via set_widget_visible()
-- - "fade_box" follows the global 0.3s config from above; "fade_box_custom"
-- overrides it with its own animationIn/animationOut (1.2s) - toggling both at once
-- with the same keypress makes the per-widget override's effect obvious.
hl.bind("ALT + SHIFT + W", function()
	if not hyprluiFadeWindowOpen then
		return
	end
	hyprluiFadeBoxVisible = not hyprluiFadeBoxVisible
	local ok, err = pcall(hl.plugin.hyprlui.set_widget_visible, HYPRLUI_FADE_WINDOW, "fade_box", hyprluiFadeBoxVisible)
	if not ok then
		hyprluiWarn("hyprlui.set_widget_visible", err)
	end
	ok, err = pcall(hl.plugin.hyprlui.set_widget_visible, HYPRLUI_FADE_WINDOW, "fade_box_custom", hyprluiFadeBoxVisible)
	if not ok then
		hyprluiWarn("hyprlui.set_widget_visible", err)
	end
end, { description = "HyprLUI: fade-toggle both test boxes' visibility" })

-- ALT + SHIFT + Q: remove_widget() both boxes for good (not just hide them -
-- gone, can't be brought back without ALT+SHIFT+T closing/reopening the
-- whole window) - each fades out using its own config (global vs override)
-- first and only actually erases once that finishes, instead of vanishing
-- instantly.
hl.bind("ALT + SHIFT + Q", function()
	if not hyprluiFadeWindowOpen then
		return
	end
	local ok, err = pcall(hl.plugin.hyprlui.remove_widget, HYPRLUI_FADE_WINDOW, "fade_box")
	if not ok then
		hyprluiWarn("hyprlui.remove_widget", err)
	end
	ok, err = pcall(hl.plugin.hyprlui.remove_widget, HYPRLUI_FADE_WINDOW, "fade_box_custom")
	if not ok then
		hyprluiWarn("hyprlui.remove_widget", err)
	end
end, { description = "HyprLUI: fade-then-remove the test box for good" })

-- ALT + SHIFT + L: toggle set_canvas_position() on/off - the fade window
-- was created with anchor="center", so the FIRST press clears that anchor
-- (see CCanvas::clearAnchor()'s doc comment) and explicitly moves it;
-- pressing again moves it back to where it originally was (still no
-- anchor from here on - this doesn't restore it).
local hyprluiFadeWindowMoved = false
hl.bind("ALT + SHIFT + L", function()
	if not hyprluiFadeWindowOpen then
		return
	end
	hyprluiFadeWindowMoved = not hyprluiFadeWindowMoved
	local ok, err =
		pcall(hl.plugin.hyprlui.set_canvas_position, HYPRLUI_FADE_WINDOW, hyprluiFadeWindowMoved and 400 or 0, 60)
	if not ok then
		hyprluiWarn("hyprlui.set_canvas_position", err)
	end
end, { description = "HyprLUI: explicitly reposition the fade test window" })

-- ALT + SHIFT + 1: toggle set_canvas_size() - pins the window to a fixed
-- 400x200 box (bigger than its natural content, so the extra space is
-- plainly visible) or lets it size-to-content again (nil/nil).
local hyprluiFadeWindowFixedSize = false
hl.bind("ALT + SHIFT + 1", function()
	if not hyprluiFadeWindowOpen then
		return
	end
	hyprluiFadeWindowFixedSize = not hyprluiFadeWindowFixedSize
	local ok, err
	if hyprluiFadeWindowFixedSize then
		ok, err = pcall(hl.plugin.hyprlui.set_canvas_size, HYPRLUI_FADE_WINDOW, 400, 200)
	else
		ok, err = pcall(hl.plugin.hyprlui.set_canvas_size, HYPRLUI_FADE_WINDOW, nil, nil)
	end
	if not ok then
		hyprluiWarn("hyprlui.set_canvas_size", err)
	end
end, { description = "HyprLUI: toggle a fixed size on the fade test window" })

-- ALT + SHIFT + 2: toggle set_widget_size() - resizes just "fade_box"
-- (normally 160x80) to a bigger 240x80, one level down from
-- set_canvas_size() above. Note this grows the BOX itself since it's a
-- plain solid-color rect (nothing inside it to fail to stretch) - a
-- container widget with fixed-size children would show the same
-- "content doesn't grow to fill" gap flagged in DESIGN.md's Phase 15 note.
local hyprluiFadeBoxFixedSize = false
hl.bind("ALT + SHIFT + 2", function()
	if not hyprluiFadeWindowOpen then
		return
	end
	hyprluiFadeBoxFixedSize = not hyprluiFadeBoxFixedSize
	local ok, err
	if hyprluiFadeBoxFixedSize then
		ok, err = pcall(hl.plugin.hyprlui.set_widget_size, HYPRLUI_FADE_WINDOW, "fade_box", 240, 80)
	else
		ok, err = pcall(hl.plugin.hyprlui.set_widget_size, HYPRLUI_FADE_WINDOW, "fade_box", 160, 80)
	end
	if not ok then
		hyprluiWarn("hyprlui.set_widget_size", err)
	end
end, { description = "HyprLUI: toggle a fixed size on the fade_box widget" })

--------------------------------------------------
---- HYPRLUI FILL TEST (Phase 15) ----
--------------------------------------------------
-- Exercises `fill` (DESIGN.md Phase 15) - "stretch to match my parent's
-- available size" - across all three places it's interpreted: a Row's
-- cross axis, a Stack's own full size, and a window's root widget vs its
-- canvas. "grow_fill" below is only 20px tall on its own, but its Row
-- sibling "short" is 60px - fill stretches it to match. "bg" fills the
-- whole Stack behind the content. The root Stack itself has fill=true,
-- so growing the WHOLE WINDOW via ALT+SHIFT+4 (set_canvas_size) grows
-- "bg" (and everything else) right along with it, instead of the content
-- just sitting in a corner of a bigger, mostly-empty window.
--
-- "content" is ALSO fill=true here (not just "bg") - deliberately: with
-- BOTH of the root Stack's children marked fill, root has zero non-fill
-- children to size itself from, which is the exact degenerate case
-- CStackWidget::measureContent()'s fallback exists for (see its doc
-- comment, ContainerWidget.cpp) - this keeps that fallback path, and the
-- leaf self-correction it depends on (CWidget::primeNaturalSize(),
-- Widget.hpp), under live regression coverage instead of only the common
-- "one real sibling to size against" case. A real live bug (`bg` not
-- shrinking back on a second ALT+SHIFT+4 press) was found through
-- exactly this config - see DESIGN.md's Phase 15/Open Questions entries.

local HYPRLUI_FILL_WINDOW = "hyprlui_fill_test"
local hyprluiFillWindowOpen = false
local hyprluiFillWindowBig = false

-- ALT + SHIFT + 3: toggle the fill test window.
hl.bind("ALT + SHIFT + 3", function()
	if hyprluiFillWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_FILL_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiFillWindowOpen = false
		hyprluiFillWindowBig = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_FILL_WINDOW,
			x = 200,
			y = 400,
			hl.plugin.hyprlui.Stack({
				id = "root",
				debug = true,
				fill = true, -- root-fills-canvas: matches the window's own size whenever set_canvas_size() below gives it one
				-- w/h are still required at construction (a Box's w/h are
				-- its actual dimensions, not an override, unlike
				-- containers/Image) even though fill overrides them
				-- immediately during arrange() - the exact values here
				-- don't matter beyond satisfying that requirement.
				hl.plugin.hyprlui.Box({ id = "bg", w = 1, h = 1, fill = true, color = 0xff222222, rounding = 8 }),
				hl.plugin.hyprlui.Column({
					id = "content",
					x = 12,
					y = 12,
					gap = 8,
					hl.plugin.hyprlui.Text({
						id = "label",
						text = "ALT+SHIFT+4 to grow the window",
						size = 12,
					}),
					hl.plugin.hyprlui.Row({
						id = "row",
						gap = 8,
						hl.plugin.hyprlui.Box({ id = "short", w = 60, h = 60, color = 0xffcc8833, rounding = 4 }),
						hl.plugin.hyprlui.Box({
							id = "grow_fill",
							w = 60,
							h = 20,
							fill = true,
							color = 0xff3388cc,
							rounding = 4,
						}),
					}),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window", err)
		return
	end
	hyprluiFillWindowOpen = true
end, { description = "HyprLUI: toggle the fill test window" })

-- ALT + SHIFT + 4: grow/shrink the fill test window - with "root"
-- (the Stack) marked fill=true, "bg" (itself fill=true within that
-- Stack) grows right along with the window instead of staying put.
hl.bind("ALT + SHIFT + 4", function()
	if not hyprluiFillWindowOpen then
		return
	end
	hyprluiFillWindowBig = not hyprluiFillWindowBig
	local ok, err
	if hyprluiFillWindowBig then
		ok, err = pcall(hl.plugin.hyprlui.set_canvas_size, HYPRLUI_FILL_WINDOW, 400, 250)
	else
		ok, err = pcall(hl.plugin.hyprlui.set_canvas_size, HYPRLUI_FILL_WINDOW, nil, nil)
	end
	if not ok then
		hyprluiWarn("hyprlui.set_canvas_size", err)
	end
end, { description = "HyprLUI: grow/shrink the fill test window" })

-- ALT + SHIFT + 5: toggle a Row-of-Columns "2x2 matrix" fill test - a
-- DIFFERENT shape than the Stack test above, specifically to exercise
-- `fill` NESTED two levels deep with two DIFFERENT cross axes in play at
-- once (a Row's cross axis is height; a Column's cross axis is width):
--   col_a (fill=true, a Row child) - col_a's own NATURAL height (from its
--     own two boxes) is smaller than col_b's, so it stretches to match
--     col_b's height (the row's cross axis).
--   a2 (fill=true, a Column child, inside col_a) - a2's own w=20 is
--     narrower than col_a's other box (a1, w=60), so it stretches to
--     match col_a's width (the COLUMN's cross axis) - note this is
--     col_a's width, unaffected by col_a's OWN height-fill above; the two
--     fills are on perpendicular axes and don't interact.
local HYPRLUI_MATRIX_WINDOW = "hyprlui_matrix_test"
local hyprluiMatrixWindowOpen = false

hl.bind("ALT + SHIFT + 5", function()
	if hyprluiMatrixWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_MATRIX_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiMatrixWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_MATRIX_WINDOW,
			x = 650,
			y = 400,
			hl.plugin.hyprlui.Row({
				id = "root",
				gap = 8,
				hl.plugin.hyprlui.Column({
					id = "col_a",
					gap = 8,
					fill = true,
					hl.plugin.hyprlui.Box({ id = "a1", w = 60, h = 40, color = 0xffcc8833, rounding = 4 }),
					hl.plugin.hyprlui.Box({ id = "a2", w = 20, h = 40, fill = true, color = 0xff3388cc, rounding = 4 }),
				}),
				hl.plugin.hyprlui.Column({
					id = "col_b",
					gap = 8,
					hl.plugin.hyprlui.Box({ id = "b1", w = 80, h = 100, color = 0xff88cc33, rounding = 4 }),
					hl.plugin.hyprlui.Box({ id = "b2", w = 80, h = 40, color = 0xffcc3388, rounding = 4 }),
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window", err)
		return
	end
	hyprluiMatrixWindowOpen = true
end, { description = "HyprLUI: toggle the row-of-columns fill matrix test" })

--------------------------------------------------
---- HYPRLUI STYLE TEST (Phase 16 follow-up) ----
--------------------------------------------------
-- Exercises `style` (DESIGN.md Phase 16) - now just another field on the
-- SAME animationIn/animationOut config shape hyprlui.animation() and
-- every widget's own override already share (enable/speed/bezier-or-
-- spring/style), NOT a separate `window{}`-level mechanism - a window's
-- root widget sliding IS the whole window sliding, exactly the same "no
-- distinction between a widget and its window" principle its opacity
-- fade already had. This window sets `style = "slide left"` on BOTH
-- animationIn and animationOut (same 0..1 progress drives opacity AND
-- slide together, they're not independently timed) - and applies
-- uniformly to ANY visibility change, not just creation: ALT+SHIFT+7
-- below just calls set_canvas_visible(), and it slides/fades exactly the
-- same as opening/closing the window would.
local HYPRLUI_STYLE_WINDOW = "hyprlui_style_test"
local hyprluiStyleWindowOpen = false
local hyprluiStyleWindowVisible = true

-- ALT + SHIFT + 6: toggle the slide test window.
hl.bind("ALT + SHIFT + 6", function()
	if hyprluiStyleWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_STYLE_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiStyleWindowOpen = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = HYPRLUI_STYLE_WINDOW,
			anchor = "left",
			x = 20,
			hl.plugin.hyprlui.Stack({
				id = "root",
				animationIn = { speed = 3, bezier = "default", style = "slide left" },
				animationOut = { speed = 3, bezier = "default", style = "slide left" },
				hl.plugin.hyprlui.Box({ id = "bg", w = 200, h = 100, color = 0xff224488, rounding = 8 }),
				hl.plugin.hyprlui.Text({
					id = "label",
					x = 12,
					y = 12,
					text = "ALT+SHIFT+7 to toggle visibility",
					size = 12,
				}),
			}),
		})
	end)
	if not ok then
		hyprluiWarn("hyprlui.window", err)
		return
	end
	hyprluiStyleWindowOpen = true
	hyprluiStyleWindowVisible = true
end, { description = "HyprLUI: toggle the style/slide test window" })

-- ALT + SHIFT + 7: toggle the style test window's visibility without
-- destroying it - same slide+fade as open/close (see the section comment
-- above for why).
hl.bind("ALT + SHIFT + 7", function()
	if not hyprluiStyleWindowOpen then
		return
	end
	hyprluiStyleWindowVisible = not hyprluiStyleWindowVisible
	local ok, err = pcall(hl.plugin.hyprlui.set_canvas_visible, HYPRLUI_STYLE_WINDOW, hyprluiStyleWindowVisible)
	if not ok then
		hyprluiWarn("hyprlui.set_canvas_visible", err)
	end
end, { description = "HyprLUI: toggle style test window visibility" })

--------------------------------------------------
---- HYPRLUI POPIN/GNOME TEST (Phase 17) ----
--------------------------------------------------
-- Exercises `style = "popin ..."`/`"gnome"` (DESIGN.md Phase 17) - unlike
-- `slide` (above), these are scoped to a window's ROOT widget only, and
-- scale the WHOLE subtree (background box AND the two text lines) as one
-- rigid unit, not just the root's own box - a Column of two differently-
-- styled Text children makes that visible (both lines shrink/grow and
-- reposition together, not independently).
local HYPRLUI_POPIN_WINDOW = "hyprlui_popin_test"
local hyprluiPopinWindowOpen = false

local function hyprluiPopinTestWindow(name, style)
	hl.plugin.hyprlui.window({
		name = name,
		x = 900,
		y = 400,
		hl.plugin.hyprlui.Stack({
			id = "root",
			-- debug = true,
			animationIn = { speed = 4, bezier = "default", style = style },
			animationOut = { speed = 4, bezier = "default", style = style },
			hl.plugin.hyprlui.Box({ id = "bg", w = 1, h = 1, fill = true, color = 0xff332266, rounding = 8 }),
			hl.plugin.hyprlui.Column({
				id = "content",
				x = 12,
				y = 12,
				gap = 4,
				hl.plugin.hyprlui.Text({ id = "title", text = style, size = 16 }),
				hl.plugin.hyprlui.Text({
					id = "subtitle",
					text = "ALT+SHIFT+8 popin, ALT+SHIFT+9 gnome",
					size = 11,
					color = { r = 0.8, g = 0.8, b = 0.8, a = 1.0 },
				}),
			}),
		}),
	})
end

-- ALT + SHIFT + 8: toggle a `popin 40%` test window.
hl.bind("ALT + SHIFT + 8", function()
	if hyprluiPopinWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_POPIN_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiPopinWindowOpen = false
		return
	end

	local ok, err = pcall(hyprluiPopinTestWindow, HYPRLUI_POPIN_WINDOW, "popin 40%")
	if not ok then
		hyprluiWarn("hyprlui.window", err)
		return
	end
	hyprluiPopinWindowOpen = true
end, { description = "HyprLUI: toggle the popin test window" })

-- ALT + SHIFT + 9: toggle the same window shape, using `gnome` instead.
hl.bind("ALT + SHIFT + 9", function()
	if hyprluiPopinWindowOpen then
		local ok, err = pcall(hl.plugin.hyprlui.remove_canvas, HYPRLUI_POPIN_WINDOW)
		if not ok then
			hyprluiWarn("hyprlui.remove_canvas", err)
		end
		hyprluiPopinWindowOpen = false
		return
	end

	local ok, err = pcall(hyprluiPopinTestWindow, HYPRLUI_POPIN_WINDOW, "gnome")
	if not ok then
		hyprluiWarn("hyprlui.window", err)
		return
	end
	hyprluiPopinWindowOpen = true
end, { description = "HyprLUI: toggle the gnome test window" })

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
