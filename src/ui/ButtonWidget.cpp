#include "ButtonWidget.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CButtonWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);

        CHyprColor  faded = effectiveFillColor(m_color);
        faded.a *= opacity;
        gfx::drawRect(boxAt(origin, scale), faded, m_rounding);

        if (m_borderWidth > 0)
            gfx::drawBorder(boxAt(origin, scale), gfx::fadeGradient(m_borderColor, opacity), m_borderWidth, m_rounding);
        // CWidget::render() composes in m_opacity (and any visibility-fade
        // progress) itself before handing that to children - passing
        // parentOpacity (NOT the already-self-composed `faded.a` factor
        // above) here avoids applying this widget's own opacity twice.
        CWidget::render(origin, parentOpacity, scale); // draws children (e.g. a label) on top, at their own manual x/y
    }

} // namespace HyprLUI
