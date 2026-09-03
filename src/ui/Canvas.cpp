#include "Canvas.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>

namespace HyprLUI {

    void CCanvas::render() {
        // Bindings first - they call setters like CTextNode::setText(),
        // which just mark content dirty. measure() (next) is what actually
        // rebuilds/re-measures against the NEW content, so it has to run
        // after this, not before.
        for (auto& binding : m_bindings)
            binding();

        if (m_root) {
            // Full-tree layout every frame - HUD-sized content, no need
            // for a layout-dirty flag (unlike CTextNode's texture cache,
            // which is the expensive part and stays cached).
            m_root->measure();

            // Sync this window's own outer size to what the tree actually
            // measured (size-to-content axes only - setFixedSize()'d axes
            // stay pinned). Without this, m_size is frozen at whatever it
            // was at creation forever, and every position/damage
            // calculation below keeps using that stale value - see
            // setFixedSize()'s doc comment for what that breaks.
            const Vector2D contentSize{m_fixedW ? *m_fixedW : m_root->size().x, m_fixedH ? *m_fixedH : m_root->size().y};
            if (contentSize.x != m_size.x || contentSize.y != m_size.y) {
                gfx::damageBox(box()); // old footprint, in case it shrunk
                m_size = contentSize;
                damage(); // new footprint (at the still-current position) + arms the redamage countdown
                if (m_onSizeChanged)
                    m_onSizeChanged(m_size);
            }
        }

        // Anchor tracking (if any) is re-resolved every frame too - cheap
        // (a handful of monitors, one name comparison each) and keeps the
        // window correctly placed across resolution/reserved-area changes
        // on its already-chosen monitor, or a content-size change just
        // above for anchors (e.g. bottom-right) whose position depends on
        // size. Damages old + new box itself when position actually moves.
        recomputeAnchorPosition();

        // Tick the redamage countdown regardless of visibility/root - this
        // is what actually clears stale pixels for a hidden/removed canvas
        // that isn't going to draw anything this frame. See damage()'s doc
        // comment in Canvas.hpp for why a single damage() call isn't
        // enough on its own.
        if (m_pendingRedamageFrames > 0) {
            gfx::damageBox(box());
            --m_pendingRedamageFrames;
        }

        if (!m_visible || !m_root)
            return;

        m_root->arrange();
        m_root->render(m_position);
    }

    void CCanvas::damage() {
        gfx::damageBox(box());
        m_pendingRedamageFrames = REDAMAGE_FRAMES;
    }

    bool CCanvas::recomputeAnchorPosition() {
        if (!m_anchor)
            return false;

        auto monitor = State::CMonitorQuery{*State::monitorState()}.name(m_anchorMonitor).run();
        if (!monitor)
            return false;

        // Read the monitor's LIVE combined reserved margins (config
        // baseline + every HyprLUI exclusive window's static-tier
        // contribution + Hyprland's own layer-shell/error-overlay dynamic
        // contributions - CReservedArea::calculate() already sums all of
        // that into these four numbers, see ReservedAreaComposer.hpp's
        // header comment for the full tier breakdown). If this window is
        // itself exclusive, exclude only ITS OWN contribution from
        // whichever single edge it reserves - a window only ever
        // contributes to one edge, so every other edge is already
        // correct as-is. This is what lets an exclusive window sit right
        // at its own natural position while still correctly avoiding
        // everything else already reserved (another HyprLUI exclusive
        // window on a different edge, the user's config baseline,
        // Hyprland's own error/debug overlay) - unlike the raw-box
        // approach this replaces, which avoided nothing, or the
        // unmodified logicalBoxMinusReserved() every other window uses,
        // which would self-referentially push this window inward by the
        // space it itself just reserved.
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

        // Same math as CReservedArea::apply(logicalBox()) (i.e. what
        // logicalBoxMinusReserved() does internally) - just against the
        // margins above instead of the monitor's own live totals
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

        if (pos.x == m_position.x && pos.y == m_position.y)
            return false;

        // Damage both where the window WAS and where it's about to be -
        // a plain damage() after moving only covers the new box, leaving
        // the old one's pixels stale (same class of bug damage() itself
        // now guards against for mutations - see its doc comment).
        const auto oldBox = this->box();
        m_position        = pos;
        gfx::damageBox(oldBox);
        damage();
        return true;
    }

} // namespace HyprLUI
