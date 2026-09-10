#include "RectNode.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CRectNode::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        CHyprColor faded = m_color;
        faded.a *= composedOpacity(parentOpacity);
        gfx::drawRect(boxAt(origin, scale), faded, m_rounding);
    }

} // namespace HyprLUI
