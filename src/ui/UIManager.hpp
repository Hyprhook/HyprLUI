#pragma once
//
// UIManager.hpp
//
// Singleton owning every Canvas. This is the surface intended to be exposed
// to Lua: a Lua binding layer (see LuaBridge.hpp) builds a Widget tree
// (Stack/Row/Column/Text/Box - see ContainerWidget.hpp/RectNode.hpp/
// TextNode.hpp) and attaches it to a Canvas via these lifecycle calls. Keep
// this the single "public API" of the library and route everything else
// through it rather than poking Canvas/Widget directly from main.cpp.

#include "Canvas.hpp"
#include "Widget.hpp"

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
