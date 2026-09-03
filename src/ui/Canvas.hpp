#pragma once
//
// Canvas.hpp
//
// A Canvas is a rectangular region on screen (screen-space, in pixels) that
// owns and draws a single root Widget - a Lua script will typically create
// one Canvas per GUI element it wants on screen (a HUD, a popup, a bar...)
// via hl.plugin.hyprlui.window{...}. Renamed to "Window" once exclusive
// zones land (DESIGN.md Phase 5) - for now it's still just CCanvas.

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
    // space along. See CCanvas::setExclusive() and
    // ReservedAreaComposer.hpp (which reuses this same type rather than
    // declaring its own, since it's fundamentally the same "which edge"
    // concept as EAnchor above).
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

        // Marks this canvas's full box dirty so Hyprland schedules a
        // repaint that actually includes it, AND keeps re-damaging that
        // same box for the next REDAMAGE_FRAMES real frames (via render()
        // above). A single damage() call is not reliably enough: Hyprland
        // renders into a rotating set of swapchain buffers, and a buffer
        // that's currently N frames stale won't show this change until
        // damage has been present for N consecutive frames (Monitor::
        // CDamageRing, src/output/DamageRing.hpp in Hyprland - buffers can
        // be up to DAMAGE_RING_PREVIOUS_LEN=3 frames stale, so 4 frames of
        // continuous damage covers every buffer in rotation). Without this,
        // mutating already-visible content intermittently flickers/ghosts
        // old + new content until something unrelated happens to repaint
        // that region - Hyprland's own dynamic overlay (NotificationOverlay)
        // sidesteps this by damaging unconditionally on every draw while
        // visible; we do the bounded equivalent since our content is
        // mostly static between mutations. Call after construction and
        // after any mutation of the tree (the Lua bridge does this for you
        // on every mutating call).
        void damage();

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

        // Finds the topmost interactive widget (currently: only
        // ButtonWidget.hpp's CButtonWidget can actually match) at global
        // point `pt`, or nullptr if this canvas is invisible/rootless or
        // nothing there is interactive. See CWidget::hitTest() - `pt` is
        // in the same global compositor-space coordinates m_position
        // already uses (confirmed: Hyprland's own pointer-position
        // accessor returns this same space, no per-monitor transform
        // needed here unlike rendering's toMonitorLocal()).
        CWidget* hitTest(const Vector2D& pt) const {
            return (m_visible && m_root) ? m_root->hitTest(m_position, pt) : nullptr;
        }

        // Creation-order stamp, set once by CUIManager::createCanvas() -
        // used to pick a topmost canvas among several overlapping ones
        // for hit-testing (newest wins). Not meant to be touched anywhere
        // else.
        void setSequence(uint64_t sequence) {
            m_sequence = sequence;
        }
        uint64_t sequence() const {
            return m_sequence;
        }

        // Reactive bindings (hyprlui.Bind(name) - see LuaBridge.cpp's
        // buildWidget()): a closure per bound widget property, re-run at
        // the top of every render() (even while invisible, so nothing
        // goes stale the instant visibility is toggled back on) to pull
        // the referenced watcher's current value and apply it via the
        // widget's own (already dirty-checked) setter. CCanvas doesn't
        // know or care what a "watcher" is - LuaBridge.cpp owns that.
        void addBinding(std::function<void()> apply) {
            m_bindings.push_back(std::move(apply));
        }

        // Fires whenever the content-size sync in render() (see
        // setFixedSize()'s doc comment) actually changes m_size, with the
        // new size - after m_size itself has already been updated.
        // CCanvas has no idea what this is for; LuaBridge.cpp uses it for
        // exclusive-zone windows (see ReservedAreaComposer) to keep their
        // reserved margin tracking their live (possibly Bind()ed/dynamic)
        // content size, same "generic hook, Lua-specific glue lives
        // elsewhere" pattern as addBinding() above.
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
        // of a raw global position. `monitorName` is resolved ONCE by the
        // caller (see LuaBridge.cpp's luaWindow(), which uses Hyprland's
        // own monitor-selector syntax to pick it) and just stored here -
        // recomputeAnchorPosition() re-looks-up that name every render()
        // and recomputes m_position from the monitor's *current* box, so
        // resolution/reserved-area changes on that monitor self-correct
        // live, but which monitor was chosen never changes after the fact
        // (no "window teleports when you focus a different screen").
        // `offset` pushes inward from whichever edge(s) the anchor touches
        // (top/right/bottom/left all use a positive offset to mean "move
        // toward center") - for "center" it's a plain x/y nudge.
        void setAnchor(EAnchor anchor, std::string monitorName, const Vector2D& offset) {
            m_anchor        = anchor;
            m_anchorMonitor = std::move(monitorName);
            m_anchorOffset  = offset;
        }

        // Exclusive windows (see ReservedAreaComposer) reserve space along
        // `edge` - so they anchor against the monitor's reserved margins
        // with THEIR OWN contribution excluded on that one edge, not the
        // raw box and not the full reserved box. A real layer-shell bar
        // sits flush against the true screen edge - it isn't pushed
        // inward by the space *it itself* reserves - but it still
        // correctly avoids space reserved by anything else (the user's
        // own config baseline, Hyprland's own error/debug overlay, other
        // HyprLUI exclusive windows on a *different* edge). Since a
        // window only ever contributes to one edge, "my own contribution"
        // is just this window's own current size along that edge's
        // perpendicular axis (m_size.y for top/bottom, m_size.x for left/
        // right) - computed live in recomputeAnchorPosition(), not stored
        // separately, same "redo it every frame" reasoning as everything
        // else here. v1 gap this does NOT solve: multiple HyprLUI
        // exclusive windows on the SAME edge don't stack relative to each
        // other (each excludes only itself, so each positions as if it
        // were the only one) - see DESIGN.md.
        void setExclusive(EEdge edge) {
            m_exclusiveEdge = edge;
        }

        // No-op if no anchor is set. If the anchor's target monitor is
        // currently unresolvable (unplugged since creation), keeps the
        // last known m_position rather than snapping to (0,0) - known v1
        // limitation, see DESIGN.md. Returns whether m_position changed.
        bool recomputeAnchorPosition();

        void setSize(const Vector2D& size) {
            m_size = size;
        }
        const Vector2D& size() const {
            return m_size;
        }

        // Pins this window's size on one or both axes instead of letting
        // it track the root widget's measured content size every frame
        // ("size-to-content" is the default - passing a value here
        // overrides one or both axes). Mirrors CWidget::setFixedSize() -
        // same idea, one level up. render() re-syncs m_size from this +
        // the root's current measured size every frame (damaging both the
        // old and new box when it actually changes) - without this, a
        // window that grows past its creation-time size (e.g. Bind()ed
        // text going from one digit to two) would have its damage box
        // permanently under-cover the new content: it wouldn't flicker
        // and self-heal like the bug damage() itself guards against, it'd
        // just never get painted at all, since damage() has no memory of
        // "this got bigger" without m_size itself being kept current.
        void setFixedSize(std::optional<double> w, std::optional<double> h) {
            m_fixedW = w;
            m_fixedH = h;
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
        // See damage()'s doc comment for why this exists and this specific
        // value - matches Hyprland's own CDamageRing depth (3) + 1.
        static constexpr int                 REDAMAGE_FRAMES = 4;

        std::string                          m_name;
        Vector2D                             m_position;
        Vector2D                             m_size;
        std::optional<double>                m_fixedW, m_fixedH;
        EZOrder                              m_zorder;
        bool                                 m_visible = true;
        PWidget                              m_root;
        std::optional<EAnchor>               m_anchor;
        std::string                          m_anchorMonitor;
        Vector2D                             m_anchorOffset;
        std::optional<EEdge>                 m_exclusiveEdge;
        int                                  m_pendingRedamageFrames = 0;
        std::vector<std::function<void()>>   m_bindings;
        uint64_t                             m_sequence = 0;
        std::function<void(const Vector2D&)> m_onSizeChanged;
    };

    using PCanvas = std::shared_ptr<CCanvas>;

} // namespace HyprLUI
