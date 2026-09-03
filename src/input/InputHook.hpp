#pragma once
//
// InputHook.hpp
//
// Registers the mouse-button and keyboard-key listeners HyprLUI needs for
// clickable Button widgets and focusable Input widgets, forwarding hit-
// testing/focus/key dispatch to UIManager. Mirrors src/render/Render.hpp's
// shape - the only file that talks to Event::bus()->m_events.input,
// keeping hook wiring separate from the UI toolkit itself, same as
// RenderHook does for render.stage.
//
// Left-click only (BTN_LEFT) in v1 - see DESIGN.md Phase 4 for why, and
// for why this only needs the button event (not .move - no hover state
// tracked yet). Keyboard side is Phase 6: raw keysym only, no text
// composition/IME - see InputWidget.hpp.

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HyprLUI::InputHook {

    void registerHooks(HANDLE handle);
    void unregisterHooks(HANDLE handle);

} // namespace HyprLUI::InputHook
