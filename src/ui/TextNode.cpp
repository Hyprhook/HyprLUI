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

        if (m_texture) {
            // Width tracks the actual rendered texture (this IS supposed
            // to vary per string - "N" should measure narrower than
            // "escape", that's what lets a container size/wrap/truncate
            // around real content). Height does NOT come from the
            // texture though - see gfx::naturalLineHeight()'s own doc
            // comment (gfx.hpp) for why: Hyprland's own renderText() ties
            // texture height to per-string ink extents (descenders like
            // "p"/"g"/"y" measure taller), which would otherwise make
            // this widget's LAYOUT footprint - what a Row/Column packs
            // against - silently depend on which characters this string
            // happens to contain. render() below still draws the actual
            // texture at ITS OWN true size regardless (same as it
            // already did for width) - only the footprint used for
            // layout/alignment purposes is standardized here.
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

        // Draw at the TEXTURE's own native size (scaled - Phase 17 - by
        // `scale`, see CWidget::render()'s own doc comment for where that
        // ever comes from), not boxAt(origin)'s (possibly min-width-
        // widened) m_size - Hyprland's texture pass element scales its
        // source to fill whatever box it's given, so using the layout box
        // here would visibly stretch/blur the glyphs instead of just
        // reserving empty space beside them (the correct, standard "min-
        // width doesn't stretch content" behavior every other toolkit
        // gives text). Harmless double-work when they're already equal
        // (the common case, and always true when a maxW constraint - not
        // minW - is what's active, since Pango already rasterizes to fit
        // that exactly).
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
