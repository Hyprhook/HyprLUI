#pragma once
//
// UIManager.hpp
//
// Singleton owning every Canvas. This is the surface intended to be exposed
// to Lua: a Lua binding layer (see LuaBridge.hpp) builds a Widget tree
// (Stack/Row/Column/Text/Box - see ContainerWidget.hpp/RectNode.hpp/
// TextNode.hpp) and attaches it to a Canvas via these lifecycle calls. Keep
// this the single "public API" of the library and route everything else
// through it rather than poking Canvas/Widget directly from main.cpp.

#include "Canvas.hpp"
#include "Widget.hpp"

#include <hyprland/src/helpers/math/Math.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace HyprLUI {

    // Result of CUIManager::hitTestWidget() - `canvasName` empty means
    // nothing interactive was hit. Named (rather than an anonymous pair)
    // since it's meant to be re-hit-tested-against and compared by
    // InputHook.cpp across a press/release pair, and stored as
    // CUIManager's m_focusedInput - a plain pair<string,string> reads
    // ambiguously at those call sites. Covers both CButtonWidget and
    // CInputWidget hits (CWidget::hitTest() doesn't distinguish which
    // interactive type it found - the caller dynamic_casts to find out).
    struct SWidgetHit {
        std::string canvasName;
        std::string widgetId;

        bool        operator==(const SWidgetHit&) const = default;
        bool        empty() const {
            return canvasName.empty();
        }
    };

    class CUIManager {
      public:
        static CUIManager& get();

        // --- Canvas management ---------------------------------------
        PCanvas createCanvas(const std::string& name, const Vector2D& position, const Vector2D& size, EZOrder zorder = EZOrder::Overlay);

        // Removes a canvas from Lua's perspective immediately (the name is
        // free to reuse right away, hasCanvas()/getCanvas() stop seeing
        // it), but keeps the underlying CCanvas alive a few more frames in
        // m_pendingRemoval purely to finish clearing its old on-screen
        // footprint - see CCanvas::damage()'s doc comment for why a single
        // damage-then-destroy isn't enough on its own.
        void    removeCanvas(const std::string& name);
        bool    hasCanvas(const std::string& name) const;
        PCanvas getCanvas(const std::string& name) const;

        // Damages every canvas that currently exists. Blunt but correct:
        // called whenever a watcher's value actually changes (see
        // CWatcherManager::notify()), which has no idea which specific
        // canvases reference that watcher - matches the "don't build
        // fine-grained invalidation" precedent for this HUD-scale toolkit.
        void damageAll();

        // --- Input -------------------------------------------------------
        // Finds the topmost interactive (CButtonWidget, CInputWidget, or
        // CCheckboxWidget) hit at global point `pt`, searching only
        // Overlay-zorder, currently-visible canvases, newest-created
        // first. Background canvases are explicitly decorative/
        // occludable-by-real-windows (see EZOrder's doc comment) - not
        // click targets. Empty result (SWidgetHit::empty()) if nothing was
        // hit; used by InputHook.cpp for both the initial press hit-test
        // and the matching re-test at release.
        SWidgetHit hitTestWidget(const Vector2D& pt) const;

        // Invokes a real click on the widget named `widgetId` on canvas
        // `canvasName`, if both still exist and the widget actually
        // resolved as a hit in the first place (false, no-op, otherwise -
        // e.g. an Input, which grabs focus on PRESS instead, see
        // handlePressFocus(), never reaches here as a "click"). Checkbox
        // gets its own dynamic_cast branch (toggle then invoke
        // onChange(bool) with the new value - a different shape from a
        // plain onClick); every other widget type falls through to the
        // generic CWidget::fireClick() (Phase 10 follow-up, DESIGN.md) -
        // onClick is a base-CWidget field now, not Button-specific,
        // though only a widget that ever actually resolves as a
        // hitTestWidget() hit can reach this call in the first place (see
        // Widget.hpp's own hitTest()/isInteractive() defaults for what
        // makes a plain Box/Text/etc. become one: having onClick set at
        // all). Called by InputHook.cpp once a press and its matching
        // release both resolve to the same SWidgetHit.
        bool clickWidget(const std::string& canvasName, const std::string& widgetId);

        // --- Keyboard focus (Input widgets) ------------------------------
        // Exactly one Input across every HyprLUI window can hold HyprLUI's
        // own keyboard focus at a time (there's only one real keyboard),
        // tracked as m_focusedInput below - entirely separate from
        // Hyprland's actual Wayland keyboard-focus-surface concept, since
        // HyprLUI canvases aren't real surfaces (see DESIGN.md Phase 6).

        // Focuses the Input widget named `widgetId` on canvas
        // `canvasName`, blurring whatever was previously focused first
        // (a no-op re-blur/re-focus if it's already this exact widget -
        // doesn't re-fire onFocus). Returns false (no state change) if no
        // such canvas/widget exists, it isn't actually a CInputWidget, or
        // it's disabled (Phase 10 - a disabled Input can't be focused,
        // neither by click - hitTest() already excludes it, see
        // InputWidget.hpp - nor programmatically through this same call).
        // Called both from InputHook.cpp's click-to-focus handling and
        // directly from Lua (hyprlui.focus_widget) for programmatic focus.
        bool focusWidget(const std::string& canvasName, const std::string& widgetId);

        // Blurs whichever Input currently has focus, if any (no-op
        // otherwise). Called from Lua (hyprlui.blur_widget), from
        // InputHook.cpp on a click that lands elsewhere, and internally
        // whenever the focused widget/canvas is about to be destroyed or
        // hidden, so onBlur always fires before a widget disappears out
        // from under Lua's own idea of "what's focused" - see
        // DESIGN.md's config-reload lifecycle notes for the class of bug
        // this sidesteps.
        void blurFocusedInput();

        bool isFocused(const std::string& canvasName, const std::string& widgetId) const {
            return !m_focusedInput.empty() && m_focusedInput.canvasName == canvasName && m_focusedInput.widgetId == widgetId;
        }

        // Whether canvas `canvasName` is the one currently holding
        // HyprLUI's keyboard focus, regardless of which widget on it -
        // used by set_canvas_visible() to blur on hide without needing to
        // know the specific widget id.
        bool isCanvasFocused(const std::string& canvasName) const {
            return !m_focusedInput.empty() && m_focusedInput.canvasName == canvasName;
        }

        // Forwards a key event to the focused Input, if any (no-op,
        // returns false, if nothing is focused). Always swallows the
        // event when something is focused - InputHook.cpp already
        // filtered out anything that's actually a real Hyprland keybind
        // before ever calling this (see its doc comment), so by the time
        // a key reaches here it's guaranteed local to this Input: it
        // shouldn't leak through to whatever real window has actual
        // Wayland keyboard focus behind it either. Called once per key
        // event from InputHook.cpp.
        bool dispatchKey(uint32_t keysym, bool pressed);

        // Press-time focus transition for InputHook.cpp's click handling:
        // clicking the already-focused Input again is a no-op; clicking
        // anything else (a different Input, a Button, empty space) blurs
        // whatever was focused, then focuses `hit` if it resolves to an
        // Input (no-op focus attempt otherwise, e.g. a Button click just
        // blurs and stops there).
        void handlePressFocus(const SWidgetHit& hit);

        // --- Hover + scroll (Phase 10) -----------------------------------
        // A single global "currently hovered" slot, same "compared by
        // value, not a raw pointer" shape as m_focusedInput - there's only
        // one real pointer, so only one widget can be hovered at a time.
        // Called by InputHook.cpp's new mouse.move handler on every move,
        // with the result of the SAME hitTestWidget() click-hit-testing
        // already uses (a disabled widget's hitTest() already excludes it,
        // so it can never resolve as the hovered one either). No-op if
        // `hit` is already the currently-hovered widget; otherwise calls
        // setHovered(false) on the old one (if any) and setHovered(true)
        // on the new one (if any) - each of those fires that widget's own
        // onHoverStart/onHoverEnd as a side effect (see Widget.hpp).
        void updateHover(const SWidgetHit& hit);

        bool isHovered(const std::string& canvasName, const std::string& widgetId) const {
            return !m_hoveredWidget.empty() && m_hoveredWidget.canvasName == canvasName && m_hoveredWidget.widgetId == widgetId;
        }

        // Forwards a scroll event to the widget at `canvasName`/`widgetId`
        // (already resolved by InputHook.cpp's hitTestWidget() call) if it
        // has an onScroll handler set - returns whether it actually fired
        // one, which is what InputHook.cpp uses to decide whether to
        // cancel the underlying mouse.axis event (only ever swallowed if
        // something was actually listening - see CWidget::fireScroll()).
        bool dispatchScroll(const std::string& canvasName, const std::string& widgetId, double delta, bool vertical);

        // --- Frame lifecycle --------------------------------------------
        // Called from the "render" hook once per relevant render stage.
        void renderOverlay();
        void renderBackground();

        void clear();

      private:
        CUIManager()  = default;
        ~CUIManager() = default;

        std::unordered_map<std::string, PCanvas> m_canvases;
        std::vector<PCanvas>                     m_pendingRemoval;
        uint64_t                                 m_nextSequence = 0;
        SWidgetHit                               m_focusedInput;
        SWidgetHit                               m_hoveredWidget;
    };

} // namespace HyprLUI
