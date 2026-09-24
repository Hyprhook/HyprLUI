#pragma once
//
// gfx.hpp / gfx.cpp
//
// Every direct call into Hyprland's internal rendering singletons lives
// behind this thin wrapper. Hyprland's internal headers (unlike the stable
// HyprlandAPI:: surface) aren't guaranteed stable across releases, so
// keeping every such call here means a build break against a newer/older
// Hyprland should only ever need fixing in this one file - the rest of the
// library only talks to this header.

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/Texture.hpp>
//#include <hyprland/src/desktop/Window.hpp> // pulls in PHLMONITOR-adjacent types

#include <optional>
#include <string>

// Forward-declared, not `#include`d, on purpose - only a `const&` of this
// type ever crosses this header's boundary (see drawBorder() below), and
// pulling in ComplexDataTypes.hpp here would push it onto every TU that
// includes gfx.hpp, not just the ones that actually construct one (gfx.cpp
// itself, and wherever a border color/gradient gets parsed).
namespace Config {
    class CGradientValueData;
}

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

    // The height ONE line of text at this (fontFamily, pointSize) should
    // occupy for LAYOUT purposes - unlike a real rendered texture's own
    // height (see makeTextTexture() above), this is the SAME value for
    // every string at that font/size regardless of which characters it
    // contains. Needed because Hyprland's text renderer sizes each
    // texture to its tight ink extent, which varies per string (a
    // descender like "p"/"g"/"y" measures taller) even at identical
    // font/size - using per-string height directly causes visible
    // sub-pixel row drift between independently-stacked columns.
    //
    // Computed by rendering a fixed reference string ("Ag" - a full
    // ascender plus a descender) through the same renderText() pipeline
    // every real text texture goes through, then keeping only its
    // height. Cached per (fontFamily, pointSize) pair.
    double naturalLineHeight(const std::string& fontFamily, int pointSize);

    // Decodes an image file (PNG/JPG/WEBP/SVG/AVIF/JXL - whatever the
    // installed `libhyprgraphics` supports) into a GPU texture,
    // synchronously - no loading-in-progress state to manage. Returns
    // nullptr on a missing file or decode failure - check before use.
    // Cache the result on the node.
    SP<HyprTexture> makeImageTexture(const std::string& path);

    // Blits a texture at `box` (screen-space, pixels) with the given alpha
    // and optional corner rounding. `clipBox`, if given (same screen-space
    // coordinates as `box`), hard-clips the drawn pixels to that rect
    // instead of `box`'s own extent - e.g. text's hard-clip overflow mode,
    // which draws the full un-truncated texture but only lets `clipBox`
    // actually show.
    void drawTexture(const SP<HyprTexture>& tex, const CBox& box, float alpha = 1.F, int rounding = 0, std::optional<CBox> clipBox = std::nullopt);

    // Draws a flat-filled rectangle, e.g. as a panel background.
    void drawRect(const CBox& box, const CHyprColor& color, int rounding = 0);

    // Draws a stroke (not a fill) around `box`'s edge, via Hyprland's own
    // border pass element/shader - a true hollow ring even with a fully
    // transparent fill, unlike a synthesized "two nested rects"
    // approximation. CSS border-box convention: the ring is drawn INSET
    // into `box` by `borderWidth` (not grown outward the way Hyprland's
    // own window borders are), so `box`'s dimensions never change.
    // `rounding` should match the paired drawRect()/drawTexture() call
    // for this box, so the ring's corner radius lines up with the fill's.
    // No-op if borderWidth <= 0.
    void drawBorder(const CBox& box, const Config::CGradientValueData& grad, int borderWidth, int rounding = 0);

    // Returns a copy of `grad` with every color's alpha multiplied by
    // `opacity`, so a fading-out widget's border fades with its fill
    // instead of staying solid.
    Config::CGradientValueData fadeGradient(const Config::CGradientValueData& grad, float opacity);

    // Marks a screen-space region dirty so Hyprland schedules a repaint
    // covering it. Call after moving/mutating/showing/hiding UI.
    void damageBox(const CBox& box);

} // namespace HyprLUI::gfx
