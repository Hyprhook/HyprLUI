#include "CheckboxWidget.hpp"
#include "../render/gfx.hpp"

#include <algorithm>

namespace HyprLUI {

    void CCheckboxWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);

        CHyprColor  outer = effectiveFillColor(m_color);
        outer.a *= opacity;
        gfx::drawRect(boxAt(origin, scale), outer, m_rounding);

        if (m_borderWidth > 0)
            gfx::drawBorder(boxAt(origin, scale), gfx::fadeGradient(m_borderColor, opacity), m_borderWidth, m_rounding);

        if (!m_checked)
            return;

        // Inner filled square inset 25% on each side (50% of the outer
        // box's own size, centered) - the "checked" indicator.
        const CBox box = boxAt(origin, scale);
        const CBox inner{{box.pos().x + box.size().x * 0.25, box.pos().y + box.size().y * 0.25}, {box.size().x * 0.5, box.size().y * 0.5}};
        CHyprColor checkedFill = m_checkedColor;
        checkedFill.a *= opacity;
        gfx::drawRect(inner, checkedFill, m_rounding > 0 ? std::max(0, m_rounding / 2) : 0);
    }

} // namespace HyprLUI
