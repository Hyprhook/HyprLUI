#pragma once
//
// ImageWidget.hpp
//
// Loads an image file (PNG/JPG/WEBP/SVG/AVIF/JXL - whatever the installed
// libhyprgraphics supports, see gfx.cpp's makeImageTexture()) and draws it
// as a texture. Size-to-content by default (the image's own natural pixel
// size) - an explicit fixed w/h (CWidget::setFixedSize(), applied
// generically in measure()) scales/stretches the image to fill that box,
// deliberately UNLIKE CTextNode's own min-width handling (see TextNode.cpp)
// - stretching a photo/icon to an explicit size is the expected default for
// an image (matches plain CSS `<img>` sizing), whereas stretching text
// glyphs looks wrong.
//
// Loading is EAGER (decoded synchronously in the constructor and again on
// every setImage() call) rather than lazily-on-first-measure() like
// CTextNode's texture cache - deliberately different: unlike text
// rasterization (which essentially never fails), a bad path/unsupported
// format is a real, likely-common config mistake, and eager decoding is
// what lets LuaBridge.cpp check loaded() and log a warning immediately at
// window-build time instead of only once this widget first gets measured.

#include "Widget.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <string>

namespace HyprLUI {

    class CImageWidget : public CWidget {
      public:
        CImageWidget(std::string id, const Vector2D& position, std::string path, int rounding = 0,
                     Config::CGradientValueData borderColor = Config::CGradientValueData{CHyprColor{}}, int borderWidth = 0);

        void render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) override;

        // Re-decodes from a new path (e.g. swapping an icon) immediately,
        // synchronously - not deferred, same reasoning as the constructor
        // (see this file's own doc comment above).
        void setImage(const std::string& path);

        void setRounding(int rounding) {
            m_rounding = rounding;
        }
        void setBorder(Config::CGradientValueData color, int width) {
            m_borderColor = std::move(color);
            m_borderWidth = width;
        }

        // False if the file was missing/unreadable or failed to decode -
        // the widget still occupies its position (0x0 size unless a fixed
        // w/h override was given) but draws nothing. Lets a caller (or
        // LuaBridge.cpp, at construction) detect and log a failure without
        // this widget type needing to know Lua/luaL_error exist.
        bool loaded() const {
            return m_texture != nullptr;
        }

      private:
        void                       reload();

        std::string                m_path;
        int                        m_rounding;
        Config::CGradientValueData m_borderColor;
        int                        m_borderWidth;
        SP<HyprTexture>            m_texture;
    };

} // namespace HyprLUI
