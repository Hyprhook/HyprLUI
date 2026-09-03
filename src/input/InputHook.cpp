#include "InputHook.hpp"
#include "../ui/UIManager.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/keybinds/Manager.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>

#include <linux/input-event-codes.h>

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace HyprLUI::InputHook {

    namespace {
        CHyprSignalListener g_buttonListener;
        CHyprSignalListener g_keyListener;

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
                HyprLUI::CUIManager::get().clickButton(releaseHit.canvasName, releaseHit.widgetId); // no-op if it's actually an Input, not a Button

            g_pressed.reset();
        }

        // Whether `sym` is a bare modifier keysym (Shift/Ctrl/Alt/Super/
        // CapsLock/NumLock) rather than something an Input could ever
        // meaningfully "type". Deliberately the exact same set Hyprland's
        // own keybind engine treats as a modifier - `modifierFromXkb()`
        // (keybinds/Manager.cpp:171, file-local `static`, so reimplemented
        // here rather than exposed) - see onKeyboardKey()'s doc comment
        // below for why matching that set exactly (not a superset/subset)
        // matters, not just "seemed reasonable".
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

        // Raw keysym forwarding for the focused Input, if any - see
        // InputWidget.hpp for why this is deliberately not a text field.
        // SKeyEvent only carries an evdev keycode (there's no
        // "which keyboard" info on the event itself, see EventBus.hpp),
        // so the keysym is resolved against whichever keyboard the seat
        // currently considers active - the only sensible source, and the
        // same one Hyprland's own keybind resolution uses.
        void onKeyboardKey(IKeyboard::SKeyEvent e, Event::SCallbackInfo& info) {
            auto keyboard = g_pSeatManager->m_keyboard.lock();
            if (!keyboard || !keyboard->m_xkbState)
                return;

            // xkbcommon keycodes are libinput/evdev + 8 - see
            // LuaEventHandler.cpp's own "input.keyboard.key" dispatch for
            // the same offset on the same event.
            const uint32_t xkbCode = e.keycode + 8;

            // Live, modifier-aware keysym - what onKey/the built-in text
            // capture actually want (Shift+a should type 'A', etc.).
            const xkb_keysym_t keysym  = xkb_state_key_get_one_sym(keyboard->m_xkbState, xkbCode);
            const bool         pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;

            // A SEPARATE, modifier-INDEPENDENT keysym for the keybind-
            // conflict query below only. Hyprland's own bind resolution
            // (Manager.cpp:206-207) resolves a bind's trigger key against
            // `keyboard->m_xkbSymState` (or a keybind-manager-private
            // `m_xkbTranslationState` when `m_resolveBindsBySym` is set -
            // not reachable from a plugin, but `m_xkbSymState` represents
            // the same "layout-aware, no momentary modifiers" base symbol
            // and matches for every ordinary case) - NOT the live per-
            // press state - so a bind named "U" matches regardless of
            // whether Shift happens to be held, with the modifier
            // requirement checked separately via the bind's own modmask.
            // `m_xkbSymState` only ever tracks group/layout changes
            // (`IKeyboard::updateXkbStateWithKey()` calls
            // `xkb_state_update_mask(m_xkbSymState, ..., group)` only,
            // never `xkb_state_update_key()` on it - confirmed by reading
            // IKeyboard.cpp) - never momentary Shift/Ctrl/Alt, matching
            // that intent exactly. **Found live, third distinct bug in
            // this exclusion logic**: using the LIVE keysym here meant a
            // bind like "ALT + SHIFT + U" was NEVER recognized as a
            // conflict at all - its registered trigger keysym is
            // unshifted 'u', but the live keysym while Shift is actually
            // held resolves to uppercase 'U', so they never compared
            // equal. Harmless while nothing was focused (Hyprland's own,
            // correctly-resolved matcher fired it regardless of what this
            // plugin thought), but once an Input *was* focused, this
            // plugin never excluded the key and swallowed it instead -
            // deterministically, every time, not a timing race like the
            // previous two fixes. A single-modifier bind like "ALT + T"
            // was unaffected only because it has no Shift component to
            // desync in the first place.
            const xkb_keysym_t bindKeysym = xkb_state_key_get_one_sym(keyboard->m_xkbSymState, xkbCode);

            // A key that currently triggers a real Hyprland keybind is
            // never forwarded to a focused Input at all - not "forwarded
            // but not swallowed", genuinely never reaches onKey - so the
            // user's keybinds behave exactly as if HyprLUI didn't exist,
            // regardless of what happens to be focused. Checked via a
            // read-only query with no side effect (does NOT invoke the
            // bind), the same call Hyprland's own global-shortcuts-portal
            // conflict check uses (protocols/Hotkey.cpp). Only considers
            // global-scope binds (CRegistry::findShortcutConflict()
            // explicitly skips binds with a non-empty submap) - a submap-
            // only bind can still reach a focused Input; no read-only way
            // to ask "is this bound in the *current* submap" was found.
            //
            // A bare modifier keysym (Alt_L, Shift_L, ...) is excluded
            // unconditionally for a different, more important reason:
            // `CKeybindManager::onKeyEvent()` (Manager.cpp:184) is called
            // for EVERY key event, including standalone modifier presses/
            // releases, and does its own essential bookkeeping there
            // (`m_inputState.press()`/`.release()`, feeding `heldKeys()`)
            // that later keybind matching depends on - REGARDLESS of
            // whether that particular modifier press happens to complete
            // a bind on its own. Cancelling one of these events makes
            // `CInputManager::onKeyboardKey()` (InputManager.cpp:1702)
            // return before ever calling `onKeyEvent()` for it, so
            // Hyprland's own held-key state silently desyncs from what's
            // physically held. **Found live**: once an Input was focused
            // via a mouse click, keyboard-driven chords (including this
            // plugin's own focus/blur test binds) became unreliable -
            // needing repeated presses, or never firing - because every
            // Alt/Shift press+release from that point on was being eaten
            // here instead of reaching Hyprland's bookkeeping. There's
            // also nothing an Input could do with a bare modifier anyway
            // (nothing to type), so excluding it costs nothing.
            //
            // Whichever of those two checks decides a PRESS, the matching
            // RELEASE must reuse the exact same decision rather than
            // re-running both checks against the *current* (release-time)
            // state - tracked here per raw evdev keycode. Necessary
            // because nobody releases a chord atomically: if e.g. Shift
            // gets released fractionally before the trigger key, the
            // trigger's own release arrives with a modifier mask that no
            // longer matches the bind, so a stateless re-check would
            // misclassify *that* release as "not a keybind" and swallow
            // it once something's focused - corrupting `m_inputState`'s
            // press/release symmetry the exact same way an eaten modifier
            // event does (**found live**, immediately after the modifier
            // fix above: focusing via keybind worked once, then every
            // subsequent chord involving that same Input broke again).
            // Hyprland's own `onKeyEvent()` sidesteps this identical
            // problem by remembering `modifiersAtPress` per key instead of
            // re-deriving it at release - this mirrors that.
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
    }

    void unregisterHooks(HANDLE handle) {
        // Hyprland automatically drops a plugin's callbacks on unload, but
        // releasing our own reference here is cheap and explicit.
        g_buttonListener.reset();
        g_keyListener.reset();
        g_pressed.reset();
        g_excludedKeycodes.clear();
    }

} // namespace HyprLUI::InputHook
