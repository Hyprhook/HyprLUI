#include "ButtonWidget.hpp"

namespace HyprLUI {

    void CButtonWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);
        renderFill(origin, scale, opacity);
        // CWidget::render() composes in m_opacity (and any visibility-fade
        // progress) itself before handing that to children - passing
        // parentOpacity (NOT the already-composed `opacity` above) here
        // avoids applying this widget's own opacity twice.
        CWidget::render(origin, parentOpacity, scale); // draws children (e.g. a label) on top, at their own manual x/y
        renderBorder(origin, scale, opacity);          // drawn last, so it's never hidden under a child that happens to reach the edge
    }

} // namespace HyprLUI
