#pragma once
//
// TextNode.hpp
//
// Renders a single line/block of text. The rasterized texture is cached and
// only regenerated when the text, size, color, or font actually change -
// rasterizing via Cairo/Pango every frame would be needlessly expensive.

#include "Widget.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI {

    class CTextNode : public CWidget {
      public:
        CTextNode(std::string id, const Vector2D& position, std::string text, int pointSize = 16, CHyprColor color = CHyprColor{1.0, 1.0, 1.0, 1.0}, std::string fontFamily = "sans");

        void render(const Vector2D& origin) override;

        void setText(const std::string& text);
        const std::string& text() const {
            return m_text;
        }

        void setColor(const CHyprColor& color);
        void setPointSize(int pointSize);

      protected:
        // Text's "natural size" comes from rasterizing it - measure() (run
        // once per frame before arrange()/render()) is what triggers the
        // rebuild now, so layout always sees an up-to-date size. render()
        // keeps its own dirty check too as a safety net.
        void measureContent() override {
            if (m_dirty)
                rebuildTexture();
        }

      private:
        void markDirty() {
            m_dirty = true;
        }
        void rebuildTexture();

        std::string  m_text;
        int          m_pointSize;
        CHyprColor   m_color;
        std::string  m_fontFamily;

        SP<HyprTexture> m_texture;
        bool         m_dirty = true;
    };

} // namespace HyprLUI
