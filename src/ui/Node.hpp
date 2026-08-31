#pragma once
//
// Node.hpp
//
// Base class for every drawable element (text, rects, and whatever you add
// next - images, buttons, progress bars...). A Node knows its own position
// and size relative to its owning Canvas, and knows how to draw itself.
//
// This is the extension point: to add a new widget type, subclass CNode,
// implement render(), and add a convenience factory on UIManager. Nothing
// else in the library needs to change.

#include <hyprland/src/helpers/math/Math.hpp>

#include <memory>
#include <string>

namespace HyprLUI {

    class CNode {
      public:
        CNode(std::string id, const Vector2D& position, const Vector2D& size) :
            m_id(std::move(id)), m_position(position), m_size(size) {}

        virtual ~CNode() = default;

        // `origin` is the top-left corner of the owning canvas in
        // screen-space pixels; implementations should render at
        // origin + m_position.
        virtual void render(const Vector2D& origin) = 0;

        const std::string& id() const {
            return m_id;
        }

        void setVisible(bool visible) {
            m_visible = visible;
        }
        bool visible() const {
            return m_visible;
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

        // Bounding box relative to a canvas origin - handy for damage
        // tracking and future hit-testing (clicks, hover, etc.).
        CBox boxAt(const Vector2D& origin) const {
            return {origin + m_position, m_size};
        }

      protected:
        std::string m_id;
        Vector2D    m_position;
        Vector2D    m_size;
        bool        m_visible = true;
    };

    using PNode = std::shared_ptr<CNode>;

} // namespace HyprLUI
