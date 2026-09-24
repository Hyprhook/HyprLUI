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
#include <optional>
#include <unordered_map>
#include <vector>

namespace HyprLUI {

    // Result of CUIManager::hitTestWidget() - `canvasName` empty means
    // nothing interactive was hit. Named (rather than an anonymous pair)
    // since it's re-hit-tested and compared by InputHook.cpp across a
    // press/release pair, and stored as CUIManager's m_focusedInput.
    // Covers both CButtonWidget and CInputWidget hits - CWidget::hitTest()
    // doesn't distinguish which interactive type it found, the caller
    // dynamic_casts to find out.
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

        // Removes a canvas from Lua's perspective immediately (name is free
        // to reuse right away, hasCanvas()/getCanvas() stop seeing it), but
        // keeps the underlying CCanvas alive a few more frames in
        // m_pendingRemoval to finish clearing its old on-screen footprint -
        // or, if a fade-out animation is enabled, for the whole fade
        // instead (still actually rendering, fading down).
        void    removeCanvas(const std::string& name);
        bool    hasCanvas(const std::string& name) const;
        PCanvas getCanvas(const std::string& name) const;

        // Damages every canvas that currently exists. Blunt but correct -
        // called whenever a watcher's value changes, which has no idea
        // which specific canvases reference that watcher.
        void damageAll();

        // --- Input -------------------------------------------------------
        // Finds the topmost interactive widget hit at global point `pt`,
        // searching only Overlay-zorder, currently-visible canvases,
        // newest-created first. Background canvases are decorative, not
        // click targets. Empty result if nothing was hit - used by
        // InputHook.cpp for both the initial press hit-test and the
        // matching re-test at release.
        SWidgetHit hitTestWidget(const Vector2D& pt) const;

        // Invokes a real click on the widget named `widgetId` on canvas
        // `canvasName`, if both still exist and the widget actually
        // resolved as a hit in the first place (false, no-op, otherwise).
        // Checkbox gets its own dynamic_cast branch (toggle then invoke
        // onChange(bool) - a different shape from a plain onClick, and
        // left-click only, unlike onClick below) - every other widget
        // type falls through to the generic CWidget::fireClick(button).
        // Called by InputHook.cpp once a press and its matching release
        // (same button) both resolve to the same SWidgetHit.
        bool clickWidget(const std::string& canvasName, const std::string& widgetId, EMouseButton button);

        // --- Keyboard focus (Input widgets) ------------------------------
        // Exactly one Input across every HyprLUI window can hold HyprLUI's
        // own keyboard focus at a time, tracked as m_focusedInput below -
        // entirely separate from Hyprland's actual Wayland keyboard-focus
        // concept, since HyprLUI canvases aren't real surfaces.

        // Focuses the Input widget named `widgetId` on canvas
        // `canvasName`, blurring whatever was previously focused first (a
        // no-op if it's already this exact widget). Returns false if no
        // such canvas/widget exists, it isn't a CInputWidget, or it's
        // disabled. Called both from InputHook.cpp's click-to-focus
        // handling and directly from Lua (hyprlui.focus_widget).
        bool focusWidget(const std::string& canvasName, const std::string& widgetId);

        // Blurs whichever Input currently has focus, if any. Called from
        // Lua (hyprlui.blur_widget), from InputHook.cpp on a click landing
        // elsewhere, and internally whenever the focused widget/canvas is
        // about to be destroyed or hidden, so onBlur always fires before a
        // widget disappears out from under Lua's own idea of "what's
        // focused."
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
        // returns false, if nothing is focused). Always swallows the event
        // when something is focused - InputHook.cpp already filtered out
        // anything that's a real Hyprland keybind before calling this, so
        // a key reaching here is guaranteed local to this Input. Called
        // once per key event from InputHook.cpp.
        bool dispatchKey(uint32_t keysym, bool pressed);

        // Press-time focus transition for InputHook.cpp's click handling:
        // clicking the already-focused Input again is a no-op; clicking
        // anything else blurs whatever was focused, then focuses `hit` if
        // it resolves to an Input.
        void handlePressFocus(const SWidgetHit& hit);

        // --- Hover + scroll -----------------------------------
        // A single global "currently hovered" slot, same shape as
        // m_focusedInput - only one widget can be hovered at a time.
        // Called by InputHook.cpp's mouse.move handler with the result of
        // the same hitTestWidget() click-hit-testing already uses. No-op
        // if `hit` is already the currently-hovered widget; otherwise
        // fires setHovered(false)/setHovered(true) on the old/new widget
        // (each fires that widget's own onHoverStart/onHoverEnd).
        void updateHover(const SWidgetHit& hit);

        bool isHovered(const std::string& canvasName, const std::string& widgetId) const {
            return !m_hoveredWidget.empty() && m_hoveredWidget.canvasName == canvasName && m_hoveredWidget.widgetId == widgetId;
        }

        // Forwards a scroll event to the widget at `canvasName`/`widgetId`
        // if it has an onScroll handler set - returns whether it actually
        // fired one, which InputHook.cpp uses to decide whether to cancel
        // the underlying mouse.axis event.
        bool dispatchScroll(const std::string& canvasName, const std::string& widgetId, double delta, bool vertical);

        // --- Frame lifecycle --------------------------------------------
        // Called from the "render" hook once per relevant render stage.
        void renderOverlay();
        void renderBackground();

        void clear();

        // --- Hot-reload visibility (window{hotReload=true}) -------------
        // Remembers whether a named window was visible right before the
        // last reload, so a fresh window() call can restore that (see
        // docs/api.md). Not part of clear() - only PLUGIN_EXIT clears it.
        std::optional<bool> hotReloadVisibility(const std::string& name) const;
        void                setHotReloadVisibility(const std::string& name, bool visible);
        void                clearHotReloadState();

      private:
        CUIManager()  = default;
        ~CUIManager() = default;

        std::unordered_map<std::string, PCanvas> m_canvases;
        std::vector<PCanvas>                     m_pendingRemoval;
        uint64_t                                 m_nextSequence = 0;
        SWidgetHit                               m_focusedInput;
        SWidgetHit                               m_hoveredWidget;
        std::unordered_map<std::string, bool>    m_hotReloadVisibility;
    };

} // namespace HyprLUI
