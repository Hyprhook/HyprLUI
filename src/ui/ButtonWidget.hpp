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
// onClick itself is CWidget's own generic field (setOnClick()/
// fireClick() aren't redeclared here) - this class only overrides
// hitTest()/isInteractive() to always be a valid leaf click target, even
// with no onClick set, unlike the generic default every other widget type
// falls back to.

#include "Widget.hpp"

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    class CButtonWidget : public CWidget {
      public:
        CButtonWidget(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0,
                      Config::CGradientValueData borderColor = Config::CGradientValueData{CHyprColor{}}, int borderWidth = 0) :
            CWidget(std::move(id), position), m_color(color), m_rounding(rounding), m_borderColor(std::move(borderColor)), m_borderWidth(borderWidth) {
            m_size = size;
        }

        void render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) override;

        void setColor(const CHyprColor& color) {
            m_color = color;
        }
        void setRounding(int rounding) {
            m_rounding = rounding;
        }
        void setBorder(Config::CGradientValueData color, int width) {
            m_borderColor = std::move(color);
            m_borderWidth = width;
        }

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

      private:
        CHyprColor                 m_color;
        int                        m_rounding;
        Config::CGradientValueData m_borderColor;
        int                        m_borderWidth;
    };

} // namespace HyprLUI
