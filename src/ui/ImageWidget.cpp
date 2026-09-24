#include "ImageWidget.hpp"

namespace HyprLUI {

    CImageWidget::CImageWidget(std::string id, const Vector2D& position, std::string path, CHyprColor color, int rounding, Config::CGradientValueData borderColor,
                               int borderWidth) :
        CRectNode(std::move(id), position, Vector2D{0, 0}, color, rounding, std::move(borderColor), borderWidth), m_path(std::move(path)) {
        reload(); // sets the real m_size from the decoded texture, overwriting the {0,0} placeholder above
    }

    CImageWidget::CImageWidget(std::string id, const Vector2D& position, const SPixelSpec& pixels, CHyprColor color, int rounding, Config::CGradientValueData borderColor,
                               int borderWidth) : CRectNode(std::move(id), position, Vector2D{0, 0}, color, rounding, std::move(borderColor), borderWidth) {
        m_texture = gfx::makeImageTexture(pixels.width, pixels.height, pixels.rowstride, pixels.hasAlpha, pixels.channels, pixels.dataBase64);
        if (m_texture)
            m_size = m_texture->m_size;
        primeNaturalSize(); // see reload()'s own note on why this is needed for Image specifically
    }

    void CImageWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);
        renderFill(origin, scale, opacity);

        if (m_texture) {
            // boxAt(origin, scale) - NOT the texture's native size -
            // deliberately unlike CTextNode::render(): stretching an image
            // to fill an explicit fixed w/h is the expected/desired
            // behavior here, so the layout box IS the draw box.
            gfx::drawTexture(m_texture, boxAt(origin, scale), opacity, m_rounding);
        }

        renderBorder(origin, scale, opacity);
    }

    void CImageWidget::setImage(const std::string& path) {
        m_path = path;
        reload();
    }

    void CImageWidget::reload() {
        m_texture = gfx::makeImageTexture(m_path);
        if (m_texture)
            m_size = m_texture->m_size;

        // Keep the base class's default measureContent() (Widget.hpp) in
        // sync - Image is the one leaf type whose natural/intrinsic size
        // can legitimately change AFTER construction (setImage() decoding
        // a differently-sized texture), so the one-time primeNaturalSize()
        // call buildWidget() makes at construction (LuaBridge.cpp) isn't
        // enough on its own here, unlike every other leaf type.
        primeNaturalSize();
    }

} // namespace HyprLUI
