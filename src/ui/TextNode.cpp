#include "TextNode.hpp"

namespace HyprLUI {

    CTextNode::CTextNode(std::string id, const Vector2D& position, std::string text, int pointSize, CHyprColor color, std::string fontFamily) :
        CNode(std::move(id), position, /* size = */ Vector2D{0, 0}), m_text(std::move(text)), m_pointSize(pointSize), m_color(color), m_fontFamily(std::move(fontFamily)) {}

    void CTextNode::rebuildTexture() {
        m_texture = gfx::makeTextTexture(m_text, m_color, m_pointSize, m_fontFamily);

        if (m_texture)
            m_size = m_texture->m_size;

        m_dirty = false;
    }

    void CTextNode::render(const Vector2D& origin) {
        if (!m_visible)
            return;

        if (m_dirty)
            rebuildTexture();

        if (!m_texture)
            return;

        gfx::drawTexture(m_texture, boxAt(origin));
    }

    void CTextNode::setText(const std::string& text) {
        if (text == m_text)
            return;
        m_text = text;
        markDirty();
    }

    void CTextNode::setColor(const CHyprColor& color) {
        m_color = color;
        markDirty();
    }

    void CTextNode::setPointSize(int pointSize) {
        m_pointSize = pointSize;
        markDirty();
    }

} // namespace HyprLUI
