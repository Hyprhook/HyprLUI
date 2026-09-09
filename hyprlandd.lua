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
