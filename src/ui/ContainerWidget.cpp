#include "ContainerWidget.hpp"

#include <algorithm>
#include <cmath>

namespace HyprLUI {

    void CStackWidget::measureContent() {
        Vector2D bounds{0, 0};
        bool     anyCounted = false;
        for (const auto& child : m_children) {
            // A `fill` child is sized BY this stack, never the other way
            // around - skip it here, same as CSS align-self: stretch never
            // inflates its own parent's size.
            if (child->fill())
                continue;
            anyCounted = true;
            bounds.x   = std::max(bounds.x, child->position().x + child->size().x);
            bounds.y   = std::max(bounds.y, child->position().y + child->size().y);
        }

        // If EVERY child is `fill`, the loop above counted nothing, and
        // falling back to bounds{0,0} would leave this stack's own m_size
        // (and every damage box derived from it) stuck at zero even while
        // these children keep rendering real content. Fall back to
        // counting the fill children at their own pre-stretch size instead
        // - relies on every widget's measure() pass having already reset
        // each child to its natural size this frame, before this parent's
        // own measureContent() runs.
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
            // A fading-out child (remove_widget()'s own animateOut, or a
            // plain setVisible(false) with an out-animation) stops
            // reserving flow space immediately, not once it's actually
            // erased - lets its siblings reflow into the gap right away.
            // It's still rendered (at wherever arrangeChildren() last put
            // it) for the rest of its fade, just no longer part of the
            // flow.
            if (child->isFadingOut())
                continue;

            if (!first)
                main += m_gap;
            first = false;

            // A child's own margin adds to, not instead of, `gap` - same
            // as CSS flexbox: the visual space between two adjacent items
            // ends up being gap + item1's trailing margin + item2's
            // leading margin.
            const auto& m = child->margin();
            if (m_direction == EFlexDirection::Row) {
                main += m.left + child->size().x + m.right;
                // A `fill` child stretches to match this row's cross-axis
                // size - it must not also contribute to that same size.
                // The main axis is unaffected either way.
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

        // Same degenerate-case fallback as CStackWidget::measureContent()
        // above, for the same reason - if every child is `fill`, fall back
        // to their pre-stretch cross sizes instead of staying stuck at 0.
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
            // Frozen at its last position for the rest of its fade - see
            // measureContent()'s own comment on why.
            if (child->isFadingOut())
                continue;

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
                // Cross-axis stretch fills the whole available cross-axis
                // space for this child, ignoring `align` entirely - the
                // main axis is untouched, only the cross-axis size.
                childCross = availForThis;
                if (m_direction == EFlexDirection::Row)
                    child->setSize({child->size().x, childCross});
                else
                    child->setSize({childCross, child->size().y});
            } else if (m_align == EAlign::Center)
                crossPos = crossPadLead + crossLead + (availForThis - childCross) / 2.0;
            else if (m_align == EAlign::End)
                crossPos = crossPadLead + availableCross - crossTrail - childCross;

            // Rounded to whole pixels - see Canvas.cpp's
            // recomputeAnchorPosition() for why (GL_LINEAR blur on a
            // fractional destination offset).
            if (m_direction == EFlexDirection::Row)
                child->setPosition({std::round(offset + mainLead), std::round(crossPos)});
            else
                child->setPosition({std::round(crossPos), std::round(offset + mainLead)});

            offset += mainLead + childMain + mainTrail + m_gap;
        }
    }

} // namespace HyprLUI
