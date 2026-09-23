#pragma once
//
// ButtonWidget.hpp
//
// A clickable rectangle: renders a flat-filled background (via the
// inherited CRectNode) then its children on top at whatever position
// they were given (manual/absolute, like CStackWidget) - lets a Lua
// caller compose a label (typically a Text child) inside it. Click
// *detection* (press/release pairing, hit-testing, which mouse button
// counts) lives entirely in src/input/InputHook.cpp/CUIManager::
// clickWidget().
//
// onClick itself is CWidget's own generic field (setOnClick()/
// fireClick() aren't redeclared here) - this class only overrides
// hitTest()/isInteractive() to always be a valid leaf click target, even
// with no onClick set, unlike the generic default every other widget type
// falls back to.

#include "RectNode.hpp"

namespace HyprLUI {

    class CButtonWidget : public CRectNode {
      public:
        using CRectNode::CRectNode;

        void render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) override;

      protected:
        // A hit-testing leaf on purpose - no buttons nested inside
        // buttons. Excludes disabled buttons (click-through). Otherwise
        // unconditional - matches even with no onClick set, unlike
        // CWidget's generic default.
        CWidget* hitTest(const Vector2D& origin, const Vector2D& point, const Vector2D& scale = {1, 1}) override {
            if (!m_visible || m_disabled)
                return nullptr;
            return boxAt(origin, scale).containsPoint(point) ? this : nullptr;
        }

        bool isInteractive() const override {
            return true;
        }
    };

} // namespace HyprLUI
