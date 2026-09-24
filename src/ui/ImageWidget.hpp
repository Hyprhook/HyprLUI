#pragma once
//
// ImageWidget.hpp
//
// Loads an image file (PNG/JPG/WEBP/SVG/AVIF/JXL) and draws it as a
// texture over the inherited CRectNode fill (default transparent).
// Size-to-content by default; a fixed w/h stretches the image, unlike
// CTextNode. Decoding is eager/synchronous, in the constructor and on
// every setImage().

#include "RectNode.hpp"
#include "../render/gfx.hpp"

#include <string>

namespace HyprLUI {

    // hints["image-data"]'s shape (freedesktop Notifications spec) -
    // already-decoded raw pixel bytes, not an encoded file. See
    // gfx::makeImageTexture()'s pixel-buffer overload for the actual
    // decode/convert work; this struct is just the parameter bundle.
    struct SPixelSpec {
        int         width = 0, height = 0, rowstride = 0, channels = 4;
        bool        hasAlpha = true;
        std::string dataBase64;
    };

    class CImageWidget : public CRectNode {
      public:
        CImageWidget(std::string id, const Vector2D& position, std::string path, CHyprColor color = CHyprColor{0.0, 0.0, 0.0, 0.0}, int rounding = 0,
                     Config::CGradientValueData borderColor = Config::CGradientValueData{CHyprColor{}}, int borderWidth = 0);

        // Raw-pixel-buffer construction - a one-shot decode, unlike the
        // path-based constructor above; there's no setPixels() to go with
        // setImage() below since nothing currently needs to re-set a
        // pixel-buffer image after construction (each notification card
        // just builds a fresh one).
        CImageWidget(std::string id, const Vector2D& position, const SPixelSpec& pixels, CHyprColor color = CHyprColor{0.0, 0.0, 0.0, 0.0}, int rounding = 0,
                     Config::CGradientValueData borderColor = Config::CGradientValueData{CHyprColor{}}, int borderWidth = 0);

        void render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) override;

        // Re-decodes from a new path (e.g. swapping an icon) immediately,
        // synchronously - not deferred.
        void setImage(const std::string& path);

        // False if the path failed to decode - fill/border still draw, just no texture.
        bool loaded() const {
            return m_texture != nullptr;
        }

      private:
        void            reload();

        std::string     m_path;
        SP<HyprTexture> m_texture;
    };

} // namespace HyprLUI
