#pragma once
//
// ButtonWidget.hpp
//
// A clickable rectangle: renders a flat-filled background (like CRectNode)
// then its children on top at whatever position they were given (manual/
// absolute, like CStackWidget) - lets a Lua caller compose a label
// (typically a Text child) inside it. Click *detection* (press/release
// pairing, hit-testing, which mouse button counts) lives entirely in
// src/input/InputHook.cpp - this class just stores the callback and
// exposes click() for the hook to invoke once it's decided a real click
// happened.
//
// Deliberately Lua-agnostic like every other widget: LuaBridge.cpp owns
// wrapping a Lua function reference into the std::function passed to
// setOnClick(), including that reference's lifetime (see LuaBridge.cpp's
// "button" handling in buildWidget()) - this class has no idea a Lua VM
// exists, matching every other widget in this tree.

#include "Widget.hpp"

#include <hyprland/src/helpers/Color.hpp>

#include <functional>

namespace HyprLUI {

    class CButtonWidget : public CWidget {
      public:
        CButtonWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0) :
            CWidget(std::move(id), position), m_color(color), m_rounding(rounding) {
            m_size = size;
        }

        void render(const Vector2D& origin) override;

        void setColor(const CHyprColor& color) {
            m_color = color;
        }
        void setRounding(int rounding) {
            m_rounding = rounding;
        }

        void setOnClick(std::function<void()> fn) {
            m_onClick = std::move(fn);
        }

        // Invokes the onClick handler, if any. Called by InputHook once a
        // press and its matching release both land on this same widget -
        // never called directly from hitTest()/rendering.
        void click() {
            if (m_onClick)
                m_onClick();
        }

      protected:
        // A button is a hit-testing leaf on purpose - we don't support
        // (or need) buttons nested inside buttons, so there's no reason
        // to search its children once its own bounds already match.
        CWidget* hitTest(const Vector2D& origin, const Vector2D& point) override {
            if (!m_visible)
                return nullptr;
            return boxAt(origin).containsPoint(point) ? this : nullptr;
        }

      private:
        CHyprColor            m_color;
        int                   m_rounding;
        std::function<void()> m_onClick;
    };

} // namespace HyprLUI
