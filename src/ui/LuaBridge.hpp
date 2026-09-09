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
// Every widget table below also accepts the shared Phase 7 "base widget
// properties" fields, on top of whatever's listed for its own type:
//
//   padding, margin
//     Either a single number (all four sides) or a table
//     { top, right, bottom, left } (an omitted side is 0 - NOT the uniform
//     value, same "shorthand table" convention `color`'s table form
//     already has). `padding` insets a CONTAINER's own children from its
//     edges - only Row/Column and Input's auto-owned label currently
//     interpret it; Stack's manual/absolute positioning leaves it unused
//     by design (full manual control already covers it). `margin` is a
//     widget's own requested space around ITSELF, read by whichever
//     container lays it out - only Row/Column currently read a child's
//     margin (added ON TOP of `gap`, CSS-flexbox-item convention: the
//     visible gap between two adjacent items ends up being gap + one
//     item's trailing margin + the next item's leading margin), Stack
//     again leaves it unused.
//
//   minW, minH, maxW, maxH
//     Clamp the measured size on each axis independently after any fixed
//     w/h override (same precedence CSS gives min/max-width over an
//     explicit width). Forces a decision for `Text`: content that doesn't
//     fit `maxW` truncates with an ellipsis - Hyprland's own Pango-based
//     text renderer already does this given a max width, so this is
//     gotten essentially for free rather than reimplemented (wrap/clip
//     modes aren't wired up - not needed once a default was picked, see
//     DESIGN.md Phase 7). `minW`/`minH` widen the LAYOUT box without
//     stretching a Text's rendered glyphs to fill it (they draw at their
//     own natural size, leaving empty space beside them) - every other
//     widget type just gets visually bigger, since a flat rect has no
//     "native size" to distort.
//
//   opacity
//     0-1, default 1. Multiplies with every ANCESTOR's own opacity, not
//     independent/absolute - a semi-transparent container fades its
//     children too, standard CSS/Qt/every-toolkit convention.
//
//   zIndex
//     Integer, default 0. Reorders paint order among a widget's OWN
//     siblings only (higher paints later/on top; ties keep insertion
//     order, so this is a pure additive extension of "later child wins"
//     with zero behavior change when unused) - not a full CSS stacking-
//     context system, a low-zIndex child of a high-zIndex widget still
//     paints "inside" its parent's turn, it can't jump above a sibling of
//     a *different* parent.
//
//   debug, debugCascade, debugShow
//     Box-model debug overlay - outlines for the margin/content/padding
//     boxes, small labels (padding/margin values, id, WxH, non-default
//     zIndex/opacity), and a translucent fill over an interactive
//     widget's actual hit-target area. Drawn as a separate pass AFTER
//     everything else, so it's always fully visible regardless of the
//     widget's own opacity/zIndex.
//
//     `debug` is tri-state: omitted means "inherit whatever the nearest
//     ancestor resolved to" (a window's root inherits `false`); an
//     explicit `true`/`false` overrides that for this widget AND
//     cascades to its own descendants the same way, until something
//     deeper overrides it again. `debugCascade` (boolean, default true)
//     walls a subtree off from that inheritance when set to `false` -
//     neither this widget's own `debug` value nor anything inherited
//     from further up reaches its children; they start completely fresh.
//     Use it either to debug one widget without lighting up its whole
//     subtree, or the reverse - leave one branch alone while debug is on
//     above it.
//
//     `debugShow = { box, padding, margin, id, size, zOpacity, hitTarget }`
//     (all optional booleans) force-overrides individual detail
//     categories on/off, bypassing the automatic "only show once this
//     widget is big enough to render it legibly" default (and, for
//     `zOpacity` only, the additional "only when opacity/zIndex are
//     actually non-default" default) - a category left out of the table
//     stays on that automatic behavior, inherited/cascaded the same way
//     as `debug` itself.
//
//     `debugFontSize` (integer, default 10) is the point size for every
//     label this widget draws - same inheritance as `debug`/`debugShow`
//     (nullopt = inherit; set once near the root to size the whole
//     tree's overlay text at once, or override it deeper down for one
//     branch).
//
//   Stack{ id, x = 0, y = 0, w, h, visible, <children...> }
//     Manual/absolute positioning - each child keeps whatever x/y it was
//     given. Size-to-content is the bounding box of its children unless
//     w/h are given.
//
//   Row{ id, x = 0, y = 0, w, h, visible, gap = 0,
//        align = "start"|"center"|"end", <children...> }
//   Column{ ...same fields... }
//     Flexbox-lite: packs children along the row/column axis with `gap`
//     between them (see `margin` above for adding more, per-child);
//     `align` controls cross-axis alignment. No wrap, no justify/
//     space-between (v1 scope).
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
//     inset by a small left `padding` by default (8px - overridable like
//     any other widget's `padding` field above) and vertically centered -
//     not one of the positional `<children...>`, though those still layer
//     on top of it same as a Button's label does). `text` seeds the
//     initial content. None of
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
//   Image{ id, x = 0, y = 0, w, h, path, rounding = 0, visible }
//     Decodes `path` (PNG/JPG/WEBP/SVG/AVIF/JXL - whatever the installed
//     libhyprgraphics supports) and draws it as a texture. Size-to-content
//     by default - the image's own natural pixel size - unless `w`/`h`
//     are given, in which case the image is scaled/stretched to fill that
//     box (unlike Text, which never stretches its own rendered glyphs -
//     stretching a photo/icon to a requested size is the normal/expected
//     thing, matching plain CSS `<img>` sizing). Decoding is synchronous
//     and happens immediately (at construction, and again on every
//     set_image() call) - a missing file or unsupported/corrupt format
//     doesn't error the whole window{} out, it just logs a warning and
//     leaves that Image drawing nothing (0x0 unless w/h were given).
//
//   Divider{ id, x = 0, y = 0, length, thickness = 1,
//            orientation = "horizontal"|"vertical", color }
//     A thin separator line - purely Lua-side sugar over a plain Box with
//     a computed w/h, not a new C++ widget behavior. `length` is the
//     dimension along the divider's own axis (maps to `w` when
//     horizontal, the default, or `h` when vertical); `thickness` is the
//     perpendicular one.
//
//   Checkbox{ id, x = 0, y = 0, w, h, color, checkedColor, rounding = 0,
//             checked = false, onChange, visible }
//     Checked/unchecked only (v1 scope) - explicitly not an animated
//     iOS-style toggle switch. Renders like Box normally (flat-filled
//     `color`); when checked, draws a smaller filled square inset inside
//     it using `checkedColor` (a plain rect indicator, not a checkmark
//     glyph - this toolkit has no icon font to draw one with). Same click
//     semantics/lifetime as Button (left-click, press+release must land
//     on the same widget, same InputHook.cpp plumbing) - the one real
//     difference is a Checkbox owns its own boolean state: a successful
//     click TOGGLES it before firing `onChange(checked)` with the new
//     value, rather than just notifying "clicked" and leaving all state
//     to the caller. `checked` seeds the initial state without going
//     through onChange (same "no invocation on load" convention
//     set_text()/set_input_text() use elsewhere). onChange errors are
//     caught and logged, not propagated, same as onClick.
//
// Composability (Phase 9, DESIGN.md):
//
//   defineComponent(name, { props?, render })
//     Registers a reusable widget template under the string `name` -
//     defining it doesn't have to happen in the same file it's used from,
//     since this is just a plain Lua-callable function (require() a
//     module that calls it, same as any other Lua code). Errors if `name`
//     is already registered.
//
//     `props` (optional) is a schema table: each entry is either
//     `{ required = true }` (must be given at every Component() call) or
//     `{ default = value }` (optional - falls back to `value` if omitted;
//     `value` can be any Lua type, including a table or function). A prop
//     can't be both required and have a default. Passing a prop at
//     Component() time that isn't in the schema is an error (typo
//     protection), same "fail loud on developer mistakes" convention used
//     everywhere else in this file.
//
//     `render` is a plain Lua function `function(props) ... end` that
//     returns EXACTLY ONE widget (the direct result of a single
//     hyprlui.Box{}/hyprlui.Column{}/etc. call, i.e. the same shape any
//     widget constructor already returns) - it's ordinary tree-building
//     code, nothing new to learn beyond what every other widget example
//     in this file already does. Runs once per Component() call, not on
//     any kind of re-render/update cycle - there is no React/Vue-style
//     per-instance component state in this system. Anything `render`
//     closes over from OUTSIDE itself (a `local` above it in its
//     defining file) is an ordinary Lua upvalue: SHARED across every
//     instance of that component everywhere, exactly like a module-level
//     variable - not per-instance state. If render() errors, or doesn't
//     return a single tagged widget table, that propagates as a real
//     build-time failure (not caught/logged like onClick/onChange - this
//     runs synchronously during tree construction, same failure class as
//     a missing required field on any other widget).
//
//   Component(name, props?, opts?)
//     Instantiates a registered component - validates `props` against
//     its schema (applying defaults, erroring on anything missing/
//     unknown), calls render(validatedProps), and returns an ordinary
//     tagged widget-spec table - slots into a parent's children exactly
//     like hyprlui.Box{}/etc. would, so it composes with everything else
//     in this file for free (nesting, Row/Column, Stack, other
//     Component() calls, ...).
//
//     Every explicit `id` set inside render()'s returned subtree is
//     rewritten to be collision-free per instance: the ROOT widget's own
//     id becomes exactly the instance key (see `opts.key` below); every
//     DESCENDANT's explicit id becomes `key .. "::" .. originalId`. A
//     component author can safely reuse the same ids across every call
//     (e.g. always `id = "label"` for the inner text) - the actual final
//     ids are always unique per instance. Duplicate ids WITHIN one
//     render() call's own output (e.g. two children both explicitly
//     `id = "label"`) are still a hard error, same as anywhere else in a
//     window{} tree (see below).
//
//     `opts` (optional) is a table of the same base widget fields every
//     other widget already accepts at its own call site - x/y, padding,
//     margin, opacity, zIndex, visible, debug/debugShow, etc. - overlaid
//     onto the rendered root AFTER render() returns, so a component's own
//     render() doesn't need to hardcode or forward its own position;
//     that stays purely the caller's concern, same as everywhere else in
//     this toolkit. `opts.key` (string) sets the instance key explicitly
//     - needed if you want to address this specific instance from
//     outside later (e.g. `remove_widget(window, key)` for the whole
//     instance, or `set_text(window, key .. "::label", ...)` for a piece
//     inside it). Without an explicit key, one is auto-generated
//     (`name .. "#" .. N`) - fine for a component you'll never need to
//     address again by id (e.g. most items in a list), not something to
//     rely on being predictable.
//
//     Example - a labeled button used twice, each independently
//     addressable via its own key:
//
//       hyprlui.defineComponent("LabeledButton", {
//           props = {
//               label = { required = true },
//               color = { default = 0x333333 },
//               onClick = { required = false },
//           },
//           render = function(props)
//               return hyprlui.Button({
//                   id = "btn", w = 120, h = 32, color = props.color, onClick = props.onClick,
//                   hyprlui.Text({ id = "label", text = props.label }),
//               })
//           end,
//       })
//
//       hyprlui.window{
//           name = "toolbar", anchor = "top",
//           hyprlui.Row{ gap = 8,
//               hyprlui.Component("LabeledButton", { label = "Save", onClick = saveFn }, { key = "save_btn" }),
//               hyprlui.Component("LabeledButton", { label = "Cancel" }, { key = "cancel_btn" }),
//           },
//       }
//
//     -- later: set_text("toolbar", "save_btn::label", "Saving...")
//
// Every window{} tree also rejects a plain duplicate id outright (not
// just the Component()-instance case above) - reusing the same explicit
// `id` twice anywhere in one window{} call is a hard error at build time.
// Previously silent: findWidget() just returns the first match, so a
// second/third widget sharing an id was permanently unreachable by
// set_text()/remove_widget()/etc. with no signal anything was wrong.
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
//   set_widget_visible(window, id, visible)
//     Shows/hides a single widget (and its subtree) within an existing
//     window - same "toggle without destroying/recreating" idea as
//     set_canvas_visible() above, one level down. The widget's own state
//     (text content, children, id) is untouched either way, it just stops
//     being drawn/hit-tested while hidden (same `visible` mechanism every
//     widget's constructor-time `visible` field already sets - this is
//     just the runtime mutator for it). Hiding the currently-focused Input
//     blurs it first, same reasoning as set_canvas_visible().
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
//   set_image(window, id, path)
//     Re-decodes an existing Image widget from a new file path (e.g.
//     swapping an icon) - synchronous, same as construction. Logs a
//     warning (doesn't error) and leaves the Image drawing nothing if the
//     new path fails to load.
//
//   set_checkbox_checked(window, id, checked)
//     Sets an existing Checkbox widget's checked state programmatically -
//     does NOT invoke onChange, same reasoning set_text() vs. a live
//     click uses.
//
//   get_checkbox_checked(window, id)
//     Returns an existing Checkbox widget's current checked state.
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
