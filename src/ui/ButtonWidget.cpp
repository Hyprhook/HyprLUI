#include "ButtonWidget.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CButtonWidget::render(const Vector2D& origin) {
        if (!m_visible)
            return;

        gfx::drawRect(boxAt(origin), m_color, m_rounding);
        CWidget::render(origin); // draws children (e.g. a label) on top, at their own manual x/y
    }

} // namespace HyprLUI
