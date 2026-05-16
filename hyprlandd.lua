hl.bind("SUPER + T", hl.dsp.exec_cmd("kitty"), { description = "Open Terminal" })
hl.bind("ALT + Q", hl.dsp.window.close(), { description = "Close window" })
if hl.plugin.hyprLUI ~= nil then
	hl.notification.create({ text = "'hyprLUI' is init.", duration = 3000 })
else
	hl.notification.create({ text = "nil for 'hyprLUI'.", duration = 3000 })
end
