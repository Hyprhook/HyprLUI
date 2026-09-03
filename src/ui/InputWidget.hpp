#pragma once
//
// InputWidget.hpp
//
// A focusable rectangle that behaves like an actual text field by default:
// typing a printable-ASCII key appends it, Backspace removes the last
// character, and the current text is rendered automatically (via an
// internally-owned CTextNode child, reusing its existing rasterization -
// not reimplemented here) - none of that is left for a caller to build
// from scratch in Lua. Still "raw keysym only" in one specific sense (see
// DESIGN.md Phase 6): no cursor/selection/IME, no non-ASCII input, no
// clipboard - a caller wanting any of that layers it on top of onKey,
// which keeps firing for every key exactly as before, in addition to (not
// instead of) the built-in capture.
//
// Focus itself is NOT this class's job - CUIManager owns the single global
// "which Input currently has focus" slot (see UIManager.hpp's
// m_focusedInput) and calls focus()/blur()/handleKey() on whichever widget
// that resolves to. This class only reacts when told to; it never grabs or
// releases focus on its own. Same Lua-agnostic split as CButtonWidget:
// LuaBridge.cpp owns wrapping Lua function refs into the std::functions
// passed to setOnKey()/setOnFocus()/setOnBlur()/setOnChange(), including
// their lifetime.

#include "Widget.hpp"
#include "TextNode.hpp"

#include <hyprland/src/helpers/Color.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace HyprLUI {

    class CInputWidget : public CWidget {
      public:
        // `textColor`/`textSize`/`textFont` style the auto-owned display
        // label - same defaults CTextNode itself uses. `initialText` seeds
        // the buffer (e.g. pre-filling a field) without going through
        // setText()/onChange machinery, matching how every other widget
        // here takes its full starting config in the constructor.
        CInputWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0, std::string initialText = "",
                     CHyprColor textColor = CHyprColor{1.0, 1.0, 1.0, 1.0}, int textSize = 14, std::string textFont = "sans");

        void render(const Vector2D& origin) override;

        void setColor(const CHyprColor& color) {
            m_color = color;
        }
        void setRounding(int rounding) {
            m_rounding = rounding;
        }

        // Sets the current text programmatically (e.g. pre-filling or
        // clearing a field from Lua) - updates the rendered label but,
        // unlike typing, does NOT invoke onChange (same "no invocation on
        // load, only on real interaction" reasoning set_text() elsewhere
        // in this toolkit uses - avoids surprising feedback loops if
        // called from inside a callback).
        void               setText(const std::string& text);
        const std::string& text() const {
            return m_text;
        }

        void setOnKey(std::function<void(uint32_t keysym, bool pressed)> fn) {
            m_onKey = std::move(fn);
        }
        void setOnChange(std::function<void(const std::string&)> fn) {
            m_onChange = std::move(fn);
        }
        void setOnFocus(std::function<void()> fn) {
            m_onFocus = std::move(fn);
        }
        void setOnBlur(std::function<void()> fn) {
            m_onBlur = std::move(fn);
        }

        // Called by CUIManager once it's decided this widget is (or is no
        // longer) the focused one - never call these directly from a hit-
        // test/render path. handleKey() is never called at all for a key
        // that currently triggers a real Hyprland keybind - InputHook.cpp
        // filters those out before CUIManager::dispatchKey() ever runs,
        // so this widget can assume every key it sees is genuinely local
        // to it (see DESIGN.md Phase 6's keybind-priority note).
        void handleKey(uint32_t keysym, bool pressed);
        void focus() {
            if (m_onFocus)
                m_onFocus();
        }
        void blur() {
            if (m_onBlur)
                m_onBlur();
        }

      protected:
        // A hit-testing leaf, same reasoning as CButtonWidget - no nested
        // interactive widgets inside an Input.
        CWidget* hitTest(const Vector2D& origin, const Vector2D& point) override {
            if (!m_visible)
                return nullptr;
            return boxAt(origin).containsPoint(point) ? this : nullptr;
        }

      private:
        CHyprColor                              m_color;
        int                                     m_rounding;
        std::string                             m_text;
        std::shared_ptr<CTextNode>              m_label; // owned display child - see the .cpp for why it's added via addChild() rather than kept purely internal
        std::function<void(uint32_t, bool)>     m_onKey;
        std::function<void(const std::string&)> m_onChange;
        std::function<void()>                   m_onFocus;
        std::function<void()>                   m_onBlur;
    };

} // namespace HyprLUI
