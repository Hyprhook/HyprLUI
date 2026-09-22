#pragma once
//
// ImageWidget.hpp
//
// Loads an image file (PNG/JPG/WEBP/SVG/AVIF/JXL - whatever the installed
// libhyprgraphics supports) and draws it as a texture. Size-to-content by
// default; an explicit fixed w/h scales/stretches the image to fill that
// box - deliberately unlike CTextNode, which never stretches its glyphs
// (stretching a photo/icon to a requested size is the expected default,
// matching plain CSS `<img>` sizing).
//
// Loading is EAGER (decoded synchronously in the constructor and again on
// every setImage() call), unlike CTextNode's lazy-on-first-measure()
// texture cache - a bad path/unsupported format is a real, likely-common
// config mistake, and eager decoding is what lets LuaBridge.cpp check
// loaded() and log a warning immediately at window-build time.

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
        // synchronously - not deferred.
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
