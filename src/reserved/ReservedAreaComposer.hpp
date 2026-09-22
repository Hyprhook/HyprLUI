#pragma once
//
// ReservedAreaComposer.hpp
//
// Composes every exclusive HyprLUI window's contribution into each
// monitor's reserved area (Desktop::CReservedArea, PHLMONITOR->
// m_reservedArea) - the mechanism that makes a tiled window layout
// actually leave space for a bar/dock-style window. Uses the *static*
// tier (setStatic(), a flat overwrite) rather than Hyprland's dynamic
// slots, which are a closed enum with no plugin-usable entry - see
// DESIGN.md's Architecture section 4 for the fuller picture.
//
// Every recompute reads the user's own `monitor{reserved:...}` baseline
// fresh (never the live, possibly-already-plugin-modified value, which
// would double-count our own prior write) and writes back baseline + our
// own composed total. Hyprland re-applies the config baseline - wiping
// our contribution - on both a config reload and a monitor layout change,
// which are genuinely different, both-required triggers (a Lua config
// error being resolved is a reload with no geometry change at all) - so
// reapplyAll() listens for both.

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

        // `force`: bypass the m_lastApplied diff-check - needed when an
        // external event (reload, hotplug) may have overwritten the
        // monitor's static tier out from under us, since the number we'd
        // recompute can be identical to the cache even though the live
        // value no longer matches it.
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
