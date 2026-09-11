#include "RectNode.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CRectNode::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);

        CHyprColor  faded = m_color;
        faded.a *= opacity;
        gfx::drawRect(boxAt(origin, scale), faded, m_rounding);

        if (m_borderWidth > 0)
            gfx::drawBorder(boxAt(origin, scale), gfx::fadeGradient(m_borderColor, opacity), m_borderWidth, m_rounding);
    }

} // namespace HyprLUI
