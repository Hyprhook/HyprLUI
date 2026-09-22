# HyprLUI Design

Living design doc for the widget-based rewrite. Tracks decisions made in
discussion, verified facts about what Hyprland's plugin API actually
supports, and the active task list. Update this as tasks land or decisions
change - it's meant to survive across sessions, not be a one-time plan.

> **Housekeeping note**: this file used to track work as 21 numbered phases
> (0-20), each with a full blow-by-blow narrative of the research/bugs/
> decisions that went into it. All 21 shipped. Once there was a working demo
> (`demos/which-key.lua`) to validate against, the project moved off
> phase-tracking to a plain task list (below) - closer to how the project
> actually operates day to day now. The old phase narratives were compressed
> into one-line entries under "Completed history"; the full history is still
> in git if a past decision's exact reasoning is ever needed again.

## Goal

A small UI toolkit, embedded in a Hyprland plugin, exposed to `hyprland.lua`
config so users can define windows/popups/HUDs declaratively - vocabulary
and ergonomics aimed at people already familiar with QuickShell (QML) and
eww, without building a full external renderer (no GTK/Qt, no real
wlr-layer-shell client surfaces). Everything renders via Hyprland's own
render pass and reads input via Hyprland's own event bus - HyprLUI has no
independent Wayland surface.

## Current state

- `Window` (a `CCanvas` internally) holds one root `Widget`. Containers
  (`Stack`/`Row`/`Column`) lay out children via a two-pass measure/arrange
  walk; leaves (`Text`, `Box`, `Button`, `Input`, `Checkbox`, `Image`,
  `Divider`) render themselves. `UIManager` singleton owns every canvas.
- Render backend (`src/render/gfx.cpp`) queues `CRectPassElement`/
  `CTexPassElement`/`CBorderPassElement` into `g_pHyprRenderer->m_renderPass`
  during `RENDER_PRE_WINDOWS` (background layer) / `RENDER_LAST_MOMENT`
  (overlay layer) - direct GL calls do **not** reach the presented frame in
  this pipeline, pass elements are required.
- Damage tracking: a single `damage()` call isn't enough to guarantee a
  change actually shows up - Hyprland's swapchain needs several consecutive
  real frames of re-damage before a stale buffer catches up
  (`REDAMAGE_FRAMES` countdown in `CCanvas`, the same trick Hyprland's own
  `NotificationOverlay` uses). Both discrete mutations and continuous
  animations re-arm this countdown every frame something's actually
  changing (`CWidget::isAnimating()`).
- **Config-reload lifecycle, deliberate mitigation, not a finished design**:
  a config reload re-runs the Lua script from scratch, but does NOT reload
  the plugin itself - HyprLUI's own C++ state (canvases, watchers, reserved-
  area contributions) would otherwise silently outlive Lua's own
  bookkeeping. Mitigated by wiping everything on `config.preReload` - closes
  the leak, but "every reload closes every window" is blunt. Active task
  list item 5 (persistent windows) addresses the single most common driver
  of this (always-present UI, e.g. a status bar); the broader question -
  distinguishing ephemeral vs. always-present UI in general, and giving Lua
  any way to know/react to what got torn down - is still open, see Open
  questions.

This proves the render pipeline end-to-end; the pieces below build on it.

## Architecture

### 1. Widget tree

- `Widget` has children; `Window` holds one root `Widget`.
- Containers: `Stack` (manual/absolute positioning, an escape hatch),
  `Row`, `Column` (flexbox-lite: direction, gap, padding, align,
  size-to-content vs. fixed size).
- Leaves: `Text`, `Box`, `Button`, `Input`, `Image`, `Divider`, `Checkbox`.
- A layout pass (measure/arrange) runs before render every frame - no
  layout-dirty flag, full relayout is cheap at HUD scale.
- Extension model: subclass `CWidget`, implement `render()` (+
  `measureContent()`/`arrangeChildren()` for containers).

### 2. Window

- **Anchor + margin** instead of raw global `x`/`y`:
  `anchor = "top-right", margin = {10, 10}`.
- **Explicit monitor targeting** (by output name, or the focused monitor by
  default) via Hyprland's own window/layer-rule `mon:` selector syntax.
  Resolved once, at creation - an anchored window never teleports to a
  different monitor just because focus moved.
- **Size-to-content** by default; fixed `w`/`h` as an override.
- `exclusive` flag + edge, feeding the reserved-area composer (below) -
  composes multiple HyprLUI windows' contributions on top of the user's own
  `monitor{reserved:...}` baseline without clobbering it.
- Visibility toggle, with an opt-in fade in/out via Hyprland's own animated-
  variable machinery (`hyprlui.animation({leaf="in"|"out", ...})`).

### 3. Reactivity: named watchers

Lua has no property-binding language feature, so this is deliberately
explicit rather than magic - matches the eww model the target users
already know:

```lua
hyprlui.watch("volume", function() return get_volume() end, { interval = 500 })
Text{ text = Bind("volume") }
```

- A watcher fires on a poll interval or an explicit
  `hyprlui.notify("name")` call.
- Firing marks every widget property that referenced that binding dirty;
  only those re-evaluate.
- No automatic dependency-tracking reactivity (metatable-based
  `__index`/`__newindex` proxies) - considered and deliberately rejected
  more than once (explicit calls over metatable magic is a consistently
  applied project-wide preference, see Phase 3/9/11/13 in the old history).

### 4. Reserved area / exclusive zones

`PHLMONITOR->m_reservedArea` is what tiling actually consults - setting it
genuinely pushes tiled windows aside. Hyprland's own dynamic-reservation
slots (`eReservedDynamicType`) are a closed enum a plugin can't get a slot
in, so HyprLUI composes its own contributions itself: track each exclusive
window's reserved margin per edge per monitor, sum them, `setStatic()` the
combined total on top of the user's own config baseline (read fresh from
`m_activeMonitorRule.m_reservedArea`, never from the live object, to avoid
double-counting HyprLUI's own last write). `ReservedAreaComposer`
(currently owned by `UIManager`) re-applies on both monitor layout changes
and config reloads (Hyprland's own `applyMonitorRuleSoft()` silently
overwrites the static tier on both).

**Known gap**: multiple HyprLUI exclusive windows on the *same* edge don't
stack relative to each other - each excludes only its own contribution, so
each computes its position as if it were the only one reserving that edge.
Explicitly deferred indefinitely (see active task list item 6) - no
concrete need for it yet.

### 5. Input handling

- `Event::bus()->m_events.input.{mouse,keyboard}` gives raw
  `Cancellable<T>` signals - the same shape the `render` hook already
  uses. Setting `info.cancelled = true` swallows an event before it
  reaches whatever would normally receive it.
- No real Wayland surface needed - raw input interception is enough, no
  layer-shell-equivalent client surface required.
- Hyprland gives coordinates only, no widget-aware hit-testing - `InputHook`
  walks the visible windows (topmost first) and hit-tests against the
  widget tree's own boxes.
- A focused `Input` never outranks a real Hyprland keybind - `InputHook`
  checks `findConflictingBind()` (with the *press-time* modifier mask,
  cached per keycode - live re-checks at release desync from held-key
  bookkeeping, see the old Phase 6 history for the four-round debugging
  story behind this) before ever touching HyprLUI's own focus system.
- **Caveat**: `keyboard.key` is raw keysym/scancode level, not the
  IME/text-input-v3 composition protocol - fine for a simple entry field,
  will not get proper CJK/dead-key composition. Known limitation, not a
  blocker.

### 6. Lua API shape

- Declarative construction is the primary path:
  `hl.plugin.hyprlui.window{ anchor = ..., <tree> }`.
- Every widget gets an `id`; targeted mutators (`set_text(window, id,
  text)`, etc.) update it after construction - no full-tree diffing/
  reconciliation engine, a change not covered by a targeted setter just
  means removing and re-adding that subtree.
- `Bind(name)` / `hyprlui.watch(name, fn, opts)` / `hyprlui.notify(name)`
  for reactivity.
- `hyprlui.defineComponent(name, {props?, render})` /
  `hyprlui.Component(name, props?, opts?)` for reusable widget templates -
  see the old Phase 9 history for the full scoping-rules writeup (`render`
  is a plain Lua closure, no per-instance component state, ids
  auto-rewritten per instance to avoid collisions).

> **Reminder:** `stubs/hyprlui.meta.lua` (LuaLS annotations for
> `hl.plugin.hyprlui.*`) is hand-maintained, not generated - Hyprland's own
> stub generator (`meta/generateLuaStubs.py` in the Hyprland repo) only
> understands its own internal `hl.*` binding pattern and types
> `hl.plugin.<name>` as `any`. **Whenever the API shape changes (new widget
> type, new field, new mutation call), update `stubs/hyprlui.meta.lua` in
> the same change.** It's installed to `share/hypr/stubs/hyprlui.meta.lua`
> by the flake's `postInstall`, alongside Hyprland's own stubs.

## Non-goals (for now)

- No real `wlr-layer-shell` client surface - render-hook + pass-elements
  only.
- No IME/text-input-v3 composition - raw key events only.
- No automatic dependency-tracking reactivity in v1 - explicit
  watch/notify only.
- No general-purpose diffing/reconciliation engine for tree updates -
  targeted setters + subtree replace is enough for config-driven UIs that
  change infrequently. (Task list item 5's window-persistence mechanism
  doesn't change this - it's a whole-window open/closed toggle, not tree
  diffing.)

## Completed history

One line per shipped phase - full research/decision/bug narrative for any
of these lives in git history (`git log -p -- DESIGN.md`) if it's ever
needed again.

- [x] **Phase 0** - Proof of concept: flat canvas, rect/text nodes,
      imperative Lua bridge. Render pipeline confirmed working end-to-end.
- [x] **Phase 1** - Widget tree + layout engine (`Stack`/`Row`/`Column`,
      padding/gap/align, size-to-content vs. fixed), full rewrite including
      the declarative Lua API.
- [x] **Phase 2** - Window anchors + explicit/focused monitor targeting,
      resolved once at creation via Hyprland's own `mon:` selector syntax.
- [x] **Phase 3** - Named watchers + `Bind()` reactivity (poll + explicit
      `notify()`), backed by Hyprland's internal event-loop timer. v1 scope:
      `Bind()` wired to `Text.text` only.
- [x] **Phase 4** - `Button` widget + pointer input hook + tree
      hit-testing. Left-click only, press-then-matching-release semantics.
- [x] **Phase 5** - Exclusive zones / `ReservedAreaComposer` - composes
      multiple HyprLUI windows' reserved-area contributions on top of the
      user's own config baseline without clobbering it.
- [x] **Phase 6** - `Input` widget + keyboard focus ownership (raw keysym
      only) - click-to-focus, absolute exclusion of real Hyprland keybinds,
      built-in text capture (type/Backspace).
- [x] **Phase 7** - Base widget properties: padding, margin, min/max sizing
      (forced the text-overflow default: truncate-with-ellipsis), opacity
      (multiplies with ancestors), z-index, runtime visibility toggle, and
      the per-widget debug overlay (`debug`/`debugCascade`/`debugShow`).
- [x] **Phase 8** - v1 widget catalog completion: `Image` (eager decode via
      `libhyprgraphics`), `Divider` (pure Lua sugar over `Box`), `Checkbox`
      (checked/unchecked, owns its own toggle state).
- [x] **Phase 9** - Widget composability: `hyprlui.defineComponent()`/
      `hyprlui.Component()` - named, string-referenced templates with a
      validated props schema and automatic id-collision-safe key rewriting.
- [x] **Phase 10** - Interactive widget layer: `disabled`/hover states
      (`hoverColor`/`disabledColor` + `onHoverStart`/`onHoverEnd`), cursor
      feedback, opt-in `onScroll`. `onClick` later generalized from
      Button-only to a shared `CWidget` base field.
- [x] **Phase 11** - `hyprlui.persistent(key, default)` - native in-memory
      store (scalars only) that survives a Lua config reload (the whole Lua
      state is destroyed and recreated on every reload, so an ordinary
      `local` can't).
- [x] **Phase 12** - Native services layer: `hyprlui.run_cmd()` /
      `hyprlui.open_socket()` (Unix domain only) - polling-based, not
      Hyprland's `doOnReadable()`, which has a confirmed gap (HANGUP can
      swallow the callback on the exact moment a command finishes/a peer
      disconnects).
- [x] **Phase 13** - Fade/opacity visibility animations via Hyprland's
      animated-variable machinery (its animation *tree* has no public
      leaf-registration API for a plugin, confirmed dead end before
      writing code). `hyprlui.animation({leaf="in"|"out", ...})`. Also
      fires on widget/canvas create and remove, not just explicit
      visibility toggles.
- [x] **Phase 14** - Window position/size mutation primitives:
      `set_canvas_position()` / `set_canvas_size()`.
- [x] **Phase 15** - `fill`: stretch to match the parent's available size
      (CSS `align-self: stretch`, not `flex-grow` - no main-axis space
      distribution).
- [x] **Phase 16** - Hyprland-style window `style` support - `slide`
      (`slide left|right|top|bottom`), folded into the existing
      `animationIn`/`animationOut`/`hyprlui.animation()` config shape
      rather than a separate field.
- [x] **Phase 17** - `popin`/`gnome` window styles - window-root only,
      matching Hyprland's own actual scope (a real Hyprland window is one
      flat texture, no sub-element concept to generalize).
- [x] **Phase 18** - Generalized `popin`/`gnome` from window-root-only to
      any widget - turned out to be a net simplification once each widget
      composes its own scale multiplicatively with its ancestors'.
- [x] **Phase 19** - Border/stroke support for widgets: `gfx::drawBorder()`
      via Hyprland's own `CBorderPassElement` (a true stroke, not a
      synthesized two-rects trick), CSS border-box model (inset, not
      Hyprland's own outward-growing window-border convention), full
      gradient support. Shipped per-widget (mirroring `rounding`'s
      existing pattern) - **since superseded by active task list item 4's
      move to a real shared rectangle base via inheritance.**
- [x] **Phase 20** - `CTextNode`'s measured height standardized per (font,
      point size), independent of string content - fixed a two-column
      row-drift bug found building the which-key demo.

## Active task list

Decided in a planning session after this file's phase history was reviewed
in full. Tracked here going forward instead of as numbered phases.

- [ ] **1. Window naming defaults** - `window{}` auto-generates a `name`
      when omitted, mirroring the existing widget `id` auto-generation
      (`__auto0`, `__auto1`, ...). `hyprlui.window()` returns the window's
      spec table back to the caller, including the resolved name (auto-
      generated or explicit), so a caller who wants to reference the window
      later (close it, toggle visibility) can grab the name off the return
      value with no separate lookup needed.
- [ ] **2. Box sizing default** - An unsized `Box` (no `w`/`h` given)
      defaults to `0x0` instead of erroring. Rationale: in practice an
      unsized Box is almost always paired with `fill` anyway (the
      which-key demo's `w=1, h=1, fill=true` placeholder is exactly this
      pattern by hand) - `0x0` just makes that the honest default instead
      of a workaround. Add a debug-flag-gated warning (reusing the
      existing per-widget `debug` flag, no new one): log **only** when `w`
      and `h` were both fully omitted (not explicitly `0`) **and** `fill`
      is not set - that specific combination means the box will be
      invisible with no other feedback.
- [ ] **3. `LuaBridge.cpp` refactor** (currently ~1800 lines) - split into:
      a new `parser/`-style directory for raw Lua-to-native resolution
      (organized by what's parsed: a core/native parser for number/string/
      boolean/table-or-not; shorthand parsers for the common number-or-
      table conventions - color, edge insets, gradients; an animation-
      config parser); per-widget-type builder functions (one function per
      type, handling only that type's unique fields, all funneling into
      the existing shared common-tail function that applies universal base
      properties - padding, margin, opacity, z-index, animation overrides,
      debug flags). Builder-pattern-style, no C++ class inheritance needed
      for this part. `luaWindow()`, the runtime mutator functions, and
      plugin registration/init stay in `LuaBridge.cpp` as-is.
      `buildWidget()`'s if-else dispatch chain stays a plain chain
      deliberately - not converting to a dispatch table now, revisit only
      if it actually becomes painful to maintain.
- [ ] **4. Rectangle as shared background-drawing base** - any widget that
      draws its own background becomes a real subclass of the `Box`/
      rectangle widget via inheritance: **Button, Input, Checkbox, and
      Image** (and any future background-drawing widget) extend the
      rectangle base rather than each separately re-implementing `color`/
      `rounding`/`borderColor`/`borderWidth` - those fields live once on
      the shared base. `Image` gains a genuine optional background-color
      fill it's never had before (default: **transparent**), drawn behind
      its texture - not just shared storage/parsing, real new visible
      behavior for `Image` specifically. **Layout containers (Row, Column,
      Stack) are explicitly excluded** - they don't draw a background, so
      border/rounding on them wouldn't mean anything; their own manual/
      automatic layout logic is unaffected. Supersedes Phase 19's
      per-widget-duplicated field approach (see Completed history above).
- [ ] **5. Config-reload persistence for whole windows** - a new,
      **separate** mechanism from `hyprlui.persistent(key, default)`
      (which already fully covers *values* surviving reload and needs no
      change) - this one is about whole windows' open/closed existence. A
      `persistent = true` attribute on window creation: HyprLUI tracks
      whether that window was open right before a config reload and
      automatically reopens it once the Lua script finishes re-running,
      without the config author needing to manually re-trigger their own
      startup/install logic. Motivating case: a status bar created via a
      one-time startup hook only gets created once - today a reload
      destroys it (the blunt wipe-everything mitigation, see Current
      state) and nothing recreates it, since the startup hook never fires
      again.
- [ ] **6. Exclusive-zone / monitor-span sizing** - same-edge exclusive-
      window stacking stays **explicitly deferred indefinitely**, no
      priority, documented known limitation (see Architecture section 4).
      Add real monitor-span sizing instead, replacing the which-key demo's
      current workaround (manually querying monitor width, setting it as a
      raw window `w`): two **independent** boolean flags (flex width to
      the monitor's width, flex height to the monitor's height - settable
      independently) plus a separate **monitor padding** value that insets
      the window from the true monitor edges once flexed (distinct from
      the existing widget-level `padding`/`margin`).
- [x] **7. Stack padding/margin** - confirmed no change: `Stack`'s manual/
      absolute positioning continues to ignore `padding`/`margin`
      entirely, by design.
- [ ] **8. Text overflow modes** - wrap explicitly rejected (would make
      text height content-dependent again, undoing Phase 20's fix). Add
      **hard-clip** as a third overflow option alongside the existing
      default truncate-with-ellipsis. Future, separate, not-yet-scoped
      feature: a marquee-style continuously-scrolling text mode - closer in
      scope to the fade/slide animation system than a simple overflow
      mode.
- [ ] **9. Runtime mutation - generic attribute setter** - one generic
      mutator, roughly `(window_name, widget_id, attribute_name: string,
      new_value)`, validating and setting any widget attribute under the
      hood. Covers opacity and z-index (both wanted) plus padding/margin/
      min-max sizing (lower priority but plausible) through one mechanism
      instead of a growing list of individual named mutators. Existing
      named mutators (`set_widget_visible`, `set_text`, etc.) stay
      unchanged as convenience shortcuts, not replaced. Opacity's lack of
      hit-testing interaction (a fully transparent widget is still
      clickable) confirmed as-is, no change - the user's own
      responsibility, not worth special-casing.
- [ ] **10. Mouse click handling - button-aware `onClick`** - `onClick`
      (already a base `CWidget` field) stays the one callback, but now
      receives an argument: which mouse button triggered it - left, right,
      or middle. Right/middle get the exact same press-then-matching-
      release semantics left click already has. (Supersedes an earlier
      separate-`onLeftClick`/`onRightClick`/`onScrollClick`-fields idea -
      do not implement that version.)
- [ ] **11. Button keyboard focus** - deferred as part of a bigger future
      feature: Button stays click-only for now, no Enter/Space activation.
      Flagged to revisit together as proper keyboard navigation (Tab-
      cycling focus through a Row/Column's items, Enter/Space activating
      whatever has focus) - Button's own keyboard-focus would be built as
      part of that, not standalone.
- [ ] **12. `onKey` modifier state** - `Input`'s raw keysym callback should
      also receive the held modifier state (Control, Alt, Super) alongside
      the existing keysym and pressed/released state. Only Shift currently
      reaches a handler implicitly (baked into the keysym itself).
- [ ] **13. Window click-consumption default** - change the default:
      windows consume/swallow all clicks landing within their bounds,
      including on non-interactive decoration (e.g. a plain `Box` with no
      `onClick`), matching how other UI toolkits (Qt, etc.) behave and how
      a real window naturally blocks clicks to whatever's behind it. This
      is a **behavior change** from today (currently only a widget with an
      actual click handler, or Button/Input/Checkbox's structural
      click-target status, consumes a click). Make it a **per-window
      option**, with the old pass-through/leak-through behavior available
      as an explicit opt-out.

## Open questions

Genuinely undecided, no concrete need forcing a decision yet - revisit if
one shows up.

- Where `ReservedAreaComposer` should live structurally (own singleton
  vs. a `UIManager` responsibility).
- Whether widget mutation handles should eventually become real Lua
  userdata with methods (`label:set_text(...)`) instead of the current
  free-function-by-id style (`hyprlui.set_text(window, id, text)`).
- Whether `Bind()` should extend beyond `Text.text` to other fields
  (`color`, `visible`, numeric fields) - same underlying mechanism would
  work, just not wired up, no concrete use case forcing it yet.
- No per-watcher removal (`hyprlui.unwatch(name)`) - watchers are assumed
  to be small, persistent, config-lifetime things (a clock, a volume
  poller), not created/destroyed per-window. Non-breaking addition
  whenever a real need shows up.
- Watcher poll ticks are independent per watcher (each its own
  `CEventLoopTimer`) rather than coalesced onto one shared timer - fine at
  the handful-of-watchers scale this is meant for; revisit only if someone
  registers enough polling watchers for the per-timer overhead to matter.
- Debug overlay's auto-show size threshold (`AUTO_MIN_W`/`AUTO_MIN_H`,
  `Widget.cpp`) has only been tuned once by eyeball off a single report
  (`48` → `96` width; height left at `16`). Revisit against a real variety
  of widget sizes/label combinations next time debug mode gets real use -
  including whether a single flat width threshold is even the right model
  vs. something that scales with the actual label text being measured.
- Config-reload design, beyond what active task list item 5 covers: no way
  for Lua to know *what* got torn down by a reload, or to react to it
  (`config.preReload` fires before the script re-runs, so the fresh script
  has no way to inspect what existed a moment ago). A fundamentally
  different alternative - `window{}`/`watch()` *reconciling* with what
  already exists under that name instead of wiping and recreating - would
  need this project's stated non-goal against a general diffing/
  reconciliation engine to be explicitly revisited, not just extended.
