#include "InputHook.hpp"
#include "../ui/UIManager.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>

#include <linux/input-event-codes.h>

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace HyprLUI::InputHook {

    namespace {
        CHyprSignalListener g_buttonListener;
        CHyprSignalListener g_keyListener;
        CHyprSignalListener g_moveListener;
        CHyprSignalListener g_axisListener;

        // The widget a press hit, if any - re-hit-tested and compared by
        // value (canvas name + widget id, not a raw pointer) at release,
        // since the widget could in principle be removed by Lua code
        // between the two events. See UIManager.hpp's SWidgetHit.
        std::optional<HyprLUI::SWidgetHit> g_pressed;

        // Raw evdev keycodes whose PRESS was excluded (bare modifier, or
        // matched a real Hyprland keybind at the time) - see
        // onKeyboardKey()'s doc comment for why the matching RELEASE must
        // reuse this same decision rather than re-deriving it live.
        std::unordered_set<uint32_t> g_excludedKeycodes;

        void                         onMouseButton(IPointer::SButtonEvent e, Event::SCallbackInfo& info) {
            if (e.button != BTN_LEFT)
                return;

            const auto pt = g_pInputManager->getMouseCoordsInternal();

            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                auto hit = HyprLUI::CUIManager::get().hitTestWidget(pt);

                // Click-to-focus/click-away-to-blur for Input widgets:
                // this decides the whole transition (blur whatever was
                // focused, then focus `hit` if it's an Input) regardless
                // of what was hit, including empty space and Buttons -
                // clicking anything that isn't the currently-focused
                // Input blurs it. Grabbed on press, not gated on a full
                // press+release like Button's onClick below - there's no
                // "cancel by dragging off" convention for focus, same as
                // a real text field.
                HyprLUI::CUIManager::get().handlePressFocus(hit);

                if (hit.empty())
                    return; // not over any of our widgets - let it through untouched

                g_pressed      = hit;
                info.cancelled = true;
                return;
            }

            // Release. Only swallow/act on it if we swallowed the
            // matching press - an unrelated release (press started
            // elsewhere, e.g. on a real window) must pass through.
            if (!g_pressed)
                return;

            info.cancelled = true;

            const auto releaseHit = HyprLUI::CUIManager::get().hitTestWidget(pt);
            if (releaseHit == *g_pressed)
                HyprLUI::CUIManager::get().clickWidget(releaseHit.canvasName, releaseHit.widgetId); // no-op if it's actually an Input, not a Button/Checkbox

            g_pressed.reset();
        }

        // Hover tracking + cursor feedback. Purely observational -
        // `info.cancelled` is never set here - so windows underneath a
        // HyprLUI overlay still get their own normal hover/motion
        // behavior; only clicks and (opted-into) scroll are swallowed.
        void onMouseMove(Vector2D, Event::SCallbackInfo& info) {
            const auto pt  = g_pInputManager->getMouseCoordsInternal();
            const auto hit = HyprLUI::CUIManager::get().hitTestWidget(pt);
            HyprLUI::CUIManager::get().updateHover(hit);

            // Hyprland's own priority-grouped cursor-override mechanism -
            // additive, not fighting Hyprland's own cursor state.
            // CURSOR_OVERRIDE_UNKNOWN is deliberately the LOWEST priority
            // group, so a real window-edge-resize or drag cursor wins
            // over a HUD hover indicator.
            if (!hit.empty())
                Pointer::Cursor::overrideController->setOverride("pointer", Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
            else
                Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
        }

        // Stays completely inert unless hit-testing lands on a widget
        // with an onScroll handler set - swallow only what's opted into.
        void onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
            const auto pt  = g_pInputManager->getMouseCoordsInternal();
            const auto hit = HyprLUI::CUIManager::get().hitTestWidget(pt);
            if (hit.empty())
                return;

            const bool vertical = e.axis == WL_POINTER_AXIS_VERTICAL_SCROLL;
            if (HyprLUI::CUIManager::get().dispatchScroll(hit.canvasName, hit.widgetId, e.delta, vertical))
                info.cancelled = true;
        }

        // Whether `sym` is a bare modifier keysym (Shift/Ctrl/Alt/Super/
        // CapsLock/NumLock) rather than something an Input could ever
        // meaningfully "type" - deliberately the same set Hyprland's own
        // keybind engine treats as a modifier (its `modifierFromXkb()` is
        // file-local, so reimplemented here rather than exposed).
        bool isModifierKeysym(xkb_keysym_t sym) {
            switch (sym) {
                case XKB_KEY_Super_L:
                case XKB_KEY_Super_R:
                case XKB_KEY_Alt_L:
                case XKB_KEY_Alt_R:
                case XKB_KEY_Control_L:
                case XKB_KEY_Control_R:
                case XKB_KEY_Shift_L:
                case XKB_KEY_Shift_R:
                case XKB_KEY_Caps_Lock:
                case XKB_KEY_Num_Lock: return true;
                default: return false;
            }
        }

        // Raw keysym forwarding for the focused Input, if any. SKeyEvent
        // only carries an evdev keycode, no "which keyboard" info, so the
        // keysym is resolved against whichever keyboard the seat
        // currently considers active - the same one Hyprland's own
        // keybind resolution uses.
        void onKeyboardKey(IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
            auto keyboard = g_pSeatManager->m_keyboard.lock();
            if (!keyboard || !keyboard->m_xkbState)
                return;

            const uint32_t xkbCode = e.keycode + 8; // xkbcommon keycodes are libinput/evdev + 8

            // Live, modifier-aware keysym - what onKey/the built-in text
            // capture actually want (Shift+a should type 'A', etc.).
            const xkb_keysym_t keysym  = xkb_state_key_get_one_sym(keyboard->m_xkbState, xkbCode);
            const bool         pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;

            // A SEPARATE, modifier-INDEPENDENT keysym for the keybind-
            // conflict query below only - Hyprland's own bind resolution
            // matches a trigger key like "U" regardless of whether Shift
            // happens to be held (the modifier requirement is checked
            // separately via the bind's own modmask). Using the LIVE
            // keysym here instead would make a bind like "ALT + SHIFT + U"
            // never register as a conflict (its trigger is unshifted 'u',
            // but the live keysym while Shift is held resolves to 'U'),
            // silently letting a focused Input swallow that keybind.
            const xkb_keysym_t bindKeysym = xkb_state_key_get_one_sym(keyboard->m_xkbSymState, xkbCode);

            // A key that currently triggers a real Hyprland keybind is
            // never forwarded to a focused Input at all, so the user's
            // keybinds behave as if HyprLUI didn't exist. Read-only query,
            // no side effect. Only considers global-scope binds - a
            // submap-only bind can still reach a focused Input.
            //
            // A bare modifier keysym (Alt_L, Shift_L, ...) is excluded
            // unconditionally too, for a different reason: Hyprland's own
            // keybind manager needs to see every modifier press/release
            // for its held-key bookkeeping, regardless of whether that
            // press completes a bind on its own - swallowing one here
            // desyncs Hyprland's held-key state from what's physically
            // held, breaking later chord matching for as long as
            // something stays focused. Costs nothing UX-wise - there's
            // nothing an Input could do with a bare modifier anyway.
            //
            // Whichever check decides a PRESS, the matching RELEASE must
            // reuse that same decision rather than re-running both checks
            // against release-time state - tracked here per raw evdev
            // keycode. Necessary because chords don't release atomically:
            // if a modifier releases fractionally before the trigger key,
            // a stateless re-check at release would see a modifier mask
            // that no longer matches the bind and misclassify it.
            bool excluded;
            if (pressed) {
                excluded = isModifierKeysym(keysym) || Keybinds::mgr()->findConflictingBind(bindKeysym, keyboard->getModifiers());
                if (excluded)
                    g_excludedKeycodes.insert(e.keycode);
                else
                    g_excludedKeycodes.erase(e.keycode);
            } else {
                excluded = g_excludedKeycodes.erase(e.keycode) > 0;
            }

            if (excluded)
                return;

            if (HyprLUI::CUIManager::get().dispatchKey(static_cast<uint32_t>(keysym), pressed))
                info.cancelled = true;
        }
    } // namespace

    void registerHooks(HANDLE handle) {
        g_buttonListener = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);
        g_keyListener    = Event::bus()->m_events.input.keyboard.key.listen(onKeyboardKey);
        g_moveListener   = Event::bus()->m_events.input.mouse.move.listen(onMouseMove);
        g_axisListener   = Event::bus()->m_events.input.mouse.axis.listen(onMouseAxis);
    }

    void unregisterHooks(HANDLE handle) {
        // Hyprland automatically drops a plugin's callbacks on unload, but
        // releasing our own reference here is cheap and explicit.
        g_buttonListener.reset();
        g_keyListener.reset();
        g_moveListener.reset();
        g_axisListener.reset();
        g_pressed.reset();
        g_excludedKeycodes.clear();
        // Don't leave the cursor stuck as "pointer" if the plugin unloads
        // mid-hover.
        Pointer::Cursor::overrideController->unsetOverride(Pointer::Cursor::CURSOR_OVERRIDE_UNKNOWN);
    }

} // namespace HyprLUI::InputHook
