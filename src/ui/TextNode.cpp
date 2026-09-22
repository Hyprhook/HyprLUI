#include "TextNode.hpp"

namespace HyprLUI {

    CTextNode::CTextNode(std::string id, const Vector2D& position, std::string text, int pointSize, CHyprColor color, std::string fontFamily) :
        CWidget(std::move(id), position), m_text(std::move(text)), m_pointSize(pointSize), m_color(color), m_fontFamily(std::move(fontFamily)) {}

    void CTextNode::rebuildTexture() {
        // maxW forwards straight into Hyprland's own text renderer, which
        // already truncates-with-ellipsis when given a max width - the
        // chosen overflow behavior, gotten for free rather than
        // reimplemented. Wrap/clip modes aren't wired up.
        const int maxWidth = m_maxW ? static_cast<int>(*m_maxW) : 0;
        m_texture          = gfx::makeTextTexture(m_text, m_color, m_pointSize, m_fontFamily, maxWidth);

        if (m_texture) {
            // Width tracks the actual rendered texture (varies per string,
            // as it should - lets a container size/wrap/truncate around
            // real content). Height uses naturalLineHeight() instead of
            // the texture's own height (see gfx::naturalLineHeight()) so
            // this widget's LAYOUT footprint doesn't depend on which
            // characters the string happens to contain (descenders like
            // "p"/"g"/"y" rasterize taller). render() still draws the
            // actual texture at its own true size regardless.
            m_size.x = m_texture->m_size.x;
            m_size.y = gfx::naturalLineHeight(m_fontFamily, m_pointSize);
        }

        m_dirty = false;
    }

    void CTextNode::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        if (m_dirty)
            rebuildTexture();

        if (!m_texture)
            return;

        // Draw at the TEXTURE's own native size, not boxAt(origin)'s
        // (possibly min-width-widened) m_size - using the layout box here
        // would visibly stretch/blur the glyphs instead of reserving
        // empty space beside them (the standard "min-width doesn't
        // stretch content" text behavior). No-op difference when they're
        // already equal, which is always true when maxW (not minW) is
        // what's active.
        const float    opacity = composedOpacity(parentOpacity);
        const Vector2D pos     = origin + (m_position + styleOffset()) * scale;
        gfx::drawTexture(m_texture, {pos, m_texture->m_size * scale}, opacity);
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
