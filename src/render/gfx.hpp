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
    // every string at that font/size, regardless of which characters it
    // actually contains. Needed because Hyprland's own text renderer
    // (IHyprRenderer::renderText(), Renderer.cpp) sizes each texture as
    // `max(logical extent, ink extent)` - the ink extent is a TIGHT
    // bounding box of the actually-painted pixels, which varies per
    // string (a descender like "p"/"g"/"y" measures taller than a string
    // without one), even at the identical font/size. Found live: a
    // demo's two side-by-side, independently-stacked Columns (one of
    // keys, one of descriptions) drifted out of row-alignment by a
    // fraction of a pixel per row, becoming visible after enough rows,
    // purely because sibling rows across the two columns happened to
    // have different ink-extent heights.
    //
    // Computed by rendering a small FIXED reference string ("Ag" - a
    // capital letter for a full ascender plus a lowercase descender, a
    // common typographic reference pair) through the exact same
    // renderText() pipeline every real text texture goes through, so the
    // result reflects THIS font's actual metrics rather than a guessed
    // multiplier - then discarding that texture and keeping only its
    // height. Cached per (fontFamily, pointSize) pair - a real texture
    // render happens ONCE per distinct combo ever used (typically a
    // handful in any real config), never per widget instance or per
    // frame. See CTextNode::rebuildTexture() for the one caller.
    double naturalLineHeight(const std::string& fontFamily, int pointSize);

    // Decodes an image file (PNG/JPG/WEBP/SVG/AVIF/JXL - whatever the
    // installed `libhyprgraphics` supports) into a GPU texture, via
    // `Hyprgraphics::CImage` (`<hyprgraphics/image/Image.hpp>` - a
    // separate library from Hyprland core, not the stable HyprlandAPI::
    // surface, same stability tier as renderText() above) - synchronous,
    // no loading-in-progress state to manage. Returns nullptr on a
    // missing file or decode failure - check before use, same convention
    // as makeTextTexture(). Cache the result on the node, same reasoning.
    SP<HyprTexture> makeImageTexture(const std::string& path);

    // Blits a texture at `box` (screen-space, pixels) with the given alpha
    // and optional corner rounding.
    void drawTexture(const SP<HyprTexture>& tex, const CBox& box, float alpha = 1.F, int rounding = 0);

    // Draws a flat-filled rectangle, e.g. as a panel background.
    void drawRect(const CBox& box, const CHyprColor& color, int rounding = 0);

    // Draws a stroke (not a fill) around `box`'s edge - via Hyprland's own
    // border pass element/shader (CBorderPassElement, SH_FRAG_BORDER1),
    // the exact same rounded-rect-outline primitive `general:col.
    // active_border` renders through - NOT a synthesized "two nested
    // rects" approximation, so a fully transparent fill with just a
    // border renders as a true hollow ring rather than a solid
    // border-colored square (two rects can't punch a hole - see Phase 19,
    // DESIGN.md).
    //
    // CSS border-box convention, deliberately NOT Hyprland's own window-
    // border convention: the ring is drawn INSET into `box` by
    // `borderWidth` on every side (flush with box's outer edge, growing
    // inward), so `box`'s own dimensions never change - same "the box you
    // pass is the box you get" contract padding/margin already have.
    // Hyprland's window borders instead grow OUTWARD past the content
    // box; replicating that here would mean every caller of Box/Button/
    // etc. would have to know to shrink their own layout math by
    // borderWidth to compensate, which nothing else in this codebase asks
    // of a widget's dimensions.
    //
    // `rounding` should be the SAME value passed to the paired
    // drawRect()/drawTexture() call for this box - the ring's corner
    // radius has to match the fill's for the two to look like one
    // continuous rounded shape rather than a fill peeking out past a
    // squarer (or rounder) border.
    //
    // No-op if borderWidth <= 0.
    void drawBorder(const CBox& box, const Config::CGradientValueData& grad, int borderWidth, int rounding = 0);

    // Returns a copy of `grad` with every color's alpha multiplied by
    // `opacity` - the border/gradient equivalent of the `faded.a *=
    // composedOpacity(...)` every widget's render() already does inline
    // for its plain CHyprColor fill before calling drawRect(). Needed so
    // a fading-out widget's border fades with it instead of staying
    // solid while the fill underneath disappears.
    Config::CGradientValueData fadeGradient(const Config::CGradientValueData& grad, float opacity);

    // Marks a screen-space region dirty so Hyprland schedules a repaint
    // covering it. Call after moving/mutating/showing/hiding UI.
    void damageBox(const CBox& box);

} // namespace HyprLUI::gfx
