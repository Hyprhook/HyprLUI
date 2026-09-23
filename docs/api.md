# HyprLUI Lua API reference

Reference and tutorial for `hl.plugin.hyprlui.*`, the API a Hyprland Lua
config uses to declare windows/popups/HUDs as widget trees and mutate them
afterwards by id. Implemented in `src/ui/LuaBridge.cpp`; LuaLS type
annotations live in `stubs/hyprlui.meta.lua` for editor autocomplete.

Widget constructors (`Box`, `Text`, `Row`, ...) each tag and return their
table argument, so they nest via plain Lua table-literal syntax -
`Column{ gap = 8, Text{...} }` puts the `Text{}` result at index 1 of the
`Column` table.

## Base widget properties

Every widget below also accepts these fields, on top of whatever's listed
for its own type:

- **`padding`, `margin`** - either a single number (all four sides) or a
  table `{ top, right, bottom, left }` (an omitted side is `0`, not the
  uniform value - same shorthand convention `color`'s table form uses).
  `padding` insets a *container's* own children from its edges - only
  `Row`/`Column` and `Input`'s auto-owned label interpret it; `Stack`'s
  manual/absolute positioning leaves it unused by design. `margin` is a
  widget's own requested space around *itself*, read by whichever
  container lays it out - only `Row`/`Column` read a child's margin
  (added on top of `gap`, CSS-flexbox-item convention).
- **`minW`, `minH`, `maxW`, `maxH`** - clamp the measured size on each axis
  independently, after any fixed `w`/`h` (same precedence CSS gives
  min/max-width over an explicit width). `Text` content that doesn't fit
  `maxW` truncates with an ellipsis. `minW`/`minH` widen the layout box
  without stretching a `Text`'s rendered glyphs to fill it - every other
  widget type just gets visually bigger.
- **`opacity`** - `0`-`1`, default `1`. Multiplies with every ancestor's
  own opacity (a semi-transparent container fades its children too).
- **`zIndex`** - integer, default `0`. Reorders paint order among a
  widget's own siblings only (higher paints later/on top; ties keep
  insertion order) - not a full CSS stacking-context system.
- **`debug`, `debugCascade`, `debugShow`, `debugFontSize`** - box-model
  debug overlay: outlines for margin/content/padding boxes, small labels
  (padding/margin values, id, size, non-default zIndex/opacity), and a
  translucent fill over an interactive widget's hit-target. Drawn as a
  separate pass after everything else.
  - `debug` is tri-state: omitted inherits whatever the nearest ancestor
    resolved to (a window's root inherits `false`); an explicit
    `true`/`false` overrides and cascades to descendants until something
    deeper overrides it again.
  - `debugCascade` (default `true`) walls a subtree off from that
    inheritance when `false` - descendants start completely fresh.
  - `debugShow = { box, padding, margin, id, size, zOpacity, hitTarget }`
    (all optional booleans) force-overrides individual detail categories,
    bypassing the automatic "only show once big enough to render legibly"
    default.
  - `debugFontSize` (default `10`) is the label point size, same
    inheritance as `debug`.

## Widgets

- **`Stack{ id, x = 0, y = 0, w, h, visible, <children...> }`** -
  manual/absolute positioning, each child keeps whatever `x`/`y` it was
  given. Size-to-content is the bounding box of its children unless `w`/`h`
  are given.
- **`Row{ id, x = 0, y = 0, w, h, visible, gap = 0, align = "start"|"center"|"end", <children...> }`**
  **/ `Column{ ...same fields... }`** - flexbox-lite: packs children along
  the row/column axis with `gap` between them; `align` controls cross-axis
  alignment. No wrap, no justify/space-between.
- **`Text{ id, x = 0, y = 0, text, size = 16, color, font = "sans", visible }`**
  - `text` may be a plain string or `Bind(name)` (see Reactivity) to keep
  it tracking a watcher's current value.
- **`Box{ id, x = 0, y = 0, w = 0, h = 0, color, rounding = 0, borderColor, borderWidth = 0, visible }`**
  - a flat-filled rectangle. `w`/`h` default to `0` if omitted rather than
  erroring - almost always paired with `fill = true` in practice. If both
  are omitted *and* `fill` isn't set, the box is invisible; logs a warning
  when `debug` is on for it (own or inherited) to flag that specific
  combination. `color`/`borderColor` are either a packed
  `0xAARRGGBB` integer or a table `{ r, g, b, a }` (components in `[0,1]`),
  or - for `borderColor` - a gradient spec `{ colors = {...}, angle }`,
  mirroring Hyprland's own `general:col.active_border` syntax. The border
  is drawn *inset* into the widget's own box (CSS border-box - the box's
  size never changes), not grown outward the way Hyprland's window borders
  are. **`Box` does not render children** - unlike `Button`/`Input`
  below, it's a pure leaf; a `Text` (or anything else) nested inside a
  `Box{}` still gets positioned/measured but is never drawn. Use a
  `Stack` with the `Box` and the label as siblings instead (a background
  panel + content layered over it, not nested).
- **`Button{ id, x = 0, y = 0, w, h, color, rounding = 0, borderColor, borderWidth = 0, visible, onClick, <children...> }`**
  - like `Box`, but always a real click target structurally, even with no
  `onClick` set (left-click only). Children are positioned
  manually/absolutely inside it, same as `Stack` - typically a `Text`
  label. Only `Overlay`-zorder windows (the default) are clickable;
  `Background` windows are decorative. A `Button`'s own bounds are the
  only clickable area.
- **`Input{ id, x = 0, y = 0, w, h, color, rounding = 0, borderColor, borderWidth = 0, visible, text = "", textColor, textSize = 14, textFont = "sans", onChange, onKey, onFocus, onBlur, <children...> }`**
  - a focusable rectangle that behaves like an actual text field: typing
  appends a character, Backspace removes the last one, the current text
  renders automatically (inset by an 8px left `padding` default,
  vertically centered). Printable-ASCII only - no cursor/selection/IME/
  non-ASCII/clipboard; layer that on top of `onKey`, which keeps firing for
  every key regardless.
  - `onChange(text)` fires on real edits only, not a programmatic
    `set_input_text()` call.
  - Gains HyprLUI's single global keyboard-focus slot by being clicked
    (grabbed immediately on press, no "cancel by dragging off"), or
    programmatically via `focus_widget()`/`blur_widget()`. Clicking
    anything else blurs it.
  - `onKey(keysym, pressed)` fires for every key while focused, in addition
    to the built-in capture. A key that currently triggers a real Hyprland
    keybind never reaches a focused `Input` at all - this is absolute, no
    per-widget opt-out (only global-scope binds are excluded this way; a
    submap-specific bind can still reach a focused `Input`). A bare
    modifier press/release is excluded the same unconditional way.
- **`Image{ id, x = 0, y = 0, w, h, path, rounding = 0, borderColor, borderWidth = 0, color, visible }`**
  - decodes `path` (PNG/JPG/WEBP/SVG/AVIF/JXL) and draws it as a texture,
  size-to-content by default unless `w`/`h` are given (stretches to fill,
  unlike `Text`). `color` is an optional solid fill drawn behind the
  texture (default: transparent) - useful as a placeholder/letterbox
  behind a transparent image or before one loads, or as a visible
  fallback if it fails to load. A missing file or unsupported format logs
  a warning and leaves the widget drawing just its fill/border (no
  texture), rather than erroring the whole window out.
- **`Divider{ id, x = 0, y = 0, length, thickness = 1, orientation = "horizontal"|"vertical", color }`**
  - a thin separator line, pure sugar over a `Box` with a computed `w`/`h`.
  `length` is the dimension along the divider's own axis.
- **`Checkbox{ id, x = 0, y = 0, w, h, color, checkedColor, rounding = 0, borderColor, borderWidth = 0, checked = false, onChange, visible }`**
  - checked/unchecked only, not an animated toggle switch. Renders like
  `Box`; when checked, draws a smaller filled square inset inside it using
  `checkedColor`. A successful click toggles its own state before firing
  `onChange(checked)` with the new value. `checked` seeds the initial state
  without invoking `onChange`.

## Interactive state

Every widget accepts these; a field only actually does anything on a
widget that's interactive - either structurally (`Button`/`Input`/
`Checkbox`) or because `onClick`/`onScroll` below was set. A decorative
`Box` with neither is inert.

- **`onClick`** - a plain no-argument callback. What actually *makes* a
  widget a real click target for anything that isn't already one
  structurally - set this on a `Box`/`Text`/`Image`/`Row`/`Column`/`Stack`
  to make that specific widget clickable, with the same
  press-must-land-on-the-same-widget-as-release semantics `Button` always
  has. A child's own click target (if any) always gets first refusal over
  an ancestor's `onClick`. Errors are caught and logged, not propagated.
- **`disabled`** - boolean, default `false`. Excludes the widget from
  hit-testing entirely - click-through/unfocusable, while it still
  renders. Mutable at runtime via `set_widget_disabled()`.
- **`hoverColor`, `disabledColor`** - optional colors, applied
  automatically in place of the widget's own `color` while
  hovered/disabled. `disabledColor` wins if both could apply (though a
  disabled widget is never hovered, since it's excluded from hit-testing).
- **`onHoverStart`, `onHoverEnd`** - fire on the hover transition, no
  arguments - the escape hatch for anything beyond a flat color swap.
  Cursor feedback (pointer cursor while hovering) happens automatically
  regardless of whether either is set.
- **`onScroll(delta, vertical)`** - stays completely inert unless set (the
  scroll event passes through to whatever's behind it otherwise). Same
  hit-target-making effect as `onClick` on a plain widget. `delta` is the
  raw axis-event value, unnormalized; `vertical` is `true` for the common
  mouse-wheel axis.
- **`animationIn`, `animationOut`** - optional tables
  (`{ enabled?, speed?, bezier?, spring?, style? }`, same shape as
  `hyprlui.animation()`'s own table minus `leaf`) overriding the *global*
  `hyprlui.animation({leaf="in"|"out", ...})` config for just this widget.
  Self-contained, not a partial merge - a widget setting
  `animationIn = { speed = 5 }` does not inherit the global bezier/spring.
  Applies uniformly regardless of *why* visibility changed - an explicit
  `set_widget_visible()` call, `remove_widget()`, or (for a window's root)
  the window opening/closing.
  - `bezier`/`spring` name a curve already registered process-wide
    (shared with Hyprland's own window/workspace animations) - at most one
    of the two; `bezier` wins if somehow both are given.
  - `style` layers a position-slide or scale animation on top of the
    opacity fade, reusing Hyprland's own `windowsIn`/`windowsOut` style
    string syntax: `"slide"` or `"slide left|right|top|bottom"` (direction
    defaults to `"left"`); `"popin"` or `"popin N%"` (minimum shrink size,
    default `0`); `"gnome"`/`"gnomed"`. Applies to *any* widget, not just a
    window's root - a window's root sliding/shrinking *is* the whole
    window doing so. Nested styles compose multiplicatively.

  ```lua
  hl.plugin.hyprlui.window({
    name = "panel", anchor = "top-right",
    hl.plugin.hyprlui.Box({
      id = "root", w = 240, h = 60, color = 0xff223344,
      animationIn = { speed = 3, bezier = "default", style = "popin 60%" },
      animationOut = { speed = 3, bezier = "default", style = "popin 60%" },
    }),
  })
  ```

- **`fill`** - boolean, default `false`. "Stretch to match my parent's
  available size instead of sizing from my own content" (CSS
  `align-self: stretch`, not `flex-grow`). Interpreted per parent:
  - `Row`/`Column`: stretches to the full cross-axis space; the main axis
    stays sized from the widget's own content.
  - `Stack`: matches the stack's own full size at `(0, 0)`.
  - A window's root widget: matches the window's size, but only on axes
    where the window has a determinate size (an explicit `w`/`h`).

  A widget with nothing establishing a real size on some axis (e.g. a
  `Stack` whose only child is also `fill`) has nothing to stretch to and
  stays at its own natural size - matches CSS stretch against an `auto`
  parent, not a bug.

Every `window{}` tree rejects a duplicate explicit `id` outright - reusing
the same `id` twice anywhere in one `window{}` call is a hard error at
build time (mutators like `set_text()`/`remove_widget()` address a widget
by id and only ever find the first match, so a silent collision would make
the second widget permanently unreachable).

## Composability

- **`defineComponent(name, { props?, render })`** - registers a reusable
  widget template under the string `name`. Doesn't have to be defined in
  the same file it's used from (`require()` a module that calls it, like
  any other Lua code). Errors if `name` is already registered.
  - `props` (optional) is a schema table: each entry is either
    `{ required = true }` or `{ default = value }` (`value` can be any Lua
    type) - never both. Passing a prop at `Component()` time that isn't in
    the schema is an error.
  - `render` is a plain Lua function `function(props) ... end` returning
    exactly one widget - ordinary tree-building code, nothing new beyond
    any other widget example above. Runs once per `Component()` call, not
    on a re-render/update cycle - there is no per-instance component state.
    Anything `render` closes over from *outside* itself is an ordinary Lua
    upvalue, shared across every instance of that component, like a
    module-level variable. If `render` errors, or doesn't return a single
    tagged widget table, that propagates as a real build-time failure.
- **`Component(name, props?, opts?)`** - instantiates a registered
  component: validates `props` against its schema, calls
  `render(validatedProps)`, and returns an ordinary tagged widget-spec
  table that composes with everything else in this API (nesting, `Row`/
  `Column`, other `Component()` calls, ...).
  - Every explicit `id` inside `render()`'s returned subtree is rewritten
    to be collision-free per instance: the root's own id becomes the
    instance key (`opts.key`, or an auto-generated `name#N`); every
    descendant's explicit id becomes `key .. "::" .. originalId`. A
    component author can safely reuse the same ids across every call.
  - `opts` (optional) carries the same base widget fields every other
    widget accepts (`x`/`y`, `padding`, `opacity`, `visible`, ...),
    overlaid onto the rendered root after `render()` returns. `opts.key`
    sets the instance key explicitly - needed to address this instance
    later (`remove_widget(window, key)`, or
    `set_text(window, key .. "::label", ...)` for a piece inside it).

```lua
hyprlui.defineComponent("LabeledButton", {
    props = {
        label = { required = true },
        color = { default = 0x333333 },
        onClick = { required = false },
    },
    render = function(props)
        return hyprlui.Button({
            id = "btn", w = 120, h = 32, color = props.color, onClick = props.onClick,
            hyprlui.Text({ id = "label", text = props.label }),
        })
    end,
})

hyprlui.window{
    name = "toolbar", anchor = "top",
    hyprlui.Row{ gap = 8,
        hyprlui.Component("LabeledButton", { label = "Save", onClick = saveFn }, { key = "save_btn" }),
        hyprlui.Component("LabeledButton", { label = "Cancel" }, { key = "cancel_btn" }),
    },
}

-- later:
set_text("toolbar", "save_btn::label", "Saving...")
```

## Reactivity

```lua
hyprlui.watch("volume", function() return get_volume() end, { interval = 500 })
Text{ text = Bind("volume") }
```

- **`watch(name, fn, opts?)`** - registers `fn` (no arguments) as a named
  watcher; its return value is cached and stringified. `fn` is called once
  immediately to seed the initial value. `opts.interval` (milliseconds),
  if given, re-calls `fn` on that cadence via Hyprland's own event-loop
  timer, independent of render activity. Errors if `name` is already
  registered. Watcher function errors are caught and logged, not
  propagated - the last good value is kept.
- **`notify(name)`** - re-invokes a watcher's function right now. Only
  needed for watchers without a poll `interval`, or to force an immediate
  refresh of one that has one.
- **`Bind(name)`** - ties a widget property (currently just `Text.text`)
  to watcher `name`'s current value, re-reading it every frame. Errors if
  `name` isn't a registered watcher yet at the point the `window{}` using
  it gets built - `watch()` must run first.

## Persistence

```lua
local vol = hyprlui.persistent("volume", 50)
print(vol:get())      -- 50 the first time ever run; whatever was
                       -- last set() otherwise, even across a reload
vol:set(vol:get() + 5)
```

- **`persistent(key, default)`** - returns a wrapper table
  (`:get()`/`:set(value)`) backed by a native store that survives a Lua
  config reload (unlike an ordinary `local`, which resets, since the whole
  config script re-runs on every reload). Survives a config reload, but
  *not* a full plugin unload or Hyprland restart - a pure in-memory store.
  `default` may be a number, string, or boolean - no tables/functions -
  and is only ever used the first time `key` has never been seen; every
  later call for the same `key` returns whatever's already stored,
  ignoring `default` (a type mismatch logs a warning but stays permissive).

## Native services

Exactly two generic primitives - everything higher-level (polling `pactl`,
talking to a D-Bus proxy over its socket, etc.) is meant to be built in
pure Lua on top of these. Both are ephemeral, script-scoped resources -
any still-in-flight command/socket is torn down on the next config reload.

- **`run_cmd(cmd, callback)`** - runs `cmd` via `/bin/sh -c`,
  asynchronously, one-shot. `callback(output)` fires exactly once with
  everything the command printed to stdout, once its stdout closes. No
  exit code is available. On a spawn failure, `callback("")` still fires
  (logged as a warning).

  ```lua
  hyprlui.run_cmd("pactl get-sink-volume @DEFAULT_SINK@", function(out)
    print(out)
  end)
  ```

- **`open_socket(path, callback)`** - connects a Unix domain socket to
  `path` (Unix domain only) and calls `callback(sock)` once connected, or
  `callback(nil)` on a connect failure. `sock` is a wrapper table:
  - `sock:read(callback)` - calls `callback(data)` once exactly one
    `read()` call's worth of data is available, or `callback(nil)` once
    the peer closes the connection. Only one pending `:read()` at a time.
  - `sock:write(data)` - a single best-effort `write()` call, no
    partial-write retry/buffering.
  - `sock:close()` - closes the connection early. Safe to call more than
    once.

## Window construction and mutation

- **`window{ name?, x = 0, y = 0, w, h, zorder = "overlay"|"background", anchor, monitor, exclusive, spanWidth = false, spanHeight = false, monitorPadding = 0, hotReload = false, <exactly one root widget> }`**
  - opens a new window with the given widget tree as its root. If `w`/`h`
  are omitted the window sizes itself to the root's measured content.
  Errors if `name` is already in use. `name` is optional - if omitted, one
  is auto-generated (`"__window0"`, `"__window1"`, ...), mirroring how a
  widget's own `id` auto-generates when omitted. **Returns the spec table
  back**, with `name` set to whatever was actually used (given or
  auto-generated) - grab it off the return value to reference this window
  later (`remove_canvas`, `set_canvas_visible`, etc.) without having to
  name it yourself: `local win = hyprlui.window{ ... }; ...
  hyprlui.remove_canvas(win.name)`.
  - Without `anchor`: `x`/`y` are a raw global (compositor-space)
    position.
  - With `anchor` (one of `top-left`/`top`/`top-right`/`left`/`center`/
    `right`/`bottom-left`/`bottom`/`bottom-right`): `x`/`y` are
    reinterpreted as an offset from that point on the target monitor's
    usable box (excluding space other bars/panels have reserved) -
    positive always pushes inward. `monitor` is optional and uses the same
    selector syntax as Hyprland's own window/layer-rule `mon:` fields,
    resolved relative to whichever monitor is focused *right now* (there's
    deliberately no `"current"`/`"focused"` keyword - omit `monitor`
    entirely for that). If a selector matches nothing, this falls back to
    the focused monitor rather than erroring. The monitor is picked *once*,
    at creation time - it never changes afterwards, even if focus moves;
    its box is still re-read every frame, so a resolution change still
    repositions the window correctly.
  - `exclusive` (`"top"`|`"right"`|`"bottom"`|`"left"`) requires `anchor`,
    and must be an edge the anchor actually touches. Reserves screen-edge
    space equal to this window's own current size along the perpendicular
    axis - tiled windows on that monitor actually leave the space empty,
    and it tracks this window's live size. Composed with the user's own
    `monitor{ reserved: ... }` config baseline and any other HyprLUI
    exclusive windows on the same monitor/edge (summed, not max). A hidden
    window reserves nothing. **Known limitation**: multiple HyprLUI
    exclusive windows on the *same* edge don't stack relative to each
    other - fine for a single top bar, a second one needs manual x/y
    offsetting.
  - `spanWidth`/`spanHeight` (booleans, default `false`, require `anchor`)
    - flex that axis to the target monitor's own width/height (re-read
    live every frame, like the anchor position itself), instead of sizing
    from `w`/`h`/content - settable independently, e.g. `spanWidth = true`
    alone leaves height sized to content. Wins over an explicit `w`/`h` on
    the same axis if both are given. `monitorPadding` (a number, or a
    table `{top, right, bottom, left}` like `padding`/`margin`) insets the
    spanned edges from the monitor's true screen edges - measured against
    the raw monitor box, not what's already reserved by other bars/
    panels, so a spanning window can still visually reach edge-to-edge
    regardless of other exclusive zones elsewhere on the monitor.
  - `hotReload` (boolean, default `false`) - a config reload destroys and
    rebuilds *every* HyprLUI window unconditionally (the whole Lua
    interpreter is destroyed and recreated on every reload, so there's no
    safe way to keep a window's callbacks alive across one). `hotReload`
    doesn't change that - it just makes HyprLUI remember whether this
    window was visible right before the last reload, and restores that
    (instead of always opening visible) the next time `window{name=...,
    hotReload=true, ...}` is called for the same name. **This only works
    if that `window{}` call actually runs again on every reload** - e.g.
    from a `require()`d module's function called unconditionally at the
    top of the config (the same pattern `demos/which-key.lua` already
    uses), not from a one-time hook like `hl.on("hyprland.start", ...)`,
    which never fires again after the first boot. This is about *window
    existence/visibility* only, not the values inside it - use
    `hyprlui.persistent(key, default)` for those (a counter, a toggle
    state, etc.), which already survives reloads independently.
  - A window's own open/close slide is configured via its root widget's
    `animationIn`/`animationOut.style` field (see Interactive state above)
    - not a separate `window{}` field. A window *is* its root widget as
    far as visibility/animation goes.
- **`remove_canvas(name)`** - closes a window and everything in it.
- **`set_canvas_visible(name, visible)`** - shows/hides a window without
  destroying its content.
- **`set_canvas_position(name, x, y)`** - repositions an already-created
  window to an explicit global position, clearing any anchor it was
  created with first.
- **`set_canvas_size(name, w, h)`** - resizes an already-created window;
  `nil` for either axis lets it size-to-content again.
- **`set_widget_visible(window, id, visible)`** - shows/hides a single
  widget (and its subtree) without destroying/recreating it. Hiding the
  currently-focused `Input` blurs it first; hiding the currently-hovered
  widget un-hovers it.
- **`set_widget_disabled(window, id, disabled)`** - runtime mutator for
  `disabled` - same "blur/un-hover first" handling.
- **`set_widget_size(window, id, w, h)`** - resizes a single widget; `nil`
  for either axis lets it size-to-content again. Does not make the
  widget's content visibly grow/shrink to match on its own unless it
  (or a descendant) uses `fill`.
- **`set_text(window, id, text)`** - updates an existing `Text` widget's
  content in place.
- **`set_input_text(window, id, text)`** - sets an `Input`'s current text
  programmatically; does not invoke `onChange`.
- **`get_input_text(window, id)`** - returns an `Input`'s current text.
- **`set_image(window, id, path)`** - re-decodes an `Image` from a new
  path; logs a warning (doesn't error) and leaves it drawing nothing if
  the new path fails to load.
- **`set_checkbox_checked(window, id, checked)`** - sets a `Checkbox`'s
  state programmatically; does not invoke `onChange`.
- **`get_checkbox_checked(window, id)`** - returns a `Checkbox`'s current
  state.
- **`remove_widget(window, id)`** - removes a single widget (and its
  subtree); blurs it first if it held keyboard focus.
- **`focus_widget(window, id)`** - gives HyprLUI's keyboard focus to the
  named `Input` programmatically. Errors if `id` isn't an `Input`. A no-op
  if it's already focused.
- **`blur_widget()`** - blurs whichever `Input` currently has focus, if
  any. No arguments - there's only ever one focus slot.

## Examples

```lua
local hyprlui = hl.plugin.hyprlui
hyprlui.window{
    name = "greeting", x = 100, y = 100,
    hyprlui.Column{ id = "root", gap = 8, padding = 16,
        hyprlui.Box{ id = "bg", w = 300, h = 80, color = 0xcc111111, rounding = 8 },
        hyprlui.Text{ id = "label", text = "Hello!", size = 20 },
    },
}
hl.bind("SUPER + G", function() hyprlui.remove_canvas("greeting") end)

-- anchored to the top-right of the focused monitor's usable area,
-- 10px in from both edges:
hyprlui.window{
    name = "hud", anchor = "top-right", x = 10, y = 10,
    hyprlui.Text{ id = "label", text = "HUD" },
}

-- reactive: polled every 500ms, no manual set_text() needed:
hyprlui.watch("volume", function() return get_volume() .. "%" end, { interval = 500 })
hyprlui.window{
    name = "vol", anchor = "bottom",
    hyprlui.Text{ id = "label", text = hyprlui.Bind("volume") },
}

-- clickable:
hyprlui.window{
    name = "btn", anchor = "center",
    hyprlui.Button{
        id = "go", w = 120, h = 32, color = 0x333333, rounding = 6,
        onClick = function() hyprlui.remove_canvas("btn") end,
        hyprlui.Text{ x = 12, y = 8, text = "Close" },
    },
}

-- focusable: typing/Backspace/display are all built in - click it,
-- then type. onKey is only needed here for Return (submit):
hyprlui.window{
    name = "search", anchor = "center",
    hyprlui.Input{
        id = "field", w = 200, h = 28, color = 0x222222,
        onKey = function(keysym, pressed)
            if pressed and keysym == 0xff0d then -- XKB_KEY_Return
                print("submitted: " .. hyprlui.get_input_text("search", "field"))
            end
        end,
    },
}

-- exclusive + spanWidth: a 32px-tall top bar spanning the full monitor
-- width (minus 8px on each side), that also reserves its own height,
-- pushing tiled windows on this monitor down out of the way:
hyprlui.window{
    name = "bar", anchor = "top", exclusive = "top",
    spanWidth = true, monitorPadding = { left = 8, right = 8 },
    hyprlui.Box{ id = "bg", h = 32, fill = true, color = 0xff222222 },
}
```

Note the `Box` above is behind/outside the visible flow in the greeting
example only to illustrate nesting - a background panel that should sit
*behind* its siblings belongs in an outer `Stack` with the `Column`
absolutely positioned on top of it, since `Row`/`Column` packs every child
into the flow. See `DESIGN.md`'s Architecture section for the layout model
this is built on.
