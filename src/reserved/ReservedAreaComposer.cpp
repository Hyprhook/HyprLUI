#include "ReservedAreaComposer.hpp"

#include <hyprland/src/desktop/reserved/ReservedArea.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <unordered_set>

namespace HyprLUI {

    namespace {
        CHyprSignalListener g_layoutChangedListener;
        CHyprSignalListener g_configReloadedListener;
    }

    CReservedAreaComposer& CReservedAreaComposer::get() {
        static CReservedAreaComposer instance;
        return instance;
    }

    void CReservedAreaComposer::setContribution(const std::string& windowName, const std::string& monitorName, EEdge edge, double size) {
        auto& c       = m_contributions[windowName];
        c.monitorName = monitorName;
        c.edge        = edge;
        c.size        = size;
        c.active      = true;
        recompute(monitorName);
    }

    void CReservedAreaComposer::setActive(const std::string& windowName, bool active) {
        auto it = m_contributions.find(windowName);
        if (it == m_contributions.end() || it->second.active == active)
            return;

        it->second.active = active;
        recompute(it->second.monitorName);
    }

    void CReservedAreaComposer::removeContribution(const std::string& windowName) {
        auto it = m_contributions.find(windowName);
        if (it == m_contributions.end())
            return;

        const std::string monitorName = it->second.monitorName;
        m_contributions.erase(it);
        recompute(monitorName);
    }

    void CReservedAreaComposer::reapplyAll() {
        std::unordered_set<std::string> monitorNames;
        for (const auto& [name, c] : m_contributions)
            monitorNames.insert(c.monitorName);

        // force=true: reapplyAll()'s whole purpose is "resync regardless"
        // - the number we'd recompute can match the cache even when the
        // live value doesn't, since an external event just reset it.
        for (const auto& monitorName : monitorNames)
            recompute(monitorName, /* force = */ true);
    }

    void CReservedAreaComposer::clear() {
        std::unordered_set<std::string> monitorNames;
        for (const auto& [name, c] : m_contributions)
            monitorNames.insert(c.monitorName);

        m_contributions.clear();

        // Now sums to zero per monitor - recompute() reapplies just each
        // monitor's config baseline, restoring the pre-plugin state.
        for (const auto& monitorName : monitorNames)
            recompute(monitorName);
    }

    void CReservedAreaComposer::recompute(const std::string& monitorName, bool force) {
        auto monitor = State::CMonitorQuery{*State::monitorState()}.name(monitorName).run();
        if (!monitor)
            return; // monitor currently gone - self-corrects via reapplyAll() when layoutChanged next fires

        double top = 0, right = 0, bottom = 0, left = 0;
        for (const auto& [name, c] : m_contributions) {
            if (c.monitorName != monitorName || !c.active)
                continue;

            switch (c.edge) {
                case EEdge::Top: top += c.size; break;
                case EEdge::Right: right += c.size; break;
                case EEdge::Bottom: bottom += c.size; break;
                case EEdge::Left: left += c.size; break;
            }
        }

        // Read the config baseline fresh from the monitor's active rule -
        // NOT the live m_reservedArea, which would already include our
        // own previous write and double-count it.
        const auto&  baseline  = monitor->m_activeMonitorRule.m_reservedArea;
        const double newTop    = baseline.top() + top;
        const double newRight  = baseline.right() + right;
        const double newBottom = baseline.bottom() + bottom;
        const double newLeft   = baseline.left() + left;

        auto&        last = m_lastApplied[monitorName];
        if (!force && last.has && last.top == newTop && last.right == newRight && last.bottom == newBottom && last.left == newLeft)
            return; // nothing changed since our last write - skip triggering a redundant relayout

        monitor->m_reservedArea.setStatic(Desktop::CReservedArea(newTop, newRight, newBottom, newLeft));
        last = {newTop, newRight, newBottom, newLeft, true};

        // setStatic() only changes the box FUTURE placement decisions
        // will use - it does not itself move/resize already-tiled
        // windows, so this forces a relayout against the new space now.
        if (g_layoutManager)
            g_layoutManager->recalculateMonitor(monitor);
    }

    void CReservedAreaComposer::registerHooks(HANDLE handle) {
        g_layoutChangedListener = Event::bus()->m_events.monitor.layoutChanged.listen([]() { CReservedAreaComposer::get().reapplyAll(); });

        // A config reload (e.g. resolving a Lua eval error) also wipes our
        // static-tier contribution, but is a genuinely different event
        // from monitor.layoutChanged (geometry/hotplug only) - both are
        // needed.
        g_configReloadedListener = Event::bus()->m_events.config.reloaded.listen([]() { CReservedAreaComposer::get().reapplyAll(); });
    }

    void CReservedAreaComposer::unregisterHooks(HANDLE handle) {
        g_layoutChangedListener.reset();
        g_configReloadedListener.reset();
    }

} // namespace HyprLUI
