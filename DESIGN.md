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
> (new widget type, new field, new mutation call - Phases 3/4/6 all touch
> this), update `stubs/hyprlui.meta.lua` in the same change.** It's
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
- [ ] **Phase 7** (stretch) - Fade animations via Hyprland's animation
      manager; metatable-based auto-tracking reactivity underneath the
      existing `Bind()` surface.

## Open questions

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
- Padding is uniform-only right now (single number, all four sides) -
  fine for Phase 1's demo-sized content; per-side padding
  (`{top=, right=, bottom=, left=}`) is a small, backwards-compatible
  addition whenever a real layout needs it.
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
  - No hover state (`mouse.move` isn't hooked at all yet) - a `Button`
    can't currently change appearance on mouse-over, or expose an
    `onHover`. Would need its own damage-triggering considerations (hover
    changing something visual has to actually get repainted). **User-
    reported gap (live testing, post-Phase-4)**: there's currently zero
    cursor feedback on hover either - hovering a Button doesn't switch to
    a pointer/hand cursor, so nothing on screen signals "this is
    clickable" before you click it. Same `mouse.move` hook would need to
    drive this - hit-test on move, and if it's currently over a Button,
    set the cursor shape (Hyprland exposes cursor-shape control via its
    cursor manager - would need the equivalent research pass this project
    did for the monitor/timer/input APIs before implementing) and restore
    it when it moves off. Same hook, same hit-testing infra Phase 4
    already built - hover state and cursor feedback are really one
    feature, not two.
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
