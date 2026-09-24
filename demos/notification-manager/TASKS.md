# notification-manager demo - task list

Demo-specific polish, plus the HyprLUI core gaps some of it depends on.
Not formal DESIGN.md tasks (yet) - specific to this demo's own needs, not
general API design decided independently of it. See DESIGN.md for
HyprLUI's own active task list.

## HyprLUI core gaps

- [x] **1. Dynamic list mutation** - turned out mostly already there:
      `remove_widget(window, id)` already existed and already
      animates out (`animateOutThenRemove`) before erasing; `addChild()`/
      `addBinding()` were already public on `CWidget`/`CCanvas`. All that
      was actually missing was a Lua-callable way to build a widget and
      append it to an existing tree - added
      `hyprlui.add_widget(window, parentId, widgetSpec)` (LuaBridge.cpp),
      reusing `buildWidget()` unchanged (the same construction path
      `window{}`'s own root/children go through). Append-only for now -
      no insert-at-index/prepend, add if something needs it.
      `CWidget::collectIds()` (new) seeds the duplicate-id check against
      the WHOLE existing tree, not just the new subtree, so a colliding
      id fails loud the same way a static duplicate-id-in-one-`window{}`
      call already did.
      `notification-manager.lua` now creates the stack window once
      (empty) and adds/removes individual cards via
      `add_widget`/`remove_widget` instead of rebuilding the whole thing
      per notification - which, as a free side effect, is what makes
      `animationIn`/`animationOut` on each card actually fire (a widget
      that gets rebuilt fresh every frame never has a "just became
      visible" transition to animate; a persistent one does) - wired
      that in too. Reflow (siblings sliding into the gap a dismissed
      card leaves) is still instant - that's task #2 below, not this
      one.
- [x] **2. Reflow/position animation** - added `animationLayout`
      (`Widget.hpp`), same `{enabled?, speed?, bezier?, spring?, style?}`
      shape as `animationIn`/`animationOut` (per the user's own request -
      reuse `optAnimationOverrideField()` unchanged, `style` accepted but
      unused, there's no slide/popin transform to layer over a position
      change). A new `PHLANIMVAR<Vector2D> m_layoutAnim` animates toward
      `m_position` whenever `setPosition()` actually changes it (opt-in,
      instant/unchanged otherwise) and NOT on a widget's very first ever
      `setPosition()` call (`m_hasBeenPositioned` guards that - a brand-
      new widget must not "fly in" from `{0,0}`, that's `animationIn`'s
      job). Read back via a new `layoutOffset()`, added into the same
      `m_position + styleOffset()` expression `render()`/`hitTest()`
      already used at all 3 call sites - so an in-flight layout-reflow
      and an in-flight visibility-slide (`styleOffset()`, a genuinely
      different animation) can both be active on the same widget without
      conflicting.
      - Confirmed with the user: dismissing a card should reflow its
        siblings AT THE SAME TIME as its own fade-out, not after. That
        needed one more piece - a fading-out widget stays in
        `m_children` (and so still reserved its own flow space) until
        `remove_widget()`'s `animateOutThenRemove()` callback actually
        erases it once the fade finishes, meaning siblings previously
        waited for the whole fade before reflowing. Added
        `CWidget::isFadingOut()` (true while `m_visibilityAnim` is
        actively animating toward a goal of 0 - `m_visible` itself stays
        true for that whole stretch, so this is the only way to tell)
        and had `CFlexWidget::measureContent()`/`arrangeChildren()`
        (`ContainerWidget.cpp`) skip fading-out children entirely -
        stops reserving their flow space immediately, frozen at their
        last real position for the rest of the fade, instead of only
        once removeChild() actually runs.
      - `notification-manager.lua`'s cards now set `animationLayout`
        alongside their existing `animationIn`/`animationOut`.
      - Bug found live testing this: dismissing a card ghosted -
        `CFlexWidget::measureContent()` skipping a fading-out child
        shrinks the CONTAINER's own measured size (and so `CCanvas`'s
        `m_size`/`fullDamageBox()`) immediately, even though that child
        (and now-reflowing, still-sliding siblings) are still actually
        being drawn beyond that smaller box for the rest of the
        animation - the EXACT class of bug `m_debugOverflow` already
        existed to solve for the debug overlay, just not for real
        content. Fixed the same way: new `CWidget::renderedBounds()`
        (`Widget.cpp`, alongside `renderDebug()`) returns the union of
        every visible widget's CURRENT `boxAt()` (which already includes
        `layoutOffset()`) - i.e. what's actually on screen this frame,
        animation included, not the settled layout size. `CCanvas`
        compares that against `box()` into a new `m_renderOverflow`
        (mirrors `m_debugOverflow` exactly) and `fullDamageBox()` now
        pads by both.
      - notification bodies are one line, ellipsis/clip only.
- [ ] **4. Icon rendering path** - `Image` only takes a file `path` - no
      icon-theme name lookup, no raw pixel buffer (`hints.image-data`)
      support. The daemon forwards `appIcon` as a raw string today; the
      UI doesn't render it at all yet.

## Demo polish

- [ ] Render the app icon - blocked by #4 above.
- [ ] Smooth per-card fade-in/out and reflow on dismiss - blocked by #1/#2.
- [ ] Multi-line body text - blocked by #3 (or work around with marquee).
- [ ] Action buttons (`Notify`'s `actions` array - already forwarded by
      the daemon, just unused by the UI) - not blocked.
- [ ] Hover-to-pause the auto-dismiss timer (`onHoverStart`/`onHoverEnd`
      already exist) - not blocked.
- [ ] Overflow handling ("+N more" once the stack gets tall) - not
      blocked.
- [ ] Notification history / do-not-disturb toggle
      (`hyprlui.persistent()` already exists) - not blocked.
