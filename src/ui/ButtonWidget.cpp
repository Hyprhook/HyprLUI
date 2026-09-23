#include "ButtonWidget.hpp"

namespace HyprLUI {

    void CButtonWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);
        renderFill(origin, scale, opacity);
        CWidget::render(origin, parentOpacity, scale); // parentOpacity, not opacity - avoids double-applying m_opacity to children
        renderBorder(origin, scale, opacity);
    }

} // namespace HyprLUI
