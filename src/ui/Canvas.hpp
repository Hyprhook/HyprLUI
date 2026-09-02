#pragma once
//
// Canvas.hpp
//
// A Canvas is a rectangular region on screen (screen-space, in pixels) that
// owns and draws a single root Widget - a Lua script will typically create
// one Canvas per GUI element it wants on screen (a HUD, a popup, a bar...)
// via hl.plugin.hyprlui.window{...}. Renamed to "Window" once anchors +
// monitor targeting land (DESIGN.md Phase 2) - for now it's still a plain
// absolute-position box.

#include "Widget.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <string>

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

        // Runs layout (measure + arrange over the whole tree) and renders
        // the root widget. Called once per relevant render stage.
        void render();

        // Marks this canvas's full box dirty so Hyprland schedules a repaint
        // that actually includes it. Hyprland only recomposites/presents
        // damaged regions - drawing into the framebuffer during render()
        // without this is not enough to make it show up on screen. Call
        // after construction and after any mutation of the tree (the Lua
        // bridge does this for you on every mutating call).
        void damage() const;

        void setRoot(PWidget root) {
            m_root = std::move(root);
        }
        CWidget* root() const {
            return m_root.get();
        }

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
        std::string m_name;
        Vector2D    m_position;
        Vector2D    m_size;
        EZOrder     m_zorder;
        bool        m_visible = true;
        PWidget     m_root;
    };

    using PCanvas = std::shared_ptr<CCanvas>;

} // namespace HyprLUI
