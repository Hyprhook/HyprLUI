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
- [x] **4a. Icon rendering path - file path / icon-theme name** - split
      into two halves after scoping with the user; this is the first.
      `notification-manager.lua`'s new `resolveIcon(appIcon, cb)`
      handles the two easy cases: `appIcon` already an absolute path
      (used directly, no lookup) and a bare freedesktop icon-theme name
      like `"firefox"`/`"dialog-warning"` (the common case for most
      native apps) - resolved by a plain recursive `find` (via
      `hl.plugin.hyprlui.run_cmd`, async/non-blocking, not a blocking
      `io.popen()`) across every installed icon theme dir plus
      `/usr/share/pixmaps`, first match wins. Deliberately NOT full XDG
      icon-theme-spec compliant - no `index.theme` parsing, no theme
      inheritance, no size/scale matching - just good enough to find
      most real icons for a demo. Lives entirely in demo-local Lua (the
      user's own call, over adding a core `hyprlui.resolve_icon()` API)
      - not reusable by other demos as-is, revisit if one needs it too.
      Results cached per `appIcon` name (`iconCache`) since most apps
      send many notifications with the same icon. `appIcon` comes off
      the session bus (any local process can call `Notify()`) and gets
      shell-interpolated into the `find` command, so it's checked
      against an allowlist pattern (`^[%w_.-]+$`, matching the
      freedesktop icon-naming convention) before ever reaching a shell -
      anything else is treated as "not found" rather than escaped.
      `addCard()` now takes an optional `iconPath` and shifts the text
      column over (`CONFIG.iconSize`/`iconGap`) when one resolved;
      omitted entirely (original layout) when not.
      - Icon didn't show up in the user's first live test - `app_icon`
        arrived empty. Confirmed via `dbus-monitor` that THIS system's
        `notify-send` (libnotify 0.8.8) doesn't populate the positional
        `app_icon` field at all for `-i NAME` - it puts `NAME` in
        `hints["image-path"]` instead (both are valid per the spec for a
        plain path/theme-name reference, unlike `image-data`, the raw-
        pixel-buffer one - #4b below). `handleNotify()` now tries
        `event.appIcon` first, falls back to `event.hints["image-path"]`.
        Not every sender necessarily behaves this way - if icons still
        don't show for some app, check with `dbus-monitor --session
        "interface='org.freedesktop.Notifications',member='Notify'"`
        which field it's actually using before assuming `resolveIcon()`
        itself is at fault.
- [x] **4b. Icon rendering path - raw pixel buffer** - initially
      deferred, then confirmed live-blocking (Vesktop/Discord sends
      icons ONLY this way, no `app_icon`/`image-path` at all - found via
      `dbus-monitor` while chasing why its icon still didn't show after
      4a). `hints["image-data"]` (also `icon_data`/`image_data` - older
      draft spellings) is a raw ARGB32 pixel buffer - a DBus STRUCT
      `(iiibiiay)` (width/height/rowstride/has_alpha/bits_per_sample/
      channels/raw bytes), not a scalar - two separate gaps, both now
      closed:
      - **Daemon** (`daemon/notification-daemon.lua`): `readHints()`
        only ever called `get_basic()` on hint values, which always
        fails for a struct (caught by its own `pcall`, hint silently
        dropped) - `decodeImageData()` (new) special-cases the
        image-data key(s), recursing into the struct's 6 scalar fields
        then byte-by-byte through its trailing `ay` (array of byte) -
        the ldbus binding has no bulk/lstring shortcut for that, only
        the standard iterate-with-get_basic()+next() protocol. Raw
        bytes are base64'd (`mime.b64()`, pulled in via the
        already-a-dependency luasocket) before going into this
        daemon's own hand-rolled `jsonEncode()` - it uses Lua's `%q`
        string quoting, not real JSON string escaping (`\ddd` decimal
        escapes for arbitrary binary bytes aren't valid JSON and would
        break `demos/jsondecode.lua`, a real parser, on the receiving
        end) - base64 is plain printable ASCII, safe either way.
      - **HyprLUI core** (`src/render/gfx.{hpp,cpp}`,
        `src/ui/ImageWidget.{hpp,cpp}`, `src/ui/WidgetBuilders.cpp`):
        new `gfx::makeImageTexture(width, height, rowstride, hasAlpha,
        channels, dataBase64)` overload (alongside the existing
        path-based one - a separate decode path, not a variant of it,
        since `Hyprgraphics::CImage` only understands ENCODED file
        formats, not raw pixel arrays) - base64-decodes (small
        table-lookup decoder, no existing one anywhere in
        Hyprland/hyprutils to reuse), then converts row-by-row
        (respecting the source `rowstride`, which may pad beyond
        `width*channels`) into what `IHyprRenderer::createTexture(int
        width, int height, unsigned char*)` actually expects - NOT the
        same as the cairo-surface overload `makeImageTexture(path)`
        uses. That raw-buffer overload uploads as `GL_RGBA` then
        swizzles R↔B on sample (matching `DRM_FORMAT_ARGB8888`'s
        in-memory BGRA byte order), tightly packed (no rowstride
        parameter at all), premultiplied - so this does the R,G,B[,A]
        (spec order, NOT premultiplied) → B,G,R,A-premultiplied
        conversion by hand per pixel. `CImageWidget` gets a second
        constructor overload (`SPixelSpec` instead of a path string) -
        a one-shot decode, no `setPixels()` counterpart to `setImage()`
        since nothing needs to re-set a pixel-buffer image after
        construction. `Image{}`'s Lua API gains a `pixels = {width,
        height, rowstride, hasAlpha, channels, dataBase64}` field as an
        ALTERNATIVE to `path` (exactly one of the two required, not
        both).
      - `notification-manager.lua`: `handleNotify()` now checks
        `hints["image-data"]` FIRST (its field names already match
        `Image{}`'s `pixels` shape directly, no remapping) before
        falling back to `resolveIcon()`'s path/theme-name handling -
        no filesystem lookup needed when the actual bytes are already
        in hand. `addCard()` passes through whichever of
        `iconPath`/`iconPixels` is set.
      - Only 8-bit-per-channel data is handled (`bits_per_sample`
        forwarded by the daemon but not read back by HyprLUI) - every
        real-world sender uses 8 bits in practice; not worth the extra
        conversion paths for a case nothing actually sends.

## Demo polish

- [x] Render the app icon - both halves done (#4a path/icon-theme-name,
      #4b raw pixel buffer).
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
