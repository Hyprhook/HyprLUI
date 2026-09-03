#pragma once
//
// Widget.hpp
//
// Base class for every element in the UI tree - both leaves (text, rects,
// later buttons/inputs) and containers (Stack/Row/Column). A Window/Canvas
// owns a single root Widget instead of a flat node list; layout runs as a
// two-pass measure() (bottom-up, natural sizes) then arrange() (top-down,
// final positions) walk before render().
//
// Extension point: to add a new leaf, subclass CWidget, implement render()
// and optionally measureContent(). To add a new container, additionally
// implement arrangeChildren() (see ContainerWidget.hpp for Stack/Row/Column).

#include <hyprland/src/helpers/math/Math.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace HyprLUI {

    class CWidget;
    using PWidget = std::shared_ptr<CWidget>;

    class CWidget {
      public:
        CWidget(std::string id, const Vector2D& position = {0, 0}) : m_id(std::move(id)), m_position(position) {}

        virtual ~CWidget() = default;

        // Two-pass layout, run once per render over the whole tree before
        // any render() call - see measureContent()/arrangeChildren() below
        // for the per-widget-type hooks. Both are non-virtual on purpose:
        // every widget gets the "measure children first, then self" /
        // "position children, then let them lay out their own children"
        // ordering for free, and only needs to override the hook relevant
        // to its own behavior.
        void measure() {
            for (auto& child : m_children)
                child->measure();
            measureContent();
            if (m_fixedW)
                m_size.x = *m_fixedW;
            if (m_fixedH)
                m_size.y = *m_fixedH;
        }

        void arrange() {
            arrangeChildren();
            for (auto& child : m_children)
                child->arrange();
        }

        // `origin` is the top-left corner of the parent in screen-space
        // pixels; implementations should render at origin + m_position.
        // Default recurses into children at their laid-out positions -
        // right for containers; leaves override this instead.
        virtual void render(const Vector2D& origin) {
            if (!m_visible)
                return;
            for (auto& child : m_children)
                child->render(origin + m_position);
        }

        void addChild(PWidget child) {
            m_children.push_back(std::move(child));
        }

        // Recursive id lookup (self included), depth-first. Returns nullptr
        // if not found anywhere in the subtree.
        CWidget* findWidget(const std::string& id) {
            if (m_id == id)
                return this;
            for (auto& child : m_children) {
                if (auto* found = child->findWidget(id))
                    return found;
            }
            return nullptr;
        }

        // Finds the topmost interactive widget whose bounds contain
        // `point`, searching this widget's subtree. `origin` is this
        // widget's PARENT's already-accumulated absolute position (same
        // convention as render()'s origin parameter). Default: not
        // interactive itself, just recurse into children - later/topmost-
        // painted children are checked first so an overlapping later
        // sibling wins. Only ButtonWidget.hpp overrides this to actually
        // match (return `this`) - everything else stays a pure pass-
        // through search, so clicking a HUD's background/label doesn't
        // swallow the click, only clicking an actual Button does.
        virtual CWidget* hitTest(const Vector2D& origin, const Vector2D& point) {
            if (!m_visible)
                return nullptr;

            const Vector2D absOrigin = origin + m_position;
            for (auto it = m_children.rbegin(); it != m_children.rend(); ++it) {
                if (auto* hit = (*it)->hitTest(absOrigin, point))
                    return hit;
            }
            return nullptr;
        }

        // Recursive removal by id, starting from this widget's children.
        // Returns true if something was removed.
        bool removeChild(const std::string& id) {
            auto it = std::find_if(m_children.begin(), m_children.end(), [&id](const PWidget& c) { return c->id() == id; });
            if (it != m_children.end()) {
                m_children.erase(it);
                return true;
            }
            for (auto& child : m_children) {
                if (child->removeChild(id))
                    return true;
            }
            return false;
        }

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

        // Pins this widget's laid-out size instead of letting measure()
        // derive it from content/children ("size-to-content" is the
        // default - passing a value here overrides one or both axes).
        void setFixedSize(std::optional<double> w, std::optional<double> h) {
            m_fixedW = w;
            m_fixedH = h;
        }

        CBox boxAt(const Vector2D& origin) const {
            return {origin + m_position, m_size};
        }

      protected:
        // Sets m_size from this widget's own content/children. Default:
        // leave m_size as-is (right for leaves that already know their
        // size, e.g. CRectNode). Containers override this to derive their
        // size from already-measured children (measure() guarantees
        // children are measured first).
        virtual void measureContent() {}

        // Positions m_children (their setPosition()) based on this
        // widget's own m_size. Default: no-op - right for leaves and for
        // CStackWidget, whose children keep whatever absolute position
        // they were given. Flex containers override this.
        virtual void          arrangeChildren() {}

        std::string           m_id;
        Vector2D              m_position;
        Vector2D              m_size;
        bool                  m_visible = true;
        std::optional<double> m_fixedW, m_fixedH;
        std::vector<PWidget>  m_children;
    };

} // namespace HyprLUI
