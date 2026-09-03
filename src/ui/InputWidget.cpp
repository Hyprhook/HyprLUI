#include "InputWidget.hpp"
#include "../render/gfx.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace HyprLUI {

    CInputWidget::CInputWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding, std::string initialText, CHyprColor textColor,
                               int textSize, std::string textFont) : CWidget(std::move(id), position), m_color(color), m_rounding(rounding), m_text(std::move(initialText)) {
        m_size = size;

        // Small fixed left padding, roughly vertically centered using the
        // configured point size as a stand-in for the rasterized text's
        // actual height (not known until rasterization runs) - good
        // enough for a single-line field, not exact. Added via addChild()
        // (not kept purely internal) so it goes through the exact same
        // measure/arrange/render/hitTest walk as any other child - no
        // separate rendering path to keep in sync, and Lua-added children
        // (added after construction, in LuaBridge.cpp's buildWidget())
        // naturally paint on top of it, same as a Button's label.
        m_label = std::make_shared<CTextNode>(m_id + "::text", Vector2D{8.0, (size.y - textSize) / 2.0}, m_text, textSize, textColor, textFont);
        addChild(m_label);
    }

    void CInputWidget::render(const Vector2D& origin) {
        if (!m_visible)
            return;

        gfx::drawRect(boxAt(origin), m_color, m_rounding);
        CWidget::render(origin); // draws children (the auto label, plus any Lua-added ones) on top
    }

    void CInputWidget::setText(const std::string& text) {
        m_text = text;
        m_label->setText(text);
    }

    void CInputWidget::handleKey(uint32_t keysym, bool pressed) {
        if (pressed) {
            bool changed = false;

            // Backspace and printable ASCII (0x20-0x7e) are the entire
            // v1 built-in vocabulary - see InputWidget.hpp's doc comment
            // for why (no cursor/selection/IME/non-ASCII, layer that on
            // top of onKey below if needed). X11/xkb keysyms mirror ASCII
            // in this range, and Shift is already baked into the keysym
            // by the time it reaches here, so no separate case-handling
            // is needed for e.g. Shift+A vs a.
            if (keysym == XKB_KEY_BackSpace) {
                if (!m_text.empty()) {
                    m_text.pop_back();
                    changed = true;
                }
            } else if (keysym >= 0x20 && keysym <= 0x7e) {
                m_text.push_back(static_cast<char>(keysym));
                changed = true;
            }

            if (changed) {
                m_label->setText(m_text);
                if (m_onChange)
                    m_onChange(m_text);
            }
        }

        if (m_onKey)
            m_onKey(keysym, pressed);
    }

} // namespace HyprLUI
