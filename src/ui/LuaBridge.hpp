#pragma once
//
// LuaBridge.hpp
//
// Registers HyprLUI's public API under hl.plugin.hyprlui.* so a Lua config
// can declare a window as a widget tree and mutate it afterwards by id.
//
// Widget constructors (each tags and returns its table argument, so they
// nest via plain Lua table-literal syntax - `Column{ gap = 8, Text{...} }`
// puts the Text{} result at index 1 of the Column table):
//
//   Stack{ id, x = 0, y = 0, w, h, visible, <children...> }
//     Manual/absolute positioning - each child keeps whatever x/y it was
//     given. Size-to-content is the bounding box of its children unless
//     w/h are given.
//
//   Row{ id, x = 0, y = 0, w, h, visible, gap = 0, padding = 0,
//        align = "start"|"center"|"end", <children...> }
//   Column{ ...same fields... }
//     Flexbox-lite: packs children along the row/column axis with `gap`
//     between them and `padding` on all sides; `align` controls cross-axis
//     alignment. No wrap, no justify/space-between (v1 scope).
//
//   Text{ id, x = 0, y = 0, text, size = 16, color, font = "sans", visible }
//   Box{ id, x = 0, y = 0, w, h, color, rounding = 0, visible }
//     Leaves. `color` is either a 0xAARRGGBB integer or a table
//     { r, g, b, a } with components in [0, 1]. `text` on Text may be a
//     plain string, or the result of Bind(name) (see below) to keep it
//     tracking a watcher's current value.
//
//   Button{ id, x = 0, y = 0, w, h, color, rounding = 0, visible, onClick,
//           <children...> }
//     Like Box, but clickable (left-click only, v1) - `onClick` is a Lua
//     function called with no arguments when a press and its matching
//     release both land on this same button (moving off between press and
//     release cancels it, same convention as every other GUI toolkit).
//     Children are positioned manually/absolutely inside it, same as
//     Stack - typically a Text label. Only Overlay-zorder windows (the
//     default) are clickable; Background windows are decorative and can
//     be occluded by real app windows, so hit-testing skips them (see
//     DESIGN.md Phase 4). A Button's own bounds are the only clickable
//     area - clicking elsewhere in the same window (its background, a
//     label, empty space) passes through to whatever's behind it
//     untouched, it does not swallow the whole window's worth of clicks.
//     onClick errors are caught and logged, not propagated - this fires
//     from the input hook, not a caller-side pcall.
//
//   Input{ id, x = 0, y = 0, w, h, color, rounding = 0, visible,
//          text = "", textColor, textSize = 14, textFont = "sans",
//          onChange, onKey, onFocus, onBlur, <children...> }
//     A focusable rectangle that behaves like an actual text field by
//     default - typing appends a character, Backspace removes the last
//     one, and the current text renders automatically (an internally-
//     owned Text label, styled by `textColor`/`textSize`/`textFont`,
//     positioned with a small fixed padding - not one of the positional
//     `<children...>`, though those still layer on top of it same as a
//     Button's label does). `text` seeds the initial content. None of
//     that capture/display/removal is something a caller has to build -
//     see DESIGN.md Phase 6 for why an earlier version left it as a "type
//     this yourself on top of onKey" exercise and why that turned out to
//     be the wrong default. Printable-ASCII only (0x20-0x7e) - no cursor/
//     selection/IME/non-ASCII/clipboard; a caller wanting any of that
//     layers it on top of onKey, which keeps firing for every key exactly
//     as before, in addition to (not instead of) the built-in capture.
//
//     `onChange(text)` fires with the new content whenever it changes
//     from typing/Backspace - not from a programmatic set_input_text()
//     call below, same "no invocation on load, only on real interaction"
//     convention set_text() elsewhere uses.
//
//     Same layout shape as Button otherwise - children positioned
//     manually/absolutely inside it, only clickable/interactive on
//     Overlay-zorder windows.
//
//     Gains HyprLUI's keyboard focus (a single global slot - only one
//     Input, across every HyprLUI window, is ever focused at a time) by
//     being clicked, same left-click convention as Button but grabbed
//     immediately on press rather than waiting for a matching release -
//     there's no "cancel by dragging off" affordance for focus, same as a
//     real text field. Clicking anything else - empty space, a Button,
//     a different Input, a real window - blurs it. Also focusable/
//     blurrable programmatically via focus_widget()/blur_widget() below,
//     e.g. to focus a search box the instant its window opens.
//
//     `onKey(keysym, pressed)` fires for every key event while focused,
//     in addition to the built-in capture above - `keysym` is an xkb
//     keysym (already layout/shift-aware, since that's baked into the
//     keysym itself by the time it reaches Lua) and `pressed` is true on
//     key-down, false on key-up. Use it for anything beyond plain ASCII
//     capture - e.g. Enter to submit, Escape to blur_widget(). `onFocus()`/
//     `onBlur()` fire on the transitions, no arguments.
//
//     A key that currently triggers a real Hyprland keybind (SUPER+...
//     etc. - checked via a read-only query, see InputHook.cpp) never
//     reaches a focused Input at all - not just "not swallowed", it's
//     never forwarded to onKey (or the built-in capture) either, so the
//     user's keybinds behave exactly as if HyprLUI didn't exist, no
//     matter what's focused. This is absolute - there's no widget-level
//     opt-out. A bare modifier key press/release (Shift/Ctrl/Alt/Super/
//     CapsLock/NumLock alone, with no other key) is excluded the same
//     unconditional way, for a different reason: Hyprland's own keybind
//     engine needs to see every one of those to keep its internal state
//     correct, even when that particular press doesn't complete a bind on
//     its own (see InputHook.cpp). Everything else is swallowed while
//     focused (doesn't leak through to whatever real window has actual
//     Wayland keyboard focus behind it). Only global-scope binds are
//     detected this way (a submap-specific bind can still reach a focused
//     Input - no read-only "is this bound in the *current* submap" query
//     exists). onKey/onChange errors are caught and logged, not
//     propagated, same as onClick.
//
// Reactivity:
//
//   watch(name, fn, opts?)
//     Registers `fn` (called with no arguments) as a named watcher -
//     its return value is cached and stringified the same way Lua's own
//     tostring()/`..` would. `fn` is called once immediately to seed the
//     initial value. `opts.interval` (milliseconds), if given, also
//     re-calls `fn` on that cadence via Hyprland's own event-loop timer
//     (independent of render activity - safe to use for a live clock,
//     volume level, etc. on an otherwise-idle desktop). Errors if `name`
//     is already registered. Watcher function errors (from either the
//     initial call, a poll tick, or notify()) are caught and logged, not
//     propagated - the last good value is kept.
//
//   notify(name)
//     Re-invokes a watcher's function right now. Errors if `name` isn't
//     registered. Only needed for watchers without a poll `interval` (or
//     to force an immediate refresh of one that has one) - e.g. call this
//     right after changing whatever state a watcher's function reads,
//     rather than waiting for its next poll tick.
//
//   Bind(name)
//     Ties a widget property (currently just Text.text - see above) to
//     watcher `name`'s current value: the property re-reads that value
//     every frame and updates via the widget's own setter (a no-op if
//     unchanged), so it stays live for as long as the window exists.
//     Errors if `name` isn't a registered watcher yet at the point the
//     window{} using it gets built - watch() must run first.
//
// Window construction and mutation:
//
//   window{ name, x = 0, y = 0, w, h, zorder = "overlay"|"background",
//           anchor, monitor, exclusive, <exactly one root widget> }
//     Opens a new window with the given widget tree as its root. If w/h
//     are omitted the window sizes itself to the root's measured content.
//     Errors if `name` is already in use.
//
//     Without `anchor`: x/y are a raw global (compositor-space) position,
//     same as Phase 1.
//
//     With `anchor` (one of top-left/top/top-right/left/center/right/
//     bottom-left/bottom/bottom-right): x/y are reinterpreted as an offset
//     from that point on the target monitor's usable box (i.e. excluding
//     space other bars/panels have already reserved) - positive x/y always
//     pushes inward from whichever edge(s) the anchor touches. `monitor` is
//     optional and, if given, uses the exact same selector syntax as
//     Hyprland's own window/layer rule `mon:` fields (a direction char,
//     "+N"/"-N", a numeric id, or a static selector/output name), resolved
//     relative to whichever monitor is focused *right now*. There is
//     deliberately no "current"/"focused" keyword: Hyprland's own selector
//     parser treats the literal string "current" as an alias for whatever
//     reference monitor it's given, which would shadow an actual monitor a
//     user has genuinely named "current" - so omit `monitor` entirely to
//     mean "the focused monitor". If a given selector matches nothing
//     (typo, unplugged output), this falls back to the focused monitor
//     rather than erroring. Either way the monitor is picked ONCE, at
//     window-creation time - it never changes afterwards, even if you
//     focus a different screen later. Its box IS re-read every frame
//     though, so a resolution/reserved-area change on that monitor still
//     moves the window correctly.
//
//     `exclusive` ("top"|"right"|"bottom"|"left") - requires `anchor`
//     (a reserved zone needs a resolved target monitor, and anchor is
//     currently the only thing that gives us one), and must be an edge
//     the anchor actually touches - anchor="top" only accepts
//     exclusive="top"; a corner anchor like "top-left" accepts either
//     "top" or "left" (whichever edge, or both, the window is meant to
//     be a bar along); "center" accepts neither, since a centered window
//     isn't at any edge. Errors otherwise - anchor="top" + exclusive=
//     "left" would reserve space nowhere near where the window actually
//     is. Reserves screen-edge space equal to this window's own current
//     size along the perpendicular axis (top/bottom -> height, left/
//     right -> width) on
//     the target monitor, matching eww's `exclusive` flag or a real
//     layer-shell surface's exclusive zone - tiled windows on that
//     monitor actually leave the space empty, and it tracks this
//     window's live size (e.g. Bind()ed content growing/shrinking).
//     Composed with the user's own `monitor{ reserved: ... }` config
//     baseline and any other HyprLUI exclusive windows on the same
//     monitor/edge (summed, not max) - see ReservedAreaComposer.hpp for
//     why this needs its own module rather than a one-line Hyprland call.
//     A hidden window (set_canvas_visible(false)) reserves nothing while
//     hidden. It positions itself excluding only its OWN contribution -
//     it still correctly avoids the user's config baseline, Hyprland's
//     own error/debug overlay, and any other HyprLUI exclusive window on
//     a *different* edge. v1 limitation: multiple HyprLUI exclusive
//     windows on the *same* edge don't stack relative to each other (each
//     still only excludes itself) - fine for a single top bar, but a
//     second top bar needs manual x/y offsetting for now.
//
//   remove_canvas(name)
//     Closes a window and everything in it.
//
//   set_canvas_visible(name, visible)
//     Shows/hides a window without destroying its content.
//
//   set_text(window, id, text)
//     Updates an existing Text widget's content in place.
//
//   set_input_text(window, id, text)
//     Sets an existing Input widget's current text programmatically (e.g.
//     pre-filling or clearing a field) - updates what's rendered but does
//     NOT invoke onChange, same reasoning set_text() vs. a live edit uses.
//
//   get_input_text(window, id)
//     Returns an Input widget's current text. Mainly for reading it from
//     somewhere other than onChange - e.g. a sibling Button's onClick
//     wanting "whatever's currently typed" at click time.
//
//   remove_widget(window, id)
//     Removes a single widget (and its subtree) from a window. Blurs it
//     first if it happened to hold HyprLUI's keyboard focus.
//
//   focus_widget(window, id)
//     Gives HyprLUI's keyboard focus to the named Input widget
//     programmatically - the same transition a click on it would trigger
//     (blurs whatever was previously focused, fires onBlur/onFocus).
//     Errors if `id` isn't an Input widget on that window. A no-op
//     (doesn't re-fire onFocus) if it's already the focused widget.
//
//   blur_widget()
//     Blurs whichever Input currently has HyprLUI's keyboard focus, if
//     any - a no-op if nothing is focused. No arguments: there's only
//     ever one focus slot, so there's nothing to disambiguate.
//
// Example, from hyprland.lua:
//
//   local hyprlui = hl.plugin.hyprlui
//   hyprlui.window{
//       name = "greeting", x = 100, y = 100,
//       hyprlui.Column{ id = "root", gap = 8, padding = 16,
//           hyprlui.Box{ id = "bg", w = 300, h = 80, color = 0xcc111111, rounding = 8 },
//           hyprlui.Text{ id = "label", text = "Hello!", size = 20 },
//       },
//   }
//   hl.bind("SUPER + G", function() hyprlui.remove_canvas("greeting") end)
//
//   -- anchored to the top-right of the focused monitor's usable area,
//   -- 10px in from both edges:
//   hyprlui.window{
//       name = "hud", anchor = "top-right", x = 10, y = 10,
//       hyprlui.Text{ id = "label", text = "HUD" },
//   }
//
//   -- reactive: polled every 500ms, no manual set_text() needed:
//   hyprlui.watch("volume", function() return get_volume() .. "%" end, { interval = 500 })
//   hyprlui.window{
//       name = "vol", anchor = "bottom",
//       hyprlui.Text{ id = "label", text = hyprlui.Bind("volume") },
//   }
//
//   -- clickable:
//   hyprlui.window{
//       name = "btn", anchor = "center",
//       hyprlui.Button{
//           id = "go", w = 120, h = 32, color = 0x333333, rounding = 6,
//           onClick = function() hyprlui.remove_canvas("btn") end,
//           hyprlui.Text{ x = 12, y = 8, text = "Close" },
//       },
//   }
//
//   -- focusable: typing/Backspace/display are all built in - click it,
//   -- then type. onKey is only needed here for Return (submit):
//   hyprlui.window{
//       name = "search", anchor = "center",
//       hyprlui.Input{
//           id = "field", w = 200, h = 28, color = 0x222222,
//           onKey = function(keysym, pressed)
//               if pressed and keysym == 0xff0d then -- XKB_KEY_Return
//                   print("submitted: " .. hyprlui.get_input_text("search", "field"))
//               end
//           end,
//       },
//   }
//
//   -- exclusive: a 32px-tall top bar that actually reserves its own
//   -- height, pushing tiled windows on this monitor down out of the way.
//   -- No "span the full monitor width" feature exists yet - the bar's
//   -- width here is just whatever its content needs (400px):
//   hyprlui.window{
//       name = "bar", anchor = "top", exclusive = "top",
//       hyprlui.Box{ id = "bg", w = 400, h = 32, color = 0xff222222 },
//   }
//
// Note the Box above is behind/outside the visible flow in this example
// only to illustrate nesting - a background panel that should sit *behind*
// its siblings belongs in an outer Stack with the Column absolutely
// positioned on top of it, since Row/Column packs every child into the
// flow. See DESIGN.md for the layout model this is built on.

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace HyprLUI::Lua {

    // Registers every hl.plugin.hyprlui.* function. Call once from PLUGIN_INIT.
    void registerFunctions(HANDLE handle);

    // Unregisters everything registered above. Call from PLUGIN_EXIT.
    void unregisterFunctions(HANDLE handle);

} // namespace HyprLUI::Lua
