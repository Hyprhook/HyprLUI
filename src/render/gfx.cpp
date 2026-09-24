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
#include <vector>
#include <array>
#include <cstdint>

namespace HyprLUI::gfx {

    namespace {
        // Standard table-lookup base64 decoder - skips whitespace/padding
        // and any other out-of-alphabet byte rather than erroring, since a
        // malformed buffer should fall through to makeImageTexture()'s own
        // size check (garbage in, nullptr out) instead of crashing here.
        std::vector<uint8_t> base64Decode(const std::string& in) {
            static constexpr auto DECODE_TABLE = []() {
                std::array<int8_t, 256> t{};
                t.fill(-1);
                const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                for (int i = 0; i < 64; ++i)
                    t[static_cast<uint8_t>(alphabet[i])] = static_cast<int8_t>(i);
                return t;
            }();

            std::vector<uint8_t> out;
            out.reserve(in.size() / 4 * 3);

            int val = 0, bits = -8;
            for (unsigned char c : in) {
                if (DECODE_TABLE[c] < 0)
                    continue; // padding ('='), whitespace, or garbage - skip
                val = (val << 6) + DECODE_TABLE[c];
                bits += 6;
                if (bits >= 0) {
                    out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
                    bits -= 8;
                }
            }
            return out;
        }
    } // namespace

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

    SP<HyprTexture> makeImageTexture(int width, int height, int rowstride, bool hasAlpha, int channels, const std::string& dataBase64) {
        if (width <= 0 || height <= 0 || channels < 3 || rowstride < width * channels)
            return nullptr;

        const auto raw = base64Decode(dataBase64);
        if (raw.size() < static_cast<size_t>(rowstride) * height)
            return nullptr;

        // IHyprRenderer::createTexture(width, height, data) (the width+
        // height+raw-bytes overload, distinct from the cairo_surface_t*
        // one used just above) uploads as GL_RGBA then swizzles R<->B on
        // sample - net effect, it expects `data` already in BGRA byte
        // order in memory (matching DRM_FORMAT_ARGB8888, what it
        // allocates the texture as), premultiplied, and tightly packed
        // (width*4 bytes/row - no rowstride parameter on that call at
        // all). The spec's own bytes are R,G,B[,A] order, NOT
        // premultiplied, and may have a rowstride padded beyond
        // width*channels - convert row by row, respecting the source
        // rowstride on read and premultiplying alpha on write.
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y) {
            const uint8_t* srcRow = raw.data() + static_cast<size_t>(y) * rowstride;
            uint8_t*       dstRow = bgra.data() + static_cast<size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                const uint8_t* srcPx = srcRow + static_cast<size_t>(x) * channels;
                const uint8_t  r     = srcPx[0];
                const uint8_t  g     = srcPx[1];
                const uint8_t  b     = srcPx[2];
                const uint8_t  a     = (hasAlpha && channels >= 4) ? srcPx[3] : 255;

                uint8_t*       dstPx = dstRow + static_cast<size_t>(x) * 4;
                dstPx[0]             = static_cast<uint8_t>((b * a + 127) / 255);
                dstPx[1]             = static_cast<uint8_t>((g * a + 127) / 255);
                dstPx[2]             = static_cast<uint8_t>((r * a + 127) / 255);
                dstPx[3]             = a;
            }
        }

        return g_pHyprRenderer->createTexture(width, height, bgra.data());
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

    void drawTexture(const SP<HyprTexture>& tex, const CBox& box, float alpha, int rounding, std::optional<CBox> clipBox) {
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
        if (clipBox)
            data.clipBox = toMonitorLocal(*clipBox);

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
