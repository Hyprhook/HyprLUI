#pragma once
//
// LuaBridge.hpp  (not wired up yet - this is a placeholder/sketch)
//
// Once Hyprland's Lua config support is in place, this is where the Lua <->
// C++ glue will live. The idea is that every function here maps ~1:1 onto
// a CUIManager call, so the binding layer stays dumb:
//
//   hyprlui.create_canvas(name, x, y, w, h[, zorder])
//   hyprlui.remove_canvas(name)
//   hyprlui.add_text(canvas, id, x, y, text[, size, r, g, b, a])
//   hyprlui.set_text(canvas, id, text)
//   hyprlui.add_rect(canvas, id, x, y, w, h, r, g, b, a[, rounding])
//   hyprlui.remove_node(canvas, id)
//
// Whichever Lua embedding Hyprland ends up using (its own engine, sol2,
// LuaJIT's C API directly, etc.), these free functions are the intended
// call targets - keep UIManager as the only thing they touch so the Lua
// layer never needs to know about Canvas/Node internals.

#include "UIManager.hpp"

namespace HyprLUI::Lua {

    // TODO: register these with Hyprland's Lua state once that API lands.
    // Sketching the intended signatures now so the C++ side stays stable:

    // void createCanvas(const std::string& name, double x, double y, double w, double h, int zorder);
    // void removeCanvas(const std::string& name);
    // void addText(const std::string& canvas, const std::string& id, double x, double y, const std::string& text, int size, double r, double g, double b, double a);
    // void setText(const std::string& canvas, const std::string& id, const std::string& text);
    // void addRect(const std::string& canvas, const std::string& id, double x, double y, double w, double h, double r, double g, double b, double a, int rounding);
    // void removeNode(const std::string& canvas, const std::string& id);

} // namespace HyprLUI::Lua
