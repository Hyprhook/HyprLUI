#pragma once
//
// ButtonWidget.hpp
//
// A clickable rectangle: renders a flat-filled background (like CRectNode)
// then its children on top at whatever position they were given (manual/
// absolute, like CStackWidget) - lets a Lua caller compose a label
// (typically a Text child) inside it. Click *detection* (press/release
// pairing, hit-testing, which mouse button counts) lives entirely in
// src/input/InputHook.cpp/CUIManager::clickWidget().
//
// The onClick callback itself is CWidget's own generic mechanism (Phase
// 10 follow-up, DESIGN.md) - setOnClick()/fireClick() aren't redeclared
// here, this class just overrides hitTest()/isInteractive() to always be
// a valid leaf click target (unconditionally, regardless of whether
// onClick happens to be set) rather than only becoming one once a
// handler's actually attached, which is the generic default every OTHER
// widget type falls back to (see Widget.hpp's hitTest()) - Button keeps
// its own "always a real button, structurally" contract from Phase 4.
//
// Deliberately Lua-agnostic like every other widget: LuaBridge.cpp owns
// wrapping a Lua function reference into the std::function passed to
// setOnClick(), including that reference's lifetime - this class has no
// idea a Lua VM exists, matching every other widget in this tree.

#include "Widget.hpp"

#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    class CButtonWidget : public CWidget {
      public:
        CButtonWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0) :
            CWidget(std::move(id), position), m_color(color), m_rounding(rounding) {
            m_size = size;
        }

        void render(const Vector2D& origin, float parentOpacity = 1.0F) override;

        void setColor(const CHyprColor& color) {
            m_color = color;
        }
        void setRounding(int rounding) {
            m_rounding = rounding;
        }

      protected:
        // A button is a hit-testing leaf on purpose - we don't support
        // (or need) buttons nested inside buttons, so there's no reason
        // to search its children once its own bounds already match.
        // Excludes disabled buttons too (Phase 10) - click-through, as if
        // this widget isn't there for interaction purposes. Unconditional
        // otherwise - matches even with no onClick set, unlike CWidget's
        // generic default (see this file's own doc comment above).
        CWidget* hitTest(const Vector2D& origin, const Vector2D& point) override {
            if (!m_visible || m_disabled)
                return nullptr;
            return boxAt(origin).containsPoint(point) ? this : nullptr;
        }

        bool isInteractive() const override {
            return true;
        }

      private:
        CHyprColor m_color;
        int        m_rounding;
    };

} // namespace HyprLUI
