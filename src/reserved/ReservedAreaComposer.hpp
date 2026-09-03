#pragma once
//
// ReservedAreaComposer.hpp
//
// Composes every exclusive HyprLUI window's contribution into each
// monitor's reserved area (Desktop::CReservedArea, PHLMONITOR->
// m_reservedArea) - the mechanism that makes a tiled window layout
// actually leave space for a bar/dock-style window, matching eww's
// `exclusive` flag or a wlr-layer-shell surface's exclusive zone.
//
// Verified before implementing (research pass, mirrors this project's
// established practice for internal-API-reliant phases): Desktop::
// CReservedArea (Hyprland's src/desktop/reserved/ReservedArea.hpp) has
// two storage tiers - a "static" one (set via setStatic(), overwritten
// wholesale, no composition) and a "dynamic" one, indexed by a CLOSED
// enum (eReservedDynamicType: only RESERVED_DYNAMIC_TYPE_LS and
// RESERVED_DYNAMIC_TYPE_ERROR_BAR exist - no plugin-usable slot). Both
// built-in dynamic slots get reset-and-recomputed by Hyprland's own core
// code on every relevant layout pass (real layer-shell surfaces / the
// crash error bar respectively), so a plugin piggybacking on either would
// have its contribution silently wiped out whenever that unrelated
// subsystem recalculates - not usable.
//
// So: this composer uses the *static* tier, the same one the user's own
// `monitor{ ..., reserved: ... }` config rule uses
// (CMonitor::applyMonitorRuleSoft(), src/output/Monitor.cpp:673-675, does
// exactly `m_reservedArea.setStatic(m_activeMonitorRule.m_reservedArea)`).
// setStatic() is a flat overwrite, not additive - so every recompute here
// reads the user's true baseline fresh from `pMonitor->
// m_activeMonitorRule.m_reservedArea` (never from the live, possibly-
// already-plugin-modified `m_reservedArea` itself, which would double-
// count our own prior contribution) and writes back baseline + our own
// composed total. Because applyMonitorRuleSoft() re-runs that same
// setStatic() call on every config reload AND on every monitor
// (re)configuration, discarding whatever we'd previously composed in,
// reapplyAll() must be called after BOTH `Event::bus()->
// m_events.config.reloaded` and `...monitor.layoutChanged` fire (see
// .cpp - they're genuinely different triggers: a Lua config error being
// resolved is a reload with no monitor geometry change at all, confirmed
// live - the earlier layoutChanged-only version left exactly that case
// unhandled) - this is a known, unavoidable v1 characteristic of the
// API, not a bug: there is currently no way for a plugin to be told
// "only the static tier changed, and only from your own last write" as
// opposed to "something else just re-applied it out from under you".
// reapplyAll() also has to bypass recompute()'s normal diff-check (see
// its `force` parameter) - the number we'd recompute can be identical to
// what we last cached even when what's actually live no longer matches,
// since neither the config baseline value nor our own sum necessarily
// changed, only the live object's contents (reset by whatever external
// event just fired).

#include "../ui/Canvas.hpp" // for EEdge - same "which edge" concept as EAnchor, defined there

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>
#include <unordered_map>

namespace HyprLUI {

    class CReservedAreaComposer {
      public:
        static CReservedAreaComposer& get();

        // Sets/updates window `windowName`'s exclusive contribution:
        // `size` logical pixels reserved along `edge` on monitor
        // `monitorName`. Recomputes and (if it actually changed)
        // reapplies that monitor's composed reserved area immediately.
        void setContribution(const std::string& windowName, const std::string& monitorName, EEdge edge, double size);

        // Toggles whether an existing contribution currently counts
        // toward the composed total, without forgetting its edge/size -
        // used for window visibility: a hidden exclusive window reserves
        // nothing (matches eww), but should resume reserving its same
        // space the instant it's shown again, no need to re-specify
        // edge/size. No-op if `windowName` has no tracked contribution.
        void setActive(const std::string& windowName, bool active);

        // Forgets this window's contribution entirely (window removed)
        // and recomputes/reapplies its monitor's composed area.
        void removeContribution(const std::string& windowName);

        // Re-derives every affected monitor's composed reserved area from
        // scratch and reapplies it. Call after monitor layout changes
        // (reload, hotplug) - see the header comment above for why this
        // is necessary rather than a nice-to-have.
        void reapplyAll();

        // Forgets every contribution and restores every affected
        // monitor's static reserved area back to just its own config
        // baseline (not, of course, whatever addType() dynamic slots
        // happen to hold - matches the "we only ever touch the static
        // tier" scope everywhere else in this class). Call from
        // PLUGIN_EXIT - don't leave stale reserved margins behind after
        // unload.
        void clear();

        void registerHooks(HANDLE handle);
        void unregisterHooks(HANDLE handle);

      private:
        struct SContribution {
            std::string monitorName;
            EEdge       edge;
            double      size   = 0;
            bool        active = true;
        };

        // `force`: bypass the m_lastApplied diff-check (see reapplyAll()'s
        // doc comment for why this is sometimes necessary, not just an
        // optimization to skip).
        void                                           recompute(const std::string& monitorName, bool force = false);

        std::unordered_map<std::string, SContribution> m_contributions; // keyed by window name

        // What we last actually wrote per monitor (baseline + our sum at
        // that time) - diffed against on every recompute() so an
        // unrelated call (e.g. another window's unrelated contribution
        // changing) doesn't redundantly re-trigger a relayout on a
        // monitor whose own composed total didn't actually move.
        struct SLastApplied {
            double top = 0, right = 0, bottom = 0, left = 0;
            bool   has = false;
        };
        std::unordered_map<std::string, SLastApplied> m_lastApplied; // keyed by monitor name
    };

} // namespace HyprLUI
