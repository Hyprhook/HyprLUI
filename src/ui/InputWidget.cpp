#include "InputWidget.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace HyprLUI {

    CInputWidget::CInputWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding, std::string initialText, CHyprColor textColor,
                               int textSize, std::string textFont, Config::CGradientValueData borderColor, int borderWidth) :
        CRectNode(std::move(id), position, size, color, rounding, std::move(borderColor), borderWidth), m_text(std::move(initialText)) {
        // Small fixed left inset by default so text doesn't touch the
        // very edge - expressed as ordinary `padding` (left only) rather
        // than baked into the label's position, so a Lua-supplied
        // `padding` overrides it like any other widget's. arrangeChildren()
        // below positions the label FROM padding() every frame.
        setPadding({.left = 8});

        // Added via addChild() (not kept purely internal) so it goes
        // through the exact same measure/arrange/render/hitTest walk as
        // any other child - no separate rendering path to keep in sync,
        // and Lua-added children (added after construction, in
        // LuaBridge.cpp's buildWidget()) naturally paint on top of it,
        // same as a Button's label. Position is a placeholder - real
        // placement happens in arrangeChildren() below, every frame.
        m_label = std::make_shared<CTextNode>(m_id + "::text", Vector2D{0, 0}, m_text, textSize, textColor, textFont);
        addChild(m_label);
    }

    void CInputWidget::render(const Vector2D& origin, float parentOpacity, const Vector2D& scale) {
        if (!m_visible)
            return;

        const float opacity = composedOpacity(parentOpacity);
        renderFill(origin, scale, opacity);
        // See CButtonWidget::render()'s comment - pass parentOpacity, not
        // an already-composed value, so m_opacity isn't applied to
        // children twice.
        CWidget::render(origin, parentOpacity, scale); // draws children (the auto label, plus any Lua-added ones) on top
        renderBorder(origin, scale, opacity);          // drawn last, so it's never hidden under a child that happens to reach the edge
    }

    void CInputWidget::arrangeChildren() {
        // Vertical centering uses the label's own just-measured height
        // (available by arrange()-time - the whole tree's measure() pass
        // already completed before arrange() ever starts, see Widget.hpp)
        // rather than approximating via the configured point size like
        // the original constructor-time version had to.
        m_label->setPosition({padding().left, (m_size.y - m_label->size().y) / 2.0});
    }

    void CInputWidget::setText(const std::string& text) {
        m_text = text;
        m_label->setText(text);
    }

    void CInputWidget::handleKey(uint32_t keysym, bool pressed) {
        if (pressed) {
            bool changed = false;

            // Backspace and printable ASCII (0x20-0x7e) are the entire
            // built-in vocabulary. X11/xkb keysyms mirror ASCII in this
            // range, and Shift is already baked into the keysym by the
            // time it reaches here, so no separate case-handling is
            // needed for e.g. Shift+A vs a.
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
