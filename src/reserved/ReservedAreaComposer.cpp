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

        // force=true: something external (config reload, monitor hotplug/
        // reconfig) may have overwritten the monitor's static tier out
        // from under us - e.g. CMonitor::applyMonitorRuleSoft() re-runs
        // setStatic(configBaseline) on every reload, wiping our own prior
        // contribution. That means the NUMBER we'd recompute today can be
        // identical to what's cached in m_lastApplied (baseline and our
        // own sum are both unchanged) even though what's actually LIVE on
        // the monitor no longer matches that cache - recompute()'s normal
        // diff-check would then wrongly skip the write. reapplyAll()'s
        // entire purpose is "resync regardless", so it has to bypass that
        // optimization, not just recompute the same number again.
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

        // Read the user's true config baseline fresh from the monitor's
        // active rule every time - NOT from the live m_reservedArea,
        // which would already include our own previous write and double-
        // count it. See this file's header comment for why.
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

        // setStatic() only changes the box future placement decisions
        // will use - it does NOT itself move/resize windows that are
        // already tiled on this monitor. Hyprland's own monitor-rule-
        // apply path always follows a reserved-area change with exactly
        // this call (e.g. CMonitor::onConnect(), Monitor.cpp:368) to
        // force existing tiled windows to re-layout against the new
        // available space right now, instead of only affecting the next
        // window that happens to get tiled.
        if (g_layoutManager)
            g_layoutManager->recalculateMonitor(monitor);
    }

    void CReservedAreaComposer::registerHooks(HANDLE handle) {
        g_layoutChangedListener = Event::bus()->m_events.monitor.layoutChanged.listen([]() { CReservedAreaComposer::get().reapplyAll(); });

        // Confirmed live (user report): resolving a Lua config eval error
        // - the scenario that made Hyprland's own error/debug overlay
        // worth checking against in the first place - means Hyprland
        // re-evaluates the config, which calls CMonitor::
        // applyMonitorRuleSoft() again and wipes our static-tier
        // contribution back to just the fresh config baseline
        // (Monitor.cpp:673-675). That's a config reload, not a monitor
        // layout change - monitor.layoutChanged is emitted from entirely
        // different places (Monitor.cpp:1422, MonitorLayoutController.cpp:
        // 75 - monitor geometry/hotplug, not config application) and
        // never fires for this. Event::bus()->m_events.config.reloaded
        // (EventBus.hpp:183) is the one that actually corresponds to what
        // just happened here.
        g_configReloadedListener = Event::bus()->m_events.config.reloaded.listen([]() { CReservedAreaComposer::get().reapplyAll(); });
    }

    void CReservedAreaComposer::unregisterHooks(HANDLE handle) {
        g_layoutChangedListener.reset();
        g_configReloadedListener.reset();
    }

} // namespace HyprLUI
