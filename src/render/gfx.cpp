#include "gfx.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

#include <hyprgraphics/image/Image.hpp>
#include <hyprgraphics/cairo/CairoSurface.hpp>

#include <unordered_map>

namespace HyprLUI::gfx {

    PHLMONITOR currentMonitor() {
        // Only meaningful while a render pass for a given output is active
        // - i.e. inside our "render" hook. pMonitor is a weak ref
        // (PHLMONITORREF); lock() it to get a strong PHLMONITOR, which may
        // be null if the monitor died mid-frame.
        return g_pHyprRenderer->renderData().pMonitor.lock();
    }

    SP<HyprTexture> makeTextTexture(const std::string& text, const CHyprColor& color, int pointSize, const std::string& fontFamily, int maxWidth, int weight) {
        // Text rasterization lives on IHyprRenderer (g_pHyprRenderer), not
        // CHyprOpenGLImpl (g_pHyprOpenGL). If this stops compiling, grep
        // your installed src/render/Renderer.hpp for `renderText` to see
        // where it lives now.
        return g_pHyprRenderer->renderText(text, color, pointSize, /* italic = */ false, fontFamily, maxWidth, weight);
    }

    double naturalLineHeight(const std::string& fontFamily, int pointSize) {
        // Keyed by the exact (family, size) pair - a real render only ever
        // happens once per distinct combo, see this function's own doc
        // comment (gfx.hpp) for why.
        static std::unordered_map<std::string, double> s_cache;

        const std::string                              key = fontFamily + ":" + std::to_string(pointSize);
        if (const auto it = s_cache.find(key); it != s_cache.end())
            return it->second;

        // Color/maxWidth/weight are irrelevant here - only the resulting
        // TEXTURE HEIGHT is ever read, the texture itself is discarded
        // immediately. Falls back to a plain multiplier if rendering
        // somehow fails (e.g. a bogus font name) - better than crashing
        // or returning 0 and collapsing every line to no height at all.
        const auto   tex    = makeTextTexture("Ag", CHyprColor{1.0, 1.0, 1.0, 1.0}, pointSize, fontFamily);
        const double height = tex ? tex->m_size.y : pointSize * 1.3;

        s_cache[key] = height;
        return height;
    }

    SP<HyprTexture> makeImageTexture(const std::string& path) {
        Hyprgraphics::CImage image(path);
        if (!image.success())
            return nullptr;

        auto surface = image.cairoSurface();
        if (!surface)
            return nullptr;

        // `createTexture(cairo_surface_t*)` is the SAME IHyprRenderer
        // interface makeTextTexture() above already uses (Renderer.hpp) -
        // no new global/include needed, despite the decode step itself
        // (Hyprgraphics::CImage) living in a separate library.
        return g_pHyprRenderer->createTexture(surface->cairo());
    }

    namespace {
        // HyprLUI's public API (Canvas position/size, node positions) is in
        // global compositor layout coordinates, logical pixels - the same
        // space g_pHyprRenderer->damageBox() already expects (it does this
        // exact translate+scale internally). But render.stage fires already
        // scoped to one monitor, and both the raw GL calls and the pass
        // elements below expect boxes in THAT monitor's local,
        // scale-adjusted framebuffer-pixel space instead - see how
        // Hyprland's own window rendering (and Hyprspace's widget) prepare
        // boxes before drawing: translate(-pMonitor->m_position) then
        // scale(pMonitor->m_scale).
        CBox toMonitorLocal(const CBox& globalBox) {
            const auto mon = currentMonitor();
            if (!mon)
                return globalBox;

            return globalBox.copy().translate(-mon->m_position).scale(mon->m_scale);
        }
    } // namespace

    void drawTexture(const SP<HyprTexture>& tex, const CBox& box, float alpha, int rounding) {
        if (!tex)
            return;

        // Draw via a pass element queued onto the current frame's render
        // pass (g_pHyprRenderer->m_renderPass), NOT a direct
        // g_pHyprOpenGL->renderTexture() call. Hyprland's own pipeline
        // (e.g. its DPMS black-screen overlay) and third-party overlay
        // plugins (Hyprspace) both draw this way - direct immediate GL
        // calls during RENDER_LAST_MOMENT get issued before the pass
        // system's own end-of-frame compositing step and never make it
        // into the presented buffer.
        CTexPassElement::SRenderData data;
        data.tex           = tex;
        data.box           = toMonitorLocal(box);
        data.a             = alpha;
        data.round         = rounding;
        data.roundingPower = 2.F;

        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }

    void drawRect(const CBox& box, const CHyprColor& color, int rounding) {
        // Same reasoning as drawTexture() above - queue a pass element
        // instead of calling g_pHyprOpenGL->renderRect() directly.
        CRectPassElement::SRectData data;
        data.box           = toMonitorLocal(box);
        data.color         = color;
        data.round         = rounding;
        data.roundingPower = 2.F;

        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(data));
    }

    void drawBorder(const CBox& box, const Config::CGradientValueData& grad, int borderWidth, int rounding) {
        if (borderWidth <= 0)
            return;

        CBorderPassElement::SBorderData data;
        // Shrink by borderWidth BEFORE scaling into monitor-local space
        // (toMonitorLocal), then let CBorderPassElement's own renderer
        // (OpenGL.cpp's renderBorder()) expand it back outward by the
        // same (now-scaled) amount when it draws - net effect, the ring
        // lands flush with `box`'s original, unshrunk outer edge and
        // grows inward, giving the CSS border-box behavior this
        // function's own header doc comment promises, using Hyprland's
        // outward-growing primitive unmodified.
        data.box           = toMonitorLocal(box.copy().expand(-borderWidth));
        data.grad1         = grad;
        data.round         = rounding;
        data.borderSize    = borderWidth;
        data.roundingPower = 2.F;

        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(data));
    }

    Config::CGradientValueData fadeGradient(const Config::CGradientValueData& grad, float opacity) {
        Config::CGradientValueData faded = grad;
        for (auto& c : faded.m_colors)
            c.a *= opacity;
        faded.updateColorsOk();
        return faded;
    }

    void damageBox(const CBox& box) {
        g_pHyprRenderer->damageBox(box);
        g_pHyprRenderer->damageBox(box);
    }

} // namespace HyprLUI::gfx
