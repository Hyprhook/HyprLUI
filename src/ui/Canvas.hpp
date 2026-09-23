#pragma once
//
// Canvas.hpp
//
// A Canvas is a rectangular region on screen (screen-space, in pixels) that
// owns and draws a single root Widget - a Lua script typically creates one
// Canvas per GUI element it wants on screen (a HUD, a popup, a bar...) via
// hl.plugin.hyprlui.window{...}.

#include "Widget.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <cstdint>
#include <functional>
#include <optional>
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

    // Point on a monitor's usable box (logicalBoxMinusReserved() - i.e.
    // excluding space already reserved by bars/panels) to anchor a
    // window's corner/edge/center against. See CCanvas::setAnchor().
    enum class EAnchor {
        TopLeft,
        Top,
        TopRight,
        Left,
        Center,
        Right,
        BottomLeft,
        Bottom,
        BottomRight,
    };

    // One edge of a monitor - which edge an exclusive window reserves
    // space along. See CCanvas::setExclusive() and ReservedAreaComposer.hpp
    // (reuses this same type rather than declaring its own).
    enum class EEdge {
        Top,
        Right,
        Bottom,
        Left,
    };

    class CCanvas {
      public:
        CCanvas(std::string name, const Vector2D& position, const Vector2D& size, EZOrder zorder = EZOrder::Overlay) :
            m_name(std::move(name)), m_position(position), m_size(size), m_zorder(zorder) {}

        // Runs layout (measure + arrange over the whole tree) and renders
        // the root widget. Called once per relevant render stage. Also
        // ticks the pending-redamage countdown (see damage() below) even
        // when invisible/rootless, since that's what actually clears stale
        // pixels left behind by a removed/hidden window.
        void render();

        // Marks this canvas's full box dirty (see fullDamageBox() below -
        // not the plain content box() alone) and keeps re-damaging it for
        // the next REDAMAGE_FRAMES real frames. A single call isn't
        // reliably enough - Hyprland's swapchain has multiple buffers and a
        // stale one won't show a change until damage has been present for
        // several consecutive frames (see DESIGN.md's Current state).
        void damage();

        // box() expanded to cover however far the debug overlay currently
        // draws beyond it - some of its labels are deliberately drawn just
        // outside a widget's own box (Widget.cpp's drawDebugOverlay()), so
        // box() alone under-damages them; harmless for an ordinary discrete
        // mutation (the overflow pixels don't change), but visibly ghosts
        // once something keeps changing there every frame (a fade).
        // `m_debugOverflow` is recomputed once per render() from
        // renderDebug()'s returned bounds.
        //
        // Also pads every side by EDGE_ROUNDING_PAD regardless of debug:
        // Hyprland's own damageBox() rounds the final scaled box to integer
        // device pixels, and our own positions are frequently fractional
        // (anchor math), so rounding can lose up to ~1 device pixel on
        // whichever side the fractional remainder rounds away from - but
        // the actual rendered content doesn't go through that same
        // rounding, so that sliver never gets repainted without this pad.
        //
        // Also accounts for the root widget's own slide offset while it's
        // actively in flight (`m_styleOffset`, cached here each frame) -
        // the actual rendered position during a slide is
        // m_position + m_styleOffset, off to one side of the settled box().
        static constexpr double EDGE_ROUNDING_PAD = 2.0;
        CBox                    fullDamageBox() const {
            const double slideLeft = std::max(0.0, -m_styleOffset.x), slideRight = std::max(0.0, m_styleOffset.x);
            const double slideTop = std::max(0.0, -m_styleOffset.y), slideBottom = std::max(0.0, m_styleOffset.y);
            const double left   = m_debugOverflow.left + EDGE_ROUNDING_PAD + slideLeft;
            const double top    = m_debugOverflow.top + EDGE_ROUNDING_PAD + slideTop;
            const double right  = m_debugOverflow.right + EDGE_ROUNDING_PAD + slideRight;
            const double bottom = m_debugOverflow.bottom + EDGE_ROUNDING_PAD + slideBottom;
            return {{m_position.x - left, m_position.y - top}, {m_size.x + left + right, m_size.y + top + bottom}};
        }

        // Whether damage() still has redamage frames pending - used by
        // CUIManager to know when a removed canvas has finished clearing
        // its old footprint and can finally be dropped for good.
        bool hasPendingRedamage() const {
            return m_pendingRedamageFrames > 0;
        }

        void setRoot(PWidget root) {
            m_root = std::move(root);
        }
        CWidget* root() const {
            return m_root.get();
        }

        // Finds the topmost interactive widget at global point `pt`, or
        // nullptr if this canvas is invisible/rootless or nothing there is
        // interactive. `pt` is in the same global compositor-space
        // coordinates m_position already uses.
        CWidget* hitTest(const Vector2D& pt) const {
            return (m_root && m_root->visible()) ? m_root->hitTest(m_position, pt) : nullptr;
        }

        // Creation-order stamp, set once by CUIManager::createCanvas() -
        // used to pick a topmost canvas among several overlapping ones for
        // hit-testing (newest wins).
        void setSequence(uint64_t sequence) {
            m_sequence = sequence;
        }
        uint64_t sequence() const {
            return m_sequence;
        }

        // Reactive bindings (hyprlui.Bind(name) - see LuaBridge.cpp's
        // buildWidget()): a closure per bound widget property, re-run at
        // the top of every render() (even while invisible, so nothing goes
        // stale the instant visibility toggles back on) to pull the
        // referenced watcher's current value and apply it via the widget's
        // own setter. CCanvas doesn't know what a "watcher" is.
        void addBinding(std::function<void()> apply) {
            m_bindings.push_back(std::move(apply));
        }

        // Fires whenever the content-size sync in render() actually
        // changes m_size, with the new size. CCanvas has no idea what this
        // is for - LuaBridge.cpp uses it for exclusive-zone windows to
        // keep their reserved margin tracking live content size.
        void setOnSizeChanged(std::function<void(const Vector2D&)> cb) {
            m_onSizeChanged = std::move(cb);
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

        // Anchors this window to a point on a monitor's usable box instead
        // of a raw global position. `monitorName` is resolved once by the
        // caller and just stored here - recomputeAnchorPosition() re-reads
        // that monitor's current box every render(), so resolution/
        // reserved-area changes self-correct live, but which monitor was
        // chosen never changes after the fact. `offset` pushes inward from
        // whichever edge(s) the anchor touches (a plain nudge for "center").
        void setAnchor(EAnchor anchor, std::string monitorName, const Vector2D& offset) {
            m_anchor        = anchor;
            m_anchorMonitor = std::move(monitorName);
            m_anchorOffset  = offset;
        }

        // Exclusive windows reserve space along `edge`, anchoring against
        // the monitor's reserved margins with their OWN contribution
        // excluded on that one edge (not the raw box, not the full
        // reserved box) - a real layer-shell bar sits flush against the
        // true screen edge, it isn't pushed inward by the space it itself
        // reserves, but still avoids space reserved by anything else.
        // "My own contribution" is just this window's current size along
        // that edge's perpendicular axis, computed live in
        // recomputeAnchorPosition() (see ReservedAreaComposer.hpp / the
        // Architecture section on exclusive zones in DESIGN.md for the
        // known same-edge-stacking gap).
        void setExclusive(EEdge edge) {
            m_exclusiveEdge = edge;
        }

        // No-op if no anchor is set. If the anchor's target monitor is
        // currently unresolvable (unplugged since creation), keeps the
        // last known m_position rather than snapping to (0,0). Returns
        // whether m_position changed.
        bool recomputeAnchorPosition();

        // Clears any anchor set via setAnchor() - recomputeAnchorPosition()
        // becomes a no-op and this window's position is solely driven by
        // explicit setPosition() calls from here on (an explicit position
        // and an anchor are mutually exclusive). Does not clear
        // m_exclusiveEdge - a still-exclusive window repositioned away from
        // its anchor keeps reserving space as if it were still there.
        void clearAnchor() {
            m_anchor.reset();
        }

        // Repositions this window to an explicit global position, clearing
        // any anchor first, and damaging both the old and new footprint. A
        // no-op if the position doesn't actually change.
        void moveTo(const Vector2D& position);

        void setSize(const Vector2D& size) {
            m_size = size;
        }
        const Vector2D& size() const {
            return m_size;
        }

        // Pins this window's size on one or both axes instead of letting it
        // track the root widget's measured content size every frame
        // (size-to-content is the default). render() re-syncs m_size from
        // this plus the root's current measured size every frame, damaging
        // both the old and new box when it changes - without this, a
        // window that grows past its creation-time size (e.g. Bind()ed
        // text) would never get its new content painted, since damage()
        // has no memory of "this got bigger" on its own.
        void setFixedSize(std::optional<double> w, std::optional<double> h) {
            m_fixedW = w;
            m_fixedH = h;
        }

        // Delegates entirely to the root widget's own setVisible()/
        // visible() - a window fading in/out IS its root widget fading in/
        // out, not a second animated value layered on top. Every visibility
        // path (set_canvas_visible, window() creation, removeCanvas(),
        // set_widget_visible, remove_widget) goes through the same
        // CWidget::setVisible()/animateOutThenRemove() mechanism this way.
        // Falls back to a plain bool if there's no root yet (a transient
        // state between createCanvas() and setRoot() within one
        // hyprlui.window() call).
        void setVisible(bool visible) {
            if (m_root) {
                m_root->setVisible(visible);
                return;
            }
            m_visibleFallback = visible;
        }
        bool visible() const {
            return m_root ? m_root->visible() : m_visibleFallback;
        }

        EZOrder zorder() const {
            return m_zorder;
        }

        // See CUIManager::hotReloadVisibility().
        void setHotReload(bool hotReload) {
            m_hotReload = hotReload;
        }
        bool hotReload() const {
            return m_hotReload;
        }

        CBox box() const {
            return {m_position, m_size};
        }

      private:
        // See damage()'s doc comment - matches Hyprland's own CDamageRing
        // depth (3) + 1.
        static constexpr int                 REDAMAGE_FRAMES = 4;

        std::string                          m_name;
        Vector2D                             m_position;
        Vector2D                             m_size;
        std::optional<double>                m_fixedW, m_fixedH;
        EZOrder                              m_zorder;
        bool                                 m_visibleFallback = true; // only consulted while m_root is null - see visible()/setVisible()
        bool                                 m_hotReload       = false;
        PWidget                              m_root;
        std::optional<EAnchor>               m_anchor;
        std::string                          m_anchorMonitor;
        Vector2D                             m_anchorOffset;
        std::optional<EEdge>                 m_exclusiveEdge;
        int                                  m_pendingRedamageFrames = 0;
        std::vector<std::function<void()>>   m_bindings;
        uint64_t                             m_sequence = 0;
        std::function<void(const Vector2D&)> m_onSizeChanged;
        SEdgeInsets                          m_debugOverflow; // how far the debug overlay currently draws beyond box() on each side - see fullDamageBox()
        Vector2D                             m_styleOffset;   // m_root->styleOffset() (slide only), cached here each render() frame for fullDamageBox() to read
    };

    using PCanvas = std::shared_ptr<CCanvas>;

} // namespace HyprLUI
