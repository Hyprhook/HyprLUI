#pragma once
//
// UIManager.hpp
//
// Singleton owning every Canvas. This is the surface intended to be exposed
// to Lua later on: a Lua binding layer (see LuaBridge.hpp) just needs to
// call these methods, so keep this the single "public API" of the library
// and route everything else through it rather than poking Canvas/Node
// directly from main.cpp or a future Lua glue file.

#include "Canvas.hpp"
#include "Node.hpp"
#include "RectNode.hpp"
#include "TextNode.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <unordered_map>

namespace HyprLUI {

    class CUIManager {
      public:
        static CUIManager& get();

        // --- Canvas management ---------------------------------------
        PCanvas createCanvas(const std::string& name, const Vector2D& position, const Vector2D& size, EZOrder zorder = EZOrder::Overlay);
        void    removeCanvas(const std::string& name);
        bool    hasCanvas(const std::string& name) const;
        PCanvas getCanvas(const std::string& name) const;

        // --- Node convenience factories --------------------------------
        // These are thin sugar over Canvas::addNode + the node
        // constructors, meant to be the kind of one-call functions a Lua
        // binding will wrap 1:1 (e.g. hyprlui.add_text(canvas, id, x, y, ...)).
        PNode addText(const std::string& canvasName, const std::string& id, const Vector2D& position, const std::string& text, int pointSize = 16, CHyprColor color = CHyprColor{1.0, 1.0, 1.0, 1.0});
        PNode addRect(const std::string& canvasName, const std::string& id, const Vector2D& position, const Vector2D& size, CHyprColor color, int rounding = 0);

        // --- Frame lifecycle --------------------------------------------
        // Called from the "render" hook once per relevant render stage.
        void renderOverlay();
        void renderBackground();

        void clear();

      private:
        CUIManager()  = default;
        ~CUIManager() = default;

        std::unordered_map<std::string, PCanvas> m_canvases;
    };

} // namespace HyprLUI
