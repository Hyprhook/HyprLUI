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
        static std::unordered_map<std::string, double> s_cache;

        const std::string                              key = fontFamily + ":" + std::to_string(pointSize);
        if (const auto it = s_cache.find(key); it != s_cache.end())
            return it->second;

        // Falls back to a plain multiplier if rendering fails (e.g. a
        // bogus font name) rather than collapsing every line to 0 height.
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

        return g_pHyprRenderer->createTexture(surface->cairo());
    }

    namespace {
        // HyprLUI's public API is in global compositor layout coordinates;
        // the pass elements below expect boxes in the current monitor's
        // local, scale-adjusted framebuffer-pixel space instead.
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

        // Queued onto the render pass, not a direct GL call - a direct
        // call during RENDER_LAST_MOMENT never makes it into the
        // presented buffer in this pipeline.
        CTexPassElement::SRenderData data;
        data.tex           = tex;
        data.box           = toMonitorLocal(box);
        data.a             = alpha;
        data.round         = rounding;
        data.roundingPower = 2.F;

        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }

    void drawRect(const CBox& box, const CHyprColor& color, int rounding) {
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
        // Shrink by borderWidth first, then let CBorderPassElement's own
        // renderer expand it back outward by the same amount when it
        // draws - net effect, the ring lands flush with `box`'s original
        // outer edge and grows inward (border-box), using Hyprland's own
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
