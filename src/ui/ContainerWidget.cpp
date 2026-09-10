#include "ContainerWidget.hpp"

#include <algorithm>
#include <cmath>

namespace HyprLUI {

    void CStackWidget::measureContent() {
        Vector2D bounds{0, 0};
        bool     anyCounted = false;
        for (const auto& child : m_children) {
            // A `fill` child (Phase 15) is sized BY this stack, never the
            // other way around - skip it here, same as CSS align-self:
            // stretch never inflates its own parent's size. This is a
            // DIRECTIONAL rule, not just an anti-staleness one - even with
            // every widget now self-correcting every frame (see
            // CWidget::measureContent()'s default in Widget.hpp), a fill
            // child's own natural size still has no business feeding back
            // into what it's stretching to match.
            if (child->fill())
                continue;
            anyCounted = true;
            bounds.x   = std::max(bounds.x, child->position().x + child->size().x);
            bounds.y   = std::max(bounds.y, child->position().y + child->size().y);
        }

        // Degenerate case (DESIGN.md's "canvas damage tracking" open
        // question): if EVERY child is `fill`, the loop above counted
        // nothing at all - falling back to bounds{0,0} here would be
        // actively wrong, not just imprecise: this stack's own m_size
        // (and so every CCanvas damage box derived from it - see
        // Canvas.cpp) would go stuck at zero even while these children
        // keep rendering their own real, non-zero content, so no damage
        // call anywhere would ever validly cover what's actually drawn -
        // silently ghosting on open AND on close. Falling back to
        // counting the fill children at their own PRE-STRETCH size
        // instead matches what setFill()'s doc comment already promises
        // ("nothing to stretch TO stays at its own natural size") and
        // keeps m_size honest. Relies on every widget's measure() pass
        // (Widget.hpp) having ALREADY reset each child to its genuine
        // natural size, THIS frame, before this parent's own
        // measureContent() runs - true for every widget type now (a leaf's
        // default measureContent() self-corrects from its own
        // primeNaturalSize()'d value, exactly like a container already
        // recomputed fresh from children), not just containers - see that
        // default's own doc comment for the live bug this closes (this
        // fallback briefly shipped without it and re-read a leaf's stale,
        // already-stretched m_size instead).
        if (!anyCounted && !m_children.empty()) {
            for (const auto& child : m_children) {
                bounds.x = std::max(bounds.x, child->position().x + child->size().x);
                bounds.y = std::max(bounds.y, child->position().y + child->size().y);
            }
        }

        m_size = bounds;
    }

    void CStackWidget::arrangeChildren() {
        for (auto& child : m_children) {
            if (!child->fill())
                continue;
            child->setPosition({0, 0});
            child->setSize(m_size);
        }
    }

    void CFlexWidget::measureContent() {
        double main         = 0;
        double cross        = 0;
        bool   crossCounted = false;

        bool   first = true;
        for (const auto& child : m_children) {
            if (!first)
                main += m_gap;
            first = false;

            // A child's own margin (Phase 7) adds to, not instead of,
            // `gap` - same as CSS flexbox: the visual space between two
            // adjacent items ends up being gap + item1's trailing margin +
            // item2's leading margin.
            const auto& m = child->margin();
            if (m_direction == EFlexDirection::Row) {
                main += m.left + child->size().x + m.right;
                // A `fill` child (Phase 15) stretches to match this row's
                // CROSS-axis size - it must not also CONTRIBUTE to that
                // same size, same as CSS align-self: stretch never
                // inflates its own parent (a directional rule, see
                // CStackWidget::measureContent()'s own doc comment). The
                // MAIN axis is unaffected either way - fill never touches
                // it, so it's always safe to count normally.
                if (!child->fill()) {
                    cross        = std::max(cross, m.top + child->size().y + m.bottom);
                    crossCounted = true;
                }
            } else {
                main += m.top + child->size().y + m.bottom;
                if (!child->fill()) {
                    cross        = std::max(cross, m.left + child->size().x + m.right);
                    crossCounted = true;
                }
            }
        }

        // Degenerate case, same reasoning (and same DESIGN.md open
        // question) as CStackWidget::measureContent()'s own fallback
        // above: if EVERY child is `fill`, the cross axis counted nothing
        // at all above and would otherwise stay stuck at 0 - which this
        // row/column's own CCanvas damage tracking would then silently
        // inherit. Fall back to the fill children's own pre-stretch cross
        // sizes instead - safe (not stale) for the same reason
        // CStackWidget::measureContent()'s own fallback is, see its doc
        // comment.
        if (!crossCounted && !m_children.empty()) {
            for (const auto& child : m_children) {
                const auto& m = child->margin();
                if (m_direction == EFlexDirection::Row)
                    cross = std::max(cross, m.top + child->size().y + m.bottom);
                else
                    cross = std::max(cross, m.left + child->size().x + m.right);
            }
        }

        const auto& p = padding();
        main += m_direction == EFlexDirection::Row ? (p.left + p.right) : (p.top + p.bottom);
        cross += m_direction == EFlexDirection::Row ? (p.top + p.bottom) : (p.left + p.right);

        m_size = m_direction == EFlexDirection::Row ? Vector2D{main, cross} : Vector2D{cross, main};
    }

    void CFlexWidget::arrangeChildren() {
        const auto& p = padding();

        // Cross-axis space to align within is the widget's own final size
        // (which may be larger than content if a fixed override was set),
        // not just the measured content extent.
        const double crossPadLead   = m_direction == EFlexDirection::Row ? p.top : p.left;
        const double crossPadBoth   = m_direction == EFlexDirection::Row ? (p.top + p.bottom) : (p.left + p.right);
        const double mainPadLead    = m_direction == EFlexDirection::Row ? p.left : p.top;
        const double availableCross = (m_direction == EFlexDirection::Row ? m_size.y : m_size.x) - crossPadBoth;

        double       offset = mainPadLead;
        for (const auto& child : m_children) {
            const auto&  m            = child->margin();
            const double childMain    = m_direction == EFlexDirection::Row ? child->size().x : child->size().y;
            double       childCross   = m_direction == EFlexDirection::Row ? child->size().y : child->size().x;
            const double crossLead    = m_direction == EFlexDirection::Row ? m.top : m.left;
            const double crossTrail   = m_direction == EFlexDirection::Row ? m.bottom : m.right;
            const double mainLead     = m_direction == EFlexDirection::Row ? m.left : m.top;
            const double mainTrail    = m_direction == EFlexDirection::Row ? m.right : m.bottom;
            const double availForThis = availableCross - crossLead - crossTrail;

            double       crossPos = crossPadLead + crossLead;
            if (child->fill()) {
                // Cross-axis stretch (Phase 15, DESIGN.md) - fills the
                // whole available cross-axis space for this child,
                // ignoring `align` entirely (there's no room left to
                // align within once this fills it). The MAIN axis is
                // untouched - only the cross-axis component of size is
                // overridden here.
                childCross = availForThis;
                if (m_direction == EFlexDirection::Row)
                    child->setSize({child->size().x, childCross});
                else
                    child->setSize({childCross, child->size().y});
            } else if (m_align == EAlign::Center)
                crossPos = crossPadLead + crossLead + (availForThis - childCross) / 2.0;
            else if (m_align == EAlign::End)
                crossPos = crossPadLead + availableCross - crossTrail - childCross;

            // Rounded to whole pixels - EAlign::Center's own `/ 2.0` above
            // can land on a fractional pixel whenever (availForThis -
            // childCross) is odd. Found live: a texture (text especially)
            // drawn 1:1 but at a fractional destination offset still
            // samples a blended average of two adjacent texels per pixel
            // under Hyprland's GL_LINEAR filtering, instead of one exact
            // texel each - visibly blurry. See Canvas.cpp's
            // recomputeAnchorPosition() for the same fix, same reasoning.
            if (m_direction == EFlexDirection::Row)
                child->setPosition({std::round(offset + mainLead), std::round(crossPos)});
            else
                child->setPosition({std::round(crossPos), std::round(offset + mainLead)});

            offset += mainLead + childMain + mainTrail + m_gap;
        }
    }

} // namespace HyprLUI
