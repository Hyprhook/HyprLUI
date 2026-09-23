#include "CheckboxWidget.hpp"
#include "../render/gfx.hpp"

#include <algorithm>

namespace HyprLUI {

    void CCheckboxWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);
        renderFill(origin, scale, opacity);

        if (m_checked) {
            // Inner filled square inset 25% on each side (50% of the outer
            // box's own size, centered) - the "checked" indicator. Drawn
            // between the fill and the border so the border stays on top.
            const CBox box = boxAt(origin, scale);
            const CBox inner{{box.pos().x + box.size().x * 0.25, box.pos().y + box.size().y * 0.25}, {box.size().x * 0.5, box.size().y * 0.5}};
            CHyprColor checkedFill = m_checkedColor;
            checkedFill.a *= opacity;
            gfx::drawRect(inner, checkedFill, m_rounding > 0 ? std::max(0, m_rounding / 2) : 0);
        }

        renderBorder(origin, scale, opacity);
    }

} // namespace HyprLUI
