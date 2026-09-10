#include "ContainerWidget.hpp"

#include <algorithm>
#include <cmath>

namespace HyprLUI {

    void CStackWidget::measureContent() {
        Vector2D bounds{0, 0};
        for (const auto& child : m_children) {
            bounds.x = std::max(bounds.x, child->position().x + child->size().x);
            bounds.y = std::max(bounds.y, child->position().y + child->size().y);
        }
        m_size = bounds;
    }

    void CFlexWidget::measureContent() {
        double main  = 0;
        double cross = 0;

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
                cross = std::max(cross, m.top + child->size().y + m.bottom);
            } else {
                main += m.top + child->size().y + m.bottom;
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
            const double childCross   = m_direction == EFlexDirection::Row ? child->size().y : child->size().x;
            const double crossLead    = m_direction == EFlexDirection::Row ? m.top : m.left;
            const double crossTrail   = m_direction == EFlexDirection::Row ? m.bottom : m.right;
            const double mainLead     = m_direction == EFlexDirection::Row ? m.left : m.top;
            const double mainTrail    = m_direction == EFlexDirection::Row ? m.right : m.bottom;
            const double availForThis = availableCross - crossLead - crossTrail;

            double       crossPos = crossPadLead + crossLead;
            if (m_align == EAlign::Center)
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
