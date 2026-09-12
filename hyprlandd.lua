-- NOTE on reload: every config reload (including one triggered just by
-- fixing an unrelated Lua error) now closes every open HyprLUI window/
-- watcher/exclusive zone outright - the plugin clears its own state on
-- config.preReload, since a reload re-runs this whole script (resetting
-- any *ToggleWindowOpen-style locals in demos/*.lua back to false)
-- without unloading the plugin itself. So after a reload, toggle binds
-- correctly start from "closed" again and need pressing again to reopen -
-- see DESIGN.md's "General plugin-lifecycle bug" note for why this
-- replaced the old (buggy) behavior of windows silently surviving a
-- reload as unreachable orphans.

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

-- Demos/one-off feature tests don't live in this file - drop a self-
-- contained .lua file under demos/ (created on demand, not checked in
-- empty) and require("./demos/whatever") it in here to load it. Hyprland's
-- own Lua loader resolves that path relative to THIS file's directory
-- regardless of which file does the requiring (ConfigManager.cpp), and
-- re-require()s it on every config reload, so editing a demo file and
-- saving takes effect live.
local which_key = require("./demos/which-key")
which_key.setup(opts)
which_key.test_binds()
