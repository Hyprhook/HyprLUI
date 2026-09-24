#include "Widget.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    namespace {
        // gfx.hpp has no stroke/outline primitive - four thin filled
        // strips is simple/cheap enough not to need one, since this is a
        // debug-only path no shipped config runs through. Strips slightly
        // overlap at the corners; harmless at this thickness.
        constexpr double STROKE_WIDTH = 1.5;

        // Returns `box` itself - the stroke never extends beyond it
        // (STROKE_WIDTH is drawn just inside the edges, not straddling
        // them). Included so every draw call in drawDebugOverlay() can
        // uniformly feed its result to expandBounds() below, whether it
        // actually expands anything or not.
        CBox strokeRect(const CBox& box, const CHyprColor& color) {
            if (box.size().x <= 0 || box.size().y <= 0)
                return box;
            gfx::drawRect({box.pos(), {box.size().x, STROKE_WIDTH}}, color);
            gfx::drawRect({{box.pos().x, box.pos().y + box.size().y - STROKE_WIDTH}, {box.size().x, STROKE_WIDTH}}, color);
            gfx::drawRect({box.pos(), {STROKE_WIDTH, box.size().y}}, color);
            gfx::drawRect({{box.pos().x + box.size().x - STROKE_WIDTH, box.pos().y}, {STROKE_WIDTH, box.size().y}}, color);
            return box;
        }

        // Unions `b` into `bounds` in place - `bounds` is always assumed
        // meaningful already, `b` may be a zero-size no-op (e.g.
        // drawLabel() on empty text), which this ignores rather than
        // collapsing `bounds` toward (0,0).
        void expandBounds(CBox& bounds, const CBox& b) {
            if (b.size().x <= 0 && b.size().y <= 0)
                return;
            const double minX = std::min(bounds.pos().x, b.pos().x);
            const double minY = std::min(bounds.pos().y, b.pos().y);
            const double maxX = std::max(bounds.pos().x + bounds.size().x, b.pos().x + b.size().x);
            const double maxY = std::max(bounds.pos().y + bounds.size().y, b.pos().y + b.size().y);
            bounds            = CBox{{minX, minY}, {maxX - minX, maxY - minY}};
        }

        // Point size every debug label falls back to when neither this
        // widget nor an ancestor set `debugFontSize`.
        constexpr int DEFAULT_FONT_SIZE = 10;

        // Rasterizes and immediately draws a small text label - not
        // cached, unlike CTextNode's own texture cache. Re-rasterizing a
        // handful of tiny debug labels every frame is an accepted
        // tradeoff for a dev-time-only, opt-in tool.
        CBox drawLabel(const std::string& text, const Vector2D& pos, const CHyprColor& color, int fontSize) {
            if (text.empty())
                return {pos, {0, 0}};
            auto tex = gfx::makeTextTexture(text, color, fontSize, "sans");
            if (!tex)
                return {pos, {0, 0}};
            gfx::drawTexture(tex, {pos, tex->m_size});
            return {pos, tex->m_size};
        }

        // "Auto" (unforced) threshold below which detail labels/insets
        // stay hidden - a widget this small can't fit them legibly
        // anyway. An explicit debugShow.* override bypasses this
        // entirely (see resolveShow() below), on purpose.
        constexpr double AUTO_MIN_W = 96;
        constexpr double AUTO_MIN_H = 16;

        bool             resolveShow(const std::optional<bool>& forced, bool autoDefault) {
            return forced.value_or(autoDefault);
        }

        // "8" if uniform on all four sides, "T8 R4 B8 L4" otherwise - one
        // compact label instead of four separate ones per box.
        std::string formatInsets(const SEdgeInsets& insets) {
            if (insets.top == insets.right && insets.right == insets.bottom && insets.bottom == insets.left)
                return std::to_string(static_cast<int>(insets.top));
            return "T" + std::to_string(static_cast<int>(insets.top)) + " R" + std::to_string(static_cast<int>(insets.right)) + " B" +
                std::to_string(static_cast<int>(insets.bottom)) + " L" + std::to_string(static_cast<int>(insets.left));
        }
    } // namespace

    SDebugSpec CWidget::resolveDebugSpec(const SDebugSpec& inherited) const {
        SDebugSpec r = inherited;
        if (m_debugSpec.enabled)
            r.enabled = m_debugSpec.enabled;
        if (m_debugSpec.showBox)
            r.showBox = m_debugSpec.showBox;
        if (m_debugSpec.showPadding)
            r.showPadding = m_debugSpec.showPadding;
        if (m_debugSpec.showMargin)
            r.showMargin = m_debugSpec.showMargin;
        if (m_debugSpec.showId)
            r.showId = m_debugSpec.showId;
        if (m_debugSpec.showSize)
            r.showSize = m_debugSpec.showSize;
        if (m_debugSpec.showZOpacity)
            r.showZOpacity = m_debugSpec.showZOpacity;
        if (m_debugSpec.showHitTarget)
            r.showHitTarget = m_debugSpec.showHitTarget;
        if (m_debugSpec.fontSize)
            r.fontSize = m_debugSpec.fontSize;
        return r;
    }

    std::optional<CBox> CWidget::renderDebug(const Vector2D& origin, const SDebugSpec& inherited) {
        if (!m_visible)
            return std::nullopt;

        const SDebugSpec    resolved = resolveDebugSpec(inherited);
        std::optional<CBox> bounds;
        if (resolved.enabled.value_or(false))
            bounds = drawDebugOverlay(origin, resolved);

        // Cascade off = wall this subtree off from both my own resolved
        // config AND anything above me - children start completely fresh,
        // not just "without my own overrides" (see setDebugCascade()'s
        // doc comment for why that's the more useful interpretation).
        const SDebugSpec forChildren = m_debugCascade ? resolved : SDebugSpec{};
        for (auto& child : m_children) {
            if (auto childBounds = child->renderDebug(origin + m_position, forChildren)) {
                if (bounds)
                    expandBounds(*bounds, *childBounds);
                else
                    bounds = childBounds;
            }
        }
        return bounds;
    }

    std::optional<CBox> CWidget::renderedBounds(const Vector2D& origin, const Vector2D& scale) const {
        if (!m_visible)
            return std::nullopt;

        CBox           bounds = boxAt(origin, scale);

        const Vector2D basePos     = origin + (m_position + styleOffset() + layoutOffset()) * scale;
        const auto     local       = popinTransform();
        const Vector2D childOrigin = basePos + local.offset * scale;
        const Vector2D childScale  = scale * local.scale;

        for (auto& child : m_children) {
            if (auto childBounds = child->renderedBounds(childOrigin, childScale))
                expandBounds(bounds, *childBounds);
        }
        return bounds;
    }

    CBox CWidget::drawDebugOverlay(const Vector2D& origin, const SDebugSpec& resolved) const {
        const CBox box       = boxAt(origin);
        const bool bigEnough = box.size().x >= AUTO_MIN_W && box.size().y >= AUTO_MIN_H;
        const int  fontSize  = resolved.fontSize.value_or(DEFAULT_FONT_SIZE);
        // Labels drawn just outside/above a box need to clear it by
        // roughly a line's height - scales with fontSize so a bigger
        // debugFontSize doesn't start overlapping the outline it's
        // labeling.
        const double     lineOffset = fontSize + 2;

        const CHyprColor marginColor{1.0F, 0.65F, 0.0F, 0.7F};
        const CHyprColor boxColor{0.25F, 0.6F, 1.0F, 0.8F};
        const CHyprColor paddingColor{0.25F, 0.85F, 0.35F, 0.7F};
        const CHyprColor hitColor{0.85F, 0.2F, 0.85F, 0.18F};
        const CHyprColor idLabelColor{1.0F, 1.0F, 0.3F, 1.0F};
        const CHyprColor sizeLabelColor{0.4F, 0.95F, 1.0F, 1.0F};
        const CHyprColor metaLabelColor{1.0F, 0.75F, 1.0F, 1.0F};

        // Starts from this widget's own plain content box - the overlay
        // always covers AT LEAST that much, even if every show* option
        // below is off - and grows from there via expandBounds() as
        // margin/label boxes (several of which are deliberately drawn
        // just outside `box`) actually get drawn. See renderDebug()'s own
        // doc comment for why CCanvas needs this back.
        CBox bounds = box;

        // Hit-target fill FIRST (underneath the outlines below) - only
        // for widgets that can ever actually match a click (see
        // isInteractive()'s doc comment); forcing debugShow.hitTarget on
        // a purely decorative widget draws nothing, since it would be
        // actively misleading. Stays within `box` - no expand needed.
        if (isInteractive() && resolveShow(resolved.showHitTarget, bigEnough))
            gfx::drawRect(box, hitColor);

        const bool hasMargin = m_margin.top > 0 || m_margin.right > 0 || m_margin.bottom > 0 || m_margin.left > 0;
        if (hasMargin && resolveShow(resolved.showMargin, bigEnough)) {
            const CBox marginBox{{box.pos().x - m_margin.left, box.pos().y - m_margin.top},
                                 {box.size().x + m_margin.left + m_margin.right, box.size().y + m_margin.top + m_margin.bottom}};
            expandBounds(bounds, strokeRect(marginBox, marginColor));
            expandBounds(bounds, drawLabel("m:" + formatInsets(m_margin), {marginBox.pos().x, marginBox.pos().y - lineOffset}, marginColor, fontSize));
        }

        if (resolveShow(resolved.showBox, bigEnough))
            strokeRect(box, boxColor); // within `box` already - no expand needed

        const bool hasPadding = m_padding.top > 0 || m_padding.right > 0 || m_padding.bottom > 0 || m_padding.left > 0;
        if (hasPadding && resolveShow(resolved.showPadding, bigEnough)) {
            const CBox paddingBox{{box.pos().x + m_padding.left, box.pos().y + m_padding.top},
                                  {box.size().x - m_padding.left - m_padding.right, box.size().y - m_padding.top - m_padding.bottom}};
            strokeRect(paddingBox, paddingColor); // inside `box` - no expand needed
            expandBounds(bounds, drawLabel("p:" + formatInsets(m_padding), {box.pos().x + 2, box.pos().y + 2}, paddingColor, fontSize));
        }

        if (resolveShow(resolved.showId, bigEnough))
            expandBounds(bounds, drawLabel(m_id, {box.pos().x + 2, box.pos().y - lineOffset}, idLabelColor, fontSize));

        if (resolveShow(resolved.showSize, bigEnough))
            expandBounds(bounds,
                         drawLabel(std::to_string(static_cast<int>(box.size().x)) + "x" + std::to_string(static_cast<int>(box.size().y)),
                                   {box.pos().x + box.size().x - (fontSize * 4), box.pos().y + box.size().y + 2}, sizeLabelColor, fontSize));

        // z/opacity's AUTO default additionally requires a non-default
        // value to actually be interesting - an explicit
        // debugShow.zOpacity=true bypasses both that AND the size gate
        // (resolveShow() only ever falls back to this auto default when
        // nothing was explicitly forced).
        const bool nonDefaultMeta = m_zIndex != 0 || m_opacity != 1.0;
        if (resolveShow(resolved.showZOpacity, bigEnough && nonDefaultMeta)) {
            std::string opacityStr = std::to_string(m_opacity);
            opacityStr.resize(4);
            expandBounds(bounds, drawLabel("z:" + std::to_string(m_zIndex) + " op:" + opacityStr, {box.pos().x + 2, box.pos().y + box.size().y + 2}, metaLabelColor, fontSize));
        }

        return bounds;
    }

} // namespace HyprLUI
