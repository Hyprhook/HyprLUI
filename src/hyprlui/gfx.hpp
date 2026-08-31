#pragma once
//
// gfx.hpp / gfx.cpp
//
// Every direct call into Hyprland's internal rendering singletons
// (g_pHyprOpenGL, g_pHyprRenderer, g_pCompositor) lives behind this thin
// wrapper. Hyprland's *internal* headers (as opposed to the stable
// HyprlandAPI:: surface) are not guaranteed stable across releases -
// signatures like renderText()/renderTexture() do shift between versions
// (e.g. renderText moved from CHyprOpenGLImpl to IHyprRenderer, and the
// texture type was renamed CTexture -> ITexture, at some point before 0.55).
//
// Keeping every such call in gfx.cpp means that if HyprLUI fails to build
// against a newer/older Hyprland, this is the only file you should need to
// touch - the rest of the library (Node/Canvas/UIManager) only talks to
// this header and stays untouched.

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/Texture.hpp>
//#include <hyprland/src/desktop/Window.hpp> // pulls in PHLMONITOR-adjacent types

#include <string>

// Hyprland's texture type. As of Hyprland 0.55 it's `ITexture`, declared in
// the top-level `::Render` namespace (src/render/Texture.hpp) - NOT the same
// as HyprLUI's own `RenderHook` namespace. Aliased here so the rest of the
// library never has to spell out `::Render::ITexture` and never risks
// colliding with a HyprLUI-side namespace of a similar name again.
using HyprTexture = ::Render::ITexture;

namespace HyprLUI::gfx {

    // Returns the monitor Hyprland is currently rendering, or nullptr if
    // called outside of a render pass. Only valid inside a Canvas::render()
    // call triggered from the "render" hook.
    PHLMONITOR currentMonitor();

    // Rasterizes `text` into a GPU texture using Hyprland's own text
    // renderer (Cairo/Pango under the hood). Cache the result on the node -
    // this is not cheap enough to call every frame.
    SP<HyprTexture> makeTextTexture(const std::string& text, const CHyprColor& color, int pointSize, const std::string& fontFamily = "sans", int maxWidth = 0,
                                    int weight = 400 /* normal */);

    // Blits a texture at `box` (screen-space, pixels) with the given alpha
    // and optional corner rounding.
    void drawTexture(const SP<HyprTexture>& tex, const CBox& box, float alpha = 1.F, int rounding = 0);

    // Draws a flat-filled rectangle, e.g. as a panel background.
    void drawRect(const CBox& box, const CHyprColor& color, int rounding = 0);

    // Marks a screen-space region dirty so Hyprland schedules a repaint
    // covering it. Call after moving/mutating/showing/hiding UI.
    void damageBox(const CBox& box);

} // namespace HyprLUI::gfx
