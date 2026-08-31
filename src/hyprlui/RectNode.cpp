#include "RectNode.hpp"
#include "gfx.hpp"

namespace HyprLUI {

    void CRectNode::render(const Vector2D& origin) {
        if (!m_visible)
            return;

        gfx::drawRect(boxAt(origin), m_color, m_rounding);
    }

} // namespace HyprLUI
