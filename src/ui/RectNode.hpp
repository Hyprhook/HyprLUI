#pragma once
//
// RectNode.hpp
//
// A flat-filled rectangle - useful as a panel/button background behind
// other nodes. Cheap enough to not need caching.

#include "Widget.hpp"

#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    class CRectNode : public CWidget {
      public:
        CRectNode(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0) :
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

      private:
        CHyprColor m_color;
        int        m_rounding;
    };

} // namespace HyprLUI
