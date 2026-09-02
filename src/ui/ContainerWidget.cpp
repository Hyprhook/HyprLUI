#include "ContainerWidget.hpp"

#include <algorithm>

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

        bool first = true;
        for (const auto& child : m_children) {
            if (!first)
                main += m_gap;
            first = false;

            if (m_direction == EFlexDirection::Row) {
                main += child->size().x;
                cross = std::max(cross, child->size().y);
            } else {
                main += child->size().y;
                cross = std::max(cross, child->size().x);
            }
        }

        main += 2 * m_padding;
        cross += 2 * m_padding;

        m_size = m_direction == EFlexDirection::Row ? Vector2D{main, cross} : Vector2D{cross, main};
    }

    void CFlexWidget::arrangeChildren() {
        // Cross-axis space to align within is the widget's own final size
        // (which may be larger than content if a fixed override was set),
        // not just the measured content extent.
        const double availableCross = (m_direction == EFlexDirection::Row ? m_size.y : m_size.x) - 2 * m_padding;

        double offset = m_padding;
        for (const auto& child : m_children) {
            const double childMain  = m_direction == EFlexDirection::Row ? child->size().x : child->size().y;
            const double childCross = m_direction == EFlexDirection::Row ? child->size().y : child->size().x;

            double crossPos = m_padding;
            if (m_align == EAlign::Center)
                crossPos = m_padding + (availableCross - childCross) / 2.0;
            else if (m_align == EAlign::End)
                crossPos = m_padding + (availableCross - childCross);

            if (m_direction == EFlexDirection::Row)
                child->setPosition({offset, crossPos});
            else
                child->setPosition({crossPos, offset});

            offset += childMain + m_gap;
        }
    }

} // namespace HyprLUI
