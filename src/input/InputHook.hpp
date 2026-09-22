#pragma once
//
// InputHook.hpp
//
// Registers the mouse-button and keyboard-key listeners HyprLUI needs for
// clickable Button/Checkbox widgets and focusable Input widgets, forwarding
// hit-testing/focus/key dispatch to UIManager - the only file that talks to
// Event::bus()->m_events.input.
//
// Left-click only (BTN_LEFT). Keyboard side is raw keysym only, no text
// composition/IME - see InputWidget.hpp.

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HyprLUI::InputHook {

    void registerHooks(HANDLE handle);
    void unregisterHooks(HANDLE handle);

} // namespace HyprLUI::InputHook
