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

    // Per-side box-model insets shared by `padding` (inset a container's
    // children from its own edges) and `margin` (a widget's own requested
    // space around itself, read by a container's layout - see
    // ContainerWidget.cpp). A plain aggregate on purpose, matching CHyprColor/
    // Vector2D's own construction style elsewhere in this codebase.
    struct SEdgeInsets {
        double top = 0, right = 0, bottom = 0, left = 0;
    };

    // A widget's own debug-overlay config, tri-state per field (nullopt is
    // meaningful and distinct from `false` in two different ways depending
    // on which field it's on - see setDebug()'s doc comment below):
    //
    //   `enabled`  - nullopt = inherit the resolved value from the nearest
    //                ancestor that hasn't walled itself off (see
    //                setDebugCascade()); the tree root inherits `false`.
    //   `show*`    - nullopt = "auto": decide per-frame from this widget's
    //                own size (and, for showZOpacity, whether it's even at
    //                a non-default value) rather than from inheritance.
    //                An explicit true/false here is a hard override that
    //                also cascades to descendants exactly like `enabled`
    //                does, until some deeper widget overrides it again.
    //   `fontSize` - nullopt = inherit (or the tree-root default, 10) -
    //                same inheritance as `enabled`/`show*`, just an int
    //                instead of a bool. Point size for every debug label
    //                this widget (and, via inheritance, its descendants)
    //                draws - id/size/padding/margin/z-opacity text alike,
    //                one shared size rather than a per-category knob.
    //
    // This same struct doubles as both "what a widget itself asked for"
    // (SLuaBridge's parsed spec) and "what's been resolved so far while
    // walking down the tree" (renderDebug()'s `inherited` parameter) -
    // merging one widget's own spec into an inherited one is the same
    // "mine wins if set, else keep theirs" operation either way, see
    // Widget.cpp's resolveDebugSpec().
    struct SDebugSpec {
        std::optional<bool> enabled;
        std::optional<bool> showBox, showPadding, showMargin, showId, showSize, showZOpacity, showHitTarget;
        std::optional<int>  fontSize;
    };

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

            // min/max clamp last, same as CSS - overrides even an explicit
            // fixed size, since "never smaller/larger than this" is a
            // stronger constraint than "this size" once both are given.
            if (m_minW)
                m_size.x = std::max(m_size.x, *m_minW);
            if (m_minH)
                m_size.y = std::max(m_size.y, *m_minH);
            if (m_maxW)
                m_size.x = std::min(m_size.x, *m_maxW);
            if (m_maxH)
                m_size.y = std::min(m_size.y, *m_maxH);
        }

        void arrange() {
            arrangeChildren();
            for (auto& child : m_children)
                child->arrange();
        }

        // `origin` is the top-left corner of the parent in screen-space
        // pixels; implementations should render at origin + m_position.
        // `parentOpacity` is the already-composed (multiplied-together)
        // opacity of every ancestor - see setOpacity()'s doc comment for
        // why multiply, not override. Default recurses into children (in
        // z-index paint order, see paintOrder() below) at their laid-out
        // positions, passing this widget's own composed opacity down -
        // right for containers; leaves override this instead and use the
        // composed value to fade what they actually draw.
        virtual void render(const Vector2D& origin, float parentOpacity = 1.0F) {
            if (!m_visible)
                return;
            const float opacity = parentOpacity * static_cast<float>(m_opacity);
            for (auto* child : paintOrder())
                child->render(origin + m_position, opacity);
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
        // interactive itself, just recurse into children in reverse paint
        // order (see paintOrder() below) - whatever painted last/on top
        // (highest z-index, ties broken by later insertion) is checked
        // first so an overlapping later/higher sibling wins. Only
        // ButtonWidget.hpp/InputWidget.hpp override this to actually match
        // (return `this`) - everything else stays a pure pass-through
        // search, so clicking a HUD's background/label doesn't swallow the
        // click, only clicking an actual interactive widget does.
        virtual CWidget* hitTest(const Vector2D& origin, const Vector2D& point) {
            if (!m_visible)
                return nullptr;

            const Vector2D absOrigin = origin + m_position;
            auto           order     = paintOrder();
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                if (auto* hit = (*it)->hitTest(absOrigin, point))
                    return hit;
            }
            return nullptr;
        }

        // Whether this widget itself is ever a real hitTest() match (i.e.
        // overrides hitTest() to return `this`) - purely descriptive, used
        // only by the debug overlay's hit-target highlight (Widget.cpp) to
        // know which widgets to draw it for. Default false; CButtonWidget/
        // CInputWidget override to true. Deliberately separate from
        // actually calling hitTest() here, which would need a point to
        // test against and could recurse - this just answers "is this
        // widget the KIND of thing that can ever match at all."
        virtual bool isInteractive() const {
            return false;
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

        // Clamps measure()'s result to [min, max] on each axis independently
        // (either bound may be omitted). Applied AFTER setFixedSize()'s
        // override, same precedence CSS gives min/max-width over an
        // explicit width. A leaf whose visual content is itself a texture
        // (CTextNode) draws that texture at its own natural size rather
        // than stretching it to fill a min-widened box - see TextNode.cpp.
        void setMinSize(std::optional<double> w, std::optional<double> h) {
            m_minW = w;
            m_minH = h;
        }
        void setMaxSize(std::optional<double> w, std::optional<double> h) {
            m_maxW = w;
            m_maxH = h;
        }

        // Inset a container's own children from its edges - only
        // CFlexWidget and CInputWidget's auto-owned label currently
        // interpret this (CStackWidget's manual positioning leaves it
        // unused by design - see DESIGN.md Phase 7).
        void setPadding(const SEdgeInsets& padding) {
            m_padding = padding;
        }
        const SEdgeInsets& padding() const {
            return m_padding;
        }

        // A widget's own requested space around itself, read by whichever
        // container is laying it out - only CFlexWidget currently reads a
        // child's margin (see ContainerWidget.cpp); CStackWidget's manual
        // positioning leaves it unused by design, same as padding above.
        void setMargin(const SEdgeInsets& margin) {
            m_margin = margin;
        }
        const SEdgeInsets& margin() const {
            return m_margin;
        }

        // Own opacity in [0, 1], multiplied with every ancestor's own
        // opacity to get what actually reaches render() (see its doc
        // comment) - CSS/Qt/every-toolkit's convention, so a semi-
        // transparent container naturally fades its children too instead
        // of each widget's opacity being independent/absolute.
        void setOpacity(double opacity) {
            m_opacity = opacity;
        }
        double opacity() const {
            return m_opacity;
        }

        // Paint-order override among this widget's OWN siblings (i.e.
        // within its parent's child list) - higher paints later/on top.
        // Ties (including the default, everyone at 0) keep insertion
        // order, so this is a pure additive extension of "later child
        // wins" with no behavior change when unused. Deliberately just a
        // sibling-local reorder, not a full CSS stacking-context system -
        // a low-z-index child of a high-z-index widget still paints
        // "inside" its parent's turn, it can't jump above a different
        // parent entirely.
        void setZIndex(int zIndex) {
            m_zIndex = zIndex;
        }
        int zIndex() const {
            return m_zIndex;
        }

        CBox boxAt(const Vector2D& origin) const {
            return {origin + m_position, m_size};
        }

        // This widget's own debug-overlay request (see SDebugSpec's doc
        // comment for what each field means and how it merges with
        // ancestors). Unset fields (nullopt) don't override anything -
        // they just fall through to whatever's inherited/auto.
        void setDebug(const SDebugSpec& debug) {
            m_debugSpec = debug;
        }
        const SDebugSpec& debugSpec() const {
            return m_debugSpec;
        }

        // Whether this widget's RESOLVED debug config (its own SDebugSpec
        // merged with whatever it inherited - see renderDebug()) becomes
        // the seed its children inherit from (the default, `true`), or
        // whether they instead start completely fresh (`false` - neither
        // this widget's own settings nor anything further up reaches
        // below it) - an escape hatch for "debug this one widget without
        // lighting up its entire subtree," or the reverse, "debug is on
        // above me, but I want this specific branch left alone."
        void setDebugCascade(bool cascade) {
            m_debugCascade = cascade;
        }
        bool debugCascade() const {
            return m_debugCascade;
        }

        // Debug-overlay tree walk - entirely separate from render()/
        // hitTest() (draws box-model outlines/labels for whichever
        // widgets resolve `enabled`, see SDebugSpec), called once per
        // frame from CCanvas::render() AFTER the real render() pass so it
        // always paints on top regardless of any widget's own z-index/
        // opacity (it's diagnostic, not real content - see Widget.cpp).
        // `inherited` is the resolved SDebugSpec accumulated from every
        // ancestor so far; the initial call from CCanvas passes a fresh
        // SDebugSpec{} (i.e. "nothing enabled, everything auto" at the
        // root). Non-virtual and implemented once for every widget type,
        // same reasoning as measure()/arrange() - the box-model
        // information it draws (position/size/padding/margin/id/zIndex/
        // opacity) is entirely made of base CWidget fields, no per-
        // subclass knowledge needed.
        void renderDebug(const Vector2D& origin, const SDebugSpec& inherited);

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
        std::optional<double> m_minW, m_minH, m_maxW, m_maxH;
        SEdgeInsets           m_padding, m_margin;
        double                m_opacity = 1.0;
        int                   m_zIndex  = 0;
        SDebugSpec            m_debugSpec;
        bool                  m_debugCascade = true;
        std::vector<PWidget>  m_children;

      private:
        // Merges m_debugSpec into `inherited` ("mine wins per-field if
        // set, else keep theirs") - see SDebugSpec's doc comment. Defined
        // in Widget.cpp alongside renderDebug()/drawDebugOverlay(), which
        // are the only callers.
        SDebugSpec resolveDebugSpec(const SDebugSpec& inherited) const;

        // Draws this widget's box-model overlay (margin/padding/content
        // outlines, id/size/padding/margin/z-opacity labels, hit-target
        // fill) per `resolved`'s already-merged show* decisions - auto
        // (nullopt) categories are decided here, from this widget's own
        // size (and, for z/opacity, whether it's at a non-default value).
        // `origin` is this widget's PARENT's already-accumulated absolute
        // position, same convention as render()/hitTest(). See Widget.cpp.
        void drawDebugOverlay(const Vector2D& origin, const SDebugSpec& resolved) const;

        // Render()/hitTest()'s shared paint-order: a stable sort of
        // m_children by zIndex() (ascending - lower paints first/behind).
        // Stable so equal-zIndex siblings (the default, everyone at 0) keep
        // plain insertion order - identical to the pre-Phase-7 unsorted
        // walk when zIndex is never set. Recomputed on every render()/
        // hitTest() call rather than cached - cheap at HUD-sized child
        // counts, matches this codebase's established "redo it every
        // frame instead of tracking a dirty flag" precedent (see Widget.
        // hpp's own measure()/arrange(), CFlexWidget's layout, etc.).
        std::vector<CWidget*> paintOrder() const {
            std::vector<CWidget*> order;
            order.reserve(m_children.size());
            for (const auto& child : m_children)
                order.push_back(child.get());
            std::stable_sort(order.begin(), order.end(), [](const CWidget* a, const CWidget* b) { return a->m_zIndex < b->m_zIndex; });
            return order;
        }
    };

} // namespace HyprLUI
