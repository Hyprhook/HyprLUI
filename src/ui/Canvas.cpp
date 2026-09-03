#include "Canvas.hpp"
#include "../render/gfx.hpp"

#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace HyprLUI {

    void CCanvas::render() {
        if (!m_visible || !m_root)
            return;

        // Anchor tracking (if any) is re-resolved every frame too, before
        // layout - cheap (a handful of monitors, one name comparison each)
        // and keeps the window correctly placed across resolution/reserved-
        // area changes on its already-chosen monitor without needing a
        // dedicated monitor-change event listener.
        recomputeAnchorPosition();

        // Full-tree layout every frame - HUD-sized content, no need for a
        // layout-dirty flag (unlike CTextNode's texture cache, which is the
        // expensive part and stays cached).
        m_root->measure();
        m_root->arrange();
        m_root->render(m_position);
    }

    void CCanvas::damage() const {
        gfx::damageBox(box());
    }

    bool CCanvas::recomputeAnchorPosition() {
        if (!m_anchor)
            return false;

        auto monitor = State::CMonitorQuery{*State::monitorState()}.name(m_anchorMonitor).run();
        if (!monitor)
            return false;

        const auto box     = monitor->logicalBoxMinusReserved();
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

        m_position = pos;
        return true;
    }

} // namespace HyprLUI
