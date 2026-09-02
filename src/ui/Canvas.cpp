#include "Canvas.hpp"
#include "../render/gfx.hpp"

namespace HyprLUI {

    void CCanvas::render() {
        if (!m_visible || !m_root)
            return;

        // Full-tree layout every frame - HUD-sized content, no need for a
        // layout-dirty flag (unlike CTextNode's texture cache, which is the
        // expensive part and stays cached).
        m_root->measure();
        m_root->arrange();
        m_root->render(m_position);
    }

    void CCanvas::damage() const {
        gfx::damageBox(box());
    }

} // namespace HyprLUI
