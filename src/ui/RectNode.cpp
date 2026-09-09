#include "RectNode.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CRectNode::render(const Vector2D& origin, float parentOpacity) {
        if (!m_visible)
            return;

        CHyprColor faded = m_color;
        faded.a *= parentOpacity * static_cast<float>(m_opacity);
        gfx::drawRect(boxAt(origin), faded, m_rounding);
    }

} // namespace HyprLUI
