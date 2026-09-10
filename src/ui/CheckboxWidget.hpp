#pragma once
//
// CheckboxWidget.hpp
//
// Checked/unchecked only (v1 scope, DESIGN.md Phase 8) - deliberately not
// an iOS-style toggle switch, no animated transition. Renders like
// CButtonWidget (flat-filled outer box, same rounding/hit-testing shape)
// plus, when checked, a smaller inset filled square drawn on top - a plain
// rect-based "checked" indicator rather than a checkmark glyph, since this
// toolkit has no icon/glyph font dependency to draw one with and a filled
// inner square is a common enough native-checkbox convention on its own.
//
// Click semantics/lifetime are identical to CButtonWidget's (see its own
// doc comment) - same InputHook.cpp press/release pairing, same
// Lua-agnostic split (LuaBridge.cpp owns wrapping the Lua onChange
// function reference). The one real difference: a Checkbox owns its own
// boolean state and TOGGLES it on a successful click before firing its
// callback with the new value, rather than just notifying "something was
// clicked" and leaving all state to the caller - a real checkbox needs a
// checked/unchecked question answerable without asking Lua, e.g. for
// get_checkbox_checked().

#include "Widget.hpp"

#include <hyprland/src/helpers/Color.hpp>

#include <functional>

namespace HyprLUI {

    class CCheckboxWidget : public CWidget {
      public:
        CCheckboxWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, CHyprColor checkedColor, int rounding = 0, bool checked = false) :
            CWidget(std::move(id), position), m_color(color), m_checkedColor(checkedColor), m_rounding(rounding), m_checked(checked) {
            m_size = size;
        }

        void render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) override;

        void setColor(const CHyprColor& color) {
            m_color = color;
        }
        void setCheckedColor(const CHyprColor& color) {
            m_checkedColor = color;
        }
        void setRounding(int rounding) {
            m_rounding = rounding;
        }

        void setOnChange(std::function<void(bool)> fn) {
            m_onChange = std::move(fn);
        }

        bool checked() const {
            return m_checked;
        }

        // Sets the checked state programmatically (e.g. from Lua's
        // set_checkbox_checked()) - does NOT fire onChange, same "no
        // invocation on load/programmatic set, only on real interaction"
        // convention set_text()/set_input_text() elsewhere use.
        void setChecked(bool checked) {
            m_checked = checked;
        }

        // Invokes a real click: toggles m_checked, then fires onChange
        // with the NEW value. Called by InputHook (via CUIManager) once a
        // press and its matching release both land on this same widget -
        // never called directly from hitTest()/rendering, same contract
        // CButtonWidget::click() has.
        void click() {
            m_checked = !m_checked;
            if (m_onChange)
                m_onChange(m_checked);
        }

      protected:
        // A hit-testing leaf, same reasoning as CButtonWidget - no nested
        // interactive widgets inside a Checkbox. Excludes disabled
        // checkboxes too (Phase 10).
        CWidget* hitTest(const Vector2D& origin, const Vector2D& point, const Vector2D& scale = {1, 1}) override {
            if (!m_visible || m_disabled)
                return nullptr;
            return boxAt(origin, scale).containsPoint(point) ? this : nullptr;
        }

        bool isInteractive() const override {
            return true;
        }

      private:
        CHyprColor                m_color;
        CHyprColor                m_checkedColor;
        int                       m_rounding;
        bool                      m_checked;
        std::function<void(bool)> m_onChange;
    };

} // namespace HyprLUI
