#pragma once
//
// TextNode.hpp
//
// Renders a single line/block of text. The rasterized texture is cached and
// only regenerated when the text, size, color, or font actually change -
// rasterizing via Cairo/Pango every frame would be needlessly expensive.

#include "Widget.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/helpers/Color.hpp>

#include <chrono>
#include <optional>

namespace HyprLUI {

    // Ellipsis (default): Hyprland's own text renderer truncates-with-
    // ellipsis for free when given a maxWidth. Clip: draws the full,
    // un-truncated glyphs but hard-clips the drawn pixels to maxWidth via
    // gfx::drawTexture()'s clipBox - no ellipsis character, no reflow.
    // No-op either way if maxW (a base CWidget field) isn't set - nothing
    // to overflow against.
    enum class ETextOverflow {
        Ellipsis,
        Clip,
    };

    // Opt-in continuous scrolling loop for text overflowing maxW (implies
    // hard-clip on its own). `curve` is resolveCurveField()'s own return
    // value verbatim (a bare bezier name, or "spring:" + a spring name) -
    // nullopt means plain constant-velocity motion.
    struct SMarqueeSpec {
        double                     pauseMs       = 1200.0;
        double                     speedPxPerSec = 40.0;
        std::optional<std::string> curve;
    };

    class CTextNode : public CWidget {
      public:
        CTextNode(std::string id, const Vector2D& position, std::string text, int pointSize = 16, CHyprColor color = CHyprColor{1.0, 1.0, 1.0, 1.0},
                  std::string fontFamily = "sans", ETextOverflow overflow = ETextOverflow::Ellipsis, std::optional<SMarqueeSpec> marquee = std::nullopt);

        void               render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) override;

        void               setText(const std::string& text);
        const std::string& text() const {
            return m_text;
        }

        void setColor(const CHyprColor& color);
        void setPointSize(int pointSize);

        // True while a marquee is actively mid-scroll (not during its
        // start-of-loop pause) - lets CCanvas::render() keep damaging
        // every frame for as long as glyphs are actually moving, same
        // reasoning as the base class's own visibility-fade tracking.
        bool isAnimating() const override {
            return CWidget::isAnimating() || (m_marqueeActive && m_marqueePhase == EMarqueePhase::Scrolling);
        }

      protected:
        // Text's "natural size" comes from rasterizing it - measure() (run
        // once per frame before arrange()/render()) is what triggers the
        // rebuild now, so layout always sees an up-to-date size. render()
        // keeps its own dirty check too as a safety net.
        void measureContent() override {
            if (m_dirty)
                rebuildTexture();
        }

      private:
        enum class EMarqueePhase {
            Paused,
            Scrolling,
        };

        void markDirty() {
            m_dirty = true;
        }
        void                                  rebuildTexture();
        void                                  renderMarquee(const Vector2D& pos, const Vector2D& scale, float opacity);

        std::string                           m_text;
        int                                   m_pointSize;
        CHyprColor                            m_color;
        std::string                           m_fontFamily;
        ETextOverflow                         m_overflow;

        SP<HyprTexture>                       m_texture;
        bool                                  m_dirty = true;

        std::optional<SMarqueeSpec>           m_marquee;
        bool                                  m_marqueeActive          = false; // text actually wider than maxW
        EMarqueePhase                         m_marqueePhase           = EMarqueePhase::Paused;
        double                                m_marqueeOffset          = 0.0; // px scrolled into the current loop
        double                                m_marqueePauseElapsedMs  = 0.0;
        double                                m_marqueeScrollElapsedMs = 0.0; // linear/bezier
        double                                m_marqueeSpringPos       = 0.0; // spring
        double                                m_marqueeSpringVel       = 0.0;
        bool                                  m_marqueeTicking         = false;
        std::chrono::steady_clock::time_point m_marqueeLastTick;
    };

} // namespace HyprLUI
