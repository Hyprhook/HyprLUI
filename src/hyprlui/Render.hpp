#pragma once
//
// Render.hpp
//
// Registers the single "render" listener HyprLUI needs and forwards it to
// UIManager at the right stage(s). This is the only file that talks to
// Event::bus()->m_events.render - keeping hook wiring separate from the UI
// toolkit itself.

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HyprLUI::RenderHook {

    void registerHooks(HANDLE handle);
    void unregisterHooks(HANDLE handle);

} // namespace HyprLUI::RenderHook
