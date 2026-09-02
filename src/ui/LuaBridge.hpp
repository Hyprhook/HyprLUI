#pragma once
//
// LuaBridge.hpp
//
// Registers HyprLUI's public API under hl.plugin.hyprlui.* so a Lua config
// can declare a window as a widget tree and mutate it afterwards by id.
//
// Widget constructors (each tags and returns its table argument, so they
// nest via plain Lua table-literal syntax - `Column{ gap = 8, Text{...} }`
// puts the Text{} result at index 1 of the Column table):
//
//   Stack{ id, x = 0, y = 0, w, h, visible, <children...> }
//     Manual/absolute positioning - each child keeps whatever x/y it was
//     given. Size-to-content is the bounding box of its children unless
//     w/h are given.
//
//   Row{ id, x = 0, y = 0, w, h, visible, gap = 0, padding = 0,
//        align = "start"|"center"|"end", <children...> }
//   Column{ ...same fields... }
//     Flexbox-lite: packs children along the row/column axis with `gap`
//     between them and `padding` on all sides; `align` controls cross-axis
//     alignment. No wrap, no justify/space-between (v1 scope).
//
//   Text{ id, x = 0, y = 0, text, size = 16, color, font = "sans", visible }
//   Box{ id, x = 0, y = 0, w, h, color, rounding = 0, visible }
//     Leaves. `color` is either a 0xAARRGGBB integer or a table
//     { r, g, b, a } with components in [0, 1].
//
// Window construction and mutation:
//
//   window{ name, x = 0, y = 0, w, h, zorder = "overlay"|"background",
//           <exactly one root widget> }
//     Opens a new window with the given widget tree as its root. If w/h
//     are omitted the window sizes itself to the root's measured content.
//     Errors if `name` is already in use.
//
//   remove_canvas(name)
//     Closes a window and everything in it.
//
//   set_canvas_visible(name, visible)
//     Shows/hides a window without destroying its content.
//
//   set_text(window, id, text)
//     Updates an existing Text widget's content in place.
//
//   remove_widget(window, id)
//     Removes a single widget (and its subtree) from a window.
//
// Example, from hyprland.lua:
//
//   local hyprlui = hl.plugin.hyprlui
//   hyprlui.window{
//       name = "greeting", x = 100, y = 100,
//       hyprlui.Column{ id = "root", gap = 8, padding = 16,
//           hyprlui.Box{ id = "bg", w = 300, h = 80, color = 0xcc111111, rounding = 8 },
//           hyprlui.Text{ id = "label", text = "Hello!", size = 20 },
//       },
//   }
//   hl.bind("SUPER + G", function() hyprlui.remove_canvas("greeting") end)
//
// Note the Box above is behind/outside the visible flow in this example
// only to illustrate nesting - a background panel that should sit *behind*
// its siblings belongs in an outer Stack with the Column absolutely
// positioned on top of it, since Row/Column packs every child into the
// flow. See DESIGN.md for the layout model this is built on.

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HyprLUI::Lua {

    // Registers every hl.plugin.hyprlui.* function. Call once from PLUGIN_INIT.
    void registerFunctions(HANDLE handle);

    // Unregisters everything registered above. Call from PLUGIN_EXIT.
    void unregisterFunctions(HANDLE handle);

} // namespace HyprLUI::Lua
