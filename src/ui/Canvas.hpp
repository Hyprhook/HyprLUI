#pragma once
//
// Canvas.hpp
//
// A Canvas is a rectangular region on screen (screen-space, in pixels) that
// owns and draws an ordered list of Nodes. Think of it as a single "window"
// or panel of your future GUI - a Lua script will typically create one
// Canvas per GUI element it wants on screen.

#include "Node.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <string>
#include <vector>

namespace HyprLUI {

    // Controls when in the render pipeline the canvas gets drawn.
    //   Overlay    - drawn last, on top of everything (windows, layers,
    //                fullscreen apps). Right for HUD-style GUIs.
    //   Background - drawn before windows, so windows/layers can occlude it.
    enum class EZOrder {
        Overlay,
        Background,
    };

    class CCanvas {
      public:
        CCanvas(std::string name, const Vector2D& position, const Vector2D& size, EZOrder zorder = EZOrder::Overlay) :
            m_name(std::move(name)), m_position(position), m_size(size), m_zorder(zorder) {}

        void render();

        // Marks this canvas's full box dirty so Hyprland schedules a repaint
        // that actually includes it. Hyprland only recomposites/presents
        // damaged regions - drawing into the framebuffer during render()
        // without this is not enough to make it show up on screen. Called
        // automatically by CUIManager on create/add/remove; call it
        // yourself too after any change that isn't already covered by one
        // of those (e.g. mutating a node in place via findNode()).
        void damage() const;

        PNode addNode(PNode node);
        void  removeNode(const std::string& id);
        PNode findNode(const std::string& id) const;

        const std::string& name() const {
            return m_name;
        }

        void setPosition(const Vector2D& position) {
            m_position = position;
        }
        const Vector2D& position() const {
            return m_position;
        }

        void setSize(const Vector2D& size) {
            m_size = size;
        }
        const Vector2D& size() const {
            return m_size;
        }

        void setVisible(bool visible) {
            m_visible = visible;
        }
        bool visible() const {
            return m_visible;
        }

        EZOrder zorder() const {
            return m_zorder;
        }

        CBox box() const {
            return {m_position, m_size};
        }

      private:
        std::string        m_name;
        Vector2D            m_position;
        Vector2D            m_size;
        EZOrder             m_zorder;
        bool                m_visible = true;
        std::vector<PNode>  m_nodes;
    };

    using PCanvas = std::shared_ptr<CCanvas>;

} // namespace HyprLUI
