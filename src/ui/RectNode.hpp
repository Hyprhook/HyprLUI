#pragma once
//
// RectNode.hpp
//
// A flat-filled rectangle - useful as a panel/button background behind
// other nodes. Cheap enough to not need caching.

#include "Node.hpp"

#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    class CRectNode : public CNode {
      public:
        CRectNode(std::string id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0) :
            CNode(std::move(id), position, size), m_color(color), m_rounding(rounding) {}

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
