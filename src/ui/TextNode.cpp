#include "TextNode.hpp"

#include <algorithm>

namespace HyprLUI {

    CTextNode::CTextNode(std::string id, const Vector2D& position, std::string text, int pointSize, CHyprColor color, std::string fontFamily, ETextOverflow overflow,
                         std::optional<SMarqueeSpec> marquee) :
        CWidget(std::move(id), position), m_text(std::move(text)), m_pointSize(pointSize), m_color(color), m_fontFamily(std::move(fontFamily)), m_overflow(overflow),
        m_marquee(marquee) {}

    void CTextNode::rebuildTexture() {
        // Clip/marquee need the texture's true, un-truncated width
        // (maxWidth = 0) - render() hard-clips/scrolls it instead.
        // Ellipsis just forwards maxW into Hyprland's own truncate-with-
        // ellipsis text renderer.
        const bool needsFullTexture = (m_overflow == ETextOverflow::Clip || m_marquee.has_value()) && m_maxW.has_value();
        const int  maxWidth         = (!needsFullTexture && m_maxW) ? static_cast<int>(*m_maxW) : 0;
        m_texture                   = gfx::makeTextTexture(m_text, m_color, m_pointSize, m_fontFamily, maxWidth);

        if (m_texture) {
            // Layout width clamps to maxW in clip/marquee mode (CSS
            // overflow:hidden box model). Height uses naturalLineHeight()
            // (not the texture's own) so layout doesn't jitter per-string
            // on descenders.
            m_size.x = needsFullTexture ? std::min(m_texture->m_size.x, *m_maxW) : m_texture->m_size.x;
            m_size.y = gfx::naturalLineHeight(m_fontFamily, m_pointSize);
        }

        m_marqueeActive = m_marquee.has_value() && m_maxW.has_value() && m_texture && m_texture->m_size.x > *m_maxW;
        if (!m_marqueeActive) {
            m_marqueePhase           = EMarqueePhase::Paused;
            m_marqueeOffset          = 0.0;
            m_marqueeScrollElapsedMs = 0.0;
            m_marqueeSpringPos       = 0.0;
            m_marqueeSpringVel       = 0.0;
            m_marqueeTicking         = false;
        }

        m_dirty = false;
    }

    void CTextNode::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible) {
            m_marqueeTicking = false; // next visible frame starts its dt fresh, not a huge stale jump
            return;
        }

        if (m_dirty)
            rebuildTexture();

        if (!m_texture)
            return;

        // Draw at the texture's own native size, not the (possibly
        // min-width-widened) layout box - avoids stretching the glyphs.
        const float    opacity = composedOpacity(parentOpacity);
        const Vector2D pos     = origin + (m_position + styleOffset()) * scale;

        if (m_marqueeActive) {
            renderMarquee(pos, scale, opacity);
            return;
        }

        // No-op (nullopt) whenever the texture already fits within maxW.
        std::optional<CBox> clipBox;
        if (m_overflow == ETextOverflow::Clip && m_maxW && m_texture->m_size.x > *m_maxW)
            clipBox = CBox{pos, Vector2D{*m_maxW, m_texture->m_size.y} * scale};

        gfx::drawTexture(m_texture, {pos, m_texture->m_size * scale}, opacity, 0, clipBox);
    }

    void CTextNode::renderMarquee(const Vector2D& pos, const Vector2D& scale, float opacity) {
        // Advance by real elapsed time, not frame count.
        const auto now = std::chrono::steady_clock::now();
        if (!m_marqueeTicking) {
            m_marqueeLastTick = now;
            m_marqueeTicking  = true;
        }
        const double dtMs = std::chrono::duration<double, std::milli>(now - m_marqueeLastTick).count();
        m_marqueeLastTick = now;

        // Fixed gap between the looping copies, scaled with the font.
        const double gap       = m_pointSize * 2.0;
        const double loopWidth = m_texture->m_size.x + gap;

        // "spring:" is resolveCurveField()'s own encoding for a spring
        // name - split back out since a spring is integrated
        // (position/velocity), not a pure function of progress like a
        // bezier.
        const bool        isSpring  = m_marquee->curve && m_marquee->curve->starts_with("spring:");
        const std::string curveName = m_marquee->curve ? (isSpring ? m_marquee->curve->substr(7) : *m_marquee->curve) : std::string{};

        if (m_marqueePhase == EMarqueePhase::Paused) {
            m_marqueePauseElapsedMs += dtMs;
            if (m_marqueePauseElapsedMs >= m_marquee->pauseMs) {
                m_marqueePauseElapsedMs = 0.0;
                m_marqueePhase          = EMarqueePhase::Scrolling;
            }
        } else if (isSpring) {
            // Semi-implicit Euler integration of a damped harmonic
            // oscillator, using the registered curve's own parameters.
            const auto spring = Animation::mgr()->getSpring(curveName);
            if (!spring) {
                m_marqueeOffset = 0.0;
                m_marqueePhase  = EMarqueePhase::Paused;
            } else {
                const double dtSec  = std::min(dtMs, 100.0) / 1000.0; // clamp a stale dt (e.g. after being hidden)
                const double target = loopWidth;
                const double accel  = -(spring->stiffness * (m_marqueeSpringPos - target) + spring->damping * m_marqueeSpringVel) / spring->mass;
                m_marqueeSpringVel += accel * dtSec;
                m_marqueeSpringPos += m_marqueeSpringVel * dtSec;
                m_marqueeOffset = m_marqueeSpringPos;

                if (std::abs(m_marqueeSpringPos - target) < spring->valueEpsilon && std::abs(m_marqueeSpringVel) < spring->velocityEpsilon) {
                    m_marqueeOffset    = 0.0;
                    m_marqueeSpringPos = 0.0;
                    m_marqueeSpringVel = 0.0;
                    m_marqueePhase     = EMarqueePhase::Paused;
                }
            }
        } else if (m_marquee->curve) {
            // Bezier: a pure function of normalized progress.
            const auto bezier = Animation::mgr()->getBezier(curveName);

            m_marqueeScrollElapsedMs += dtMs;
            const double durationMs = (loopWidth / std::max(m_marquee->speedPxPerSec, 1.0)) * 1000.0;
            const double t          = std::clamp(m_marqueeScrollElapsedMs / durationMs, 0.0, 1.0);
            const double progress   = bezier ? bezier->getYForPoint(static_cast<float>(t)) : t;
            m_marqueeOffset         = progress * loopWidth;

            if (t >= 1.0) {
                m_marqueeOffset          = 0.0;
                m_marqueeScrollElapsedMs = 0.0;
                m_marqueePhase           = EMarqueePhase::Paused;
            }
        } else {
            // Default: plain constant-velocity motion, no smoothing.
            m_marqueeOffset += m_marquee->speedPxPerSec * (dtMs / 1000.0);
            if (m_marqueeOffset >= loopWidth) {
                m_marqueeOffset = 0.0;
                m_marqueePhase  = EMarqueePhase::Paused;
            }
        }

        const CBox     clipBox{pos, Vector2D{*m_maxW, m_texture->m_size.y} * scale};

        const Vector2D primaryPos = pos - Vector2D{m_marqueeOffset, 0.0} * scale;
        gfx::drawTexture(m_texture, {primaryPos, m_texture->m_size * scale}, opacity, 0, clipBox);

        // Wrap-around copy - only needed once the gap/next-copy would
        // show through the clip window's right edge.
        if (m_marqueeOffset + *m_maxW > m_texture->m_size.x) {
            const Vector2D wrapPos = primaryPos + Vector2D{loopWidth, 0.0} * scale;
            gfx::drawTexture(m_texture, {wrapPos, m_texture->m_size * scale}, opacity, 0, clipBox);
        }
    }

    void CTextNode::setText(const std::string& text) {
        if (text == m_text)
            return;
        m_text = text;
        markDirty();
    }

    void CTextNode::setColor(const CHyprColor& color) {
        m_color = color;
        markDirty();
    }

    void CTextNode::setPointSize(int pointSize) {
        m_pointSize = pointSize;
        markDirty();
    }

} // namespace HyprLUI
