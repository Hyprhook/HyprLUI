#include "TextNode.hpp"

namespace HyprLUI {

    CTextNode::CTextNode(std::string id, const Vector2D& position, std::string text, int pointSize, CHyprColor color, std::string fontFamily) :
        CWidget(std::move(id), position), m_text(std::move(text)), m_pointSize(pointSize), m_color(color), m_fontFamily(std::move(fontFamily)) {}

    void CTextNode::rebuildTexture() {
        // A max-width constraint (Phase 7's `maxW`, set via CWidget::
        // setMaxSize()) is forwarded straight into Hyprland's own text
        // renderer, which already truncates-with-ellipsis when given one
        // (Renderer.cpp's renderText(): `pango_layout_set_ellipsize(...,
        // PANGO_ELLIPSIZE_END)` whenever maxWidth > 0) - this is the chosen
        // v1 overflow behavior (see DESIGN.md Phase 7), gotten essentially
        // for free rather than reimplemented here. Wrap/clip modes aren't
        // wired up - not needed once a default was picked.
        const int maxWidth = m_maxW ? static_cast<int>(*m_maxW) : 0;
        m_texture          = gfx::makeTextTexture(m_text, m_color, m_pointSize, m_fontFamily, maxWidth);

        if (m_texture)
            m_size = m_texture->m_size;

        m_dirty = false;
    }

    void CTextNode::render(const Vector2D& origin, float parentOpacity) {
        if (!m_visible)
            return;

        if (m_dirty)
            rebuildTexture();

        if (!m_texture)
            return;

        // Draw at the TEXTURE's own native size, not boxAt(origin)'s
        // (possibly min-width-widened) m_size - Hyprland's texture pass
        // element scales its source to fill whatever box it's given, so
        // using the layout box here would visibly stretch/blur the glyphs
        // instead of just reserving empty space beside them (the correct,
        // standard "min-width doesn't stretch content" behavior every
        // other toolkit gives text). Harmless double-work when they're
        // already equal (the common case, and always true when a maxW
        // constraint - not minW - is what's active, since Pango already
        // rasterizes to fit that exactly).
        const float opacity = composedOpacity(parentOpacity);
        gfx::drawTexture(m_texture, {origin + m_position, m_texture->m_size}, opacity);
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
