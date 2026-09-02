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
  mutation so Hyprland actually repaints affected regions.

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
- [ ] **Phase 2** - Window anchors + explicit/focused monitor targeting
      (replaces raw global `x`/`y`).
- [ ] **Phase 3** - Named watchers + `Bind()` reactivity (poll + explicit
      `notify()`).
- [ ] **Phase 4** - `Button` widget + pointer input hook (`InputHook`) +
      tree hit-testing.
- [ ] **Phase 5** - Exclusive zones / `ReservedAreaComposer`.
- [ ] **Phase 6** - `Input` widget + keyboard focus ownership (raw keysym
      only).
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
  implementation cost, not yet decided. Phase 1 kept the free-function
  style since it already existed; worth revisiting once `Button`
  (Phase 4) needs an `onClick` callback attached to a specific widget.
- Padding is uniform-only right now (single number, all four sides) -
  fine for Phase 1's demo-sized content; per-side padding
  (`{top=, right=, bottom=, left=}`) is a small, backwards-compatible
  addition whenever a real layout needs it.
