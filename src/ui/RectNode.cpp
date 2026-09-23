#include "RectNode.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CRectNode::renderFill(const Vector2D& origin, const Vector2D& scale, float opacity) {
        CHyprColor faded = effectiveFillColor(m_color);
        faded.a *= opacity;
        gfx::drawRect(boxAt(origin, scale), faded, m_rounding);
    }

    void CRectNode::renderBorder(const Vector2D& origin, const Vector2D& scale, float opacity) {
        if (m_borderWidth > 0)
            gfx::drawBorder(boxAt(origin, scale), gfx::fadeGradient(m_borderColor, opacity), m_borderWidth, m_rounding);
    }

    void CRectNode::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);
        renderFill(origin, scale, opacity);
        renderBorder(origin, scale, opacity);
    }

} // namespace HyprLUI
