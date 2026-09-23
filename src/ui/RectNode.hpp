#pragma once
//
// RectNode.hpp
//
// A flat-filled rectangle - useful standalone (Box) or as the shared
// background-drawing base every other widget that draws its own
// color/rounding/border (Button, Input, Checkbox, Image) inherits from,
// instead of each separately re-implementing the same fields/rendering.
// Cheap enough to not need caching.

#include "Widget.hpp"

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    class CRectNode : public CWidget {
      public:
        CRectNode(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0,
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
        // Split so a subclass can draw content between fill and border.
        void                       renderFill(const Vector2D& origin, const Vector2D& scale, float opacity);
        void                       renderBorder(const Vector2D& origin, const Vector2D& scale, float opacity);

        CHyprColor                 m_color;
        int                        m_rounding;
        Config::CGradientValueData m_borderColor;
        int                        m_borderWidth;
    };

} // namespace HyprLUI
