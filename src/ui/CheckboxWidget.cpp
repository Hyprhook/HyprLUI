#include "CheckboxWidget.hpp"
#include "../render/gfx.hpp"

#include <algorithm>

namespace HyprLUI {

    void CCheckboxWidget::render(const Vector2D& origin, float parentOpacity) {
        if (!m_visible)
            return;

        const float opacity = parentOpacity * static_cast<float>(m_opacity);

        CHyprColor  outer = m_color;
        outer.a *= opacity;
        gfx::drawRect(boxAt(origin), outer, m_rounding);

        if (!m_checked)
            return;

        // Inner filled square inset 25% on each side (50% of the outer
        // box's own size, centered) - the "checked" indicator. A plain
        // rect rather than a checkmark glyph, see this class's own doc
        // comment for why.
        const CBox box = boxAt(origin);
        const CBox inner{{box.pos().x + box.size().x * 0.25, box.pos().y + box.size().y * 0.25}, {box.size().x * 0.5, box.size().y * 0.5}};
        CHyprColor checkedFill = m_checkedColor;
        checkedFill.a *= opacity;
        gfx::drawRect(inner, checkedFill, m_rounding > 0 ? std::max(0, m_rounding / 2) : 0);
    }

} // namespace HyprLUI
