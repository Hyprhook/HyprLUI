# HyprLUI Design

Living design doc for the widget-based rewrite. Tracks decisions made in
discussion, verified facts about what Hyprland's plugin API actually
supports, and the rough phase order we agreed on. Update this as phases
land or decisions change - it's meant to survive across sessions, not be
a one-time plan.

## Goal

A small UI toolkit, embedded in a Hyprland plugin, exposed to `hyprland.lua`
config so users can define windows/popups/HUDs declaratively - vocabulary
and ergonomics aimed at people already familiar with QuickShell (QML) and
eww, without building a full external renderer (no GTK/Qt, no real
wlr-layer-shell client surfaces). Everything renders via Hyprland's own
render pass and reads input via Hyprland's own event bus - HyprLUI has no
independent Wayland surface.

## Current state (v0 - done, working)

- `Canvas` (soon: `Window`) = a flat list of absolutely-positioned `Node`s.
- `RectNode`, `TextNode` (cached texture, rebuilt on dirty).
- `UIManager` singleton owns all canvases.
- Render backend (`src/render/gfx.cpp`) queues `CRectPassElement`/
  `CTexPassElement` into `g_pHyprRenderer->m_renderPass` during
  `RENDER_PRE_WINDOWS` (background layer) / `RENDER_LAST_MOMENT` (overlay
  layer) - direct GL calls do **not** reach the presented frame in this
  pipeline, pass elements are required.
- `LuaBridge` exposes an imperative, table-argument API under
  `hl.plugin.hyprlui.*`: `create_canvas`, `remove_canvas`,
  `set_canvas_visible`, `add_rect`, `add_text`, `set_text`, `remove_node`.
- Damage tracking wired through `Canvas::damage()`, called on every
  mutation so Hyprland actually repaints affected regions. **Bug found and
  fixed post-Phase-2**: a single `damage()` call intermittently flickered/
  ghosted old+new content (reported after Phase 2 - mutating text, toggling
  visibility, and creating/removing windows would sometimes show a stale
  "ghost" for a couple of frames before self-correcting, and pressing the
  same keybind again fixed it instantly). Root cause: Hyprland renders into
  a rotating set of swapchain buffers, and a buffer that's currently up to
  `DAMAGE_RING_PREVIOUS_LEN=3` frames stale (`Monitor::CDamageRing`,
  Hyprland's `src/output/DamageRing.hpp`) won't show a change until damage
  has been present for enough *consecutive real frames* to cover it - a
  single damage() call, or even several calls made synchronously back-to-
  back in the same instant (tried, didn't help - `CDamageRing::damage()`
  just accumulates into one pending region regardless of call count before
  the next frame's transaction), doesn't guarantee that. Confirmed against
  Hyprland's own precedent: `NotificationOverlay` (Hyprland's first-party
  dynamic overlay) sidesteps this by damaging its box unconditionally on
  *every draw* while visible, not just when content changes. Fix (`Canvas.
  hpp/.cpp`): `damage()` now also arms a `REDAMAGE_FRAMES = 4` countdown
  that `render()` ticks down each real frame, re-damaging the box each
  time - bounded instead of "every frame forever" since our content is
  mostly static between mutations, unlike a notification's animation.
  `recomputeAnchorPosition()` damages both the old and new box when an
  anchored window's position actually moves, for the same reason. Window
  *removal* needed a matching fix in `UIManager.cpp`: the canvas is dropped
  from `m_canvases` (and thus Lua's view of the world) immediately, but a
  moved-out copy is kept alive in `m_pendingRemoval` - invisible, drawing
  nothing - purely so its `render()` calls can keep ticking the countdown
  for a few more real frames before it's finally freed; otherwise there'd
  be nothing left to drive the re-damage after the object's gone.

  **Second bug found and fixed, same underlying class of issue, surfaced by
  a Phase 3 `Bind()`ed counter crossing into two digits**: a size-to-content
  window's outer `m_size` was computed once at creation and never updated
  again, no matter how the tree's actual content grew or shrank afterwards.
  Unlike the flicker bug above, this one didn't self-heal - `damage()` had
  no memory that anything had gotten bigger, so the region the extra
  character needed was simply never painted, permanently (and for an edge-
  relative anchor like `bottom-right`, the position itself was computed
  from the stale size too, shifting the whole window wrong). Fix
  (`Canvas.hpp/.cpp`): `CCanvas::setFixedSize()` mirrors `CWidget::
  setFixedSize()` one level up (pins an axis instead of tracking content);
  `render()` now re-syncs `m_size` from the root widget's freshly-measured
  size every frame (size-to-content axes only), damaging both the old and
  new box when it actually changes, *before* `recomputeAnchorPosition()`
  runs so edge/corner anchors see the current size rather than last
  frame's. `render()`'s ordering also had to become bindings → measure()
  → size-sync → anchor-reposition → redamage-tick → arrange+render (bindings
  must run before measure(), since a bound `setText()` only marks content
  dirty - measure() is what actually rebuilds/re-measures it).
  `LuaBridge.cpp`'s `luaWindow()` now calls `canvas->setFixedSize(fw, fh)`
  in both the anchored and non-anchored paths, reusing the exact same
  optional-axis fields it already computed the *initial* size from.

**General plugin-lifecycle bug found and mitigated, not fully solved yet
(post-Phase-5, found via config-reload testing)**: config reload re-runs
the Lua script from scratch (its `local` variables, including any "is
this window open" toggle bookkeeping, reset to their initial values) -
but the plugin itself is NOT unloaded/reloaded when the config reloads,
only the script is re-run. So HyprLUI's own C++-side state (`CUIManager`'s
canvases, `CWatcherManager`'s watchers, `CReservedAreaComposer`'s
contributions) silently outlived the Lua-side bookkeeping meant to own
it. Confirmed live: toggling a window open, then reloading (e.g. fixing
an unrelated Lua config eval error - which forces exactly this), left
the window open in C++ while the Lua toggle variable that tracked it
reset to "closed" on re-run, permanently orphaning it - unreachable
through that keybind ever again, since the Lua side had no memory it
existed.

Mitigated in `main.cpp`: a `resetAllState()` helper (calls all three
managers' `clear()`, same teardown `PLUGIN_EXIT` already did) now also
runs from a new listener on `Event::bus()->m_events.config.preReload` -
confirmed via Hyprland's own source (`ConfigManager.cpp:647-648`) that
this fires as the very first thing inside `CConfigManager::reload()`,
strictly before any re-parsing begins, so clearing there can never wipe
out anything the fresh script is about to create. This turns the bug
from "silent unreachable orphan" into "every reload closes everything,
full stop" - correct and no longer a leak, but a blunt instrument, not
the final design. **Deliberately not marked resolved** - see Open
questions for what a better version of this would need to account for
(distinguishing declarative/always-recreated UI from state a user
actually wants to survive an unrelated reload, some way to communicate
back to Lua what got torn down, etc.). `hyprlandd.lua`'s toggle-style
test binds correctly reopen on their next press after a reload under the
current mitigation (no code changes needed there - the Lua toggle
variables already reset to `false`/closed on re-run, which now at least
matches reality instead of silently diverging from it).

**Validation gap found and fixed (post-Phase-5)**: `window{}` let
`anchor` and `exclusive` be set to genuinely incompatible edges - e.g.
`anchor = "top"` with `exclusive = "left"` was accepted silently, even
though the window sits at the top and reserving space on the left has no
visual relationship to where it actually is. `LuaBridge.cpp` gained
`anchorAllowsExclusiveEdge(EAnchor, EEdge)`: an edge-anchor (`"top"` etc.)
only accepts its own single edge; a corner anchor (`"top-left"` etc.)
accepts either of its two edges (a corner-docked window could sensibly be
a horizontal or a vertical bar); `"center"` accepts neither. Errors
(`luaL_error`) otherwise, matching the "fail loud on developer mistakes"
convention used everywhere else in this file. Had to be validated
*before* `mgr.createCanvas()` runs, not after - an earlier draft of this
fix checked it too late and would have left an orphaned, empty, never-
damaged canvas registered under `name` on the invalid-combination path
(caught before ever building; `luaL_error`'s longjmp doesn't unwind C++
state, so a canvas created just before erroring would never get cleaned
up - same class of resource-lifetime care as everywhere else `luaL_error`
appears in this file, see the earlier `luaL_error` mechanics discussion).

This proves the render pipeline end-to-end. Everything below is the next
layer on top of it - the low-level pieces (gfx backend, damage, pass
elements) stay as-is.

## Target architecture

### 1. Widget tree (replaces the flat node list)

- `Widget` gains children; `Canvas`/`Window` holds one root `Widget`
  instead of a flat `vector<PNode>`.
- Containers: `Stack` (manual/absolute - today's behavior, kept as an
  escape hatch), `Row`, `Column` (flexbox-lite: direction, gap, padding,
  align, size-to-content vs. fixed size).
- Leaves: `Text`, `Rect`/`Box`, later `Button`, `Input`.
- A layout pass (measure/arrange) runs before render, using the same
  dirty-flag pattern `TextNode` already uses for its texture cache.
- Extension model stays what `Widget.hpp` already documents: subclass,
  implement render (+ now measureContent/arrangeChildren for containers).

### 2. Window (renamed from Canvas)

- **Anchor + margin** instead of raw global `x`/`y`:
  `anchor = "top-right", margin = {10, 10}`.
- **Explicit monitor targeting** (by output name, or "focused") instead of
  relying on whichever monitor's layout box happens to contain a global
  coordinate.
- **Size-to-content** by default; fixed `w`/`h` as an override.
- `exclusive` flag + edge + zone size, feeding the reserved-area composer
  (below).
- Visibility toggle now; fade in/out later, ideally hooking Hyprland's own
  animation manager rather than reimplementing easing.

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
  only those re-evaluate/re-layout, not the whole tree.
- Future option, **not** v1: metatable-based auto dependency tracking
  (`__index`/`__newindex` proxies, Vue/MobX-style) so plain field reads
  register dependencies automatically. Deliberately deferred - the
  `Bind(name)` widget-facing surface doesn't have to change if this gets
  added later, only how invalidation is triggered underneath it.

### 4. Reserved area / exclusive zones

**Verified against Hyprland source + Hyprspace (a real, working plugin
that already does this):**

- `PHLMONITOR->m_reservedArea` (`Monitor.hpp:87`) is a plain public
  member of type `Desktop::CReservedArea`, and it's what the tiling
  algorithms actually consult (`MasterAlgorithm.cpp`'s
  `reservedLeft`/`reservedRight`, `LayoutManager.cpp` edge-snapping,
  `Monitor::logicalBox()`). Setting it genuinely pushes tiled windows
  aside - not cosmetic.
- Hyprspace does this directly: `pMonitor->m_reservedArea =
  Desktop::CReservedArea(top, right, bottom, left)`
  (`Hyprspace/src/Layout.cpp:27`).
- **Wrinkle**: Hyprland's own subsystems compose multiple contributors via
  `m_reservedArea.addType(eReservedDynamicType, ...)` /
  `resetType(...)`, but `eReservedDynamicType` is a closed enum baked into
  Hyprland's own header (`RESERVED_DYNAMIC_TYPE_LS`,
  `RESERVED_DYNAMIC_TYPE_ERROR_BAR`, `_END`) - a plugin cannot get its own
  slot in that system. Hyprspace gets away with a flat assignment because
  it only ever owns one exclusive panel.
- **Consequence for HyprLUI**: if more than one exclusive window can be
  open at once (e.g. a top bar and a bottom dock), *we* must do the
  composition ourselves - track the reserved margin contributed by each
  HyprLUI window with `exclusive = true`, per edge, per monitor; sum them;
  call `m_reservedArea.setStatic(combined)` on change. Must also preserve
  whatever the user's own `monitor=...,reserved:...` config rule set
  (same field, `setStatic` overwrites, not additive) - read and keep that
  baseline separately, add HyprLUI's contribution on top rather than
  clobbering it.
- Needs a small new component, tentatively `ReservedAreaComposer`, likely
  owned by `UIManager` (or a peer singleton), keyed by monitor.

### 5. Input handling

**Verified against Hyprland source + Hyprspace (mouse input is already
live in a real plugin today):**

- `EventBus.hpp` has an `input` block, structurally identical to the
  `render` block HyprLUI's `RenderHook` already listens to:
  ```cpp
  struct {
      Cancellable<Vector2D>               move;
      Cancellable<IPointer::SButtonEvent> button;
      Cancellable<IPointer::SAxisEvent>   axis;
  } mouse;
  struct {
      Cancellable<IKeyboard::SKeyEvent> key;
      ...
  } keyboard;
  ```
- `Cancellable<T>` hands the listener a `SCallbackInfo&`; setting
  `info.cancelled = true` swallows the event so it never reaches whatever
  window would've normally received it.
- Hyprspace already does exactly this, live:
  `g_pMouseButtonHook = listenCancellable<IPointer::SButtonEvent>(
  Event::bus()->m_events.input.mouse.button, onMouseButton)`.
- **No real Wayland surface needed for this** - corrects an earlier
  assumption in this design process that buttons/input fields would
  require standing up a real layer-shell-equivalent surface. They don't;
  raw input interception is enough.
- Hyprland gives coordinates only (`g_pInputManager->
  getMouseCoordsInternal()`) - no widget-aware hit-testing. That part is
  entirely ours: on `mouse.button`/`mouse.move`, walk the visible windows
  (topmost first) and hit-test the coordinate (transformed the same way
  `gfx.cpp::toMonitorLocal` already does, inverted) against the widget
  tree's boxes.
- **Caveat**: `keyboard.key` is raw keysym/scancode level, not the
  IME/text-input-v3 composition protocol - fine for a simple entry field,
  will not get proper CJK/dead-key composition. Known v1 limitation, not
  a blocker.
- New module, tentatively `InputHook` (mirrors `RenderHook`'s shape):
  registers the mouse/keyboard listeners, hit-tests, dispatches synthetic
  click/hover/focus/key events into the widget tree, invokes Lua callback
  refs on `Button`/`Input` widgets.
- Text input additionally needs a "which window/widget currently owns
  keyboard focus" concept (set when an `Input` is clicked, cleared on
  click-away or window close).

### 6. Lua API shape (sketch, not final)

- Declarative construction stays the primary path:
  `hl.plugin.hyprlui.window{ anchor = ..., <tree> }`.
- Keyed widgets (already-established pattern: every widget gets a Lua
  `id`) stay mutable after construction via calls like the existing
  `set_text(window, id, text)`, generalized to other properties as
  needed - no full-tree diffing/reconciliation engine planned, changes
  that aren't covered by a targeted setter just mean removing and
  re-adding that subtree.
- `Bind(name)` / `hyprlui.watch(name, fn, opts)` / `hyprlui.notify(name)`
  for reactivity (see above).
- `Button{ onClick = fn }`, later `Input{ onChange = fn }` once the input
  hook lands.

> **Reminder:** `stubs/hyprlui.meta.lua` (LuaLS annotations for
> `hl.plugin.hyprlui.*`) is hand-maintained, not generated - Hyprland's own
> stub generator (`meta/generateLuaStubs.py` in the Hyprland repo) only
> understands its own internal `hl.*` binding pattern and types
> `hl.plugin.<name>` as `any`. **Whenever this section's API shape changes
> (new widget type, new field, new mutation call - Phases 3/4/6/7/8 all
> touch this), update `stubs/hyprlui.meta.lua` in the same change.** It's
> installed to `share/hypr/stubs/hyprlui.meta.lua` by the flake's
> `postInstall`, alongside Hyprland's own stubs.

## Non-goals (for now)

- No real `wlr-layer-shell` client surface - render-hook + pass-elements
  only.
- No IME/text-input-v3 composition - raw key events only.
- No automatic dependency-tracking reactivity in v1 - explicit
  watch/notify only (auto-tracking is a possible later addition that
  doesn't change the widget-facing API).
- No general-purpose diffing/reconciliation engine for tree updates -
  targeted setters + subtree replace is enough for config-driven UIs that
  change infrequently.

## Phase order

Rationale: the tree underlies everything else, so it goes first. Anchors
are cheap and immediately useful, so they ride along early. Reactivity is
placed before input because a button's `onClick` will usually just be
"mutate state, then `hyprlui.notify(...)`" - the two need to agree on the
same dirty/re-render plumbing. Buttons come before exclusive zones because
they validate hit-testing without touching monitor layout at all. Input
fields come last because they're gated on the same
focus/input-ownership questions as exclusive zones, and are the riskiest
piece (raw-keysym limitation).

- [x] **Phase 0** - Proof of concept: flat canvas, rect/text nodes,
      imperative Lua bridge. Render pipeline confirmed working
      end-to-end.
- [x] **Phase 1** - Widget tree + layout engine (`Stack`/`Row`/`Column`,
      padding/gap/align, size-to-content vs. fixed) - done as a *full*
      rewrite including the declarative Lua API sketched in section 6
      (`hyprlui.window{ hyprlui.Column{ ... } }`), not just the C++ engine.
      `CWidget` (renamed from `CNode`) gained `m_children`; layout is a
      two-pass `measure()`/`arrange()` walk run once per frame (no
      layout-dirty flag - HUD-sized trees, full relayout is cheap; text
      texture caching is unaffected, still keyed on its own dirty flag).
      `CCanvas` now holds one root `PWidget` instead of a flat node list.
      `CUIManager::addText`/`addRect` were removed - tree construction now
      lives entirely in `LuaBridge.cpp`'s recursive `buildWidget()` (or in
      C++ directly, see `main.cpp`'s demo). Old imperative `add_rect`/
      `add_text`/absolute-x,y-per-canvas API is gone, replaced outright
      (pre-release, no back-compat kept - see LuaBridge.hpp for the new
      shape). `align` supports start/center/end only; no justify/wrap.
- [x] **Phase 2** - Window anchors + explicit/focused monitor targeting.
      `window{}` kept raw global `x`/`y` as an escape hatch (no `anchor`
      given - unchanged Phase 1 behavior) rather than replacing it; when
      `anchor` *is* given, x/y are reinterpreted as an offset from that
      anchor point instead of a global position (positive always pushes
      inward, regardless of which edge) - the same "relative to parent"
      convention a widget's x/y already has relative to its parent widget,
      just one level up. `monitor` is optional and reuses Hyprland's own
      window/layer-rule `mon:` selector syntax (`State::CMonitorQuery::
      configString()`, `src/state/MonitorQueryCore.cpp:196`
      `fromConfigString()`) via `.relativeTo(Desktop::focusState()->
      monitor())`, so `"+1"`/direction chars/numeric ids resolve relative
      to focus - not a bespoke selector language. **Deliberately no
      `"current"`/`"focused"` keyword**: `fromConfigString()` treats the
      literal string `"current"` as a magic alias for whatever
      `.relativeTo()` was given, which would shadow an actual monitor a
      user has genuinely named "current" in their own `monitor{}` rules -
      caught during implementation and corrected. So: omitting `monitor`
      entirely means "the focused monitor", and if a given selector
      matches nothing (typo, unplugged output), it falls back to the
      focused monitor too rather than erroring - graceful degradation over
      a hard failure for something this recoverable. The monitor is
      resolved **once**, at creation (`luaWindow()` in `LuaBridge.cpp`),
      and only its *name* is cached on `CCanvas` (`m_anchorMonitor`) -
      never a `PHLMONITOR` handle, to sidestep any lifetime questions
      across frames. Anchoring is against `logicalBoxMinusReserved()`
      (`Monitor.cpp:1810`), not the raw monitor box, so anchored windows
      already avoid existing bars/panels for free, ahead of Phase 5.
      `CCanvas::recomputeAnchorPosition()` re-resolves that cached name and
      recomputes position every `render()` call (same "just redo it every
      frame, no dirty flag" philosophy as Phase 1's layout) - so
      resolution/reserved-area changes on the chosen monitor self-correct
      live, but *which* monitor was picked never changes after creation
      (no window-teleports-on-focus-change surprise - this was an explicit
      user call, see Open questions below for the alternative). If the
      target monitor briefly can't be resolved (unplugged), the window
      just keeps its last known position - no monitor-hotplug event
      listener yet, known v1 gap.
- [x] **Phase 3** - Named watchers + `Bind()` reactivity (poll + explicit
      `notify()`). New `src/reactive/Watcher.hpp/.cpp` (`CWatcherManager`
      singleton): `hyprlui.watch(name, fn, {interval})` registers a Lua
      function, calls it once immediately to seed a cached value, and -
      if `interval` is given - arms a repeating poll. `hyprlui.notify(name)`
      and the poll timer both funnel through the same `notify()`, which
      re-calls `fn`, and only if the (stringified) result actually changed,
      calls `CUIManager::damageAll()`. **Polling required reaching into
      Hyprland's internal event-loop timer** (`CEventLoopTimer` +
      `g_pEventLoopManager`, `src/managers/eventLoop/`) - confirmed via
      research that `HyprlandAPI::` (the stable plugin surface) exposes no
      timer of its own, and that Hyprland's own `hl.timer()` Lua binding
      (`src/config/lua/bindings/LuaBindingsToplevel.cpp`, `hlTimer()`) uses
      this exact same internal primitive, including the
      `self->updateTimeout(...)` re-arm-before-running pattern this
      mirrors. Deliberately *not* tied to `render.stage`: that only fires
      when Hyprland is actually rendering a frame for some other reason, so
      an idle desktop could go a long time between ticks - wrong for "a
      clock/volume readout should update on schedule regardless."
      Internal API, not guaranteed stable across Hyprland releases - kept
      isolated to `Watcher.cpp` for exactly that reason, same reasoning as
      `gfx.cpp` for the rendering internals. `CWatcherManager::clear()` is
      called from `PLUGIN_EXIT` to cancel every timer and release every Lua
      function reference before unload.

      **Deliberately simpler than the original brainstormed sketch**: no
      fine-grained "mark dirty, only re-evaluate what referenced it"
      system. Since Phase 1 already does a full measure/arrange/render
      pass every frame regardless, a `Bind()`ed field just needs *some*
      code to re-read the watcher's current value each frame and call the
      widget's own (already dirty-checked, e.g. `CTextNode::setText()`)
      setter - correct and cheap at HUD scale without inventing an
      invalidation system. Concretely: `LuaBridge.cpp`'s `buildWidget()`
      collects one closure per `Bind()`-tagged field into a
      `std::vector<std::function<void()>>` threaded through the recursion
      (same pattern as the existing `autoId` counter), and `luaWindow()`
      attaches them to the finished `CCanvas` (`CCanvas::addBinding()`,
      applied at the top of every `render()`, even while invisible so
      nothing goes stale the instant visibility toggles back on).
      `CWatcherManager` itself has zero knowledge of which widgets/canvases
      reference a given watcher - a value change just triggers the blunt
      `damageAll()` above, and each canvas independently re-reads whatever
      it's bound to. Simplest correct thing, matches this project's
      established "redo it every frame rather than build fine-grained
      invalidation" precedent throughout.

      **v1 scope**: `Bind()` only wired up for `Text.text` (the one case in
      the original sketch, and the dominant real use case - clock, volume,
      workspace name). Other fields (color, visibility, numeric size) can
      reuse the exact same mechanism later without changing `Bind()`'s
      syntax - see Open questions. A watcher must be registered
      (`hyprlui.watch()`) *before* a `window{}` that `Bind()`s to it is
      built, or `buildWidget()` `luaL_error`s - no forward-reference
      support, matches this project's "fail loud on developer mistakes"
      convention used everywhere else in `LuaBridge.cpp`.
- [x] **Phase 4** - `Button` widget + pointer input hook (`InputHook`) +
      tree hit-testing. New `src/ui/ButtonWidget.hpp/.cpp` (`CButtonWidget`,
      renders like `CRectNode` then its children on top like
      `CStackWidget` - deliberately Lua-agnostic like every other widget,
      just holds a plain `std::function<void()> m_onClick` and exposes
      `click()`) and `src/input/InputHook.hpp/.cpp` (mirrors `RenderHook`'s
      shape - the only file that talks to
      `Event::bus()->m_events.input.mouse.button`).

      **Verified via research before implementing** (no stable
      `listenCancellable` API - that turned out to be a private
      Hyprspace-local hack around a protected method, not real Hyprland
      API; used the plain `.listen()` pattern Hyprland's own core code
      uses instead, `EventBus.hpp:107` `Cancellable<IPointer::SButtonEvent>`
      = `CSignalT<IPointer::SButtonEvent, Event::SCallbackInfo&>`).
      Pointer coordinates (`g_pInputManager->getMouseCoordsInternal()`,
      `InputManager.cpp:1112`) are already global compositor-space, the
      same space every widget/canvas position already lives in - no
      per-monitor transform needed for hit-testing, unlike rendering's
      `toMonitorLocal()`. `IPointer::SButtonEvent` has no unified "click" -
      separate press (`WL_POINTER_BUTTON_STATE_PRESSED`) and release
      events the plugin pairs up itself.

      **Click semantics**: left-click only (`BTN_LEFT`, `linux/input-
      event-codes.h`) in v1. Press hit-tests via
      `CUIManager::hitTestButton()`; if it hits a button, cancels the
      event (`info.cancelled = true`) and remembers the hit as an
      `SButtonHit` (canvas name + widget id, **not** a raw pointer - the
      widget could in principle be removed by Lua code between press and
      release). Release re-hit-tests and only fires `click()` (via
      `CUIManager::clickButton()`) if the two `SButtonHit`s match exactly -
      moving off the button between press and release cancels the click,
      same convention as every GUI toolkit. If press doesn't hit anything,
      the event (and its later matching release) passes through completely
      untouched - normal window focus/clicks elsewhere are unaffected.

      **Hit-testing lives on `CWidget` itself** (`hitTest()`, new virtual
      method, default: not interactive, just recurse into children
      newest/topmost-painted-first) rather than as an external free
      function, since children are protected members with no public
      iterator - `CButtonWidget` is the only override, and it's a hit-
      testing *leaf* (checks its own bounds, does not recurse into its own
      children) since nesting buttons inside buttons isn't a supported/
      needed case. This keeps `Widget.hpp` from needing to know
      `CButtonWidget` exists at all (pure virtual dispatch), and
      `CUIManager::hitTestButton()` doesn't need to know about
      `CButtonWidget` either - `CWidget::hitTest()` can only ever return a
      match that's already a button by construction, so the generic
      `CWidget::id()` is all `hitTestButton()` needs.

      **Only `Overlay`-zorder, visible canvases are click targets** -
      `Background` canvases are explicitly documented as decorative/
      occludable-by-real-windows (`EZOrder`'s own doc comment); making them
      hit-testable would need full compositor z-order awareness (is a real
      window currently covering this pixel?), well beyond v1. Among
      multiple overlapping Overlay canvases, newest-created wins - each
      `CCanvas` gets a monotonic `m_sequence` stamped by
      `CUIManager::createCanvas()`.

      **A Button's own bounds are the only clickable area** - clicking
      elsewhere within the same window (its background, a label, empty
      space) passes through untouched. This was a deliberate v1 choice,
      not the only reasonable one: an alternative is "clicking anywhere
      within an Overlay window's bounds is swallowed, matching how real
      windows behave" - rejected because it would've made every *existing*
      Phase 1-3 test window (none of which have buttons) start blocking
      clicks in whatever corner of the screen they occupy, which is
      surprising for what's meant to be a HUD/popup toolkit, not a click-
      blocking exclusive-input layer (that's closer to Phase 5's
      `exclusive` flag territory, and still not quite the same thing).

      `onClick`'s Lua function reference lifetime: `LuaBridge.cpp` wraps it
      in a small `SLuaFnRef` RAII guard (mirrors `CWatcherManager`'s ref-
      counted-manually approach) held via `shared_ptr` inside the returned
      `std::function` (a bare move-only guard wouldn't satisfy
      `std::function`'s copyable-target requirement) - releases the
      registry slot whenever the button widget itself is destroyed.

      **Bug found and fixed live, first click**: `fieldOnClick()` originally
      built this via `make_shared<SLuaFnRef>(SLuaFnRef{L, ref})` - that
      constructs a *temporary* `SLuaFnRef` first, copies it into the
      shared object, and then destroys the temporary at the end of the
      expression, which immediately `luaL_unref()`s the slot - before the
      button is ever clicked. Lua then reused that now-free registry slot
      for something else by the time a real click fired
      (`lua_rawgeti` pushed whatever now occupied it, not the callback -
      surfaced as `attempt to call a number value`). Fixed by constructing
      in place instead - `make_shared<SLuaFnRef>(L, ref)`, no intermediate
      temporary, no premature destructor call (relies on C++20's
      aggregate-initialization-via-parentheses, P0960, since `SLuaFnRef`
      has no user-declared constructor).

      `onClick` errors are caught and logged (`Log::logger`), not
      propagated - same reasoning as `CWatcherManager::callWatcherFn()`:
      this fires from the input hook, which has no caller-side `pcall`.
- [x] **Phase 5** - Exclusive zones / `ReservedAreaComposer`. New
      `src/reserved/ReservedAreaComposer.hpp/.cpp` (`CReservedAreaComposer`
      singleton, mirrors `CWatcherManager`'s shape). `window{ exclusive =
      "top"|"right"|"bottom"|"left" }` (requires `anchor`) reserves screen-
      edge space equal to the window's own current size along the
      perpendicular axis, per the user's explicit ask for all four edges
      (not just top/bottom).

      **Re-verified the whole API before implementing** (research pass,
      same practice as every internal-API-reliant phase) since the earlier
      brainstorm-stage research on this predates every other phase and
      Hyprland's internals had already shifted once mid-project.
      Confirmed still accurate: `Desktop::CReservedArea` (`src/desktop/
      reserved/ReservedArea.hpp`) has two tiers - a *static* one
      (`setStatic()`, flat overwrite) and a *dynamic* one indexed by a
      still-closed `eReservedDynamicType` enum (only
      `RESERVED_DYNAMIC_TYPE_LS`/`RESERVED_DYNAMIC_TYPE_ERROR_BAR` exist).
      Newly confirmed (wasn't visible in the original research): both
      built-in dynamic slots get reset-and-recomputed by Hyprland's own
      core code on every relevant layout pass (real layer-shell surfaces /
      the crash error bar respectively) - so a plugin piggybacking on
      either, as Hyprspace's own `Layout.cpp:25` does via a flat
      `pMonitor->m_reservedArea = CReservedArea(...)` overwrite, would
      have its contribution silently wiped out. **Not usable, and not a
      pattern worth following** despite being the only real-world plugin
      precedent found.

      Resolved the previously-open question of how to compose without
      clobbering the user's config: `CMonitor::applyMonitorRuleSoft()`
      (`Monitor.cpp:673-675`) applies the user's `monitor{ reserved: ...
      }` rule via `m_reservedArea.setStatic(m_activeMonitorRule.
      m_reservedArea)` - i.e. the user's *true* baseline is readable at
      any time from `pMonitor->m_activeMonitorRule.m_reservedArea`,
      independent of whatever's currently live in `m_reservedArea`
      (which may already include our own prior write). So: every
      recompute reads that baseline fresh, sums it with our own current
      per-edge contributions (multiple exclusive windows on the same edge
      **add**, not max), and `setStatic()`s the combined total - never
      reading from the live object, which would double-count our own
      last write. Diffed against a small locally-tracked "last applied
      per monitor" cache before actually calling `setStatic()`, to avoid
      triggering a redundant relayout when an unrelated contribution
      changes elsewhere.

      **Unavoidable consequence, not a bug**: `applyMonitorRuleSoft()`
      re-runs that same `setStatic()` on every config reload/monitor
      reconfiguration, discarding whatever we'd composed in - there is no
      way for a plugin to be told "only the static tier changed, and only
      because of your own last write" vs. "the user's config just got
      re-applied out from under you". `CReservedAreaComposer` listens for
      `Event::bus()->m_events.monitor.layoutChanged` and calls
      `reapplyAll()` to self-heal from this.

      **Real correctness bug found and fixed before ever running**: an
      exclusive window's own `recomputeAnchorPosition()` was initially
      still using `logicalBoxMinusReserved()` like every other anchored
      window - which would include *its own* just-applied contribution,
      pushing e.g. a top-anchored exclusive bar downward by its own
      height every frame (self-referential). Real layer-shell bars don't
      do this - they sit flush against the true screen edge; only *other*
      content avoids their reserved region. First fix used the monitor's
      **raw** box (`logicalBox()`) instead, ignoring ALL reserved area -
      simple, but wrong in a different way (see next paragraph).

      **Superseded by a proper fix** once live testing surfaced the actual
      consequence: the raw-box approach meant an exclusive window ignored
      *everyone's* reservation, not just its own - so it would visually
      collide with anything else also reserving that edge, most
      concretely Hyprland's own config-error overlay (which reserves
      space the exact same "ignore my own reservation, sit flush at the
      raw edge" way - `errorOverlay/Overlay.cpp`). Fixed properly: since a
      window only ever contributes to *one* edge, `CCanvas::
      setExclusive(EEdge)` now reads the monitor's live *combined*
      margins (`m_reservedArea.top()/right()/bottom()/left()` - already
      correctly summing the static tier and both dynamic slots, see
      below) and subtracts only *this window's own* current size
      (`m_size.y` or `m_size.x`, whichever axis matches its edge) from
      that one edge - every other edge is used unmodified. No separate
      stored "my contribution" field needed - it's just `m_size`, already
      tracked, read live every frame like everything else in this file.
      This is strictly more correct than the raw-box version and replaces
      it outright (not a fallback/opt-in) - a window now sits at its own
      natural position while still correctly avoiding the user's config
      baseline, Hyprland's error/debug overlay, and any other HyprLUI
      exclusive window on a *different* edge.

      **Still an open gap, not solved by this fix**: multiple HyprLUI
      exclusive windows on the *same* edge don't stack relative to each
      other - each excludes only its own contribution, so each computes
      its position as if it were the only one reserving that edge (both
      would settle at the same offset, or interleave oddly if their sizes
      differ). Fixing that needs actual ordered-stacking logic (e.g. sort
      by creation order, each computing its offset as the sum of same-
      edge contributions that come *before* it) - a real feature, not a
      one-line change; see Open questions.

      **Correction to an earlier claim in this section**: originally
      reasoned "no fix needed when the error overlay disappears" and
      verified only half of what actually happens. That half is still
      correct and needed no fix: `recomputeAnchorPosition()` re-reads the
      monitor's *live* reserved totals every frame, so once Hyprland's
      error overlay's own dynamic slot resets to zero
      (`resetType(RESERVED_DYNAMIC_TYPE_ERROR_BAR)`), an exclusive HyprLUI
      window sitting below it self-corrects (shifts back up) automatically,
      correctly damaged, no new hook needed. **What was missed**: an error
      message only ever gets *resolved* by fixing the Lua config, which
      Hyprland then re-evaluates - a genuine config reload, which calls
      `CMonitor::applyMonitorRuleSoft()` again and (separately from the
      error overlay's own dynamic slot) `setStatic()`s the monitor's
      *static* tier back to just the fresh config baseline, wiping out
      HyprLUI's own composed contribution entirely. **Two bugs, found
      live, fixed together**:
      1. `CReservedAreaComposer` only listened for `Event::bus()->
         m_events.monitor.layoutChanged` to know when to `reapplyAll()`.
         That event is emitted for monitor geometry/hotplug changes only
         (`Monitor.cpp:1422`, `MonitorLayoutController.cpp:75`) - config
         reload is a *different* event, `Event::bus()->
         m_events.config.reloaded` (`EventBus.hpp:183`), which was never
         hooked at all. Now listens to both.
      2. Even with that event hooked, `reapplyAll()` → `recompute()` would
         still have silently no-op'd: its diff-check compares the number
         it's *about* to write against `m_lastApplied`'s cache, and after
         a reload that number is often identical to the cache (neither
         the config baseline value nor HyprLUI's own sum necessarily
         changed - only the *live* object's contents did, reset by
         Hyprland's own `setStatic()` call in between). `recompute()`
         gained a `force` parameter; `reapplyAll()` now always passes
         `force=true`, since its entire purpose is "something external
         may have invalidated what I think is live," which the normal
         optimization can't distinguish from "genuinely nothing changed."

      A window's contribution tracks its *live* size via a new generic
      `CCanvas::setOnSizeChanged()` hook (fires from the same content-
      size-sync block that fixed the earlier digit-cutoff bug), same
      "Canvas exposes a generic mechanism, `LuaBridge.cpp` supplies the
      manager-specific glue" pattern `addBinding()` already established -
      so a `Bind()`ed exclusive bar's reserved height stays correct if its
      content grows/shrinks. Hiding a window (`set_canvas_visible(false)`)
      makes its contribution inactive (reserves nothing, matches eww)
      without forgetting its edge/size, so showing it again doesn't need
      re-specifying anything. `PLUGIN_EXIT` calls `clear()`, which
      restores every affected monitor back to just its config baseline -
      no stale reserved margins left behind after unload.

      **Bug found and fixed live, first real test**: `setStatic()` alone
      only changes the box *future* tiling decisions will use - it does
      NOT itself move/resize windows that are already tiled on that
      monitor. Reported symptom: toggling the exclusive top bar on had no
      effect on an already-open terminal, but a *newly* opened terminal
      correctly avoided the reserved space - exactly what you'd expect
      from a value that only affects placement, not a live relayout.
      Confirmed Hyprland's own monitor-rule-apply path (e.g.
      `CMonitor::onConnect()`, `Monitor.cpp:368`) always follows a
      reserved-area change with `g_layoutManager->recalculateMonitor
      (monitor)` (`src/layout/LayoutManager.hpp:102`) to force existing
      tiled windows to re-layout right now. `CReservedAreaComposer::
      recompute()` was missing this call entirely - added right after
      `setStatic()`, using the default `RECALCULATE_MONITOR_REASON_
      UNKNOWN` reason (matches what Hyprland's own code uses for this
      exact "something about my reserved space changed" case, as opposed
      to the more specific workspace-change/fullscreen-toggle reasons
      that exist for other call sites).

      **Verified this doesn't break Hyprland's own config-error overlay**
      (the bar Hyprland shows on a Lua config eval error, which also
      reserves space so it doesn't overlap other windows - user flagged
      this as worth checking given how close it is to what we're doing).
      Confirmed by reading `CReservedArea`'s full implementation
      (`ReservedArea.cpp`): the static tier (`m_initialTopLeft`/
      `m_initialBottomRight`, what our `setStatic()` writes) and the
      dynamic array (`m_dynamicReserved[]`, what the error overlay's
      `resetType`/`addType(RESERVED_DYNAMIC_TYPE_ERROR_BAR, ...)` writes,
      `errorOverlay/Overlay.cpp:166,171`) are genuinely separate storage -
      `setStatic()` never touches `m_dynamicReserved`, `addType()`/
      `resetType()` never touch the static fields. Both call `calculate()`
      immediately after writing, which re-sums *whatever's currently in
      both* tiers - so regardless of which side (us or the error overlay)
      wrote most recently, `left()/top()/right()/bottom()` always reflect
      both contributions correctly; there's no stale-read race, since both
      sides mutate the same shared `CReservedArea` object directly. The
      error overlay's own relayout trigger is also fully self-contained
      (`arrangeLayersForMonitor()` → `g_layoutManager->
      invalidateMonitorGeometries()`, a different/lazier trigger than the
      `recalculateMonitor()` this composer uses) - entirely independent of
      `CReservedAreaComposer`. No code change was needed; this was already
      correct by construction, specifically *because* Phase 5 chose the
      static tier to avoid stepping on either of the two dynamic slots.
- [x] **Phase 6** - `Input` widget + keyboard focus ownership (raw keysym
      only). New `src/ui/InputWidget.hpp/.cpp` (`CInputWidget`, mirrors
      `CButtonWidget`'s shape - flat-filled background + children on top,
      leaf hit-test) plus `Input{ ... onKey, onFocus, onBlur }` in
      `LuaBridge.cpp`. Explicitly out of v1 scope, per
      the phase's own name: no text composition/cursor/selection/IME -
      just raw xkb keysym + pressed forwarded to `onKey`, same "build a
      real text field in Lua on top of this" escape hatch as everything
      else in this toolkit.

      **Researched before implementing** (same practice as every internal-
      API-reliant phase): `Event::bus()->m_events.input.keyboard.key` is a
      `Cancellable<IKeyboard::SKeyEvent>` (`EventBus.hpp`), same shape as
      the mouse-button bus `InputHook.cpp` already used for Phase 4, and
      emitted from the same place/order as that bus's cousin -
      `CInputManager::onKeyboardKey()` (`InputManager.cpp:1701`) emits it
      *before* `Keybinds::mgr()->onKeyEvent()` runs, so cancelling it
      swallows both the key itself and any keybind bound to it, same
      swallow-semantics already established for Button clicks. `SKeyEvent`
      only carries a raw evdev `keycode` (xkbcommon keycodes are that +8,
      confirmed against Hyprland's own `LuaEventHandler.cpp:182`'s
      identical `input.keyboard.key` dispatch to its own Lua event system)
      - no keysym, and no "which keyboard" info on the event itself. The
      keysym is resolved here via `xkb_state_key_get_one_sym()` against
      whichever keyboard `g_pSeatManager->m_keyboard` currently is (the
      same "active" keyboard Hyprland's own keybind resolution uses) -
      already layout/shift-aware, since xkbcommon bakes that into the
      keysym itself. Also confirmed `input.keyboard.focus`
      (`Event<SP<CWLSurfaceResource>>`) is Hyprland's *real* Wayland
      keyboard-focus-surface signal - genuinely irrelevant here, since
      HyprLUI canvases aren't real surfaces at all; "focus" for an Input
      has to be a concept HyprLUI invents and tracks itself, entirely
      parallel to Hyprland's own.

      **Design decisions, all explicit user calls** (asked up front, same
      "dig in and prompt me the questions" practice as every phase):
      - Focus model: click-to-focus/click-away-to-blur is the *default*
        (clicking an Input grabs HyprLUI's single global focus slot,
        immediately on press - not gated on a full press+release like
        Button's onClick, since there's no "cancel by dragging off"
        convention for focus, same as a real text field; clicking
        anything else - empty space, a Button, a different Input, a real
        window - blurs it), but it's also explicitly focusable/blurrable
        from Lua (`focus_widget(window, id)` / `blur_widget()`) so a
        caller can drive focus with its own triggers (e.g. focus a search
        box the instant its window opens) without needing a synthetic
        click.
      - `onFocus`/`onBlur` callbacks exist (not just `onKey`) specifically
        so Lua can react to *either* path (a real click or a programmatic
        `focus_widget()` call) the same way, rather than only knowing
        about clicks.
      - Keybind priority: a focused Input should never outrank Hyprland's
        own keybinds (SUPER+... etc.) just by being focused - a HyprLUI
        widget silently eating the user's keybinds because it happened to
        be clicked would be a nasty surprise.

      **Keybind-priority default revised twice after live use, converged
      on an absolute exclusion**:
      1. The initial implementation took "don't outrank keybinds" to mean
         `onKey` should just observe and never cancel by default
         (`consumeAllKeys`, off by default, was the only way to swallow
         anything). **User-reported gap, live testing**: this meant
         *ordinary typing* leaked straight through a focused Input to
         whatever real window actually had Wayland keyboard focus behind
         it - technically "didn't outrank keybinds" but not actually
         usable, since typing into a HyprLUI widget also typed into
         whatever app was behind it.
      2. Revised to swallow everything by default *except* keys that are
         real Hyprland keybinds - researched `Keybinds::mgr()->
         findConflictingBind(xkb_keysym_t, Input::ModifierMask)`
         (`Manager.hpp`/`.cpp:798`) for this: a read-only query, no side
         effect, does not invoke the bind, resolving to `CRegistry::
         findShortcutConflict()` - the exact call Hyprland's own global-
         shortcuts-portal conflict check uses (`protocols/Hotkey.cpp:
         121,168`) to answer "does a bind already exist for this
         keysym+modifiers". Introduced `consumeAllKeys` (off by default)
         as the opt-in for a widget that wants to swallow the keybind too,
         like a true modal grab. **User-reported gap again, immediately
         after**: an Input that *observes* a keybind's key at all (even
         without cancelling the underlying event) is still wrong - the
         key should not "be used by the input field" in the first place
         when it's a real keybind, full stop, no per-widget exception.
      3. **Final design**: `InputHook.cpp`'s `onKeyboardKey()` calls
         `findConflictingBind()` (with `keyboard->getModifiers()` for the
         live modifier mask) *before* ever touching HyprLUI's focus
         system - if it matches, the function returns immediately, so the
         key is never forwarded to `onKey`, `CUIManager::dispatchKey()` is
         never even called, and the event is left uncancelled: Hyprland's
         keybind resolution and normal delivery proceed exactly as if no
         HyprLUI widget existed. Every key that *does* reach
         `dispatchKey()` is therefore guaranteed non-keybind, so it always
         swallows unconditionally - no per-widget flag needed anymore,
         and `consumeAllKeys` was removed as dead weight (there was no
         longer a "swallow it anyway" case left to opt into).
      **Known limitation, not fixed**: `findShortcutConflict()` explicitly
      skips any bind with a non-empty submap (`Registry.cpp:59`), so it
      only sees global-scope binds - a submap-specific keybind can still
      reach a focused Input. No read-only "is this bound in the *current*
      submap" query was found; revisit if this turns out to matter in
      practice.

      **Second bug found live, right after step 3 above**: with the
      absolute keybind exclusion in place, keyboard-driven `focus_widget`/
      `blur_widget` test binds became unreliable *specifically once an
      Input had already been focused via a mouse click* - needing repeated
      presses, or never firing at all - while the equivalent mouse-driven
      focus/blur continued to work perfectly. Root-caused by reading
      `CKeybindManager::onKeyEvent()` (`Manager.cpp:184`): it runs for
      *every* key event, including standalone modifier presses/releases
      (Alt_L, Shift_L, ...), and does essential bookkeeping there
      (`m_inputState.press()`/`.release()`, feeding `heldKeys()`) that
      later keybind matching depends on, independent of whether that
      particular press completes a bind on its own. A bare modifier key
      doesn't match anything in `findConflictingBind()` (nobody binds
      "Alt" alone), so once something was focused, `dispatchKey()` swallowed
      it - and cancelling that event makes `CInputManager::onKeyboardKey()`
      (`InputManager.cpp:1702`) return before `onKeyEvent()` is ever
      called for it, silently desyncing Hyprland's own held-key state from
      what's physically held, breaking chord matching (including this
      plugin's own test binds) for as long as focus remained. Fixed by
      excluding bare modifier keysyms unconditionally, in addition to the
      keybind check - `InputHook.cpp`'s new `isModifierKeysym()` matches
      Hyprland's own `modifierFromXkb()` set exactly (`Manager.cpp:171`,
      file-local `static`, so reimplemented rather than exposed): Super_L/
      R, Alt_L/R, Control_L/R, Shift_L/R, Caps_Lock, Num_Lock. Costs
      nothing UX-wise either - there's nothing an Input could do with a
      bare modifier anyway.

      **Third bug found live, immediately after**: **user-reported** the
      same class of symptom persisted even with the modifier fix in place
      - "focusing the first time works on the first press of the keybind
      but after that the bug is still the same as earlier". Root cause was
      the same underlying mistake as bug two, just one key later:
      `findConflictingBind()` (and `isModifierKeysym()`) were being
      re-evaluated *live* on every event, including the RELEASE of the
      trigger key itself - but nobody releases a chord atomically. If a
      modifier (Shift, say) happens to release fractionally before the
      trigger key does - the ordinary case, not an edge case - the
      trigger's own release event arrives with a modifier mask that no
      longer matches the bind, so the live re-check misclassifies *that*
      release as "not a keybind" and `dispatchKey()` swallows it once the
      just-focused Input is active - corrupting `m_inputState`'s press/
      release symmetry the exact same way an eaten modifier event does,
      just via the trigger key instead of a modifier key. This is exactly
      the problem Hyprland's own `onKeyEvent()` already solves for itself
      by remembering `modifiersAtPress` per key rather than re-deriving it
      at release (`Manager.cpp:255,283` - `pressedInput->modifiersAtPress`)
      - our exclusion logic needed the equivalent. Fixed by tracking the
      press-time exclusion decision in a small `g_excludedKeycodes` set
      keyed by raw evdev keycode (stable across a hold, unlike a re-
      resolved keysym or live modifier mask) and reusing that exact
      decision at release, instead of re-running either check against
      release-time state.

      **Implementation notes**:
      - `CUIManager`'s `SButtonHit` was generalized/renamed to
        `SWidgetHit` and `hitTestButton()` to `hitTestWidget()` - the
        underlying `CWidget::hitTest()` walk never distinguished *which*
        interactive type it found (only `CButtonWidget`/`CInputWidget`
        override it to match), so Button and Input share the exact same
        hit-testing plumbing; only `clickButton()` (dynamic_cast to
        `CButtonWidget`) and the new focus methods (dynamic_cast to
        `CInputWidget`) diverge on what a given hit actually is.
      - The single global focus slot (`m_focusedInput`, same "compared by
        value, not a raw pointer" reasoning as the original `SButtonHit`)
        lives on `CUIManager`, not on `CInputWidget` itself - a widget
        only ever reacts (`handleKey()`/`focus()`/`blur()`) when told to;
        it has no idea whether it's "the" focused one.
      - **Applied the config-reload-lifecycle lesson proactively this
        time**, rather than waiting for it to be reported live: whichever
        Input currently holds focus is explicitly blurred (`onBlur`
        fires) before its owning canvas is destroyed
        (`CUIManager::removeCanvas()`), before just that one widget is
        removed (`remove_widget` in `LuaBridge.cpp`), and before its
        canvas is hidden (`set_canvas_visible(false)`, matching the
        existing "hidden exclusive window reserves nothing" precedent) -
        so Lua's own idea of "what's focused" (whatever it's tracking via
        onFocus/onBlur) never silently goes stale the way the pre-Phase-6
        config-reload bug did. `CUIManager::clear()` (the config-reload
        wipe) just drops the tracking state directly instead, since every
        widget is already gone by the time it runs - nothing left to call
        `blur()` on.

      **Scope revised after the text-field demo shipped**: the original
      "raw keysym only" framing was read (reasonably, at the time) as
      "capture/display/Backspace-removal is a caller concern, same as any
      other custom behavior built on top of onKey" - the very first test
      config did exactly that (a hand-rolled Lua string buffer + a child
      Text label + manual set_text() calls). **User pushed back**: basic
      text-entry behavior (type a character, see it appear, Backspace
      removes it) is what an "actual input field" does by default -
      that's not optional custom behavior for every caller to reimplement,
      it's what makes something an input field at all. Moved that
      behavior into `CInputWidget` itself: it now owns an internal
      `CTextNode` child (added via `addChild()` in the constructor, so it
      goes through the exact same measure/arrange/render/hitTest walk as
      any other child, not a separate rendering path - reuses
      `CTextNode`'s existing rasterization rather than duplicating it) and
      a `std::string m_text` buffer that `handleKey()` mutates directly on
      printable-ASCII (0x20-0x7e) and `XKB_KEY_BackSpace`, before still
      forwarding every key to `onKey` same as before (built-in capture is
      additive, not a replacement for raw access). New `text`/`textColor`/
      `textSize`/`textFont` constructor-time spec fields style the label;
      new `onChange(text)` fires on real edits; new `set_input_text()`/
      `get_input_text()` mutate/read it from outside a callback (e.g. a
      sibling Button reading the current value on click). Still
      deliberately NOT a full text field - no cursor, selection, IME, or
      non-ASCII input; DESIGN.md's "raw keysym only" now specifically
      means *that* boundary, not "you get a bare keysym and nothing else".
      One new plumbing detail this required: `CUIManager::dispatchKey()`
      now calls `canvas->damage()` itself after `handleKey()` - text
      content changing doesn't repaint on its own (same contract
      `set_text()` already follows), and this mutation now happens
      outside Lua's control, so nothing else would have called it.

      **Fourth bug found live - the actual root cause, isolated by a much
      more precise repro**: after confirming a genuine full plugin reload,
      user testing narrowed the symptom to something deterministic rather
      than flaky: `ALT + SHIFT + U` was voided *every single time* while
      an Input was focused, never otherwise - while `ALT + T` (single-
      modifier) kept working regardless of focus. That determinism ruled
      out both earlier fixes as the explanation (a state-corruption theory
      would degrade `ALT + T` too) and pointed at the exclusion *query*
      itself being wrong for this specific bind, not at Hyprland's
      bookkeeping being desynced.
      Root cause, confirmed by reading `CKeybindManager::onKeyEvent()`
      (`Manager.cpp:206-207`): Hyprland resolves a bind's own trigger key
      against a **modifier-independent** xkb state -
      `keyboard->m_resolveBindsBySym ? keyboard->m_xkbSymState :
      m_xkbTranslationState` (the latter private to `CKeybindManager`, not
      reachable from a plugin; `m_xkbSymState` is the public equivalent
      concept on `IKeyboard` - confirmed via `IKeyboard::
      updateXkbStateWithKey()` in `IKeyboard.cpp`, which only ever updates
      `m_xkbSymState`'s *group/layout* via `xkb_state_update_mask()`,
      never its modifiers via `xkb_state_update_key()`) - specifically so
      a bind named "U" matches regardless of whether Shift happens to be
      held, with the modifier requirement checked separately via the
      bind's own `modmask`. `InputHook.cpp`'s exclusion query was instead
      resolving its keysym from the **live**, fully modifier-aware
      `keyboard->m_xkbState` (needed for onKey/the built-in text capture,
      where Shift+a *should* type 'A') - so while Shift was actually held,
      the query asked "does a bind match uppercase 'U'?" against a
      registry entry whose trigger is registered as lowercase 'u',
      permanently failing to match. Harmless while nothing was focused
      (Hyprland's own correctly-resolved matcher fired the bind
      regardless of what this plugin's query concluded), but once an
      Input *was* focused, the always-wrong "not a keybind" conclusion
      meant `dispatchKey()` swallowed the key instead of excluding it -
      deterministically, matching the report exactly. `ALT + T` has no
      Shift component, so its live and neutral keysyms coincided and it
      was never affected. Fixed by resolving two separate keysyms per
      event: the existing live one (`keysym`, still used for `onKey`/text
      capture/dispatch) and a new modifier-independent one (`bindKeysym`,
      resolved via `keyboard->m_xkbSymState`, used only for the
      `findConflictingBind()` query).
      This is the fourth distinct bug found in this exclusion logic across
      three rounds of live testing (bare-modifier events being swallowed;
      press/release exclusion-decision asymmetry from live modifier-mask
      re-checks; and now this shifted-vs-unshifted keysym mismatch) - a
      genuine measure of how much internal, undocumented-to-plugins state
      Hyprland's real keybind resolution depends on, and how easy it is
      for a stateless reimplementation to diverge from it in a way that
      only surfaces for specific key combinations. **User-confirmed fixed**
      after live re-testing both directions (`focus_widget`/`blur_widget`
      keybinds), unlike the previous two rounds - this one actually
      resolved it.
- [x] **Phase 7** - Base widget properties: padding, margin, min/max
      sizing (+ the text-overflow decision it forced), opacity, z-index,
      and a per-widget runtime visibility toggle. All six now live directly
      on `CWidget` (`Widget.hpp`) - shared by every widget type, not
      reimplemented per subclass.

      **Three sub-decisions resolved up front** (asked via three targeted
      questions, same "surface the call before implementing, don't guess"
      practice as every prior phase):
      1. Text overflow default: **truncate with ellipsis** over wrap or
         hard clip.
      2. Margin vs. the existing container `gap`: **kept separate** - a
         new per-widget `margin` (CSS-flexbox-item style, read by the
         container laying the widget out) that ADDS to `gap`, rather than
         merging the two concepts into one.
      3. Opacity composition: **multiply** with every ancestor's own
         opacity (CSS/Qt/every-toolkit convention), not override/
         independent.

      **Padding**: promoted from a private uniform `double` that only
      `CFlexWidget` had (Phase 1) to a shared `SEdgeInsets{top, right,
      bottom, left}` on `CWidget` itself (`setPadding()`/`padding()`) -
      this is the reconciliation the phase's own kickoff note flagged as
      needed, done by generalizing the existing field rather than adding a
      second one alongside it. Only `CFlexWidget` (insets its own children
      from its edges, `ContainerWidget.cpp`) and `CInputWidget` (insets its
      auto-owned label, replacing a hardcoded `8.0` left-offset literal
      with `setPadding({.left = 8})` as its own constructor-time default -
      see below) actually interpret it; `CStackWidget`'s manual/absolute
      positioning leaves it unused by design, documented directly in
      `ContainerWidget.hpp` - full manual control already covers spacing
      there, and silently offsetting explicit x/y would be surprising.

      **Margin**: new `SEdgeInsets` field on `CWidget`, read only by
      `CFlexWidget` (a child's own margin, not the container's) - added on
      top of `gap` in both `measureContent()` (main-axis sum includes each
      child's leading+trailing margin; cross-axis max includes it too) and
      `arrangeChildren()` (position offset advances by
      `mainLead + childMain + mainTrail + gap`; cross-axis alignment
      subtracts `crossLead + crossTrail` from the space it aligns within,
      so Center/End respect an asymmetric margin correctly, not just a
      symmetric one). `CStackWidget` leaves it unused, same reasoning as
      padding above.

      **Min/max sizing**: `setMinSize()`/`setMaxSize()` on `CWidget`,
      clamped in `measure()` AFTER the existing fixed-size override (same
      precedence CSS gives min/max-width over an explicit width - "never
      smaller/larger than this" is a stronger constraint than "this size"
      once both are given). This is what forced the text-overflow decision:
      `CTextNode::rebuildTexture()` now forwards `maxW` straight into
      `gfx::makeTextTexture()`'s existing (previously always-0, unused)
      `maxWidth` parameter - which Hyprland's own `IHyprRenderer::
      renderText()` (`Renderer.cpp:1583`) already truncates-with-ellipsis
      given one (`pango_layout_set_width()` + `pango_layout_set_ellipsize
      (..., PANGO_ELLIPSIZE_END)`, confirmed by reading it before
      implementing). The chosen default came essentially for free from
      Hyprland's own text renderer rather than needing to be built - wrap/
      clip modes were never wired up, since only one mode was needed once a
      default was picked. **A real correctness issue found and fixed
      before it could surface live**: `CTextNode::render()` originally drew
      via `boxAt(origin)` (the LAYOUT box, `m_size` - which `minW`/`minH`
      can widen beyond the rasterized texture's native size), and
      Hyprland's texture pass element scales its source to fill whatever
      box it's given - so a min-widened Text would have visibly
      stretched/blurred its own glyphs to fill the extra space, the wrong
      behavior every other toolkit avoids for text specifically (min-width
      reserves empty layout space, it doesn't stretch content). Fixed by
      drawing at `{origin + m_position, m_texture->m_size}` - the
      texture's own native size - instead, which is a no-op difference
      whenever they're already equal (the common case, and always true
      when `maxW`, not `minW`, is what's active, since Pango already
      rasterizes to fit that exactly).

      **Opacity**: `CWidget::render()`'s signature gained a
      `float parentOpacity = 1.0F` parameter - the already-composed
      opacity of every ancestor - multiplied by this widget's own
      `m_opacity` before being used (for a leaf: faded straight into
      `CHyprColor.a` for `CRectNode`/`CButtonWidget`/`CInputWidget`'s flat
      fills, or passed as `gfx::drawTexture()`'s existing separate `alpha`
      parameter for `CTextNode`) and threaded down to children. **A
      double-multiplication bug caught before it could ship**: both
      `CButtonWidget::render()` and `CInputWidget::render()` draw their own
      background AND then delegate to `CWidget::render()` for their
      children (same shape Phase 4/6 already established) - an early draft
      passed the already-self-multiplied `parentOpacity * m_opacity` value
      into that `CWidget::render()` call, which then multiplies by
      `m_opacity` AGAIN internally (since `this` is still the same widget
      instance), fading a Button/Input's children by its own opacity
      twice. Fixed by passing the ORIGINAL `parentOpacity` through
      unchanged to the base-class call, letting it do its own single
      multiply, matching every other container.

      **Z-index**: `int m_zIndex = 0` on `CWidget`, with a new private
      `paintOrder()` helper (a `std::stable_sort` of `m_children` by
      `zIndex()`, ascending) that both the default `render()` (paints
      ascending - lower z first/behind) and the default `hitTest()`
      (walks that same order in reverse - highest z first, so an
      overlapping higher sibling wins the hit) now use instead of the raw
      unsorted child list. Deliberately just a sibling-local reorder, not a
      full CSS stacking-context system - a low-`zIndex` child of a high-
      `zIndex` widget still paints "inside" its parent's turn, it can't
      jump above a sibling of a *different* parent. `stable_sort` means
      ties (including the default - everyone at 0, unless a spec actually
      sets `zIndex`) keep plain insertion order, so this is a pure additive
      extension with zero behavior change for any existing config that
      never mentions `zIndex`.

      **Visibility toggle**: turned out to be mostly already in place -
      `CWidget::setVisible()`/`visible()`/`m_visible` existed since Phase 1
      and `render()`/`hitTest()` already both early-return on it; the
      `visible` constructor-time spec field was already wired in
      `buildWidget()`. The missing piece was a RUNTIME mutator (toggling it
      after construction, "without destroying and recreating its subtree"
      per this phase's own framing) - added as `set_widget_visible(window,
      id, visible)` in `LuaBridge.cpp`, generic over any `CWidget` (not
      type-specific like `set_text`/`set_input_text`), following the exact
      same "blur first if this happens to be the focused Input" precedent
      `set_canvas_visible()`/`remove_widget()` already established.

      **Lua surface**: every new base property (`padding`, `margin`,
      `minW`/`minH`/`maxW`/`maxH`, `opacity`, `zIndex`) is parsed
      generically in `buildWidget()`'s common tail (after the per-type
      branch, alongside the existing `visible` handling) rather than
      per widget type - `padding`/`margin` accept either a single number
      (uniform) or a table `{top, right, bottom, left}` (CSS shorthand-
      table convention, matching `color`'s existing number-or-table
      pattern; an omitted side in the table form is 0, not the uniform
      default). Each is only actually *applied* (via the corresponding
      `CWidget` setter) if the Lua spec mentions the field at all - a new
      `optInsetsField()` helper (mirrors `optFixedField()`, itself reused
      as-is for opacity/zIndex/min/max since it was already a fully generic
      "optional numeric field" reader despite its size-specific name)
      returns `std::nullopt` on a missing field so a widget's own
      constructor-chosen default (`CInputWidget`'s left padding, in
      particular) isn't silently zeroed out by a spec that just doesn't
      mention `padding` - caught during design, before it could ship as a
      live regression the first time someone used an `Input{}` without
      explicitly repeating `padding = 8`.

      **`CInputWidget`'s default padding, and why it's now recomputed every
      frame instead of baked in once**: the label's position used to be a
      one-time `Vector2D` computed in the constructor. Since Phase 7's
      `padding` field is applied via `setPadding()` AFTER construction
      (same as every other post-construction field in `buildWidget()`), a
      Lua-supplied custom `padding` on an `Input{}` would have silently had
      no visible effect - the label's position was already baked in before
      `setPadding()` ever ran. Fixed by giving `CInputWidget` a new
      `arrangeChildren()` override that repositions the label from
      `padding().left` and the current `m_size`/label height EVERY
      `arrange()` pass (matching `CFlexWidget`'s already-established "redo
      layout every frame, no dirty flag" pattern) instead of once. This
      also incidentally improved vertical centering accuracy: by
      `arrange()`-time the whole tree's `measure()` pass has already run
      (see `Widget.hpp`'s `measure()`-before-`arrange()` two-pass
      contract), so the label's ACTUAL rasterized height is available -
      the original constructor-time code had to approximate it via the
      configured point size instead, since rasterization hadn't happened
      yet.

      **Debug overlay, added post-Phase-7 in response to live testing**:
      once padding/margin/etc. were actually being used, the user reported
      no way to visually confirm they were applied correctly - everything
      about the box model was invisible unless you already knew the
      numbers. Brainstormed (three questions, same up-front practice as
      every design decision this session) and converged on: a per-widget
      `debug` field (tri-state, cascades to descendants by default),
      `debugCascade` to wall a subtree off from that inheritance, and
      `debugShow{...}` to force individual detail categories on/off,
      layered on top of a "show based on size" automatic default.

      New `CWidget::renderDebug()` (declared in `Widget.hpp`, implemented
      in the first-ever `Widget.cpp` - previously fully header-only) is a
      THIRD tree walk, entirely separate from `render()`/`hitTest()`,
      invoked once per frame from `CCanvas::render()` right after the real
      `render()` call so debug overlays always paint on top regardless of
      any widget's own z-index/opacity (diagnostic, not real content - it
      shouldn't itself be faded/reordered by the thing it's diagnosing).
      Non-virtual and implemented exactly once, same reasoning as
      `measure()`/`arrange()`: the box-model information it draws
      (position/size/padding/margin/id/zIndex/opacity) is entirely made of
      base `CWidget` fields, no per-subclass knowledge needed - the one
      exception is a new `virtual bool isInteractive() const` hook
      (default false; overridden true by `CButtonWidget`/`CInputWidget`)
      so the hit-target fill only ever draws for widgets that can actually
      be clicked, never on a purely decorative one even if force-shown via
      `debugShow.hitTarget = true`.

      New `SDebugSpec{ enabled, showBox, showPadding, showMargin, showId,
      showSize, showZOpacity, showHitTarget }` (all `std::optional<bool>`)
      does double duty as both "what a widget's own Lua spec asked for"
      and "what's been resolved while walking down the tree so far" -
      merging one into the other is the same "mine wins if set, else keep
      theirs" operation either way (`resolveDebugSpec()`). `enabled`
      always resolves to a concrete bool by the time it reaches the root's
      initial `SDebugSpec{}` (unset = `false`); the `show*` categories can
      stay unresolved (`nullopt`) all the way to the actual draw call,
      where that means "decide automatically" rather than "still
      inheriting" (see below) - two different meanings for the same
      "unset" state at two different points, made unambiguous by which
      code path is asking. `debugCascade` (default true, NOT part of
      `SDebugSpec` itself - a purely local, non-inherited per-widget
      switch) governs what a widget's children inherit: `true` passes down
      this widget's own just-resolved `SDebugSpec`; `false` resets to a
      fresh, all-`nullopt` one - children start over as if nothing above
      them had ever set `debug` at all, not merely "without this widget's
      own overrides." Chosen over the narrower "just skip me, keep
      inheriting from further up" interpretation because it's simpler to
      implement AND matches the concrete use case that motivated asking
      for it in the first place (a noisy subtree the user wants left alone
      entirely, not partially).

      Draws three nested/expanded outline boxes per widget (four thin
      filled `gfx::drawRect()` strips each - `gfx.hpp` has no stroke
      primitive, and this is cheap/simple enough not to need one): a
      margin box (expanded outward from the content box by `margin()`,
      orange), the content/padding box itself (`boxAt()`, blue), and a
      padding-inset box (shrunk inward by `padding()`, green, only drawn
      when padding is actually non-zero). Text labels (id, `WxH`, compact
      padding/margin values - `"8"` if uniform on all sides, `"T8 R4 B8
      L4"` otherwise) are rasterized via the same `gfx::makeTextTexture()`
      every other text in this toolkit uses, but **deliberately NOT
      cached** the way `CTextNode` caches its own texture - a debug label
      is rebuilt from scratch every single frame it's shown. Accepted,
      documented tradeoff rather than an oversight: debug mode is an
      opt-in, dev-time-only tool nobody ships a real config with turned
      on, so the re-rasterization cost (real, and exactly what
      `TextNode.hpp`'s own doc comment warns against doing every frame for
      *shipped* content) doesn't need paying for with cache-invalidation
      complexity here. Revisit only if debug mode turns out to get used
      heavily enough, on large enough trees, for this to actually matter
      in practice.

      "Auto" sizing gate: labels/insets stay hidden below a `96x16`
      content-box threshold (too small to render them legibly - widened
      from an initial `48` after live testing showed labels like `T8 R4
      B8 L4`/`z:2 op:0.35` still getting clipped at that width) unless
      explicitly force-shown via `debugShow`, which bypasses the gate
      entirely. `showZOpacity`'s auto default has a second condition on
      top of the size gate - `zIndex != 0 || opacity != 1.0` - since an
      always-default z/opacity isn't interesting to surface uninvited;
      again, an explicit `debugShow.zOpacity = true` bypasses both gates
      at once, not just the size one.

      **`debugFontSize` added right after, on request**: `SDebugSpec`
      gained an `std::optional<int> fontSize` field, inherited/cascaded
      exactly like `enabled`/`show*` (default 10, same default the
      overlay always used before this existed). Label Y-offsets (how far
      above/below its box an id/margin label sits) now scale with the
      resolved font size instead of a flat `12`, so a larger
      `debugFontSize` doesn't start overlapping the outline it's labeling.
- [x] **Phase 8** - v1 widget catalog completion: `Image`, `Divider`,
      `Checkbox` (checked/unchecked only - explicitly not an iOS-style
      toggle switch). `Slider`/`ProgressBar` remain deliberately deferred
      to a fast-follow release after v1, not part of this phase.

      **`Image` - researched before implementing** (a fork was sent to
      find how Hyprland loads images internally, same practice as every
      internal-API-reliant phase; its report named `g_pHyprOpenGL`/
      `OpenGL.hpp` for the texture-upload step, which turned out to be
      wrong on independent verification - `createTexture(cairo_surface_t*)`
      is actually declared on `IHyprRenderer` (`Renderer.hpp:176`), the
      exact same interface `g_pHyprRenderer` already uses for
      `renderText()` - so `gfx::makeImageTexture()` needed no new global
      or include beyond what `gfx.cpp` already had. Lesson: verify a
      subagent's cited file/line directly with a grep before building on
      it, even when the rest of its report is accurate). Decoding itself
      uses `Hyprgraphics::CImage` (`<hyprgraphics/image/Image.hpp>`) - a
      separate library (`libhyprgraphics`) Hyprland core also depends on,
      not the stable `HyprlandAPI::` surface, same stability tier as
      `renderText`. Confirmed via `ldd`: PNG/JPEG/WEBP/SVG/AVIF/JPEG-XL all
      supported "for free." Synchronous (the constructor decodes
      immediately) - deliberately not using Hyprland's separate async
      `CAsyncResourceGatherer`/`CImageResource` layer (used internally for
      wallpaper prefetch), which would need a loading-in-progress state
      this toolkit has no other precedent for; a real config's image count
      is small enough that a synchronous decode at window-build time is
      fine, matching this project's general "simplest correct thing, don't
      build machinery a HUD-scale toolkit doesn't need" bias.

      Needed an actual new build dependency (`hyprgraphics`) - the FIRST
      time this project has needed one. Added to both `Makefile` (cflags
      AND, unlike every other pkg-config dependency in that file, explicit
      `-l`/`--libs` linking) and `meson.build`. The explicit linking is a
      deliberate departure from this project's established "cflags only,
      let the host process's own already-loaded symbols resolve
      everything else at dlopen time" pattern (which every other internal
      API call so far has relied on, e.g. `renderText`/Lua's C API) -
      `Hyprgraphics::CImage`'s constructor/destructor/etc. are actual code
      implemented inside `libhyprgraphics.so` itself, a genuinely separate
      shared library, not just a method call through an already-resolved
      Hyprland singleton - so this can't lean on the same "it's all in the
      host process's own symbol table already" assumption. Verified by a
      real `make` build + link (clean) and the established
      `nm -D | grep "U _Z.*lua"` extern-C regression check (still 0).

      `CImageWidget` decodes EAGERLY (constructor and every `setImage()`
      call), unlike `CTextNode`'s lazy-on-first-`measure()` texture cache -
      a deliberate divergence: text rasterization essentially never fails,
      but a bad image path/unsupported format is a realistic, common
      config mistake, and eager decoding is what lets `LuaBridge.cpp` check
      `loaded()` and log a warning (not `luaL_error` - the rest of the
      window is still meaningful even with one broken icon, unlike a
      genuinely malformed spec) immediately at build time rather than only
      once the widget first gets measured. Size-to-content by default (the
      decoded texture's natural pixel size); an explicit fixed `w`/`h`
      (reusing `CWidget::setFixedSize()` - the exact same mechanism
      containers already use, no new plumbing) scales/stretches the image
      to fill that box - deliberately the OPPOSITE choice from
      `CTextNode::render()`'s Phase 7 fix, which draws at the texture's own
      native size specifically to avoid stretching text glyphs. Stated
      explicitly in both classes' doc comments since they look like the
      same kind of leaf and aren't: stretching a photo/icon to a requested
      size is the normal, expected behavior (matches plain CSS `<img>`
      sizing); stretching text glyphs looks wrong.

      **`Divider`** needed no new C++ widget class at all - purely
      Lua-side sugar in `LuaBridge.cpp`'s `buildWidget()` that constructs a
      plain `CRectNode` with a `w`/`h` computed from `length`/`thickness`/
      `orientation`. A Divider has no behavior a Box doesn't already have;
      the whole point is not having to remember "just make one axis 1px"
      by hand.

      **`Checkbox`** (`CCheckboxWidget`, mirrors `CButtonWidget`'s shape -
      flat-filled outer box, leaf hit-test, `isInteractive() = true`)
      renders a smaller inset filled square on top when checked, using a
      separate `checkedColor` - a plain rect indicator rather than a
      checkmark glyph, since this toolkit has no icon/glyph font dependency
      to draw one with (and a filled square is itself a common enough
      native-checkbox convention). The one real difference from Button:
      `click()` TOGGLES its own `m_checked` before firing
      `onChange(bool)` with the NEW value, rather than just notifying
      "something was clicked" and leaving all state to the caller - a real
      checkbox needs an answerable checked/unchecked question independent
      of Lua (`get_checkbox_checked()`). This meant generalizing
      `CUIManager::clickButton()` (renamed `clickWidget()`) to
      `dynamic_cast` against `CButtonWidget` OR `CCheckboxWidget` and
      forward to whichever matches' own `click()` - same "generalize the
      dispatch, let each type's own method decide what a click means for
      it" pattern Phase 6 already set for `SWidgetHit` covering both Button
      and Input at the hit-testing level.

      New Lua-facing mutators: `set_image()`, `set_checkbox_checked()`,
      `get_checkbox_checked()` - same "no `onChange`/onLoad invocation on
      a programmatic set" convention `set_text()`/`set_input_text()`
      already established. A new `fieldOnChangeBool()` helper in
      `LuaBridge.cpp` mirrors `fieldOnChange()`'s shape exactly, just
      pushing a boolean argument instead of a string.

      **Known gap, deliberately deferred - low-priority TODO, not worth it
      without a concrete driving need**: `Image{}` cannot render/animate
      GIFs. Two separate problems, not one: (1) `Hyprgraphics::CImage`
      (what `makeImageTexture()` uses) has no GIF decode at all - its
      `eImageFormat` enum only lists PNG/AVIF/JPEG/JXL/BMP/SVG/WEBP, and
      `libhyprgraphics.so` doesn't even link `giflib` (confirmed via
      `ldd`) - so GIF support would need an entirely separate decoder
      (`giflib` is the obvious choice) linked directly by HyprLUI,
      bypassing Hyprgraphics for this one format. (2) Even with a decoder,
      actual *animated* playback needs real machinery this toolkit doesn't
      have yet: per-frame textures (decoded once, or lazily per-frame),
      something to advance the current frame on a schedule - reusing
      `CWatcherManager`'s internal Hyprland event-loop-timer mechanism
      (`CEventLoopTimer`, the same primitive Phase 3's polling watchers
      already use) rather than inventing a second timer path - and
      continuous re-damage while animating, same "damage every draw while
      visible" trick `NotificationOverlay` uses (see this doc's "Current
      state" section) - a single mutation's worth of damage isn't enough
      for something that keeps changing every frame on its own. If this
      ever gets picked up, it's a real chunk of new work (new dependency +
      new timer-driven animation pattern), not a small extension of the
      existing `Image{}` code path.
- [x] **Phase 9** - Widget composability: `hyprlui.defineComponent(name,
      { props?, render })` registers a reusable, string-referenced widget
      template (in any file - it's a plain Lua-callable function, so
      `require()`ing a module that calls it is all "cross-file" needs);
      `hyprlui.Component(name, props?, opts?)` instantiates one.

      **First round of analysis (before the user weighed in) undersold the
      problem**: the initial framing was "does reusing a widget variable
      give you a fresh instance?" - investigation showed `buildWidget()`
      already constructs a fresh `CWidget` every time it runs on a spec
      table, so a plain Lua function returning a fresh table literal per
      call already gave "fresh instance per call," and `Bind()` already
      composed correctly through it (each call's binding closures capture
      their own widget pointers independently). An initial 3-question
      AskUserQuestion round framed around that narrower analysis was
      rejected - **the user wanted something closer to a real component
      system**: definitions in a separate file, registration, a validated
      props schema with defaults, and instantiation by STRING reference
      (not by holding a Lua function value) - closer to a Vue component
      than a bare closure. Re-scoped around that instead.

      **Design, worked out in a written proposal and confirmed before
      implementing** (same "propose the mechanics and scoping rules
      explicitly, get confirmation, then build" practice as every
      contested design decision this project has made):
      - `render(props)` is a plain Lua function returning exactly one
        widget (the direct result of a single existing widget-constructor
        call) - no new construction primitive needed *inside* it, it's
        ordinary tree-building code.
      - **Scoping, the part explicitly flagged as needing real answers**:
        `render` is an ordinary Lua closure - anything it captures from
        OUTSIDE itself is a normal Lua upvalue, SHARED across every
        instance of that component everywhere (a module-level variable,
        not per-instance state). There is no re-render cycle in this
        system at all (`render()` runs once, at `Component()`-call time,
        same as any other widget constructor) and deliberately no React/
        Vue-style per-instance component state mechanism - state after
        construction lives either in the widget tree itself (mutate-by-id,
        same as everything else already works) or in ordinary Lua
        variables the config author manages themselves. Stated explicitly
        as a documented gotcha (LuaBridge.hpp, ComponentRegistry.hpp)
        rather than left implicit, since it's the single most likely
        source of confusion for someone coming from a real frontend
        framework's component model.
      - **The actual bug this closes**: not "instances aren't fresh" (they
        already were) but that a component's internally-hardcoded ids
        (e.g. always `id = "label"`) collide across every instance beyond
        the first - previously silent (`findWidget()` just returns the
        first match), so the 2nd/3rd/... instance's mutations silently
        landed on the 1st forever. Solved by auto-rewriting every explicit
        id in `render()`'s output: the ROOT's own id becomes exactly the
        instance's `key` (`opts.key` if given, else an auto-generated
        `name#N`); every DESCENDANT's explicit id becomes
        `key .. "::" .. originalId`. A component author can safely reuse
        the same ids in every call - the rewritten ids are always unique
        per instance by construction. Root-gets-bare-key (not
        `key::rootId`) was a deliberate choice so the whole instance stays
        addressable with just the key (`remove_widget(win, key)`), not
        also requiring the caller to know whatever id the author happened
        to give the root internally.
      - `opts` (third arg) carries the same base widget fields (x/y/
        padding/opacity/debug/etc.) every other widget already accepts at
        its own call site, overlaid onto the root after `render()`
        returns - so a component's `render()` never needs to hardcode or
        forward its own position; that stays purely the caller's concern,
        consistent with every other widget in this toolkit. `opts.key`
        sets the instance key explicitly (consumed here, never copied onto
        the root as an actual widget field); every other `opts` field is
        copied onto the root generically (not restricted to a fixed
        WidgetCommon field-name whitelist), so this doesn't need updating
        every time a future phase adds a new base widget property.
      - Unknown props (not in the schema) are a hard error, same class as
        a missing required one - schema is parsed ONCE at
        `defineComponent()` time into a C++-side map, not re-validated
        against a raw Lua table on every `Component()` call (both for
        safety - see the `lua_next` note below - and because a component
        instantiated in a loop shouldn't re-pay Lua-table schema-walking
        cost per iteration).
      - A `render()` that errors, or doesn't return a single tagged
        widget-spec table, propagates as a real build-time failure
        (`lua_call`, not `lua_pcall`) - same failure class as a missing
        required field on any other widget (this runs synchronously during
        tree construction), deliberately NOT caught-and-logged like
        `onClick`/`onChange` (those fire from input-handling contexts with
        no caller-side pcall of their own - `Component()` isn't one).

      New `src/ui/ComponentRegistry.hpp/.cpp` (`CComponentRegistry`
      singleton, mirrors `CWatcherManager`'s shape - a named,
      `lua_State*`-owning, Lua-callback-backed registry) - doesn't touch
      the widget tree or `CWidget` at all. `instantiate()` resolves
      entirely at the Lua-table level (validate props, call `render()`,
      rewrite ids, overlay `opts`) and leaves an ORDINARY already-
      `__type`-tagged widget-spec table on the stack, indistinguishable
      from calling `hyprlui.Box{}` directly - so `LuaBridge.cpp`'s
      `buildWidget()` needed ZERO changes to handle a `Component()`'d
      subtree; it just recurses into it like any other child.

      **First-ever use of `lua_next` (generic Lua table iteration) in this
      codebase** - every previous field read anywhere in `LuaBridge.cpp`
      reads a specific, known-in-advance field name via `lua_getfield`.
      Needed here three times (walking an author-defined props schema's
      keys at `defineComponent()` time; checking a caller's `props` table
      for keys not in the schema; copying every `opts` field generically
      onto the root) since prop/opts field names aren't known in advance.
      Traced the stack-balance of every branch by hand before trusting
      it (the Lua manual's specific hazard - calling `lua_tolstring` on a
      non-string key mutates it in place and corrupts an in-progress
      traversal - is avoided throughout by always checking `lua_type(...)
      == LUA_TSTRING` before ever calling `lua_tostring` on a `lua_next`
      key, and non-string keys are silently skipped rather than crashing
      the traversal).

      **New general win, not specific to `Component()`**: `buildWidget()`
      now hard-errors on ANY duplicate explicit `id` reused twice within
      one `window{}` tree, threaded through its recursion as a new
      `std::unordered_set<std::string>& seenIds` parameter (alongside the
      existing `autoId`/`bindings`). This was previously silent everywhere
      in this toolkit, not just for components (`findWidget()` always just
      returns the first match) - and closes the one residual gap the
      per-instance key-rewriting above doesn't: two children INSIDE one
      `render()` call's own output both explicitly reusing the same id
      (e.g. two children both `id = "label"`) still collide after
      rewriting (both become `key::label`) - now caught immediately as a
      build-time error instead of silently misdirecting mutations, for
      hand-written trees and component output alike.

      **Deliberately out of v1 scope**: no slots/children-passthrough
      (Vue's `<slot>`) - `render(props)` only ever gets data via `props`,
      never positional child widgets from the call site. Nothing in the
      motivating use case needed it; additive later if a real need shows
      up (would need render() to accept and re-splice caller-supplied
      children into a placeholder position in its own output, real added
      complexity).
- [x] **Phase 10** - Interactive widget layer: `disabled` state, hover
      (with automatic `hoverColor`/`disabledColor` application plus
      `onHoverStart`/`onHoverEnd` callbacks), cursor feedback, and opt-in
      scroll capture (`onScroll`) - all shared `CWidget` base fields, only
      actually interpreted by widgets whose `isInteractive()` is true
      (`Button`/`Input`/`Checkbox`), same "shared base field, selectively
      used" pattern Phase 7's padding/margin/opacity/etc. already
      established (not a literal FSM class - three booleanish states with
      simple, non-overlapping transitions didn't warrant one).

      **Researched before implementing** (same practice as every internal-
      API-reliant phase): confirmed `mouse.move` (`Cancellable<Vector2D>`)
      and `mouse.axis` (`Cancellable<IPointer::SAxisEvent>`) already exist
      in `EventBus.hpp` alongside `mouse.button` in the exact same shape,
      unused until now. For cursor control, found
      `Pointer::Cursor::overrideController` (`src/pointer/cursor/
      CursorShapeOverrideController.hpp`) - a priority-grouped override
      system genuinely designed for exactly this ("something wants to
      request a cursor shape without fighting other cursor state, e.g.
      window-edge-resize or drag-and-drop cursors") - confirmed by reading
      `CInputManager`'s own constructor, which already listens on its
      `overrideChanged` signal and applies the result via
      `setCursorFromName()`, the same call `IHyprRenderer` uses for every
      other cursor change. `CURSOR_OVERRIDE_UNKNOWN` (the lowest-priority
      group) is used for HyprLUI's hover cursor on purpose - a real
      window-edge-resize or drag cursor should win over a HUD hover
      indicator, not get fought with it.

      **First use of a header-defined `inline` global (not an `extern`-
      declared pointer like every other Hyprland singleton this project
      has reached into so far) - verified rather than assumed**:
      `overrideController` is `inline UP<CShapeOverrideController>
      overrideController = ...` at namespace scope, a genuinely different
      cross-shared-library-boundary pattern than `g_pHyprRenderer`/
      `g_pCompositor`/etc. Confirmed via a real build + `nm -D`: the
      global itself shows as a defined weak (`V`) symbol in `HyprLUI.so`
      (standard, correct behavior for an inline variable - every
      including translation unit gets its own instance, weak-linked so
      the loader coalesces them with whichever other definition is
      already present, e.g. Hyprland's own executable's), while
      `CShapeOverrideController::setOverride()`/`unsetOverride()`
      themselves show as undefined (`U`) - deferred to resolve against the
      host process at dlopen time, the exact same mechanism already
      proven throughout this project for every other internal API call.

      **Design decisions**:
      - `disabled` excludes a widget from `hitTest()` entirely (added to
        each of `CButtonWidget`/`CInputWidget`/`CCheckboxWidget`'s own
        override) - click-through/unfocusable, as if it isn't there for
        interaction purposes, while it still renders. A direct
        consequence: hover and disabled can never co-occur (a disabled
        widget can never resolve as the hovered one, since hit-testing
        already excludes it) - so `effectiveFillColor()`'s disabled-then-
        hover fallback has no real precedence ambiguity to resolve, it's
        just two sequential checks.
      - `hoverColor`/`disabledColor` are declarative, automatic
        alternate-color fields (same shape `Checkbox.checkedColor`,
        Phase 8, already established) rather than callback-only - the
        common case (a flat color swap) needs zero Lua round-trip.
        `onHoverStart`/`onHoverEnd` (mirroring `onFocus`/`onBlur`'s shape)
        remain as the escape hatch for anything beyond that, e.g.
        changing a SIBLING widget's appearance. This deliberately departs
        from Phase 6's "expose the hook, let Lua own all the cosmetics"
        stance for FOCUS - reasoned to be the right call specifically
        here since hover/disabled naturally reduce to "one alternate
        fill color" far more often than focus (which is more often a
        ring/border than a fill swap), and the callback escape hatch is
        still available for anyone who needs more.
      - Since `hoverColor` needs to apply automatically inside `render()`
        (no Lua round-trip), unlike Input's `focus()`/`blur()` (Phase 6),
        which fire a callback but store no state on the widget at all -
        `CWidget` gained an actual `bool m_hovered` flag this time,
        updated via `setHovered()` (called by `CUIManager`, mirrors how
        `input->focus()`/`blur()` are already invoked externally).
      - `onScroll` stays completely inert unless set - `InputHook.cpp`'s
        new `onMouseAxis()` only cancels `mouse.axis` when hit-testing
        lands on a widget whose `fireScroll()` actually invoked something
        (`CWidget::fireScroll()` reports whether a handler was set),
        matching Phase 6's keybind-priority "swallow only what's opted
        into" philosophy exactly. Scoped to the same interactive widgets
        reusing the EXISTING `hitTest()` infra as-is (deliberately not
        opened up to arbitrary widgets like `Box`/`Column`, which would
        need a wholly separate hit-testing concept - see Open questions).
      - `mouse.move` is never cancelled (purely observational) - swallowing
        it would block whatever real window is under a HyprLUI overlay
        from its own normal hover/motion feedback, an unwanted side
        effect nothing else in this toolkit does; only click and (opted-
        into) scroll are ever actually swallowed.

      New `CUIManager::m_hoveredWidget` (an `SWidgetHit`, same "compared
      by value, not a raw pointer" shape as `m_focusedInput` - there's
      only one real pointer, so only one widget can be hovered at a time)
      + `updateHover()`/`isHovered()`/`dispatchScroll()`. `focusWidget()`
      now also rejects a disabled `Input`. `removeCanvas()`/`clear()`
      extended to un-hover (fire `onHoverEnd` against a still-live widget)
      the same way they already blur, for the same "don't let Lua's own
      state silently go stale" reasoning documented for the config-reload
      lifecycle bug. `set_widget_visible()`/new `set_widget_disabled()`
      both un-hover (and, for the latter, blur) a widget that becomes
      hidden/disabled while currently focused/hovered.

      **Scope note, superseded within the same conversation**: this
      originally said `onScroll`/hover only worked on `Button`/`Input`/
      `Checkbox`, with opening it up to arbitrary widgets left as a future
      "would need a separate hit-testing concept" item. That turned out to
      be wrong almost immediately - see the `onClick` generalization
      below, which changes `CWidget`'s own default `hitTest()` to match
      any widget with `onClick` OR `onScroll` set. Since hover-tracking
      and scroll dispatch (`InputHook.cpp`) both go through that exact
      same `hitTestWidget()` lookup, a plain `Box`/`Text`/`Image`/`Row`/
      `Column`/`Stack` with `onScroll` (or `onClick`) set is now hoverable
      and scrollable too, no separate concept needed after all - it was
      already the same mechanism, just not extended to check `onScroll`
      yet. `hoverColor`/`disabledColor`/`onHoverStart`/`onHoverEnd` all
      work on such a widget the same way they do on Button/Input/Checkbox.

      **`onClick` generalized from Button-only to a `CWidget` base field,
      in a same-session follow-up prompted by the user noticing the
      inconsistency directly**: `onHoverStart`/`onHoverEnd`/`onScroll`
      were already parsed generically for every widget type (Phase 10, per
      the whole section above) but silently inert on anything that wasn't
      already a `Button`/`Input`/`Checkbox`, since only those three
      overrode `hitTest()` to ever match at all - meanwhile `onClick`
      itself was still Button-specific (its own private field on
      `CButtonWidget`, parsed only in `buildWidget()`'s "button" branch).
      Fixed by moving `onClick` onto `CWidget` itself (`setOnClick()`/
      `fireClick()`, mirroring `fireScroll()`'s "report whether a handler
      actually fired" shape) and changing `CWidget`'s DEFAULT `hitTest()`
      (previously: never matches, pure pass-through) to check children
      first (unchanged - an interactive descendant still wins over an
      ancestor that's ALSO clickable), then fall back to matching itself
      if `(onClick || onScroll)` is set and the widget isn't disabled.
      `isInteractive()`'s default was updated to match (`onClick ||
      onScroll`, rather than always `false`) so the debug overlay's hit-
      target highlight stays accurate for a widget that became interactive
      this way. `CButtonWidget`/`CInputWidget`/`CCheckboxWidget` keep their
      own unconditional-leaf-match `hitTest()` overrides unchanged (Button
      is a real click target even with no `onClick` set at all - that
      "always structurally clickable" contract predates this change and
      wasn't worth disturbing) - `CButtonWidget` just stopped having its
      OWN separate `m_onClick`/`setOnClick()`/`click()`, relying entirely
      on the inherited generic version instead (name-hiding wasn't a
      concern once the duplicate was removed, since `buildWidget()`'s
      common tail calls `setOnClick()` through a `PWidget` = `shared_ptr
      <CWidget>`-typed variable, which always resolves to the base
      class's non-virtual method regardless of the pointee's dynamic
      type - had `CButtonWidget` kept its OWN `setOnClick()` alongside
      this, that method would have silently SHADOWED the base one for any
      direct `CButtonWidget*`-typed call, while the common tail's
      `PWidget`-typed call would still have hit the base version instead -
      two competing onClick storages on the same object, only one of
      which `CUIManager::clickWidget()` would ever actually have checked;
      removing the duplicate rather than adding a second competing field
      sidesteps that trap entirely). `CUIManager::clickWidget()` keeps its
      own `dynamic_cast<CCheckboxWidget*>` branch (toggle + `onChange
      (bool)` is a different shape from a plain no-arg `onClick`, so
      Checkbox intentionally does NOT use the generic mechanism) but its
      former `CButtonWidget` branch was removed entirely - Button now
      falls through to the same generic `widget->fireClick()` every other
      widget type uses.
- [x] **Phase 11** - Persistence: `hyprlui.persistent(key, default)`,
      backed by a native C++ store that survives a Lua config reload -
      unlike an ordinary `local`, which resets every time, since the
      WHOLE config script re-runs on every reload. Split out from the
      original combined "Phase 11" (persistence + native services) once
      it became clear the two are genuinely independent pieces of work
      that don't need to land together.

      **A wrong initial assumption, caught by verifying against Hyprland's
      source before implementing** (same practice as every internal-API-
      reliant phase - this one paid off immediately): the first mental
      model was "the same `lua_State` persists across a reload, so
      `persistent()` could just keep a `LUA_REGISTRYINDEX` ref into it -
      no native re-encoding needed, any Lua value type for free." Reading
      `CConfigManager::reload()` disproved this directly:
      `reinitLuaState()` runs unconditionally on EVERY reload and does
      `lua_close(m_lua)` then `m_lua = luaL_newstate()` - the entire
      interpreter, registry included, is destroyed and a genuinely fresh
      one created each time. There is no "the persistent state" to hold a
      ref into across that boundary. This also exposed an EXISTING, wrong
      doc comment in `Watcher.hpp` (from Phase 3) claiming exactly the
      disproven assumption - corrected in place: `CWatcherManager` was
      never actually relying on state surviving a reload, it just happens
      to be safe regardless, because `clear()` releases every Lua ref it
      holds on `config.preReload`, which fires strictly before the old
      state is destroyed - so nothing there was ever at risk of
      dereferencing a dangling ref into a freed interpreter, for a
      different reason than the comment gave.

      **Scope decisions, all asked explicitly given how much the above
      changed the actual implementation cost**:
      - Durability: survives a config reload (the plugin process keeps
        running), NOT a full plugin unload or Hyprland restart - a pure
        in-memory store (`std::unordered_map<std::string,
        PersistentValue>`), no disk I/O, no file-location/serialization-
        format questions to answer.
      - Value types: `PersistentValue = std::variant<double, std::string,
        bool>` - scalars only, no tables/functions. Covers the realistic
        use case (a volume level, a theme name, a toggle) without a
        recursive Lua<->native conversion layer.
      - Wrapper API: explicit `:get()`/`:set(value)` methods, not a
        mutable `.value` field - consistent with this project's
        repeatedly-stated preference for explicit calls over
        `__index`/`__newindex` metatable magic (Phase 3's reactivity,
        Phase 9's components both made the same call).
      - Type mismatch on re-declaration (calling `persistent(key,
        default)` again for an existing `key` whose stored value's type
        differs from this call's `default`): logs a warning (`Log::WARN`,
        not `luaL_error`) but stays permissive either way - always
        returns whatever's actually stored, `default` is only ever
        consulted the very first time nothing was stored yet.

      New `src/persistence/PersistenceStore.hpp/.cpp` (`CPersistenceStore`
      singleton, first widget-unrelated top-level subsystem directory,
      alongside `src/reactive/`/`src/reserved/`/`src/input/`/`src/render/`/
      `src/ui/`) - `getOrInit()` (used by `persistent()` itself: seeds or
      warns-and-returns-existing), `getRaw()` (used by the wrapper's
      `:get()` - a plain lookup, no default/warning logic, since
      `getOrInit()` already handled that once at `persistent()`-call
      time), `set()` (used by `:set()` - overwrites unconditionally,
      including a type change, on the theory that an explicit `:set()`
      call is always deliberate, unlike a `default` argument that might
      just be stale). **`clear()` is the one piece of state in this
      entire codebase that must NEVER be wired into `resetAllState()`/
      `config.preReload`** - every other manager's whole reason for
      having a `clear()` is to get wiped exactly there; this one's whole
      reason to exist is to survive it. Called only from `PLUGIN_EXIT`, a
      real unload - flagged explicitly in the header comment as a trap
      not to "fix" by matching the other managers' pattern.

      Lua-side implementation detail: the wrapper table's `get`/`set`
      fields are C closures created via `lua_pushcclosure()` with the
      `key` string as their one upvalue (`lua_upvalueindex(1)`) - `:`
      method-call sugar (`store:get()`) passes `store` itself as the
      closure's first ARGUMENT, which both closures simply ignore, since
      the actual key lookup comes from the upvalue, not any argument.
      Reading a number/string/boolean argument uses `lua_type()` (the
      exact type tag), deliberately NOT `lua_isnumber()`/`lua_isstring()`
      - those two are coercion-aware in the Lua C API (a numeric-looking
      string like `"123"` satisfies `lua_isnumber()` too), which would
      have silently misclassified a string `default`/`:set()` value as a
      number.
- [x] **Phase 12** - Native services layer. Split out from the same
      original "Phase 11" as its own phase, for the same reason. Exposes
      exactly two generic Lua primitives: run a command, and open a raw
      socket. Every higher-level integration - D-Bus, JSON parsing, any
      specific protocol - gets built in pure Lua on top of those two
      primitives rather than natively in C++, keeping the native surface
      area deliberately small. A second, separate community repository is
      also planned to host shareable Lua-built widgets/integrations, kept
      apart from core on purpose to avoid the maintenance burden seen in
      projects like Waybar.
    - `hyprlui.run_cmd(cmd, callback)` - runs `cmd` via `/bin/sh -c` (same
      shell-string convention as `hl.exec_cmd`, decided explicitly rather
      than an argv array), asynchronously, one-shot only - no
      streaming/repeat (decided explicitly; a caller wanting polling just
      calls `run_cmd` again from a timer/watcher). `callback(output)`
      fires exactly once with everything the command printed to stdout,
      once its stdout closes.
    - No exit code is reported, discovered rather than assumed: Hyprland's
      own `main.cpp` (`reapZombieChildrenAutomatically()`) sets
      `SA_NOCLDWAIT` on `SIGCHLD` globally at startup, so the kernel
      auto-reaps every child process - including ones this plugin spawns -
      with no zombie ever appearing and no `waitpid()` ever needed. That's
      good news for the "don't risk blocking the compositor reaping a
      child" question this was originally checked for, but it also means
      a `waitpid()` call to retrieve the child's exit status would
      unconditionally fail with `ECHILD` (the kernel already reaped it) -
      there is no way to obtain an exit code at all under this global
      setting, so `run_cmd`'s callback doesn't try to offer one. On a
      spawn/pipe failure, `callback("")` still fires (logged as a
      `Log::WARN`, not a `luaL_error` - an environmental failure, not a
      config-authoring mistake) - the callback is guaranteed to fire
      exactly once either way, with nothing else to check.
    - `hyprlui.open_socket(path, callback)` - connects a Unix domain
      socket to `path` (Unix domain only, decided explicitly - no TCP/UDP,
      not needed for any realistic desktop-integration target: PipeWire,
      most D-Bus session buses, Hyprland's own IPC sockets, etc. are all
      Unix domain). `callback(sock)` fires once connected, or
      `callback(nil)` on a connect failure (logged as a warning; unlike
      `run_cmd`, "did this even connect" is a load-bearing distinction a
      caller must be able to check, so this can't just paper over it with
      an empty placeholder the way `run_cmd` does). `connect()` itself is
      a blocking call here, deliberately - for a *local* Unix domain
      socket (no DNS, no network round-trip) this is realistically
      instant, unlike a network `connect()` (which Phase 12 already
      excludes). All ongoing read/write after that point is non-blocking.
    - `sock:read(callback)` - fires `callback(data)` with exactly one
      `read()` call's worth of data (decided explicitly - no internal
      draining/batching across multiple reads, matching the "expose the
      raw primitive" stance elsewhere in this API), or `callback(nil)`
      once the peer closes the connection (after which the socket is torn
      down; a stray `:read()` after that point still fires `callback(nil)`
      immediately rather than erroring - a benign, non-config-authoring
      race, not something worth crashing a script over). Only one pending
      `:read()` at a time per socket - a second call before the first
      resolves replaces it (releases the old callback ref), matching
      "last call wins" rather than an internal queue.
    - `sock:write(data)` - a single best-effort `write()` call, no
      partial-write retry/buffering. `sock:close()` - closes the
      connection early; safe to call more than once.
    - Async I/O model - **a real correctness finding, not a style choice**:
      the obvious primitive for "call me when this fd is readable" is
      `CEventLoopManager::doOnReadable()` (already the established
      internal-API-reliance pattern, see Watcher.hpp/.cpp), but reading
      its Wayland-side handler (`handleWaiterFD()` in
      `EventLoopManager.cpp`) before using it turned up a real gap: the
      handler checks `mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)` FIRST,
      and if either is set, drops the waiter via `onFdReadableFail()`
      WITHOUT ever invoking the registered callback at all. On Linux, a
      pipe or a stream socket whose peer has closed commonly reports
      `HANGUP` together with `READABLE` in the exact same readiness
      notification (the "there's trailing data AND the writer is already
      gone" case) - meaning `doOnReadable`'s callback can simply never
      fire for the exact moment a command finishes or a peer disconnects,
      which is the single most important moment for both of this phase's
      primitives. Rather than build on an internal API with a confirmed
      gap for precisely the case this code needs, both `run_cmd` and
      `sock:read()` instead poll on a short repeating `CEventLoopTimer`
      (the same primitive Watcher.cpp already uses for interval-based
      watchers, at a 16ms interval) and drive reads via a plain
      non-blocking `read()` - `EAGAIN` means keep polling, `0` means EOF,
      a positive count is data, any other `errno` is a real error. Trades
      a small amount of latency (at most one poll interval) for actually
      being correct on the close/EOF case.
    - Also discovered while implementing this:
      `Hyprutils::OS::CFileDescriptor::getFlags()`/`setFlags()` are
      `F_GETFD`/`F_SETFD` (the close-on-exec fd flag) - **not**
      `F_GETFL`/`F_SETFL` (the file status flags `O_NONBLOCK` actually
      lives under). Setting a pipe/socket fd non-blocking goes through a
      raw `fcntl()` call directly, not through `CFileDescriptor`'s own
      flag methods, which would silently do the wrong thing here.
    - Lifecycle: unlike Phase 11's `CPersistenceStore` (which deliberately
      does NOT clear on a config reload), `CNativeServices::clear()` IS
      wired into `resetAllState()` (`config.preReload`) as well as
      `PLUGIN_EXIT` - these are ephemeral, script-scoped resources, the
      opposite lifecycle from persistence. `clear()` cancels every
      in-flight poll timer, releases every Lua callback ref, best-effort
      `SIGTERM`s any still-running command's process (again, no `waitpid`
      needed or possible - see above), and closes every open socket fd.
    - Both running commands and open sockets are stored keyed by a stable
      integer id in an `unordered_map`, looked up fresh by id on every
      timer tick, rather than a raw pointer captured directly into the
      timer's closure. Reason: a cancelled `CEventLoopTimer` may still be
      referenced by `CEventLoopManager`'s own internal timer list for a
      little while after `cancel()` returns (shared ownership via `SP<>`),
      so a closure that captured a raw pointer into a command/socket
      struct already destroyed by `clear()` in the meantime would be a
      dangling-pointer call if the manager ever invoked it again before
      actually purging it. An id lookup that just returns early on a miss
      is the same defense Watcher.cpp's own timers already use (by
      watcher name, not a pointer) - this project's established pattern
      for the same hazard, not a new one invented here.
- [x] **Phase 13** (stretch) - Fade animations via Hyprland's animation
      manager. Kept last on purpose - animation polish makes the most
      sense once the widgets it'd animate (and the state that drives
      them, Phases 11/12) already exist.
    - **Scoped down from the original two-part description**: the
      "metatable-based auto-tracking reactivity underneath `Bind()`" half
      was dropped after an explicit discussion - every prior phase (3, 9,
      11) deliberately chose explicit calls over `__index`/`__newindex`
      metatable magic, and auto-tracking would have been a one-off reversal
      of that consistently-applied principle for no pressing need (the
      explicit `watch()`/`Bind()` surface already works). Phase 13 is
      animations-only.
    - Researched Hyprland's OWN animation tree first (`src/config/shared/
      animation/AnimationTree.cpp`) specifically because the user wanted to
      mirror its "element + action" shape (`windowsIn`/`windowsOut`/
      `windowsMove`, `fadeIn`/`fadeOut`, etc., all descending from one
      `global` root, each leaf inheriting its parent's speed/bezier unless
      overridden via `hl.animation({leaf=..., ...})`). **Key finding that
      changed the plan**: `Config::CAnimationTreeController` exposes no
      public way to register a NEW leaf node from outside -
      `Hyprutils::Animation::CAnimationConfigTree::createNode()` is a
      private member (`CAnimationTreeController::reset()` hardcodes the
      one fixed tree at startup), and `hl.animation()` itself errors with
      "no such animation leaf" for any name that isn't already one of
      those. So HyprLUI's widget fades can NOT be configured via the
      user's existing `hl.animation({leaf="hyprluiIn", ...})` the way that
      would have been most idiomatic - confirmed dead end before writing
      any code, not a runtime surprise.
    - What IS reusable, and independently confirmed working: the bezier-
      curve registry (`Animation::mgr()->bezierExists()/getBezier()` - a
      flat name->curve map, entirely separate machinery from the tree, so
      a user's own `hl.curve()`-defined curves are referenceable by name
      from HyprLUI too) and the animated-variable ticking/interpolation
      machinery itself. Confirmed by reading
      `CHyprAnimationManager::tick()`/`handleUpdate()`: a
      `CGenericAnimatedVariable` whose `SAnimationContext` has no window/
      workspace/layer set still gets ticked and interpolated correctly
      every frame - the `if/else if` chain in `handleUpdate()` only
      special-cases those three owner types for an early-return (skip if
      the owning monitor vanished) or a per-window `noAnim` rule check;
      with none of them set it just falls through to the normal
      interpolation step. It only skips Hyprland's own per-owner damage
      bookkeeping, which HyprLUI doesn't want anyway (`CUIManager::
      damageAll()` already does this, same as `Watcher.cpp`'s `notify()`).
    - So `CWidgetAnimations` (`Widget.hpp`) is a small, self-contained
      config store instead of a tree-node registration - exactly 2 shared
      slots, `IN`/`OUT` (mirroring Hyprland's own windows/fade in-vs-out
      split, move explicitly deferred - see below), each holding one
      `SP<SAnimationPropertyConfig>` created ONCE and mutated in place on
      every later `configure()` call, never replaced. This mutate-not-
      replace rule matters because widgets' `CAnimatedVariable`s only hold
      a WEAK reference to it (`CBaseAnimatedVariable::setConfig()`) -
      swapping in a new object would leave already-animating widgets
      pointing at a stale one. Confirmed correct by reading
      `CBaseAnimatedVariable::getPercent()/enabled()/getBezierName()`,
      which all dereference `m_pConfig->pValues->X` fresh on every single
      call, never caching - matches exactly how Hyprland's own
      `CAnimationConfigTree::setConfigForNode()` behaves (mutates a node's
      existing config in place).
    - `hyprlui.animation({leaf="in"|"out", enabled=true, speed, bezier})` -
      deliberately shaped like `hl.animation()`'s own table call (same
      field names, same `speed` unit - DECISECONDS, confirmed by reading
      `CBaseAnimatedVariable::getPercent()`'s
      `(DURATIONPASSED / 100.f) / internalSpeed` calculation) for
      familiarity, but scoped to exactly the two leaves HyprLUI supports -
      `leaf` outside `"in"`/`"out"` is a hard `luaL_error`, not silently
      accepted. Disabled (the default, until this is ever called) means
      `setVisible()` stays exactly as instant as it always was - purely
      opt-in, zero behavior change to any existing script that never calls
      it.
    - `CWidget::setVisible(bool)` (`Widget.hpp`) now branches on whether
      the relevant leaf is enabled. Disabled: unchanged instant path
      (also drops any in-flight `m_visibilityAnim`, so toggling the
      feature off mid-fade snaps cleanly rather than leaving a stuck
      partial-opacity state). Enabled: lazily creates a
      `PHLANIMVAR<float> m_visibilityAnim` the first time it's actually
      needed (most widgets never touch this, so it's a single null
      pointer's worth of overhead otherwise) via `Animation::mgr()->
      createAnimation()`, then animates it toward 1.0 (showing) or 0.0
      (hiding). Showing flips `m_visible = true` IMMEDIATELY (so the
      widget still participates in layout/hit-testing from frame 1, same
      as the instant path always did) and only the opacity ramps up;
      hiding keeps `m_visible` true and rendering until the fade-out
      animation's end callback actually flips it false - otherwise the
      widget would vanish from the tree (and render()'s `if (!m_visible)
      return`) before ever visibly fading. That end callback re-checks
      `m_visibilityAnim->goal() == 0.0f` before flipping `m_visible` -
      guards against a show() reversing the fade mid-flight (the SAME
      animated variable's goal gets reassigned back to 1.0, but the OLD
      "hide finished" callback registered by the earlier call would
      otherwise still fire once THAT new transition completes too,
      incorrectly re-hiding a widget that just finished fading back in).
    - `composedOpacity(parentOpacity)` - a new small `CWidget` helper
      multiplying `parentOpacity * m_opacity * (m_visibilityAnim ?
      m_visibilityAnim->value() : 1.0F)`. Every `render()` override across
      the codebase (the default container implementation plus
      `RectNode`/`TextNode`/`ButtonWidget`/`InputWidget`/
      `CheckboxWidget`/`ImageWidget`, 7 call sites total) now goes through
      this instead of inlining `parentOpacity * m_opacity` directly, so
      the fade applies uniformly without every leaf type needing its own
      awareness of `m_visibilityAnim`. A widget that's never been animated
      computes identically to before this phase.
    - **Verified the internal-API reliance resolves exactly like every
      other internal dependency this project already leans on**: `nm -D`
      on the built `.so` shows `Animation::mgr()` and every
      `CBaseAnimatedVariable`/`CAnimationManager` method as undefined
      (`U`), resolved against the host Hyprland process at `dlopen()`
      time, and `CHyprAnimationManager::createAnimation<float>` itself as
      a defined weak (`W`) template instantiation inside HyprLUI's own
      `.so` - same mechanism as `g_pEventLoopManager`/`CEventLoopTimer` in
      Watcher.cpp and NativeServices.cpp.
    - **Follow-up (same session): the fade also fires on widget/canvas
      CREATE and REMOVE, not just explicit `set_widget_visible()`/
      `set_canvas_visible()` toggles** - the user's own explicit ask.
      `CWidget::setVisible()`'s core logic (instant-vs-animate, plus the
      reversal-guarded hide-then-flip-invisible callback) was factored out
      into a free function, `applyAnimatedVisibility()` (Widget.hpp),
      specifically so `CCanvas::setVisible()` could reuse the exact same
      logic rather than duplicating it - both a single widget and a whole
      canvas fade the same way.
        - `hyprlui.window()` (canvas creation): calls a new
          `CCanvas::fadeInOnCreate()` right after `setRoot()` - seeds the
          canvas's own animated opacity at 0 and animates it to 1 if "in"
          is enabled (a no-op otherwise). `CCanvas::render()` now passes
          this value as `m_root->render()`'s `parentOpacity` argument
          (previously always an implicit 1.0F), so it cascades down to
          every widget in the newly-created tree via their own
          `composedOpacity()` - the WHOLE window fades in together, not
          each widget separately, since there's no existing primitive to
          add a single widget to an already-live canvas (every widget in
          a window is always created together, at `window()` time).
        - `hyprlui.remove_widget()`: previously called `removeChild(id)`
          immediately. Now calls a new `CWidget::fadeOutThenRemove(onDone)`
          first - fades the target widget out (if "out" is enabled) and
          only calls `onDone` (which does the actual `removeChild()` +
          `damage()`) once that finishes; immediate, same as before this
          follow-up, if "out" isn't enabled. The widget doesn't know its
          own parent, so it can't erase itself - `onDone` captures the
          owning `PCanvas` (shared ownership, so the canvas can't be
          destroyed out from under the deferred callback) and calls
          `removeChild()` on its root from there.
        - `CUIManager::removeCanvas()` needed NO changes at all - it
          already called `canvas->setVisible(false)` before pushing to
          `m_pendingRemoval`; making `CCanvas::setVisible()` itself
          animation-aware (via `applyAnimatedVisibility()`) was
          sufficient; the existing call site just started doing the right
          thing once the method underneath it changed.
        - **The one real design gap this follow-up surfaced**: a
          continuous multi-second fade changes rendered opacity every
          single frame, unlike every other mutation in this codebase
          (which changes state once, damages a few frames via
          `REDAMAGE_FRAMES`, and is done - see `CCanvas::damage()`'s own
          doc comment for why even a ONE-SHOT change needs that many
          frames, let alone a continuous animation). Without handling
          this, a fade would visually freeze after 4 frames even though
          the underlying value kept interpolating. Fixed with a new
          `CWidget::isAnimating()` (recursive: true if this widget's own
          `m_visibilityAnim` or any descendant's is
          `CBaseAnimatedVariable::isBeingAnimated()`) - `CCanvas::render()`
          calls `damage()` every frame while either its own fade or
          `m_root->isAnimating()` is true, continuously re-arming the
          redamage countdown for as long as the fade actually takes. This
          also happens to be exactly what makes `removeCanvas()`'s
          existing `m_pendingRemoval` sweep (which drops a canvas once
          `!hasPendingRedamage()`) naturally keep a fading-out removed
          canvas alive for its whole fade with no changes to that sweep's
          own logic - `hasPendingRedamage()` just stays true for as long
          as something keeps calling `damage()`.
    - **Two more ghosting bugs found live while actually testing the fade
      (same underlying bug CLASS as the original Phase-2 ghost-widget fix
      - under/mis-damaging, not the original's specific multi-frame-
      buffer timing cause), both fixed same-session:**
        1. **Debug overlay under-damage.** Several of its own labels (id,
           margin, size, z/opacity) are DELIBERATELY drawn just outside a
           widget's own box (`Widget.cpp`'s `drawDebugOverlay()`) - fine
           for an ordinary discrete mutation (the overflowing pixels just
           don't change between mutations, so stale-but-correct sitting
           there forever is invisible) but visibly ghosts once something
           (a fade) changes them every frame, since `CCanvas::damage()`
           only ever damaged the plain content `box()`. Fixed by having
           `drawDebugOverlay()`/`renderDebug()` return the actual union of
           every pixel they drew (most already had the position+size on
           hand, just weren't reporting it) - `CCanvas` tracks this as
           `m_debugOverflow` (recomputed once per `render()` call) and
           exposes `fullDamageBox()` (`box()` expanded by that overflow),
           which every damage call site now uses instead of bare `box()`.
        2. **Sub-pixel rounding under-damage at window edges** - reported
           live as "ghosting on the left edge of one window, the top edge
           of another," which is itself the tell: a FIXED off-by-one in
           our own math would hit every window's edges the same way;
           "depends on the window" points at each window's own specific
           fractional position instead. Root cause, found by reading
           Hyprland's own `IHyprRenderer::damageBox()`
           (`src/render/Renderer.cpp`): it does `box.copy().
           translate(-m->m_position).scale(m->m_scale).round()` -
           rounding the final SCALED box to integer device pixels.
           HyprLUI's own canvas positions are frequently fractional
           (anchor math like `(boxSize.x - m_size.x) / 2.0` for a
           "center" anchor), and rounding a fractional box can shrink it
           by up to ~1 device pixel on whichever side the fractional
           remainder happens to round away from - which side depends on
           that specific box's own fractional offset, matching the
           reported symptom exactly. The actual rendered content doesn't
           go through this same rounding (`CRectPassElement`/
           `CTexPassElement` position themselves via their own, separate
           math in `toMonitorLocal()`), so that sliver never gets
           re-painted. Fixed with a small fixed `EDGE_ROUNDING_PAD = 2.0`
           (logical pixels) added to every side of `fullDamageBox()`,
           unconditionally - deliberately not computed precisely per-
           monitor-scale (would need `currentMonitor()`, which isn't
           always valid where `damage()` gets called from, e.g. directly
           from LuaBridge.cpp outside any render pass) for a strip this
           thin; a few pixels of over-damage is cheap insurance, matching
           this codebase's existing `REDAMAGE_FRAMES=4` precedent of
           "generous bound over precise calculation."
        - Both fixes reinforce the same lesson the original Phase-2 bug
          already taught: this project's damage model is fundamentally
          "assume the runtime under/over-rounds or under-covers in ways
          you can't fully control from here, and budget a deliberate
          margin rather than chasing pixel-perfect precision."
    - **`move` is explicitly deferred, not forgotten.** There is currently
      no primitive to reposition an already-constructed widget at all
      (widgets are laid out once by their container at construction/
      re-render time) - animating a position change has nothing to hook
      into yet. Revisit once/if a position-mutation primitive exists.
    - **Follow-up (same session): per-widget override of the global
      in/out config**, per explicit request - the global
      `hyprlui.animation({leaf="in"|"out", ...})` config from earlier in
      this phase applies to every widget uniformly; some widgets may want
      a different speed/curve, or to opt out of a globally-enabled fade
      entirely.
        - `CWidget` gained `setFadeInOverride()`/`setFadeOutOverride()`
          (`SP<SAnimationPropertyConfig>`, null = "use the global config
          for this leaf" - the default, so nothing changes for a widget
          that doesn't set either). A widget's own `fadeIn`/`fadeOut`
          table field (`{ enabled?, speed?, bezier? }` - same shape as
          `hyprlui.animation()`'s own table, minus `leaf`) is parsed once
          at construction (`buildWidget()`'s common tail, `LuaBridge.cpp`)
          into a standalone config via a new `makeAnimationConfig()`
          helper (`Widget.hpp`) - built once, never mutated in place
          afterward, unlike the global slots (which DO get mutated live by
          further `hyprlui.animation()` calls) - there's no live-
          reconfigure API for one specific widget's own override.
        - The override REPLACES the global config for that widget/leaf
          entirely rather than merging with it - a widget setting
          `fadeIn = { speed = 5 }` does not inherit the global's bezier,
          it gets `"default"` unless it names its own. This also means a
          widget can force `fadeOut = { enabled = false }` to opt itself
          OUT of a globally-enabled fade (or the reverse: `fadeIn`
          enabled on one widget while the global "in" leaf stays off).
        - `applyAnimatedVisibility()` (the shared `CWidget`/`CCanvas`
          helper from earlier in this phase) was refactored to take an
          already-RESOLVED `enabled`/`config` pair instead of looking up
          `CWidgetAnimations::get()` itself - `CWidget::setVisible()`/
          `fadeOutThenRemove()` now resolve "this widget's own override if
          it has one, else the global config for this leaf" before
          calling it; `CCanvas` (which has no per-widget-style override
          concept - a whole window is one thing, not a tree of
          independently-overridable widgets) just passes the global
          config straight through, unchanged from before this follow-up.
    - **Second follow-up (same session), per explicit request**: (1) a
      window's open/close should be governed by the exact same mechanism
      as an explicit visibility toggle or `remove_widget()` - no separate
      "creation animation" concept; (2) the whole feature renamed away
      from "fade" - `fadeIn`/`fadeOut` → `animationIn`/`animationOut`
      (Lua fields), `setFadeInOverride`/`setFadeOutOverride` →
      `setAnimationInOverride`/`setAnimationOutOverride`,
      `fadeOutThenRemove` → `animateOutThenRemove`,
      `CCanvas::fadeInOnCreate()` removed entirely (see below) - opacity
      is the only thing actually animated today, but the "in"/"out"
      leaf/override mechanism itself is generic, so nothing in the public
      surface should imply it's opacity-only.
        - This actually simplified (1) automatically: `CCanvas` no longer
          has its own separate `m_visibilityAnim`/animated `setVisible()`
          at all - `CCanvas::setVisible()`/`visible()` now delegate
          entirely to the root widget's own `setVisible()`/`visible()`
          (falling back to a plain bool only in the - never actually
          observed - window between `createCanvas()` and `setRoot()`
          within a single `hyprlui.window()` call). A window fading in/out
          IS its root widget fading in/out, which already cascades to
          every descendant via `composedOpacity()`'s multiplicative
          chain - not a second, separate animated value layered on top.
          `CCanvas::render()` now passes a plain `1.0F` as
          `m_root->render()`'s `parentOpacity` (the root's own
          `composedOpacity()` already folds in its own animation
          progress) and gates on `m_root->visible()` instead of its own
          field; the continuous re-damage-while-animating check collapses
          to just `m_root->isAnimating()`.
        - `hyprlui.window()`'s creation path replaced
          `canvas->fadeInOnCreate()` with a new `CWidget::primeHidden()`
          (forces `m_visible = false` and drops any in-flight animation,
          with NO end-callback - a raw state reset, not an animated hide)
          immediately followed by `root->setVisible(true)` - the exact
          same call an explicit `set_widget_visible(window, id, true)`
          makes, just on the root widget specifically. `primeHidden()`
          exists only because `setVisible(true)` needs something to
          animate FROM; calling `setVisible(false)` there instead would be
          wrong whenever "out" also happens to be enabled (it would
          itself animate 1→0 instead of snapping, defeating the point).
        - `CUIManager::removeCanvas()` needed NO changes at all (again) -
          it already called `canvas->setVisible(false)`, which now
          transparently delegates to the root's own (possibly overridden)
          `setVisible(false)`.
        - Net effect: a root widget's own `animationIn`/`animationOut`
          override now ALSO governs its whole window's open/close, with
          no separate per-canvas override concept needed - exactly the
          "should not distinguish toggle vs. creation/removal" requirement
          this follow-up was about.

## Open questions

- **Widget composability (Phase 9) - unsolved.** Storing a constructed
  widget in a Lua variable and reusing that variable currently reuses the
  *same instance*, not a fresh tree per use - no component/template
  concept exists yet. See Phase 9 above for the likely direction (a plain
  Lua function returning a fresh tree per call) and what's still
  undecided (props/children passing, per-call `id` collisions,
  interaction with `Bind()`).
- ~~Exact flexbox subset for Phase 1~~ - resolved: `gap`, `padding`
  (uniform, not per-side), `align` (start/center/end only). No
  justify/space-between/wrap - can be added later without changing the
  `Row`/`Column` call shape if a real need shows up.
- Where `ReservedAreaComposer` should live structurally - own singleton
  vs. a responsibility of `UIManager`.
- Whether widget mutation handles should be real Lua userdata with
  methods (`label:set_text(...)`) vs. the current free-function-by-id
  style (`hyprlui.set_text(window, id, text)`) - ergonomics vs.
  implementation cost, still not decided. Phase 4's `Button.onClick`
  didn't force the question either way - it's set once, declaratively, at
  construction time (a widget-spec table field, same as `color` or
  `text`), not via a later mutation call, so free-function-by-id is still
  the only style anything actually needs so far. Would resurface if a
  future phase needs to *change* a callback after construction (e.g. a
  hypothetical `hyprlui.set_onclick(window, id, fn)`).
- ~~Padding is uniform-only right now (single number, all four sides)~~ -
  resolved by Phase 7: `padding` (and the new `margin`) now accept either a
  single uniform number or a per-side `{top, right, bottom, left}` table,
  same shorthand convention `color`'s number-or-table form already had.
- ~~Should an anchored window's monitor be resolved once at creation, or
  tracked live?~~ - resolved for Phase 2: once, at creation (explicit user
  call) - a HUD you already have open won't jump to a different screen
  just because you focused a window there. A live-follow-focus mode (an
  anchored window always tracking "whichever monitor is focused right
  now") is a legitimate alternative some users may want (e.g. a volume
  popup that should always show on the active screen) - if it comes up,
  it's a small, additive change: reuse the exact same `configString`
  resolution in `recomputeAnchorPosition()` instead of a cached name, gated
  behind something like `monitor = "focused-live"` so it doesn't change
  today's default behavior.
- `Bind()` only wired up for `Text.text` in Phase 3 - extending it to
  `color`/`visible`/numeric fields (`Box.color = Bind(...)`, etc.) reuses
  the exact same `fieldBindName()` + binding-closure mechanism per field,
  no `Bind()` syntax change needed. Not done yet since there was no
  concrete use case driving it.
- No per-watcher removal (`hyprlui.unwatch(name)`) - matches the original
  brainstormed sketch, which didn't call for one either. Watchers are
  expected to be small, persistent, config-lifetime things (a clock, a
  volume poller), not created/destroyed per-window. Non-breaking addition
  whenever a real need for it shows up.
- Watcher poll ticks are independent per watcher (each with its own
  `CEventLoopTimer`) rather than coalesced onto one shared timer - simpler,
  and fine at the handful-of-watchers scale this is meant for; revisit only
  if someone actually registers enough polling watchers for the per-timer
  overhead to matter.
- Phase 4 left several things deliberately out of v1 scope, all additive
  (none require an API-shape decision to add later):
  - ~~No hover state (`mouse.move` isn't hooked at all yet)~~ - resolved
    by Phase 10: `hoverColor`/`onHoverStart`/`onHoverEnd` plus automatic
    pointer-cursor feedback via `Pointer::Cursor::overrideController`,
    confirming this really was one feature, not two, as predicted here.
  - Right-click/middle-click pass through untouched even over a Button -
    left-click only. A future `onRightClick`/generic `onClick(button)`
    with the physical button code passed through is additive.
  - ~~No keyboard hook yet at all (Phase 6's territory)~~ - resolved by
    Phase 6, but only for the new `Input` widget - `Button` still has no
    concept of keyboard focus/activation via Enter/Space (it's not
    focusable at all). Additive if it comes up: `Button` could grow the
    same click-to-focus behavior `Input` has and treat Enter/Space as a
    synthetic click while focused.
  - "Only a Button's own bounds are clickable, not the whole window" was
    a deliberate v1 choice (see Phase 4's note above) - Phase 5's
    exclusive zones turned out to be about *layout* space, not *input*
    blocking, so this didn't get resolved by Phase 5 after all; still
    worth revisiting if a real "block everything under me" use case shows
    up.
- Phase 5 left one thing deliberately out of v1 scope:
  - ~~Exclusive windows don't avoid each other/Hyprland's own error
    overlay~~ - resolved (see Phase 5's note - `CCanvas::
    setExclusive(EEdge)` excludes only a window's own contribution from
    its own edge, reading everything else's live). What's genuinely still
    open: multiple HyprLUI exclusive windows on the *same* edge (e.g. two
    separate top bars) don't stack relative to *each other* - each still
    only excludes itself, so with two windows both reserving "top", both
    compute their position as if they were the only one there. A top bar
    plus a *different-edge* right dock is already fully correct today;
    it's specifically same-edge-multiple-HyprLUI-windows that needs real
    ordered-stacking logic (sort by creation order or an explicit
    priority, each computing its offset as the sum of same-edge
    contributions before it) to solve - a real feature, not a one-line
    fix, and no concrete use case has asked for it yet.
  - No "span the full monitor width/height" sizing option - a real bar
    almost always wants this, but nothing in the widget/window sizing
    model (fixed vs. size-to-content, per Phase 1) expresses "size to the
    monitor" at all. Would need either a new window-level size keyword
    (e.g. `w = "monitor"`) or exposing the target monitor's width so Lua
    can compute it itself - not decided, no immediate use case forced the
    choice yet.
- Phase 6 left several things deliberately out of v1 scope, all additive:
  - No modifier state passed to `onKey` - only `keysym` + `pressed`. Shift
    is already baked into the keysym by xkbcommon (Shift+A's keysym
    differs from A's), but Ctrl/Alt/Super held-state isn't exposed at
    all, so an `onKey` handler can't currently distinguish plain `c` from
    Ctrl+`c`. Hyprland's own `InputManager` already computes a modifier
    mask (`getHyprlandModState()`/`Input::ModifierMask`) for its own
    keybind resolution - forwarding that alongside keysym/pressed is a
    small, additive change whenever a real use case needs it.
  - No built-in focus-ring/visual "this is focused" affordance - left
    entirely to Lua via `onFocus`/`onBlur` (e.g. swap the Input's own
    color, or a sibling widget's), consistent with this toolkit's general
    "expose the hook, let Lua own the cosmetics" pattern.
  - No Tab (or any other) key to cycle focus between multiple Inputs -
    only click-to-focus and the explicit `focus_widget()`/`blur_widget()`
    calls. A caller wanting Tab-cycling can already build it today by
    listening for the Tab keysym in whichever Input currently has focus
    (reaches `onKey` like any other non-keybind key, no flag needed - see
    Phase 6's keybind-priority note) and calling `focus_widget()` on the
    next one - no new primitive needed, just not automatic.
  - `Button` still isn't keyboard-focusable/activatable (Enter/Space) -
    see the amended note under Phase 4's list above.
- Phase 7 left a few things deliberately out of v1 scope, all additive:
  - `padding`/`margin` are only interpreted by `CFlexWidget` (and, for
    padding, `CInputWidget`'s own label) - `CStackWidget`'s manual/absolute
    positioning ignores both by design (see Phase 7's own note above).
    Worth revisiting only if a real use case wants "manual position PLUS
    automatic inset/spacing" at the same time, which nothing has asked for
    yet.
  - Only `Text`'s overflow behavior was actually decided/implemented
    (truncate-with-ellipsis, via `maxW` forwarded to Hyprland's own Pango
    text renderer) - wrap and hard-clip modes were never wired up, since a
    single default was enough once chosen. Adding a selectable `overflow =
    "wrap"|"truncate"|"clip"` field later wouldn't need `maxW`/`minW`'s own
    shape to change.
  - No `set_widget_opacity`/`set_widget_z_index`/etc. runtime mutators -
    unlike `visible` (which grew `set_widget_visible` this same phase,
    since "toggle without recreating" was explicitly the ask), opacity/
    z-index/padding/margin/min-max are currently construction-time-only
    (set once when the widget is built, no way to change them afterwards
    short of `remove_widget()` + rebuilding). Additive whenever a real
    animation/interaction use case needs one - Phase 10's hover/focus/
    disabled state machine is a likely first real driver for this.
  - Opacity has no interaction with hit-testing - a fully-transparent
    (`opacity = 0`) widget is still clickable/focusable, matching how CSS
    itself treats opacity vs. `pointer-events: none` (two independent
    concepts). Not treated as a bug; a `pointerEvents`-style opt-out is a
    separate, additive feature if a real need for "invisible AND
    unclickable" shows up.
- **Debug overlay (post-Phase-7) - flagged for a recheck, not yet done**:
  user reported the auto-show size threshold (`AUTO_MIN_W`/`AUTO_MIN_H`,
  `Widget.cpp`) was too small in practice - widened once, `48` to `96`
  (width only; height left at `16`, not reported as a problem yet). Only
  tuned by eyeball off one report so far, not verified against a real
  variety of widget sizes/label combinations - revisit this whole area
  (including whether height also needs widening, and whether a single
  flat width threshold is even the right model vs. something that scales
  with the actual label text being measured) next time debug mode gets
  real use.
- **Config-reload state handling is a mitigation, not a real design** (see
  the "General plugin-lifecycle bug" note above) - "close absolutely
  everything before every reload" is correct (no more silent orphans) but
  cruder than it needs to be. What a better version would need to
  account for, none of it decided yet:
  - Right now there's no distinction between UI a user *toggled open* at
    runtime (arguably fine to lose on an unrelated reload - it was
    ephemeral anyway) and UI meant to be *always present* (a status bar
    declared unconditionally at the top of the script). The latter
    already survives correctly today only because it gets unconditionally
    recreated on every script run anyway - so the blunt "clear everything"
    approach happens to be harmless for that case in practice, but that's
    incidental, not something the design actually distinguishes.
  - No way for Lua to know what got torn down, or to react to it - e.g. a
    config author can't currently ask "was anything closed by this
    reload?" or get a callback to redo their own bookkeeping instead of
    it just silently happening. `config.preReload` fires before the
    script re-runs, so by the time the script's own top-level code runs
    again it has no way to inspect what state existed a moment ago.
  - A fundamentally different alternative worth weighing: instead of
    wiping and letting the fresh script recreate things, make `window{}`/
    `watch()` *reconcile* with what already exists under that name (e.g.
    update-in-place instead of erroring "already exists") - closer to how
    a declarative UI framework's reconciliation usually works, but a much
    bigger design change than the current "erase, matches how Hyprland's
    binds/rules already behave" approach, and conflicts with this
    project's stated non-goal of "no general-purpose diffing/
    reconciliation engine for tree updates" (see Non-goals) - would need
    that non-goal explicitly revisited, not just extended, if pursued.
