#include "ImageWidget.hpp"

namespace HyprLUI {

    CImageWidget::CImageWidget(std::string id, const Vector2D& position, std::string path, int rounding) :
        CWidget(std::move(id), position), m_path(std::move(path)), m_rounding(rounding) {
        reload();
    }

    void CImageWidget::render(const Vector2D& origin, float parentOpacity) {
        if (!m_visible || !m_texture)
            return;

        // boxAt(origin) - NOT the texture's native size - deliberately
        // unlike CTextNode::render(): stretching an image to fill an
        // explicit fixed w/h is the expected/desired behavior here (see
        // ImageWidget.hpp's doc comment), so the layout box IS the draw
        // box.
        gfx::drawTexture(m_texture, boxAt(origin), parentOpacity * static_cast<float>(m_opacity), m_rounding);
    }

    void CImageWidget::setImage(const std::string& path) {
        m_path = path;
        reload();
    }

    void CImageWidget::reload() {
        m_texture = gfx::makeImageTexture(m_path);
        if (m_texture)
            m_size = m_texture->m_size;
    }

} // namespace HyprLUI
