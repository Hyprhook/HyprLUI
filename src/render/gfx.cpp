#include "gfx.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

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

    void damageBox(const CBox& box) {
        g_pHyprRenderer->damageBox(box);
        g_pHyprRenderer->damageBox(box);
    }

} // namespace HyprLUI::gfx
