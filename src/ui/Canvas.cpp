#include "Canvas.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>
#include <cmath>

namespace HyprLUI {

    void CCanvas::render() {
        // Bindings first - they call setters like CTextNode::setText(),
        // which just mark content dirty. measure() (next) is what actually
        // rebuilds/re-measures against the new content, so it has to run
        // after this, not before.
        for (auto& binding : m_bindings)
            binding();

        if (m_root) {
            // Full-tree layout every frame - HUD-sized content, no need
            // for a layout-dirty flag.
            m_root->measure();

            // Sync this window's own outer size to what the tree actually
            // measured (size-to-content axes only - setFixedSize()'d axes
            // stay pinned) - see setFixedSize()'s doc comment for why this
            // has to happen every frame.
            const Vector2D contentSize = resolveSpan({m_fixedW ? *m_fixedW : m_root->size().x, m_fixedH ? *m_fixedH : m_root->size().y});
            if (contentSize.x != m_size.x || contentSize.y != m_size.y) {
                gfx::damageBox(fullDamageBox()); // old footprint, in case it shrunk
                m_size = contentSize;
                damage(); // new footprint (at the still-current position) + arms the redamage countdown
                if (m_onSizeChanged)
                    m_onSizeChanged(m_size);
            }

            // Root-fills-canvas: if the root widget wants to fill its
            // window (CWidget::fill()) and this axis has a determinate
            // size (m_fixedW/H OR spanWidth/Height set - both make m_size
            // authoritative rather than content-derived), force the
            // root's just-measured size on that axis to match m_size
            // instead of leaving it at whatever it naturally measured. An
            // auto-sized axis has nothing determinate to fill and stays
            // untouched. Runs after the sync above so it reads m_size's
            // settled value.
            if (m_root->fill()) {
                Vector2D size = m_root->size();
                if (m_fixedW || m_spanWidth)
                    size.x = m_size.x;
                if (m_fixedH || m_spanHeight)
                    size.y = m_size.y;
                m_root->setSize(size);
            }
        }

        // The root widget's own slide-style offset, if any (CWidget::
        // styleOffset() - {0,0} for popin/gnome/no-style, those are a
        // scale handled by popinTransform() instead). Cached here (not
        // read inline where used below) so fullDamageBox() - used by the
        // redamage tick just below, which also runs while invisible - has
        // a value ready before this frame's real render happens. Never
        // added to the origin passed to m_root->render() below - that
        // would double-apply it, since root's own render() already
        // applies this internally like any other widget.
        m_styleOffset = m_root ? m_root->styleOffset() : Vector2D{0, 0};

        // Anchor tracking (if any) is re-resolved every frame too - cheap,
        // and keeps the window correctly placed across resolution/
        // reserved-area changes, or a content-size change just above for
        // anchors whose position depends on size. Damages old + new box
        // itself when position actually moves.
        recomputeAnchorPosition();

        // Tick the redamage countdown regardless of visibility/root - this
        // is what actually clears stale pixels for a hidden/removed canvas
        // that isn't going to draw anything this frame.
        if (m_pendingRedamageFrames > 0) {
            gfx::damageBox(fullDamageBox());
            --m_pendingRedamageFrames;
        }

        if (!m_root || !m_root->visible())
            return;

        // An animation changes rendered opacity (and/or position) every
        // frame for its whole duration - keep re-damaging every frame
        // while anything in this window's tree is still animating, same
        // reasoning as damage()'s own multi-frame redamage countdown, just
        // driven continuously. Also what keeps a removeCanvas()'d, fading-
        // out canvas alive in CUIManager's m_pendingRemoval for as long as
        // its animation takes.
        if (m_root->isAnimating())
            damage();

        m_root->arrange();
        // No separate canvas-level opacity/scale multiplier - the root
        // widget's own composedOpacity()/styleOffset()/popinTransform()
        // already fold in its own animation progress, root included - a
        // window's root is just an ordinary widget as far as any of this
        // is concerned.
        m_root->render(m_position, 1.0F);

        // Debug overlay is an entirely separate pass, run after normal
        // content so it always paints on top regardless of z-index/
        // opacity. A fresh SDebugSpec{} means "nothing enabled, everything
        // auto" at the root.
        const auto debugBounds = m_root->renderDebug(m_position, {});

        // Recompute how far that overlay currently extends beyond box() on
        // each side, for fullDamageBox() to use on the NEXT damage() call
        // (one-frame-stale, self-correcting). No overlay active anywhere
        // this frame means zero overflow.
        m_debugOverflow = {};
        if (debugBounds) {
            const auto b           = box();
            m_debugOverflow.left   = std::max(0.0, b.pos().x - debugBounds->pos().x);
            m_debugOverflow.top    = std::max(0.0, b.pos().y - debugBounds->pos().y);
            m_debugOverflow.right  = std::max(0.0, (debugBounds->pos().x + debugBounds->size().x) - (b.pos().x + b.size().x));
            m_debugOverflow.bottom = std::max(0.0, (debugBounds->pos().y + debugBounds->size().y) - (b.pos().y + b.size().y));
        }
    }

    void CCanvas::damage() {
        gfx::damageBox(fullDamageBox());
        m_pendingRedamageFrames = REDAMAGE_FRAMES;
    }

    Vector2D CCanvas::resolveSpan(Vector2D size) const {
        if ((!m_spanWidth && !m_spanHeight) || !m_anchor)
            return size;

        auto monitor = State::CMonitorQuery{*State::monitorState()}.name(m_anchorMonitor).run();
        if (!monitor)
            return size;

        // The monitor's raw box, not logicalBoxMinusReserved() - spanning
        // is measured against the true screen edges (minus monitorPadding
        // only), regardless of what else is currently reserved.
        const auto box = monitor->logicalBox();
        if (m_spanWidth)
            size.x = box.size().x - m_monitorPadding.left - m_monitorPadding.right;
        if (m_spanHeight)
            size.y = box.size().y - m_monitorPadding.top - m_monitorPadding.bottom;
        return size;
    }

    bool CCanvas::recomputeAnchorPosition() {
        if (!m_anchor)
            return false;

        auto monitor = State::CMonitorQuery{*State::monitorState()}.name(m_anchorMonitor).run();
        if (!monitor)
            return false;

        // Read the monitor's live combined reserved margins (config
        // baseline + every HyprLUI exclusive window's contribution +
        // Hyprland's own layer-shell/error-overlay contributions -
        // CReservedArea::calculate() already sums all of that). If this
        // window is itself exclusive, exclude only its own contribution
        // from whichever single edge it reserves - lets an exclusive
        // window sit at its own natural position while still avoiding
        // everything else already reserved, unlike the unmodified
        // logicalBoxMinusReserved() every other window uses (which would
        // self-referentially push this window inward by the space it
        // itself just reserved).
        double top    = monitor->m_reservedArea.top();
        double right  = monitor->m_reservedArea.right();
        double bottom = monitor->m_reservedArea.bottom();
        double left   = monitor->m_reservedArea.left();

        if (m_exclusiveEdge) {
            switch (*m_exclusiveEdge) {
                case EEdge::Top: top = std::max(0.0, top - m_size.y); break;
                case EEdge::Right: right = std::max(0.0, right - m_size.x); break;
                case EEdge::Bottom: bottom = std::max(0.0, bottom - m_size.y); break;
                case EEdge::Left: left = std::max(0.0, left - m_size.x); break;
            }
        }

        // Same math as CReservedArea::apply(logicalBox()) - just against
        // the margins above instead of the monitor's own live totals
        // unmodified, since those may include this window's own
        // contribution.
        const auto rawBox = monitor->logicalBox();
        const CBox box{rawBox.pos().x + left, rawBox.pos().y + top, rawBox.size().x - left - right, rawBox.size().y - top - bottom};
        const auto boxPos  = box.pos();
        const auto boxSize = box.size();

        Vector2D   pos;
        switch (*m_anchor) {
            case EAnchor::TopLeft: pos = {boxPos.x + m_anchorOffset.x, boxPos.y + m_anchorOffset.y}; break;
            case EAnchor::Top: pos = {boxPos.x + (boxSize.x - m_size.x) / 2.0 + m_anchorOffset.x, boxPos.y + m_anchorOffset.y}; break;
            case EAnchor::TopRight: pos = {boxPos.x + boxSize.x - m_size.x - m_anchorOffset.x, boxPos.y + m_anchorOffset.y}; break;
            case EAnchor::Left: pos = {boxPos.x + m_anchorOffset.x, boxPos.y + (boxSize.y - m_size.y) / 2.0 + m_anchorOffset.y}; break;
            case EAnchor::Center: pos = {boxPos.x + (boxSize.x - m_size.x) / 2.0 + m_anchorOffset.x, boxPos.y + (boxSize.y - m_size.y) / 2.0 + m_anchorOffset.y}; break;
            case EAnchor::Right: pos = {boxPos.x + boxSize.x - m_size.x - m_anchorOffset.x, boxPos.y + (boxSize.y - m_size.y) / 2.0 + m_anchorOffset.y}; break;
            case EAnchor::BottomLeft: pos = {boxPos.x + m_anchorOffset.x, boxPos.y + boxSize.y - m_size.y - m_anchorOffset.y}; break;
            case EAnchor::Bottom: pos = {boxPos.x + (boxSize.x - m_size.x) / 2.0 + m_anchorOffset.x, boxPos.y + boxSize.y - m_size.y - m_anchorOffset.y}; break;
            case EAnchor::BottomRight: pos = {boxPos.x + boxSize.x - m_size.x - m_anchorOffset.x, boxPos.y + boxSize.y - m_size.y - m_anchorOffset.y}; break;
        }

        // Round to a whole pixel: Hyprland samples textures with
        // GL_LINEAR unless the destination is an exact 1:1 pixel match, so
        // a fractional position (common here - `/2.0` centering lands on
        // one whenever (boxSize - m_size) is odd) blends two adjacent
        // texels per pixel instead of reading one exactly - visibly blurry
        // for text/images, though invisible on a solid-color rect.
        pos.x = std::round(pos.x);
        pos.y = std::round(pos.y);

        if (pos.x == m_position.x && pos.y == m_position.y)
            return false;

        // Damage both where the window WAS and where it's about to be - a
        // plain damage() after moving only covers the new box, leaving the
        // old one's pixels stale. fullDamageBox() (not the plain box()) so
        // the debug overlay's own overflow moves with it too.
        const auto oldBox = this->fullDamageBox();
        m_position        = pos;
        gfx::damageBox(oldBox);
        damage();
        return true;
    }

    void CCanvas::moveTo(const Vector2D& position) {
        clearAnchor();

        if (position.x == m_position.x && position.y == m_position.y)
            return;

        // Same old+new damage dance as recomputeAnchorPosition() above.
        const auto oldBox = fullDamageBox();
        m_position        = position;
        gfx::damageBox(oldBox);
        damage();
    }

} // namespace HyprLUI
