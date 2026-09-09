#include "ButtonWidget.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CButtonWidget::render(const Vector2D& origin, float parentOpacity) {
        if (!m_visible)
            return;

        CHyprColor faded = m_color;
        faded.a *= parentOpacity * static_cast<float>(m_opacity);
        gfx::drawRect(boxAt(origin), faded, m_rounding);
        // CWidget::render() multiplies in m_opacity itself before handing
        // that composed value to children - passing parentOpacity (NOT the
        // already-self-multiplied `faded.a` factor above) here avoids
        // applying this widget's own opacity to its children twice.
        CWidget::render(origin, parentOpacity); // draws children (e.g. a label) on top, at their own manual x/y
    }

} // namespace HyprLUI
