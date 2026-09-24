local M = {}
-- task-checks.lua
--
-- Small demos exercising DESIGN.md's active task list items that
-- actually have observable Lua-facing behavior: tasks 1, 2, 4, 5, 6, 8, 16.
-- Task 3 (LuaBridge.cpp split) is pure internal C++ restructuring with
-- zero change to the Lua API - nothing to demo. Task 7 (Stack padding/
-- margin) was a confirmed no-change. Not exhaustive coverage, just
-- enough to eyeball-confirm each one's real runtime behavior.
--
-- Tasks 1/2/4 use the same toggle-window pattern as demos/which-key.lua:
-- a local tracks whether this demo's window is currently open, reset to
-- closed on every config reload along with everything else (see
-- hyprlandd.lua's own note on why - HyprLUI wipes all windows on
-- config.preReload, and this module's locals reset the same way since
-- the whole script re-runs). Task 5's window is deliberately NOT
-- toggle-style - see its own section below for why.

local function warn(label, err)
	hl.notification.create({ text = label .. " failed: " .. tostring(err), timeout = 3000 })
end

--------------------------------------------------
---- Task 1: window naming defaults ----
--------------------------------------------------
-- `window{}` no longer requires `name` - omitted here on purpose. The
-- call now returns the spec table with the resolved (auto-generated)
-- name written onto it, which this demo displays and then reuses to
-- close this exact window on the next press - proving the round-trip
-- actually works, not just that the field is present.

local task1Window = nil -- resolved name of the open task-1 window, if any

local function toggleTask1()
	if task1Window then
		hl.plugin.hyprlui.remove_canvas(task1Window)
		task1Window = nil
		return
	end

	local ok, result = pcall(function()
		return hl.plugin.hyprlui.window({
			anchor = "top-left",
			x = 20,
			y = 20,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 4,
				padding = 12,
				hl.plugin.hyprlui.Text({ id = "title", text = "task 1: window naming", size = 14, color = 0xffcba6f7 }),
				hl.plugin.hyprlui.Text({ id = "name", text = "...", size = 12, color = 0xff89b4fa }),
			}),
		})
	end)
	if not ok then
		warn("hyprlui.window (task1)", result)
		return
	end

	task1Window = result.name
	hl.plugin.hyprlui.set_text(task1Window, "name", "auto name: " .. task1Window)
end

--------------------------------------------------
---- Task 2: Box sizing default ----
--------------------------------------------------
-- Three Boxes:
--   A: no w/h, fill=true, inside a sized Stack - stretches to fill it,
--      proving the 0x0 default doesn't break the normal fill pattern.
--   B: no w/h, no fill, debug=true - invisible (0x0) AND should log a
--      warning (check Hyprland's log/console for exactly one).
--   C: w=0/h=0 EXPLICITLY given (not omitted), no fill, debug=true -
--      also invisible, but must NOT warn (the warning is only for
--      omitted w/h, not an explicit 0).

local TASK2_WINDOW = "hyprlui_task2_demo"
local task2Open = false

local function toggleTask2()
	if task2Open then
		hl.plugin.hyprlui.remove_canvas(TASK2_WINDOW)
		task2Open = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = TASK2_WINDOW,
			anchor = "top",
			x = 0,
			y = 20,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				padding = 12,
				hl.plugin.hyprlui.Text({
					id = "title",
					text = "task 2: Box sizing default",
					size = 14,
					color = 0xffcba6f7,
				}),
				hl.plugin.hyprlui.Row({
					id = "row",
					gap = 12,
					hl.plugin.hyprlui.Stack({
						id = "a_wrap",
						w = 60,
						h = 24,
						hl.plugin.hyprlui.Box({ id = "a", fill = true, color = 0xff89b4fa, rounding = 4 }),
					}),
					hl.plugin.hyprlui.Box({ id = "b", debug = true, color = 0xfffac6a7 }),
					hl.plugin.hyprlui.Box({ id = "c", w = 0, h = 0, debug = true, color = 0xffa6e3a1 }),
				}),
				hl.plugin.hyprlui.Text({
					id = "hint",
					text = "A fills its box; B/C are 0x0 - check the log for exactly one warning (B, not C)",
					size = 11,
					color = 0xff6c7086,
				}),
			}),
		})
	end)
	if not ok then
		warn("hyprlui.window (task2)", err)
		return
	end
	task2Open = true
end

--------------------------------------------------
---- Task 4: Rectangle as shared background-drawing base ----
--------------------------------------------------
-- Image now inherits CRectNode: it gets a real optional `color` fill
-- (default transparent) drawn behind its texture, and still draws
-- fill+border when the texture fails to load, instead of nothing.
-- Also: renderFill() uses effectiveFillColor() uniformly now, so a
-- plain Box wired up with onClick+hoverColor gets the same hover-color
-- swap that used to be Button/Input/Checkbox-only.

local TASK4_WINDOW = "hyprlui_task4_demo"
local ICON_PATH = "/home/moritzgleissner/dev/HyprLUI/test-icon.png"
local task4Open = false

local function toggleTask4()
	if task4Open then
		hl.plugin.hyprlui.remove_canvas(TASK4_WINDOW)
		task4Open = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = TASK4_WINDOW,
			anchor = "top-right",
			x = 20,
			y = 20,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				padding = 12,
				hl.plugin.hyprlui.Text({
					id = "title",
					text = "task 4: shared rectangle base",
					size = 14,
					color = 0xffcba6f7,
				}),
				hl.plugin.hyprlui.Row({
					id = "images",
					gap = 12,
					-- real image - semi-transparent fill drawn behind it
					hl.plugin.hyprlui.Image({
						id = "img_ok",
						path = ICON_PATH,
						w = 48,
						h = 48,
						color = 0x8089b4fa,
						rounding = 6,
						borderColor = 0xffcba6f7,
						borderWidth = 2,
					}),
					-- bad path - falls back to fill+border, not nothing
					hl.plugin.hyprlui.Image({
						id = "img_bad",
						path = "/does/not/exist.png",
						w = 48,
						h = 48,
						color = 0xfff38ba8,
						rounding = 6,
						borderColor = 0xffcba6f7,
						borderWidth = 2,
					}),
				}),
				hl.plugin.hyprlui.Box({
					id = "hover_box",
					w = 180,
					h = 32,
					color = 0xff313244,
					hoverColor = 0xff89b4fa,
					rounding = 6,
					onClick = function()
						hl.notification.create({ text = "hover_box clicked", timeout = 1500 })
					end,
					hl.plugin.hyprlui.Text({ x = 10, y = 8, text = "hover me (plain Box)", size = 12 }),
				}),
			}),
		})
	end)
	if not ok then
		warn("hyprlui.window (task4)", err)
		return
	end
	task4Open = true
end

--------------------------------------------------
---- Task 5: hotReload window attribute ----
--------------------------------------------------
-- Created unconditionally below (required for hotReload to do anything,
-- see docs/api.md), not toggle-style like the others - explicit
-- ALT+SHIFT+5/6 hide/show since there's no get_canvas_visible() to read
-- state back after a reload. Verify: hide, reload, should come back
-- hidden (not visible); same the other way with show.
local TASK5_WINDOW = "hyprlui_task5_demo"

if hl.plugin.hyprlui ~= nil then
	local ok5, err5 = pcall(function()
		hl.plugin.hyprlui.window({
			name = TASK5_WINDOW,
			hotReload = true,
			anchor = "bottom-left",
			x = 20,
			y = 20,
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 4,
				padding = 12,
				hl.plugin.hyprlui.Text({ id = "title", text = "task 5: hotReload", size = 14, color = 0xffcba6f7 }),
				hl.plugin.hyprlui.Text({
					id = "hint",
					text = "ALT+SHIFT+5 hides, ALT+SHIFT+6 shows - then reload, state should survive",
					size = 11,
					color = 0xff6c7086,
				}),
			}),
		})
	end)
	if not ok5 then
		warn("hyprlui.window (task5)", err5)
	end
end

--------------------------------------------------
---- Task 6: monitor-span sizing ----
--------------------------------------------------
-- A full-width, exclusive top bar - spanWidth stretches it to the
-- monitor's own width (minus monitorPadding on each side) instead of a
-- manually-queried width, replacing the workaround which-key.lua used to
-- need (see its own history). Root is a Stack (fill=true picks up the
-- spanned width) with the Box and Text as SIBLINGS, not nested - Box
-- never renders children (only Button/Input do), see docs/api.md.

local TASK6_WINDOW = "hyprlui_task6_demo"
local task6Open = false

local function toggleTask6()
	if task6Open then
		hl.plugin.hyprlui.remove_canvas(TASK6_WINDOW)
		task6Open = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = TASK6_WINDOW,
			anchor = "top",
			exclusive = "top",
			spanWidth = true,
			monitorPadding = { left = 20, right = 20 },
			hl.plugin.hyprlui.Stack({
				id = "root",
				h = 32,
				fill = true,
				hl.plugin.hyprlui.Box({ id = "bg", fill = true, color = 0xff313244 }),
				hl.plugin.hyprlui.Text({
					x = 10,
					y = 8,
					text = "task 6: spanWidth + exclusive",
					size = 12,
					color = 0xffcba6f7,
				}),
			}),
		})
	end)
	if not ok then
		warn("hyprlui.window (task6)", err)
		return
	end
	task6Open = true
end

--------------------------------------------------
---- Task 8: text overflow modes ----
--------------------------------------------------
-- Same long string, same maxW, side by side: default ellipsis truncation
-- vs the new overflow = "clip" hard-clip (no "...", glyphs just cut off).

local TASK8_WINDOW = "hyprlui_task8_demo"
local task8Open = false

local function toggleTask8()
	if task8Open then
		hl.plugin.hyprlui.remove_canvas(TASK8_WINDOW)
		task8Open = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = TASK8_WINDOW,
			anchor = "center",
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				padding = 12,
				hl.plugin.hyprlui.Text({
					id = "title",
					text = "task 8: text overflow modes",
					size = 14,
					color = 0xffcba6f7,
				}),
				hl.plugin.hyprlui.Text({
					id = "ellipsis",
					text = "default (ellipsis): a long string that overflows maxW",
					maxW = 160,
					size = 12,
					color = 0xff89b4fa,
				}),
				hl.plugin.hyprlui.Text({
					id = "clip",
					text = "overflow = clip: a long string that overflows maxW",
					maxW = 160,
					overflow = "clip",
					size = 12,
					color = 0xffa6e3a1,
				}),
			}),
		})
	end)
	if not ok then
		warn("hyprlui.window (task8)", err)
		return
	end
	task8Open = true
end

--------------------------------------------------
---- Task 16: marquee text ----
--------------------------------------------------
-- Four copies of the same long string: static clip, marquee = true
-- (linear), and marquee with a curve - one bezier, one spring, both
-- custom-defined below via hl.curve() (Hyprland's own Lua API), not
-- Hyprland's built-in "default". Registered once at module load, before
-- toggleTask16() ever builds a Text referencing them by name.
hl.curve("hyprlui_task16_bezier", { type = "bezier", points = { { 0.65, 0 }, { 0.35, 1 } } })
-- `dampening`, not `damping` - this Hyprland build's hl.curve() still
-- expects the older field name.
hl.curve("hyprlui_task16_spring", { type = "spring", stiffness = 120, dampening = 14, mass = 1 })

local TASK16_WINDOW = "hyprlui_task16_demo"
local task16Open = false

local function toggleTask16()
	if task16Open then
		hl.plugin.hyprlui.remove_canvas(TASK16_WINDOW)
		task16Open = false
		return
	end

	local ok, err = pcall(function()
		hl.plugin.hyprlui.window({
			name = TASK16_WINDOW,
			anchor = "center",
			hl.plugin.hyprlui.Column({
				id = "root",
				gap = 8,
				padding = 12,
				hl.plugin.hyprlui.Text({
					id = "title",
					text = "task 16: marquee text",
					size = 14,
					color = 0xffcba6f7,
				}),
				hl.plugin.hyprlui.Text({
					id = "static",
					text = "static clip (no marquee) - a long string that overflows maxW",
					maxW = 180,
					overflow = "clip",
					size = 12,
					color = 0xff89b4fa,
				}),
				hl.plugin.hyprlui.Text({
					id = "marquee",
					text = "marquee = true - a long string that overflows maxW and scrolls",
					maxW = 180,
					marquee = true,
					size = 12,
					color = 0xffa6e3a1,
				}),
				hl.plugin.hyprlui.Text({
					id = "marquee_bezier",
					text = "marquee = { bezier = '...' } - eased with a custom bezier",
					maxW = 180,
					marquee = { bezier = "hyprlui_task16_bezier" },
					size = 12,
					color = 0xfffab387,
				}),
				hl.plugin.hyprlui.Text({
					id = "marquee_spring",
					text = "marquee = { spring = '...' } - eased with a custom spring",
					maxW = 180,
					marquee = { spring = "hyprlui_task16_spring" },
					size = 12,
					color = 0xfff38ba8,
				}),
			}),
		})
	end)
	if not ok then
		warn("hyprlui.window (task16)", err)
		return
	end
	task16Open = true
end

function M.test_binds()
	hl.bind("ALT + SHIFT + 1", toggleTask1, { description = "task-checks: toggle task 1 demo (window naming)" })
	hl.bind("ALT + SHIFT + 2", toggleTask2, { description = "task-checks: toggle task 2 demo (Box sizing)" })
	hl.bind("ALT + SHIFT + 4", toggleTask4, { description = "task-checks: toggle task 4 demo (shared rect base)" })
	hl.bind("ALT + SHIFT + 5", function()
		hl.plugin.hyprlui.set_canvas_visible(TASK5_WINDOW, false)
	end, { description = "task-checks: hide task 5 demo (hotReload)" })
	hl.bind("ALT + SHIFT + 6", function()
		hl.plugin.hyprlui.set_canvas_visible(TASK5_WINDOW, true)
	end, { description = "task-checks: show task 5 demo (hotReload)" })
	hl.bind("ALT + SHIFT + 3", toggleTask6, { description = "task-checks: toggle task 6 demo (monitor-span sizing)" })
	hl.bind("ALT + SHIFT + 7", toggleTask8, { description = "task-checks: toggle task 8 demo (text overflow modes)" })
	hl.bind("ALT + SHIFT + 8", toggleTask16, { description = "task-checks: toggle task 16 demo (marquee text)" })
end

return M
