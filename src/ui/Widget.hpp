#pragma once
//
// Widget.hpp
//
// Base class for every element in the UI tree - both leaves (text, rects,
// later buttons/inputs) and containers (Stack/Row/Column). A Window/Canvas
// owns a single root Widget instead of a flat node list; layout runs as a
// two-pass measure() (bottom-up, natural sizes) then arrange() (top-down,
// final positions) walk before render().
//
// Extension point: to add a new leaf, subclass CWidget, implement render()
// and optionally measureContent(). To add a new container, additionally
// implement arrangeChildren() (see ContainerWidget.hpp for Stack/Row/Column).

#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace HyprLUI {

    // Phase 13 (DESIGN.md): shared duration/bezier config for widget
    // visibility fades (hyprlui.animation({leaf="in"|"out", ...}), see
    // LuaBridge.cpp's luaAnimation()) - mirrors Hyprland's own
    // windowsIn/windowsOut split, but is NOT hooked into Hyprland's real
    // animation tree: verified (Config::AnimationTree.hpp) that
    // CAnimationTreeController exposes no way to register a new leaf node
    // from outside - `reset()` hardcodes the fixed tree once at startup,
    // and hl.animation() itself errors on any name that isn't already one
    // of those. So this is a small self-contained config store instead,
    // reusing only what IS generically shared process-wide: the bezier-
    // curve registry (Animation::mgr()->bezierExists()/getBezier(), a flat
    // name->curve map, unrelated to the tree) and the animated-variable
    // ticking machinery itself (confirmed by reading
    // CHyprAnimationManager::tick()/handleUpdate(): a CGenericAnimatedVariable
    // whose SAnimationContext has no window/workspace/layer set still gets
    // ticked/interpolated correctly - it just skips Hyprland's own
    // per-owner damage tracking, which HyprLUI doesn't want anyway, since
    // it damages its own canvases itself, same as Watcher.cpp's notify()).
    //
    // Each slot's SAnimationPropertyConfig is created ONCE (in the
    // constructor) and mutated in place on every later configure() call,
    // never replaced - widgets' CAnimatedVariables hold only a WEAK
    // reference to it (CBaseAnimatedVariable::setConfig()), which would
    // dangle if this were ever swapped for a new object instead of edited
    // in place. Matches how Hyprland's own
    // CAnimationConfigTree::setConfigForNode() behaves (mutates, never
    // replaces) - confirmed by reading CBaseAnimatedVariable's own
    // getPercent()/enabled()/getBezierName(), which all dereference
    // m_pConfig->pValues->X fresh on every call, never caching.
    class CWidgetAnimations {
      public:
        enum EKind {
            IN,
            OUT
        };

        static CWidgetAnimations& get() {
            static CWidgetAnimations instance;
            return instance;
        }

        // hyprlui.animation({leaf="in"|"out", enabled, speed, bezier}) -
        // `speed` is in DECISECONDS (tenths of a second), matching
        // Hyprland's own hl.animation()'s unit exactly, so a user's
        // existing mental model transfers directly. Disabled (the
        // default, until configure() is ever called) means
        // setVisible() stays exactly as instant as it always was - this
        // is purely opt-in, no existing behavior changes unless a config
        // author explicitly turns it on.
        void configure(EKind kind, bool enabled, float speedDeciseconds, const std::string& bezier) {
            auto& cfg            = slot(kind);
            cfg->internalEnabled = enabled ? 1 : 0;
            cfg->internalSpeed   = speedDeciseconds;
            cfg->internalBezier  = bezier;
        }

        bool enabled(EKind kind) const {
            return slot(kind)->internalEnabled != 0;
        }

        // The shared config object for `kind` - widgets pass this
        // straight to Animation::mgr()->createAnimation()/setConfig().
        SP<Hyprutils::Animation::SAnimationPropertyConfig> config(EKind kind) const {
            return slot(kind);
        }

      private:
        CWidgetAnimations() {
            for (auto* cfg : {&m_in, &m_out}) {
                *cfg                    = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
                (*cfg)->internalEnabled = 0;
                (*cfg)->internalSpeed   = 3.f;
                (*cfg)->internalBezier  = "default";
                (*cfg)->pValues         = *cfg; // self-referencing root node, see class comment
            }
        }

        SP<Hyprutils::Animation::SAnimationPropertyConfig>& slot(EKind kind) {
            return kind == IN ? m_in : m_out;
        }
        const SP<Hyprutils::Animation::SAnimationPropertyConfig>& slot(EKind kind) const {
            return kind == IN ? m_in : m_out;
        }

        SP<Hyprutils::Animation::SAnimationPropertyConfig> m_in, m_out;
    };

    // Builds a standalone SAnimationPropertyConfig (self-referencing
    // pValues, same shape as CWidgetAnimations' own slots above) - used
    // for a per-widget animationIn/animationOut override (see CWidget::
    // setAnimationInOverride()/setAnimationOutOverride() below), which
    // unlike the global slots is built ONCE at construction time from a
    // Lua table and never mutated in place afterward - there's no live-
    // reconfigure API for one specific widget's own override the way
    // hyprlui.animation() live-reconfigures the global one.
    inline SP<Hyprutils::Animation::SAnimationPropertyConfig> makeAnimationConfig(bool enabled, float speedDeciseconds, const std::string& bezier) {
        auto cfg             = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
        cfg->internalEnabled = enabled ? 1 : 0;
        cfg->internalSpeed   = speedDeciseconds;
        cfg->internalBezier  = bezier;
        cfg->pValues         = cfg;
        return cfg;
    }

    // Shared implementation for CWidget::setVisible()/CCanvas::setVisible()
    // - both are unrelated classes that need exactly this logic (a widget
    // and a whole canvas fade the same way), so this is a free function
    // operating on the caller's own `outVisible`/`anim` members by
    // reference rather than duplicating it twice. `enabled`/`config` are
    // already RESOLVED by the caller (CWidget layers its own optional
    // per-widget fadeIn/fadeOut override on top of CWidgetAnimations'
    // global one before calling this; CCanvas has no override concept and
    // just passes the global one straight through) - this function itself
    // doesn't know or care where they came from. Instant (the pre-
    // Phase-13 behavior) if `enabled` is false; otherwise lazily creates
    // `anim` the first time it's actually needed and animates it toward
    // 1.0 (showing, `outVisible` flips true immediately so the caller
    // keeps participating in layout/hit-testing/rendering from frame 1)
    // or 0.0 (hiding, `outVisible` only flips false once `onHideFinished`
    // actually runs). `onHideFinished`'s own goal re-check guards against
    // a show() reversing the fade mid-flight: if a later call already
    // reassigned the goal back to 1.0 by the time this fires, it's a
    // no-op instead of incorrectly re-hiding something that just finished
    // fading back in.
    inline void applyAnimatedVisibility(bool visible, bool enabled, const SP<Hyprutils::Animation::SAnimationPropertyConfig>& config, bool& outVisible, PHLANIMVAR<float>& anim,
                                        std::function<void()> onHideFinished) {
        if (!enabled) {
            outVisible = visible;
            anim.reset();
            return;
        }

        if (!anim)
            Animation::mgr()->createAnimation(outVisible ? 1.0f : 0.0f, anim, config, AVARDAMAGE_NONE);
        else
            anim->setConfig(config);

        if (visible) {
            outVisible = true;
            *anim      = 1.0f;
        } else {
            *anim = 0.0f;
            auto* rawAnim =
                anim.get(); // the goal re-check below needs to read it AFTER this call returns, once the callback actually fires later - `anim` itself (the caller's member) stays alive at least that long, so a raw pointer into it is safe
            anim->setCallbackOnEnd([rawAnim, onHideFinished](WP<Hyprutils::Animation::CBaseAnimatedVariable>) {
                if (rawAnim->goal() == 0.0f)
                    onHideFinished();
            });
        }
    }

    // Per-side box-model insets shared by `padding` (inset a container's
    // children from its own edges) and `margin` (a widget's own requested
    // space around itself, read by a container's layout - see
    // ContainerWidget.cpp). A plain aggregate on purpose, matching CHyprColor/
    // Vector2D's own construction style elsewhere in this codebase.
    struct SEdgeInsets {
        double top = 0, right = 0, bottom = 0, left = 0;
    };

    // A widget's own debug-overlay config, tri-state per field (nullopt is
    // meaningful and distinct from `false` in two different ways depending
    // on which field it's on - see setDebug()'s doc comment below):
    //
    //   `enabled`  - nullopt = inherit the resolved value from the nearest
    //                ancestor that hasn't walled itself off (see
    //                setDebugCascade()); the tree root inherits `false`.
    //   `show*`    - nullopt = "auto": decide per-frame from this widget's
    //                own size (and, for showZOpacity, whether it's even at
    //                a non-default value) rather than from inheritance.
    //                An explicit true/false here is a hard override that
    //                also cascades to descendants exactly like `enabled`
    //                does, until some deeper widget overrides it again.
    //   `fontSize` - nullopt = inherit (or the tree-root default, 10) -
    //                same inheritance as `enabled`/`show*`, just an int
    //                instead of a bool. Point size for every debug label
    //                this widget (and, via inheritance, its descendants)
    //                draws - id/size/padding/margin/z-opacity text alike,
    //                one shared size rather than a per-category knob.
    //
    // This same struct doubles as both "what a widget itself asked for"
    // (SLuaBridge's parsed spec) and "what's been resolved so far while
    // walking down the tree" (renderDebug()'s `inherited` parameter) -
    // merging one widget's own spec into an inherited one is the same
    // "mine wins if set, else keep theirs" operation either way, see
    // Widget.cpp's resolveDebugSpec().
    struct SDebugSpec {
        std::optional<bool> enabled;
        std::optional<bool> showBox, showPadding, showMargin, showId, showSize, showZOpacity, showHitTarget;
        std::optional<int>  fontSize;
    };

    class CWidget;
    using PWidget = std::shared_ptr<CWidget>;

    class CWidget {
      public:
        CWidget(std::string id, const Vector2D& position = {0, 0}) : m_id(std::move(id)), m_position(position) {}

        virtual ~CWidget() = default;

        // Two-pass layout, run once per render over the whole tree before
        // any render() call - see measureContent()/arrangeChildren() below
        // for the per-widget-type hooks. Both are non-virtual on purpose:
        // every widget gets the "measure children first, then self" /
        // "position children, then let them lay out their own children"
        // ordering for free, and only needs to override the hook relevant
        // to its own behavior.
        void measure() {
            for (auto& child : m_children)
                child->measure();
            measureContent();
            if (m_fixedW)
                m_size.x = *m_fixedW;
            if (m_fixedH)
                m_size.y = *m_fixedH;

            // min/max clamp last, same as CSS - overrides even an explicit
            // fixed size, since "never smaller/larger than this" is a
            // stronger constraint than "this size" once both are given.
            if (m_minW)
                m_size.x = std::max(m_size.x, *m_minW);
            if (m_minH)
                m_size.y = std::max(m_size.y, *m_minH);
            if (m_maxW)
                m_size.x = std::min(m_size.x, *m_maxW);
            if (m_maxH)
                m_size.y = std::min(m_size.y, *m_maxH);
        }

        void arrange() {
            arrangeChildren();
            for (auto& child : m_children)
                child->arrange();
        }

        // `origin` is the top-left corner of the parent in screen-space
        // pixels; implementations should render at origin + m_position.
        // `parentOpacity` is the already-composed (multiplied-together)
        // opacity of every ancestor - see setOpacity()'s doc comment for
        // why multiply, not override. Default recurses into children (in
        // z-index paint order, see paintOrder() below) at their laid-out
        // positions, passing this widget's own composed opacity down -
        // right for containers; leaves override this instead and use the
        // composed value to fade what they actually draw.
        virtual void render(const Vector2D& origin, float parentOpacity = 1.0F) {
            if (!m_visible)
                return;
            const float opacity = composedOpacity(parentOpacity);
            for (auto* child : paintOrder())
                child->render(origin + m_position, opacity);
        }

        void addChild(PWidget child) {
            m_children.push_back(std::move(child));
        }

        // Recursive id lookup (self included), depth-first. Returns nullptr
        // if not found anywhere in the subtree.
        CWidget* findWidget(const std::string& id) {
            if (m_id == id)
                return this;
            for (auto& child : m_children) {
                if (auto* found = child->findWidget(id))
                    return found;
            }
            return nullptr;
        }

        // Finds the topmost interactive widget whose bounds contain
        // `point`, searching this widget's subtree. `origin` is this
        // widget's PARENT's already-accumulated absolute position (same
        // convention as render()'s origin parameter). Default: not
        // interactive itself, just recurse into children in reverse paint
        // order (see paintOrder() below) - whatever painted last/on top
        // (highest z-index, ties broken by later insertion) is checked
        // first so an overlapping later/higher sibling wins. Only
        // ButtonWidget.hpp/InputWidget.hpp override this to actually match
        // (return `this`) - everything else stays a pure pass-through
        // search, so clicking a HUD's background/label doesn't swallow the
        // click, only clicking an actual interactive widget does.
        // Children get first refusal (unchanged - an interactive child
        // wins over an ancestor that's ALSO clickable, e.g. a Checkbox
        // inside a Row that also has its own onClick). Only once nothing
        // below matched does this widget check itself: any widget with an
        // onClick OR onScroll handler set becomes a real hit target this
        // way, without needing to be a CButtonWidget - see setOnClick()/
        // setOnScroll() below. Checking onScroll too (not just onClick)
        // matters - hitTestWidget() is the SAME lookup hover-tracking and
        // scroll dispatch both go through (InputHook.cpp), so a widget
        // with only onScroll set (no onClick - e.g. a scroll-driven
        // custom control) still needs to actually match here, or scroll
        // would never reach it despite the field being "generically"
        // accepted. CButtonWidget/CInputWidget/CCheckboxWidget still
        // override this entirely (unconditional leaf match, regardless of
        // whether either callback happens to be set) for their own
        // specific reasons - Button's "always a valid click target
        // structurally" contract, Input's click-to-focus, Checkbox's
        // toggle - this default is only what a plain Box/Text/Image/Row/
        // Column/Stack falls back to.
        virtual CWidget* hitTest(const Vector2D& origin, const Vector2D& point) {
            if (!m_visible)
                return nullptr;

            const Vector2D absOrigin = origin + m_position;
            auto           order     = paintOrder();
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                if (auto* hit = (*it)->hitTest(absOrigin, point))
                    return hit;
            }

            if ((m_onClick || m_onScroll) && !m_disabled && boxAt(origin).containsPoint(point))
                return this;
            return nullptr;
        }

        // Whether this widget itself is ever a real hitTest() match (i.e.
        // overrides hitTest() to return `this`) - purely descriptive, used
        // only by the debug overlay's hit-target highlight (Widget.cpp) to
        // know which widgets to draw it for. Default reflects whether THIS
        // instance actually has an onClick or onScroll handler set
        // (matching hitTest()'s own default above exactly) -
        // CButtonWidget/CInputWidget/CCheckboxWidget override to
        // unconditional `true` instead, same reasoning as their own
        // hitTest() overrides. Deliberately separate from actually calling
        // hitTest() here, which would need a point to test against and
        // could recurse - this just answers "is this widget the KIND of
        // thing that can ever match at all."
        virtual bool isInteractive() const {
            return static_cast<bool>(m_onClick) || static_cast<bool>(m_onScroll);
        }

        // A plain no-argument click callback, available on ANY widget
        // (not just Button) - see hitTest()'s default above for what
        // actually makes this functional, not just stored. Same
        // press-must-land-on-the-same-widget-as-release semantics as
        // Button always had (InputHook.cpp), now via CUIManager::
        // clickWidget()'s generic fireClick() fallback rather than a
        // Button-specific dynamic_cast. Checkbox intentionally does NOT
        // use this - its click() toggles state and fires onChange(bool)
        // instead, a different shape; setting onClick on a Checkbox is
        // harmless but inert (CUIManager::clickWidget() resolves the
        // CCheckboxWidget branch first and never reaches this fallback).
        void setOnClick(std::function<void()> fn) {
            m_onClick = std::move(fn);
        }

        // Invokes the onClick handler, if any, and reports whether one
        // was actually set - called by CUIManager::clickWidget() once a
        // press and its matching release both land on this same widget.
        bool fireClick() {
            if (!m_onClick)
                return false;
            m_onClick();
            return true;
        }

        // Interactive state (Phase 10, DESIGN.md) - shared across every
        // interactive widget type (CButtonWidget/CInputWidget/
        // CCheckboxWidget) instead of each reinventing its own hover/
        // disabled bookkeeping, same "shared base field, only some
        // subclasses actually interpret it" pattern Phase 7's padding/
        // margin/opacity/etc. already established. Only meaningful for a
        // widget whose isInteractive() is true - a decorative Box setting
        // these does nothing (its hitTest() never matches, so it can
        // never become hovered/disabled-and-skipped in the first place).
        //
        // `disabled`: excludes this widget from hitTest() entirely (see
        // CButtonWidget/CInputWidget/CCheckboxWidget's own overrides) -
        // click-through/unfocusable, as if it isn't there for interaction
        // purposes, while still rendering. A disabled widget can therefore
        // never become the hovered one either - hover and disabled are
        // mutually exclusive by construction, not just by convention.
        void setDisabled(bool disabled) {
            m_disabled = disabled;
        }
        bool disabled() const {
            return m_disabled;
        }

        // `hovered`: current hover state, set by CUIManager (via
        // setHovered()) as the pointer moves - never set directly by a
        // widget itself. setHovered() both updates the stored flag AND
        // fires onHoverStart/onHoverEnd on the transition, unlike Input's
        // focus()/blur() (Phase 6) which don't store any state on the
        // widget at all - hoverColor's automatic, no-Lua-round-trip
        // application (see effectiveFillColor() below) is what forces
        // this one to actually remember its own state, since render()
        // needs to know it directly.
        void setHovered(bool hovered) {
            if (hovered == m_hovered)
                return;
            m_hovered = hovered;
            if (hovered && m_onHoverStart)
                m_onHoverStart();
            else if (!hovered && m_onHoverEnd)
                m_onHoverEnd();
        }
        bool hovered() const {
            return m_hovered;
        }

        void setHoverColor(const std::optional<CHyprColor>& color) {
            m_hoverColor = color;
        }
        void setDisabledColor(const std::optional<CHyprColor>& color) {
            m_disabledColor = color;
        }
        void setOnHoverStart(std::function<void()> fn) {
            m_onHoverStart = std::move(fn);
        }
        void setOnHoverEnd(std::function<void()> fn) {
            m_onHoverEnd = std::move(fn);
        }

        // Picks which color a leaf should actually fill with this frame -
        // `disabledColor` if disabled and set, else `hoverColor` if
        // hovered and set, else `base` (the widget's own normal color,
        // e.g. CButtonWidget's m_color) unchanged. No precedence conflict
        // between the two overrides is possible (see `disabled`'s doc
        // comment above - a disabled widget is never the hovered one), so
        // this is a plain two-step fallback, not a priority system.
        const CHyprColor& effectiveFillColor(const CHyprColor& base) const {
            if (m_disabled && m_disabledColor)
                return *m_disabledColor;
            if (m_hovered && m_hoverColor)
                return *m_hoverColor;
            return base;
        }

        // Scroll (Phase 10): stays completely inert unless a handler is
        // explicitly set - matches the "swallow only what's opted into"
        // philosophy already established for Phase 6's keybind-priority
        // default. `vertical` is true for the common mouse-wheel axis,
        // false for horizontal scroll; `delta` is the raw
        // IPointer::SAxisEvent value forwarded as-is (see InputHook.cpp),
        // no attempt to normalize/invert it into a "lines scrolled" unit.
        void setOnScroll(std::function<void(double delta, bool vertical)> fn) {
            m_onScroll = std::move(fn);
        }

        // Invokes the onScroll handler, if any, and reports whether one
        // was actually set - InputHook.cpp only cancels the underlying
        // mouse.axis event when this returns true, so scroll passes
        // through untouched to whatever's behind an unhandled widget.
        bool fireScroll(double delta, bool vertical) {
            if (!m_onScroll)
                return false;
            m_onScroll(delta, vertical);
            return true;
        }

        // Recursive removal by id, starting from this widget's children.
        // Returns true if something was removed.
        bool removeChild(const std::string& id) {
            auto it = std::find_if(m_children.begin(), m_children.end(), [&id](const PWidget& c) { return c->id() == id; });
            if (it != m_children.end()) {
                m_children.erase(it);
                return true;
            }
            for (auto& child : m_children) {
                if (child->removeChild(id))
                    return true;
            }
            return false;
        }

        const std::string& id() const {
            return m_id;
        }

        // Per-widget override of the global hyprlui.animation({leaf=
        // "in"|"out", ...}) config, set once at construction from this
        // widget's own `animationIn`/`animationOut` table field (see
        // LuaBridge.cpp's buildWidget()) - nullptr (the default) means
        // "use the global config for this leaf, whatever it currently
        // is." A non-null override completely REPLACES the global one for
        // this widget (including its own enabled/disabled state - e.g. a
        // widget can force `animationOut = { enabled = false }` to opt
        // itself OUT of a globally-enabled animation), it does not merge
        // with it. Only affects setVisible()/animateOutThenRemove() below,
        // not the global config itself or any other widget. Deliberately
        // NOT named "fade" anywhere in this API - opacity is the only
        // thing actually animated today, but the leaf/override mechanism
        // itself is generic (any future animatable property would reuse
        // the same "in"/"out" config shape), so nothing here should imply
        // it's opacity-only.
        void setAnimationInOverride(SP<Hyprutils::Animation::SAnimationPropertyConfig> config) {
            m_animationInOverride = std::move(config);
        }
        void setAnimationOutOverride(SP<Hyprutils::Animation::SAnimationPropertyConfig> config) {
            m_animationOutOverride = std::move(config);
        }

        // Instant by default, exactly as before Phase 13 - only animates
        // if the relevant leaf (`IN` when becoming visible, `OUT` when
        // becoming hidden) resolves enabled - this widget's own override
        // (see setAnimationInOverride()/setAnimationOutOverride() above)
        // if it has one, else the global hyprlui.animation() config - so
        // this is purely opt-in. Used identically regardless of WHY
        // visibility is changing - an explicit hyprlui.set_widget_visible()
        // call, or this widget being a window's root widget and the whole
        // window opening/closing (see CCanvas::setVisible(), which just
        // delegates to its root's own setVisible()) - there is no separate
        // "creation" or "toggle" animation concept, just becoming visible
        // or becoming hidden. When animating a hide, `m_visible` itself
        // doesn't flip to false until the animation actually finishes (via
        // the end callback below) - the widget keeps rendering/laying out
        // at its fading-down opacity until then. When animating a show,
        // `m_visible` flips true immediately (same as the instant path)
        // so it participates in layout/hit-testing from frame 1, and only
        // its opacity ramps up.
        void setVisible(bool visible) {
            const auto& widgetCfg = visible ? m_animationInOverride : m_animationOutOverride;
            if (widgetCfg) {
                applyAnimatedVisibility(visible, widgetCfg->internalEnabled != 0, widgetCfg, m_visible, m_visibilityAnim, [this]() { m_visible = false; });
                return;
            }
            auto&      anims = CWidgetAnimations::get();
            const auto kind  = visible ? CWidgetAnimations::IN : CWidgetAnimations::OUT;
            applyAnimatedVisibility(visible, anims.enabled(kind), anims.config(kind), m_visible, m_visibilityAnim, [this]() { m_visible = false; });
        }
        bool visible() const {
            return m_visible;
        }

        // Forces this widget fully hidden with NO animation and no
        // end-callback - used ONLY by hyprlui.window()'s creation path
        // (LuaBridge.cpp) to seed a brand-new root widget into a "just
        // built, about to animate in" state before immediately calling
        // setVisible(true) on it, so that call has something to actually
        // animate FROM. A plain setVisible(false) there would be wrong
        // whenever this widget's "out" also happens to be enabled (it
        // would itself animate 1->0 instead of snapping instantly,
        // breaking the very assumption the following setVisible(true)
        // relies on). Not for general use - an ordinary hide should always
        // go through setVisible(false) instead, so its own end-callback
        // semantics apply.
        void primeHidden() {
            m_visible = false;
            m_visibilityAnim.reset();
        }

        // Like setVisible(false), but the caller intends to actually
        // ERASE this widget afterward (hyprlui.remove_widget(), or a
        // whole window closing via CUIManager::removeCanvas() delegating
        // to its root - not just hide it) - `onDone` fires once that's
        // safe: immediately if "out" isn't enabled (this widget's own
        // override if it has one, else the global config), or once the
        // animation finishes otherwise. This widget doesn't know its own
        // parent, so it can't call removeChild() on itself - the caller
        // (LuaBridge.cpp) does the actual erase from `onDone`. No
        // reversal-guard needed here (unlike applyAnimatedVisibility's
        // hide path) - nothing ever calls setVisible(true) on a widget
        // that's about to be removed.
        void animateOutThenRemove(std::function<void()> onDone) {
            const auto& config  = m_animationOutOverride ? m_animationOutOverride : CWidgetAnimations::get().config(CWidgetAnimations::OUT);
            const bool  enabled = m_animationOutOverride ? m_animationOutOverride->internalEnabled != 0 : CWidgetAnimations::get().enabled(CWidgetAnimations::OUT);

            if (!enabled) {
                onDone();
                return;
            }

            if (!m_visibilityAnim)
                Animation::mgr()->createAnimation(m_visible ? 1.0f : 0.0f, m_visibilityAnim, config, AVARDAMAGE_NONE);
            else
                m_visibilityAnim->setConfig(config);

            *m_visibilityAnim = 0.0f;
            m_visibilityAnim->setCallbackOnEnd([onDone](WP<Hyprutils::Animation::CBaseAnimatedVariable>) { onDone(); });
        }

        // Whether this widget's own visibility fade, or any descendant's,
        // is actively interpolating right now - used by CCanvas::render()
        // to know whether to keep damaging every frame while a fade is in
        // flight. A fade changes rendered opacity every frame for its
        // whole duration, unlike every other mutation in this codebase
        // (which changes state once and is done) - see CCanvas::damage()'s
        // doc comment for why even a single one-shot mutation needs
        // several frames of damage, let alone a multi-second continuous
        // one.
        bool isAnimating() const {
            if (m_visibilityAnim && m_visibilityAnim->isBeingAnimated())
                return true;
            for (auto& child : m_children)
                if (child->isAnimating())
                    return true;
            return false;
        }

        void setPosition(const Vector2D& position) {
            m_position = position;
        }
        const Vector2D& position() const {
            return m_position;
        }

        void setSize(const Vector2D& size) {
            m_size = size;
        }
        const Vector2D& size() const {
            return m_size;
        }

        // Pins this widget's laid-out size instead of letting measure()
        // derive it from content/children ("size-to-content" is the
        // default - passing a value here overrides one or both axes).
        void setFixedSize(std::optional<double> w, std::optional<double> h) {
            m_fixedW = w;
            m_fixedH = h;
        }

        // Cross-axis stretch (a Phase 15 prerequisite for animated window
        // sizing, DESIGN.md) - "this widget should match its parent's
        // available size instead of sizing itself from its own content."
        // Interpreted differently per parent type, all via the SAME
        // public setSize() above, applied during arrange() (AFTER the
        // whole tree's measure() pass has already finished, so this never
        // feeds back into any size CALCULATION, only overrides the
        // already-settled result for layout purposes) - deliberately a
        // per-frame, non-pinning override (unlike setFixedSize() above,
        // which persists and is re-applied every future measure() too):
        //   - CFlexWidget (Row/Column): stretches to the row/column's
        //     full CROSS-axis space (like CSS align-self: stretch) - the
        //     MAIN axis is untouched, still sized from this widget's own
        //     content (flex-grow along the main axis is explicitly out of
        //     scope, see DESIGN.md).
        //   - CStackWidget: matches the stack's own full measured size at
        //     position (0, 0) - ignores the stack's padding, consistent
        //     with CStackWidget's own already-established "manual
        //     positioning, no padding interpretation" design.
        //   - A window's root widget: matches its CCanvas's own size, but
        //     only on axes where the canvas actually HAS a determinate
        //     size (an explicit w/h from hyprlui.window() or
        //     set_canvas_size()) - an auto-sized (size-to-content) axis
        //     has nothing determinate to fill, so is left untouched, same
        //     inherent limitation CSS stretch has against an "auto"
        //     parent.
        // A widget with no non-fill sibling/ancestor establishing a real
        // size on some axis (e.g. a Stack whose ONLY child is also
        // `fill`, or a root widget in a fully auto-sized window) has
        // nothing to stretch TO on that axis and stays at its own natural
        // size there - expected, not a bug, same as CSS.
        void setFill(bool fill) {
            m_fill = fill;
        }
        bool fill() const {
            return m_fill;
        }

        // Clamps measure()'s result to [min, max] on each axis independently
        // (either bound may be omitted). Applied AFTER setFixedSize()'s
        // override, same precedence CSS gives min/max-width over an
        // explicit width. A leaf whose visual content is itself a texture
        // (CTextNode) draws that texture at its own natural size rather
        // than stretching it to fill a min-widened box - see TextNode.cpp.
        void setMinSize(std::optional<double> w, std::optional<double> h) {
            m_minW = w;
            m_minH = h;
        }
        void setMaxSize(std::optional<double> w, std::optional<double> h) {
            m_maxW = w;
            m_maxH = h;
        }

        // Inset a container's own children from its edges - only
        // CFlexWidget and CInputWidget's auto-owned label currently
        // interpret this (CStackWidget's manual positioning leaves it
        // unused by design - see DESIGN.md Phase 7).
        void setPadding(const SEdgeInsets& padding) {
            m_padding = padding;
        }
        const SEdgeInsets& padding() const {
            return m_padding;
        }

        // A widget's own requested space around itself, read by whichever
        // container is laying it out - only CFlexWidget currently reads a
        // child's margin (see ContainerWidget.cpp); CStackWidget's manual
        // positioning leaves it unused by design, same as padding above.
        void setMargin(const SEdgeInsets& margin) {
            m_margin = margin;
        }
        const SEdgeInsets& margin() const {
            return m_margin;
        }

        // Own opacity in [0, 1], multiplied with every ancestor's own
        // opacity to get what actually reaches render() (see its doc
        // comment) - CSS/Qt/every-toolkit's convention, so a semi-
        // transparent container naturally fades its children too instead
        // of each widget's opacity being independent/absolute.
        void setOpacity(double opacity) {
            m_opacity = opacity;
        }
        double opacity() const {
            return m_opacity;
        }

        // Combines `parentOpacity` with this widget's own `m_opacity` AND
        // (Phase 13) any in-flight visibility-fade progress from
        // setVisible() - every render() override (this default container
        // implementation and each leaf widget's own) multiplies through
        // this rather than inlining `parentOpacity * m_opacity` directly,
        // so the fade applies uniformly without each leaf needing its own
        // awareness of m_visibilityAnim. A widget that's never been
        // animated (m_visibilityAnim still null) computes identically to
        // before Phase 13 - zero behavior change unless actually used.
        float composedOpacity(float parentOpacity) const {
            float o = parentOpacity * static_cast<float>(m_opacity);
            if (m_visibilityAnim)
                o *= m_visibilityAnim->value();
            return o;
        }

        // Paint-order override among this widget's OWN siblings (i.e.
        // within its parent's child list) - higher paints later/on top.
        // Ties (including the default, everyone at 0) keep insertion
        // order, so this is a pure additive extension of "later child
        // wins" with no behavior change when unused. Deliberately just a
        // sibling-local reorder, not a full CSS stacking-context system -
        // a low-z-index child of a high-z-index widget still paints
        // "inside" its parent's turn, it can't jump above a different
        // parent entirely.
        void setZIndex(int zIndex) {
            m_zIndex = zIndex;
        }
        int zIndex() const {
            return m_zIndex;
        }

        CBox boxAt(const Vector2D& origin) const {
            return {origin + m_position, m_size};
        }

        // This widget's own debug-overlay request (see SDebugSpec's doc
        // comment for what each field means and how it merges with
        // ancestors). Unset fields (nullopt) don't override anything -
        // they just fall through to whatever's inherited/auto.
        void setDebug(const SDebugSpec& debug) {
            m_debugSpec = debug;
        }
        const SDebugSpec& debugSpec() const {
            return m_debugSpec;
        }

        // Whether this widget's RESOLVED debug config (its own SDebugSpec
        // merged with whatever it inherited - see renderDebug()) becomes
        // the seed its children inherit from (the default, `true`), or
        // whether they instead start completely fresh (`false` - neither
        // this widget's own settings nor anything further up reaches
        // below it) - an escape hatch for "debug this one widget without
        // lighting up its entire subtree," or the reverse, "debug is on
        // above me, but I want this specific branch left alone."
        void setDebugCascade(bool cascade) {
            m_debugCascade = cascade;
        }
        bool debugCascade() const {
            return m_debugCascade;
        }

        // Debug-overlay tree walk - entirely separate from render()/
        // hitTest() (draws box-model outlines/labels for whichever
        // widgets resolve `enabled`, see SDebugSpec), called once per
        // frame from CCanvas::render() AFTER the real render() pass so it
        // always paints on top regardless of any widget's own z-index/
        // opacity (it's diagnostic, not real content - see Widget.cpp).
        // `inherited` is the resolved SDebugSpec accumulated from every
        // ancestor so far; the initial call from CCanvas passes a fresh
        // SDebugSpec{} (i.e. "nothing enabled, everything auto" at the
        // root). Non-virtual and implemented once for every widget type,
        // same reasoning as measure()/arrange() - the box-model
        // information it draws (position/size/padding/margin/id/zIndex/
        // opacity) is entirely made of base CWidget fields, no per-
        // subclass knowledge needed.
        //
        // Returns the union of every pixel actually drawn anywhere in
        // this subtree's overlay (nullopt if nothing anywhere has debug
        // enabled) - several of drawDebugOverlay()'s own labels (id,
        // margin, size, z/opacity) are DELIBERATELY drawn just outside a
        // widget's own box, so CCanvas needs this back to know how far
        // beyond its normal content box it has to damage - see
        // CCanvas::fullDamageBox()'s doc comment for the ghosting bug
        // this fixes (content box alone under-damages this overlay,
        // which self-heals for ordinary discrete mutations, since the
        // overflowing pixels just don't change between them, but visibly
        // ghosts the instant something DOES keep changing there every
        // frame - a Phase 13 fade being the most common case).
        std::optional<CBox> renderDebug(const Vector2D& origin, const SDebugSpec& inherited);

      public:
        // Snapshots this widget's CURRENT m_size as its own natural/
        // intrinsic content size, read back by the default measureContent()
        // below every frame - called once by buildWidget() (LuaBridge.cpp)
        // right after a widget is fully constructed (including any type-
        // specific size assignment, e.g. CRectNode's w/h constructor args),
        // before it could ever participate in a frame's measure()/arrange()
        // pass. Also called again by CImageWidget::reload() whenever
        // setImage() genuinely changes the decoded texture's size - the one
        // leaf type whose natural size can legitimately change AFTER
        // construction without going through setFixedSize(). A container or
        // CTextNode's own measureContent() override never reads
        // m_naturalSize at all (they derive their size some other way every
        // frame), so calling this on them is harmless.
        void primeNaturalSize() {
            m_naturalSize = m_size;
        }

      protected:
        // Sets m_size from this widget's own content/children every frame.
        // Default: reset to this widget's own natural/intrinsic size (see
        // primeNaturalSize() above) - correct for a LEAF (CRectNode/
        // CImageWidget/CButtonWidget/etc; nothing about its own size
        // depends on anything that changes frame to frame, the same way a
        // container's already-existing override derives its size fresh
        // from children every frame). Containers (CStackWidget/CFlexWidget)
        // and CTextNode override this entirely instead, deriving their size
        // some other way every frame - m_naturalSize is irrelevant to them.
        //
        // This is what makes a LEAF self-correcting exactly like a
        // container already was: without it, a `fill` child's stretch
        // (setFill(), applied via setSize() during arrange()) would leave
        // m_size permanently at whatever it was last stretched to, since
        // nothing else would ever touch it again - the exact sticky-growth
        // hazard CStackWidget::measureContent()/CFlexWidget::
        // measureContent()'s own doc comments describe, except now closed
        // at the SOURCE for every leaf type instead of needing each
        // container type to separately guard against a stale value leaking
        // in. Found live (DESIGN.md, Phase 15) via a `fill` root Stack
        // whose ONLY children were both `fill` - CStackWidget::
        // measureContent()'s "count fill children at their own pre-stretch
        // size when there's nothing else" fallback initially reused
        // whatever a leaf's `m_size` currently held, which - before this
        // fix - could still be a stale, already-stretched value from
        // several frames ago, not a genuine pre-stretch one.
        virtual void measureContent() {
            m_size = m_naturalSize;
        }

        // Positions m_children (their setPosition()) based on this
        // widget's own m_size. Default: no-op - right for leaves and for
        // CStackWidget, whose children keep whatever absolute position
        // they were given. Flex containers override this.
        virtual void                                       arrangeChildren() {}

        std::string                                        m_id;
        Vector2D                                           m_position;
        Vector2D                                           m_size;
        Vector2D                                           m_naturalSize; // Phase 15 follow-up - see primeNaturalSize()'s doc comment
        bool                                               m_visible = true;
        std::optional<double>                              m_fixedW, m_fixedH;
        bool                                               m_fill = false; // Phase 15 - see setFill()'s doc comment
        std::optional<double>                              m_minW, m_minH, m_maxW, m_maxH;
        SEdgeInsets                                        m_padding, m_margin;
        double                                             m_opacity = 1.0;
        int                                                m_zIndex  = 0;
        SDebugSpec                                         m_debugSpec;
        bool                                               m_debugCascade = true;
        bool                                               m_disabled     = false;
        bool                                               m_hovered      = false;
        std::optional<CHyprColor>                          m_hoverColor, m_disabledColor;
        std::function<void()>                              m_onHoverStart, m_onHoverEnd;
        std::function<void(double delta, bool vertical)>   m_onScroll;
        std::function<void()>                              m_onClick;
        std::vector<PWidget>                               m_children;
        PHLANIMVAR<float>                                  m_visibilityAnim; // Phase 13 - lazily created only once setVisible() actually animates, see CWidgetAnimations
        SP<Hyprutils::Animation::SAnimationPropertyConfig> m_animationInOverride,
            m_animationOutOverride; // Phase 13 follow-up - per-widget animationIn/animationOut override, null = use the global config

      private:
        // Merges m_debugSpec into `inherited` ("mine wins per-field if
        // set, else keep theirs") - see SDebugSpec's doc comment. Defined
        // in Widget.cpp alongside renderDebug()/drawDebugOverlay(), which
        // are the only callers.
        SDebugSpec resolveDebugSpec(const SDebugSpec& inherited) const;

        // Draws this widget's box-model overlay (margin/padding/content
        // outlines, id/size/padding/margin/z-opacity labels, hit-target
        // fill) per `resolved`'s already-merged show* decisions - auto
        // (nullopt) categories are decided here, from this widget's own
        // size (and, for z/opacity, whether it's at a non-default value).
        // `origin` is this widget's PARENT's already-accumulated absolute
        // position, same convention as render()/hitTest(). Returns the
        // union of every pixel actually drawn (at least this widget's own
        // box, even if nothing else ended up drawn) - see renderDebug()'s
        // own doc comment for why. See Widget.cpp.
        CBox drawDebugOverlay(const Vector2D& origin, const SDebugSpec& resolved) const;

        // Render()/hitTest()'s shared paint-order: a stable sort of
        // m_children by zIndex() (ascending - lower paints first/behind).
        // Stable so equal-zIndex siblings (the default, everyone at 0) keep
        // plain insertion order - identical to the pre-Phase-7 unsorted
        // walk when zIndex is never set. Recomputed on every render()/
        // hitTest() call rather than cached - cheap at HUD-sized child
        // counts, matches this codebase's established "redo it every
        // frame instead of tracking a dirty flag" precedent (see Widget.
        // hpp's own measure()/arrange(), CFlexWidget's layout, etc.).
        std::vector<CWidget*> paintOrder() const {
            std::vector<CWidget*> order;
            order.reserve(m_children.size());
            for (const auto& child : m_children)
                order.push_back(child.get());
            std::stable_sort(order.begin(), order.end(), [](const CWidget* a, const CWidget* b) { return a->m_zIndex < b->m_zIndex; });
            return order;
        }
    };

} // namespace HyprLUI
