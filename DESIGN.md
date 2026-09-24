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
- `CWidget::measureContent()`'s default resets a leaf to its own natural/
  intrinsic size every frame, not just once - needed so a widget stretched
  via `fill` self-corrects if whatever grew it (e.g. its parent) later
  shrinks back; without this it would stay stuck at its stretched size
  forever, since nothing else ever touches `m_size` again.
- Computed positions (anchor placement, flex layout) round to whole
  pixels - Hyprland samples with `GL_LINEAR` unless the destination is an
  exact 1:1 pixel match, so a fractional position blends two adjacent
  texels into one, visibly blurring text/images (invisible on a
  solid-color rect, which is why this is easy to miss in testing).

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
  see `docs/api.md` for the full scoping-rules writeup (`render` is a
  plain Lua closure, no per-instance component state, ids auto-rewritten
  per instance to avoid collisions).

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

- [x] **1. Window naming defaults** - `window{}` auto-generates a `name`
      (`"__window0"`, `"__window1"`, ... - a function-local static counter
      in `luaWindow()`, never reset, same "harmless, nothing depends on
      staying small" reasoning as `CComponentRegistry`'s own instance-id
      counter) when omitted, mirroring the existing widget `id` auto-
      generation. `hyprlui.window()` now returns the window's spec table
      back to the caller (the resolved `name` written onto it either way,
      given or generated) instead of nothing, so a caller who wants to
      reference the window later can grab the name off the return value
      with no separate lookup needed. `docs/api.md` updated. Build
      verified clean, standing extern-C leak check still at 0.
- [x] **2. Box sizing default** - An unsized `Box` defaults to `0x0`
      instead of erroring (`buildBoxWidget()`, `WidgetBuilders.cpp`).
      Warns via `Log::WARN` only when `w`/`h` are both fully omitted (not
      explicitly `0`) *and* `fill` isn't set.
      - The warning gate (`debug`) needed to be the **cascade-resolved**
        value, not just this widget's own literal field - `debug`
        normally gets set once near a window's root and inherited, so an
        own-field-only check would almost never fire in practice.
        `resolveDebugSpec()`'s cascade only runs per-frame though, not at
        construction time - so `buildWidget()` now separately computes
        the same merge once during construction (`inheritedDebug`
        parameter, threaded through the recursion) purely for this
        diagnostic; the real per-frame resolve for the debug overlay
        itself is untouched.
      - Build verified clean, standing extern-C leak check still at 0.
        `docs/api.md` updated.
- [x] **3. `LuaBridge.cpp` refactor** (was ~1800 lines) - split into
      `src/ui/parser/` (`ValueParsers` - number/string/boolean/table-or-
      not; `ShorthandParsers` - color, edge insets, gradients;
      `AnimationParsers` - curve/style/override; `CallbackParsers` -
      onClick/onKey/onChange/onScroll field-to-std::function wrapping) and
      `src/ui/WidgetBuilders.{hpp,cpp}` (one function per widget type,
      each handling only that type's unique fields, funneling into a
      shared `applyCommonWidgetProperties()` common-tail - padding,
      margin, opacity, z-index, debug flags, interactive state, animation
      overrides, fill). Builder-pattern-style, no C++ class inheritance
      needed for this part. `luaWindow()`, the runtime mutator functions,
      and plugin registration/init stayed in `LuaBridge.cpp` (now ~870
      lines). `buildWidget()`'s if-else dispatch chain stayed a plain
      chain, per the original plan - not converted to a dispatch table.
      Pure internal restructuring - the Lua-facing API in `docs/api.md`
      is unchanged, nothing in `demos/`/`hyprlandd.lua` needed touching.
      Build verified clean from a fresh `build/` dir, standing extern-C
      leak check still at 0.
- [x] **4. Rectangle as shared background-drawing base** - `CButtonWidget`,
      `CInputWidget`, `CCheckboxWidget`, and `CImageWidget` are now real
      subclasses of `CRectNode` (`: public CRectNode`, not `CWidget`
      directly) - `color`/`rounding`/`borderColor`/`borderWidth` and their
      setters live once on the shared base, not duplicated per class.
      `CImageWidget` gains a genuine optional background-color fill it
      never had before (`color`, default **transparent**), drawn behind
      its texture - real new visible behavior, not just shared storage.
      **Layout containers (Row, Column, Stack) stay untouched** - they
      don't draw a background, so this doesn't apply to them.
      Supersedes Phase 19's per-widget-duplicated field approach.
      - `CRectNode` split its rendering into `renderFill()`/
        `renderBorder()` (protected) so a subclass that draws its own
        content BETWEEN the two (Image's texture, Button/Input's
        children) can sequence fill → own content → border, keeping the
        border always on top instead of getting hidden under opaque
        content drawn after the old combined fill+border step. Checkbox's
        inner checked-square and Button/Input's children were reordered
        to this same fill → content → border shape (no visible pixel
        change for them - their content was already inset far enough
        from the border band either way).
      - `renderFill()` uses `effectiveFillColor()` (hover/disabled color
        swap) uniformly now, rather than each subclass's own copy of that
        call - as a side effect, a plain `Box` made interactive via
        `onClick`/`onScroll` + `hoverColor` now gets the same hover-color
        swap Button/Input/Checkbox already had, closing a gap that
        existed before this refactor (Box never called
        `effectiveFillColor()` at all).
      - A failed `Image` load now still draws its fill/border (useful as
        a visible fallback) instead of drawing nothing at all.
      - Build verified clean from a fresh `build/` dir, standing extern-C
        leak check still at 0. `docs/api.md` updated for `Image`'s new
        `color` field.
- [x] **5. `hotReload` window attribute** - renamed from the originally
      planned `persistent` during implementation: it isn't real
      persistence (the canvas itself is still destroyed and rebuilt on
      every reload, same as any other window - Hyprland's own
      `reinitLuaState()` `lua_close()`s the whole interpreter on every
      reload, so nothing holding a Lua callback ref could safely survive
      anyway), it's closer to a hot-reload of *visibility state* only.
      **Separate from `hyprlui.persistent(key, default)`** (unchanged,
      already covers arbitrary *values* surviving reload) - this is only
      about a window's open/closed state.
      - `CUIManager` gained a small `name -> bool` map
        (`m_hotReloadVisibility`), deliberately NOT wired into `clear()`
        (the reload-wipe) - only `PLUGIN_EXIT` clears it, same "must
        never be part of resetAllState()" pattern `CPersistenceStore`
        already established. `CCanvas` gained a `hotReload` bool flag.
      - `window{hotReload=true, ...}`: on creation, resolves initial
        visibility from the tracked value if one exists (seeding `true`
        on a first-ever run) instead of always opening visible.
        `set_canvas_visible()`/`remove_canvas()` keep the tracked value
        up to date on every explicit visibility change for such a
        window. The automatic reload-wipe itself never touches the
        tracked value - only explicit Lua-driven visibility changes do -
        so whatever was last explicitly set is exactly what's still
        there the instant `config.preReload` fires.
      - **Real usage constraint, not just an implementation detail**: this
        only works if the window's own `window{}` call actually runs
        again on every reload (e.g. from a `require()`d module's
        function called unconditionally, like `demos/which-key.lua`
        already does) - NOT from a one-time hook like
        `hl.on("hyprland.start", ...)`, which never fires again after
        first boot. That was the original motivating gap (a status bar
        built inside such a hook) - `hotReload` doesn't remove the need
        to restructure that pattern, it only means the config author no
        longer has to write their own preReload-detection/re-open logic
        once they do.
      - `docs/api.md`, `stubs/hyprlui.meta.lua` updated. Build verified
        clean, standing extern-C leak check still at 0.
- [x] **6. Exclusive-zone / monitor-span sizing** - same-edge exclusive-
      window stacking stays **explicitly deferred indefinitely**, no
      priority, documented known limitation (see Architecture section 4).
      Added real monitor-span sizing: `window{}` gained `spanWidth`/
      `spanHeight` (independent booleans, require `anchor`) and
      `monitorPadding` (number or `{top,right,bottom,left}`, reusing the
      same `optInsetsField()` parser as widget `padding`/`margin`). Wins
      over an explicit `w`/`h` on the same axis if both are given.
      - `CCanvas::resolveSpan()` re-reads the live monitor's raw box
        (not reserved-adjusted - "true monitor edges" per the task's own
        wording) every `render()` frame, same as anchor position already
        does, and overrides the size `render()`'s existing content-size
        sync would otherwise have used. `luaWindow()` also applies it
        once at creation time (using the monitor it already resolved for
        anchoring) so exclusive-zone contribution seeding isn't a frame
        late.
      - `demos/which-key.lua`'s manual `focusedMonitorWidth()` workaround
        (a raw `hl.get_monitors()` query) is gone, replaced by
        `spanWidth = true, monitorPadding = CONFIG.xOffset` - the actual
        motivating case this task named.
      - `docs/api.md`, `stubs/hyprlui.meta.lua` updated (including the
        `window{}` exclusive-bar example, which used to explicitly call
        out this as a missing feature). Build verified clean, standing
        extern-C leak check still at 0.
      - **Bug found live (task-checks.lua's own task 6 demo) and fixed
        same-session**: an unsized, `fill=true` root widget rendered at
        0x0 (invisible) on a spanned window - `render()`'s root-fills-
        canvas step only treated an axis as "determinate" (worth
        stretching `fill` to) when `m_fixedW`/`m_fixedH` was set, a check
        that predates spanning and never learned about it, even though
        `resolveSpan()` a few lines above already made `m_size` itself
        correct. Fixed by also checking `m_spanWidth`/`m_spanHeight`
        there. Affected `demos/which-key.lua`'s own root `Stack` too
        (same `fill=true` + `spanWidth` combination), not just the new
        demo.
- [x] **7. Stack padding/margin** - confirmed no change: `Stack`'s manual/
      absolute positioning continues to ignore `padding`/`margin`
      entirely, by design.
- [x] **8. Text overflow modes** - wrap explicitly rejected (would make
      text height content-dependent again, undoing Phase 20's fix). Added
      **hard-clip** as a second overflow option alongside the existing
      default truncate-with-ellipsis, selected via `Text{ overflow =
      "ellipsis"|"clip" }` (string enum, matching `align`/`zorder`'s own
      convention rather than a boolean - leaves room for a future
      marquee mode without a breaking change). No-op either way unless
      `maxW` is also set - nothing to overflow against otherwise.
      Ellipsis mode is unchanged (still just forwards `maxW` into
      Hyprland's own Pango-based `renderText()`, which truncates-with-
      ellipsis for free). Clip mode needed a new primitive: Hyprland's
      renderer has no clip/hard-cutoff mode of its own, so
      `CTextNode::rebuildTexture()` rasterizes the FULL un-truncated
      texture (`maxWidth = 0`) instead, and `render()` hard-clips the
      drawn pixels to `maxW` via a new optional `clipBox` parameter on
      `gfx::drawTexture()`, which forwards straight into
      `CTexPassElement::SRenderData::clipBox` - a scissor rect Hyprland's
      own tex-pass element already supports (confirmed against
      Hyprland's actual render pass source, not guessed: a default/empty
      `CBox{}` is a no-op, gated by `.width != 0 && .height != 0`
      throughout `ElementRenderer.cpp`/`OpenGL.cpp`). The widget's
      LAYOUT footprint (`m_size.x`) is clamped to `maxW` in clip mode too
      (CSS `overflow: hidden` box-model - reserved space matches what's
      visible, not the full un-clipped glyph run), same as ellipsis
      mode's texture-width-already-fits-`maxW` behavior gets for free.
      Future, separate, not-yet-scoped feature: a marquee-style
      continuously-scrolling text mode - closer in scope to the
      fade/slide animation system than a simple overflow mode.
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
- [x] **10. Mouse click handling - button-aware `onClick`** - `onClick`
      (already a base `CWidget` field) stays the one callback, now
      receiving an argument: which mouse button triggered it -
      `"left"`/`"right"`/`"middle"` (string enum, matching `overflow`/
      `anchor`'s own convention). Right/middle get the exact same
      press-then-matching-release semantics left click already had.
      Root cause of the old left-only limit: `InputHook.cpp::
      onMouseButton()` hardcoded `if (e.button != BTN_LEFT) return;` at
      its very top, discarding right/middle before any hit-testing ever
      ran. Fixed by a small `toMouseButton()` mapper
      (`BTN_LEFT`/`BTN_RIGHT`/`BTN_MIDDLE` -> `EMouseButton`, `nullopt`
      for anything else - side buttons etc. still pass through
      untouched, same as before). The press/release-matching state
      (`g_pressed`) now also tracks WHICH button was pressed
      (`g_pressedButton`), and a release only fires a click if both the
      widget AND the button match the press - a left-press +
      right-release (or vice versa) is not a click. New `EMouseButton`
      enum lives on `Widget.hpp` (owns `onClick`'s own type). New
      `fieldOnClick()` in `CallbackParsers.hpp/.cpp` (mirrors
      `fieldOnScroll()`'s multi-arg pattern) replaces the old
      `fieldZeroArgFn(L, idx, "onClick")` call in
      `WidgetBuilders.cpp::applyCommonWidgetProperties()`.
      - `Checkbox` was NOT in this task's original scope (it uses its own
        `click()`/`onChange(bool)` path, not `onClick`) - confirmed with
        the user live: toggle stays **left-click only**, matching how
        checkboxes behave in every real UI toolkit; right/middle-click on
        a `Checkbox` now correctly does nothing (previously couldn't even
        reach it, since the whole input hook was left-only). Gated in
        `CUIManager::clickWidget()`'s own `dynamic_cast<CCheckboxWidget*>`
        branch, not in `InputHook.cpp` itself - keeps the button-matching
        logic itself uniform/button-agnostic there, only the FINAL
        dispatch (Checkbox-toggle vs generic `fireClick()`) cares which
        button it was.
      - Click-to-focus/blur for `Input` (`handlePressFocus()`) was left
        unconditional for any of the three recognized buttons, not
        left-only - clicking away from a focused `Input` with any button
        blurs it, matching ordinary "click elsewhere" convention; not
        explicitly asked for, but a natural default rather than a new
        special case.
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
- [ ] **14. Anchor resolution at zero-monitor cold boot** - `luaWindow()`
      currently `luaL_error`s outright if no monitor is available at all
      when resolving an `anchor`ed window (the fallback chain is: named
      selector -> focused monitor -> error if neither resolves anything).
      Found via task 5's own usage guidance: creating an anchored window
      unconditionally at top-level script load (the pattern `hotReload`
      needs - see its own docs/api.md note) can occasionally race a
      genuine Hyprland cold boot where no monitor has attached yet,
      erroring instead of degrading gracefully. Every demo already
      `pcall`s its `window()` calls, so this doesn't crash anything today
      - it just means the window silently never appears until the next
      reload, with only a notification/log line, no automatic retry.
      Candidate fix: don't error immediately - retry once a monitor
      actually connects (`Event::bus()`'s monitor-connected signal,
      already used elsewhere for reserved-area reapplication) instead of
      failing outright. Not yet designed.
- [ ] **15. Extend the rectangle base to `Text`** - `CTextNode` still
      inherits `CWidget` directly, not `CRectNode` - task 4 deliberately
      scoped it to `Button`/`Input`/`Checkbox`/`Image` (widgets that
      already drew their own fill/border) and left `Text` out, since it
      only rasterizes glyphs and had no background concept to unify.
      Found live: `demos/task-checks.lua`'s task 6 demo tried nesting a
      `Text` inside a `Box` expecting a labeled background panel as one
      widget - doesn't work (`Box` never renders children, see task 6's
      own entry above), current workaround is `Stack{Box, Text}` as
      siblings, which stays the standard pattern for now. Would let a
      single `Text{ color=..., borderColor=..., borderWidth=... }` draw
      its own background/border behind its glyphs, same as the other
      four. Not yet designed - in particular whether `renderFill()`'s
      existing gfx::drawRect() call and `CTextNode::render()`'s texture
      draw would need reordering the same way Image's did in task 4 (fill
      -> texture -> border, so the border stays on top).
- [x] **16. Marquee text** - the continuously-scrolling text mode task 8
      flagged as future/separate. `Text{ marquee = true | { pauseMs, speed }
      }`, independent of `overflow` (implies clip-style hard-clipping on
      its own) and a no-op unless the text is actually wider than `maxW`,
      same rule `overflow = "clip"` follows. Motion, confirmed with the
      user over two rounds (their first description read as a discrete
      pause/scroll/pause/snap-back cycle; they then clarified they actually
      wanted a continuous seamless loop instead, with the pause only at the
      point the view returns to the very start of the string - not also at
      the tail): pause for `pauseMs` showing the start of the text, then
      scroll left continuously at `speed` px/s - a second, gap-separated
      copy of the texture trails in from the right so the wrap point is
      never a visible jump-cut - until exactly one full cycle (text width +
      gap) has passed, at which point the view is back at the start and it
      pauses again. Default motion is plain constant-velocity - no
      smoothing.
      - Follow-up #1 (same session): the scroll phase's start/end were an
        instantaneous 0-to-full-speed jump, jarring against the pause
        either side of it - tried a hand-rolled duration-based
        ease-in-out-quad curve.
      - Follow-up #2 (same session, superseding #1): the user pointed out
        Hyprland's own bezier-curve system - the same `PHLANIMVAR`/
        `SAnimationPropertyConfig` machinery `animationIn`/`animationOut`
        already use - was a better fit than a hardcoded quad, so it
        replaced #1 rather than layering on top of it. Reverted the
        DEFAULT back to plain constant-velocity (no smoothing, matching
        the original ship), and added `bezier`/`spring` fields to
        `marquee`'s own table, parsed via the exact same
        `AnimationParsers::resolveCurveField()` helper `animationIn`/
        `animationOut` call - so a curve registered in the user's own
        hyprland.conf works here too, not just a fixed formula. When set:
        a per-node `PHLANIMVAR<float>` (lazily created via
        `Animation::mgr()->createAnimation()`) animates the offset 0 ->
        loopWidth each scroll phase, using `makeAnimationConfig()` (the
        same helper `optAnimationOverrideField()` already builds
        `animationIn`/`animationOut` overrides from) with `speed`
        converted from px/s to Hyprland's own deciseconds-per-loop unit;
        completion is polled via `isBeingAnimated()` each frame (simpler
        than a callback chain for a value that gets manually restarted
        every cycle) and the variable is `setValueAndWarp(0.f)` back to 0
        before the next cycle's assignment - a plain `operator=()` would
        silently no-op there since the new goal (`loopWidth`) already
        equals the previous cycle's goal.
      - Follow-up #3 (same session): demo now defines and uses its OWN
        bezier and spring, not Hyprland's built-in "default" - no HyprLUI
        code needed for this at all, since Hyprland's own Lua API already
        exposes curve registration: `hl.curve(name, { type = "bezier",
        points = {{x0,y0},{x1,y1}} })` / `hl.curve(name, { type =
        "spring", stiffness, damping (or the older `dampening`), mass })`
        (confirmed against Hyprland's actual source, not guessed -
        `LuaBindingsConfigRules.cpp`'s `hlCurve()`/`hl.curve` registration
        - the same `Animation::mgr()` registry hyprland.conf's own
        `bezier =` config line and HyprLUI's `resolveCurveField()`
        validation both already read from). Hit live: the installed
        Hyprland runtime here still expects the OLDER field name -
        `dampening`, not `damping` - errored "dampening expects a
        number" at runtime until switched; the checked-out Hyprland
        source read for this task supports both (`damping` primary,
        `dampening` as a documented back-compat fallback), so the
        installed build predates that rename. `demos/task-checks.lua`'s
        task 16 section now calls `hl.curve()` twice at module load
        (before `toggleTask16()` ever builds a `Text` referencing them by
        name) and the demo shows four variants side by side: static clip,
        linear marquee, custom-bezier marquee, custom-spring marquee.
      - Follow-up #4 (same session): reported live - both bezier and
        spring marquees sat frozen at the pause position, never scrolling.
        Root cause not conclusively pinned down (needs the actual
        compositor's compiled internals, not just headers, to confirm),
        but the `PHLANIMVAR<float>` from #2 never appeared to advance past
        its initial value despite `*anim = goal` firing (same construction
        pattern `CWidget`'s own working `m_visibilityAnim` uses) - possibly
        Hyprland's `AnimationManager` not ticking a freestanding,
        context-less per-node variable reliably in this build, though
        that's inference, not a confirmed cause. Rather than keep
        debugging an opaque dependency, replaced it with fully
        self-contained per-frame evaluation - still genuinely reading the
        SAME registered curve data `hl.curve()` wrote via
        `Animation::mgr()`, just applied by `CTextNode` itself instead of
        relying on the manager's own tick to update a value we then
        sample:
        - Bezier: a pure function of normalized time - `t = elapsed /
          duration` (same duration math as #2), progress =
          `Animation::mgr()->getBezier(name)->getYForPoint(t)`
          (`CBezierCurve`'s own evaluator, confirmed present in
          hyprutils - no guessing at its curve math, just calling it).
        - Spring: NOT a pure function of progress (a spring has no fixed
          duration - it settles asymptotically), so integrated instead:
          semi-implicit Euler stepping of a standard damped-harmonic-
          oscillator (`accel = -(stiffness*(pos-target) + damping*vel) /
          mass`) each frame using dtMs, reading the REAL registered
          `SSpringCurve`'s `stiffness`/`damping`/`mass` via
          `Animation::mgr()->getSpring(name)`, "done" once within its own
          `valueEpsilon`/`velocityEpsilon` of the target (the same
          completion fields Hyprland's own spring config already has, read
          rather than reimplemented).
        - The `PHLANIMVAR`/`m_marqueeAnim` field, `makeAnimationConfig()`
          call, and `isBeingAnimated()` polling from #2 were all removed -
          no longer needed now that nothing depends on the manager's tick.
          `resolveCurveField()`'s own `"spring:" + name` encoding (used to
          disambiguate a bezier name from a spring name in one string
          field, matching `internalBezier`'s own convention) is now parsed
          back out locally instead of being handed to
          `SAnimationPropertyConfig`.
      - Needed a new low-level primitive: Hyprland's own text renderer has
        no clip mode at all (confirmed against its actual render-pass
        source, not guessed - see task 8's own entry), so
        `gfx::drawTexture()` gained an optional `clipBox` parameter,
        forwarding straight into `CTexPassElement::SRenderData::clipBox` (a
        scissor rect the pass element already supports, gated by
        `.width != 0 && .height != 0` - an omitted/default `CBox{}` is
        already a no-op, so this is purely additive for every existing
        caller). Task 8's own hard-clip mode was reimplemented on top of
        the same parameter instead of anything bespoke.
      - `CWidget::isAnimating()` had to become `virtual` - it was a plain
        non-virtual method (only checking the visibility-fade animation +
        recursing children) with no extension point for a leaf that needs
        its own independent, indefinitely-looping animation signal, unlike
        Hyprland's own bezier-curve `SAnimationPropertyConfig` machinery
        (one-shot 0->1, wrong shape for an infinite loop). `CTextNode`
        overrides it to also report true while actively mid-scroll (not
        during the start-of-loop pause), which is what keeps
        `CCanvas::render()` damaging every frame for as long as glyphs are
        actually moving - same mechanism the fade animations already
        relied on, just fed from a second source now.
      - The pause/scroll state machine advances by real elapsed wall-clock
        time (`std::chrono::steady_clock`) inside `CTextNode::render()`
        itself, not frame count - render() runs every frame this widget is
        visible regardless of `isAnimating()`'s own one-frame-stale damage
        signal (same already-accepted staleness pattern
        `CCanvas::render()`'s debug-overlay-overflow tracking uses), so the
        timer stays accurate even through the (harmless, self-correcting)
        first transition frame where damage() lags by one tick.
      - Bug caught by clangd static analysis before ever reaching a real
        build: `Vector2D{someDouble, 0}` (int literal `0` alongside a
        `double`) is genuinely ambiguous between `Vector2D`'s `(double,
        double)` and `(int, int)` constructor overloads - fixed by writing
        `0.0`. Notable since most clangd diagnostics this session have been
        the unrelated, unreliable `libudev.h`-cascade false-positive
        pattern - this one was real and worth catching before a full
        rebuild.

- [ ] **17. Overlay renders over the cursor** - found live running on real
      hardware (nvidia, not the nested-Hyprland dev setup this project was
      previously tested under, where it never showed up): `RenderHook.cpp`
      hooked `RENDER_LAST_MOMENT`, which fires AFTER Hyprland's own cursor
      render (`Renderer.cpp` - confirmed by reading the actual render-stage
      emission order, not guessed), so every HyprLUI window drew on top of
      the cursor whenever Hyprland falls back to software cursor rendering
      (`CMonitor::shouldUseSoftwareCursors()` - true by default for nvidia
      + multi-GPU/VRR, which is exactly this host). Switched the hook to
      `RENDER_POST_WINDOWS` instead - the latest stage that still fires
      before the cursor - fixing it, at the cost of now also firing before
      the top/overlay `wlr-layer-shell` surfaces (waybar/eww etc., traced
      via `renderAllClientsForWorkspace()`'s own render order), which
      RENDER_LAST_MOMENT didn't have to worry about. No stage exists
      upstream between "after top/overlay layer-shell surfaces" and
      "before cursor" - a real fix needs a new Hyprland render stage there
      (e.g. `RENDER_POST_CURSOR`, or splitting cursor render into its own
      explicit stage boundary) - **needs a Hyprland PR**, tracked here
      until that lands and HyprLUI can switch to it.

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
