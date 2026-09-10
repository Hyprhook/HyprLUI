#include "ButtonWidget.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CButtonWidget::render(const Vector2D& origin, float parentOpacity) {
        if (!m_visible)
            return;

        CHyprColor faded = effectiveFillColor(m_color);
        faded.a *= composedOpacity(parentOpacity);
        gfx::drawRect(boxAt(origin), faded, m_rounding);
        // CWidget::render() composes in m_opacity (and any visibility-fade
        // progress) itself before handing that to children - passing
        // parentOpacity (NOT the already-self-composed `faded.a` factor
        // above) here avoids applying this widget's own opacity twice.
        CWidget::render(origin, parentOpacity); // draws children (e.g. a label) on top, at their own manual x/y
    }

} // namespace HyprLUI
