#pragma once
//
// Widget.hpp
//
// Base class for every element in the UI tree - both leaves (text, rects,
// buttons/inputs) and containers (Stack/Row/Column). A Window/Canvas owns a
// single root Widget instead of a flat node list; layout runs as a two-pass
// measure() (bottom-up, natural sizes) then arrange() (top-down, final
// positions) walk before render().
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

    // Shared duration/bezier config for widget visibility fades
    // (hyprlui.animation({leaf="in"|"out", ...})). NOT hooked into
    // Hyprland's own animation tree - a plugin can't register a new leaf
    // node there, so this is a small self-contained config store instead,
    // reusing only what's generically shared process-wide: the bezier-curve
    // registry and the animated-variable ticking machinery itself.
    //
    // Each slot's SAnimationPropertyConfig is created once and mutated in
    // place on every later configure() call, never replaced - widgets'
    // CAnimatedVariables hold only a weak reference to it, which would
    // dangle if this were ever swapped for a new object.
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

        // hyprlui.animation({leaf="in"|"out", enabled, speed, bezier,
        // style}) - `speed` is in deciseconds, matching Hyprland's own
        // hl.animation(). Disabled by default, so this is purely opt-in.
        // `style` is Hyprland's own windowsIn/windowsOut style syntax
        // ("slide", "slide left|right|top|bottom", "popin"/"gnome") -
        // see CWidget::styleOffset()/popinTransform() for how it's turned
        // into an actual position/scale offset, applied uniformly to any
        // widget, not just a window's root.
        void configure(EKind kind, bool enabled, float speedDeciseconds, const std::string& bezier, const std::string& style = "") {
            auto& cfg            = slot(kind);
            cfg->internalEnabled = enabled ? 1 : 0;
            cfg->internalSpeed   = speedDeciseconds;
            cfg->internalBezier  = bezier;
            cfg->internalStyle   = style;
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
                (*cfg)->pValues         = *cfg; // self-referencing root node
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

    // Builds a standalone SAnimationPropertyConfig for a per-widget
    // animationIn/animationOut override (see CWidget::
    // setAnimationInOverride()/setAnimationOutOverride() below) - unlike
    // CWidgetAnimations' global slots, this is built once at construction
    // and never mutated in place afterward.
    inline SP<Hyprutils::Animation::SAnimationPropertyConfig> makeAnimationConfig(bool enabled, float speedDeciseconds, const std::string& bezier, const std::string& style = "") {
        auto cfg             = makeShared<Hyprutils::Animation::SAnimationPropertyConfig>();
        cfg->internalEnabled = enabled ? 1 : 0;
        cfg->internalSpeed   = speedDeciseconds;
        cfg->internalBezier  = bezier;
        cfg->internalStyle   = style;
        cfg->pValues         = cfg;
        return cfg;
    }

    // Shared implementation for CWidget::setVisible()/CCanvas::setVisible()
    // - a free function operating on the caller's own outVisible/anim
    // members by reference, since both need identical fade logic.
    // `enabled`/`config` are already resolved by the caller. Instant if
    // `enabled` is false; otherwise lazily creates `anim` and animates it
    // toward 1.0 (showing - outVisible flips true immediately so the
    // caller keeps participating in layout/hit-testing from frame 1) or
    // 0.0 (hiding - outVisible only flips false once `onHideFinished`
    // runs). `onHideFinished`'s own goal re-check guards against a show()
    // reversing the fade mid-flight.
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
                anim.get(); // safe: `anim` (the caller's member) outlives the callback below, which only reads this after anim() returns
            anim->setCallbackOnEnd([rawAnim, onHideFinished](WP<Hyprutils::Animation::CBaseAnimatedVariable>) {
                if (rawAnim->goal() == 0.0f)
                    onHideFinished();
            });
        }
    }

    // Per-side box-model insets shared by `padding` (inset a container's
    // children from its own edges) and `margin` (a widget's own requested
    // space around itself, read by a container's layout).
    struct SEdgeInsets {
        double top = 0, right = 0, bottom = 0, left = 0;
    };

    // A widget's own debug-overlay config. Tri-state per field:
    //   `enabled`  - nullopt = inherit the resolved value from the nearest
    //                ancestor that hasn't walled itself off (see
    //                setDebugCascade()); the tree root inherits `false`.
    //   `show*`    - nullopt = "auto": decide per-frame from this widget's
    //                own size. An explicit true/false is a hard override
    //                that also cascades to descendants, like `enabled`.
    //   `fontSize` - nullopt = inherit (or the tree-root default, 10).
    //
    // Doubles as both "what a widget itself asked for" and "what's been
    // resolved so far while walking down the tree" - merging one into the
    // other is "mine wins if set, else keep theirs" either way, see
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
        // for the per-widget-type hooks. Both non-virtual: every widget
        // gets the "measure children first, then self" / "position
        // children, then let them arrange their own" ordering for free.
        void measure() {
            for (auto& child : m_children)
                child->measure();
            measureContent();
            if (m_fixedW)
                m_size.x = *m_fixedW;
            if (m_fixedH)
                m_size.y = *m_fixedH;

            // min/max clamp last, same as CSS - overrides even an explicit
            // fixed size.
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

        // `origin` is the parent's already-accumulated absolute position;
        // implementations render at origin + m_position. `parentOpacity`
        // is every ancestor's already-composed opacity (see
        // composedOpacity()). `scale` is a per-axis multiplier
        // accumulated from every ancestor's own popin/gnome style, if
        // any. Default recurses into children (in z-index paint order) at
        // their laid-out positions; leaves override this to draw
        // themselves instead, using composedOpacity()/boxAt() to fade and
        // position what they draw.
        virtual void render(const Vector2D& origin, float parentOpacity = 1.0F, const Vector2D& scale = {1, 1}) {
            if (!m_visible)
                return;
            const float    opacity     = composedOpacity(parentOpacity);
            const Vector2D basePos     = origin + (m_position + styleOffset()) * scale;
            const auto     local       = popinTransform();
            const Vector2D childOrigin = basePos + local.offset * scale;
            const Vector2D childScale  = scale * local.scale;
            for (auto* child : paintOrder())
                child->render(childOrigin, opacity, childScale);
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
        // `point`, searching this widget's subtree (`origin` = parent's
        // already-accumulated absolute position). Default: not interactive
        // itself, recurses into children in reverse paint order (highest
        // z-index/latest-inserted first) so an overlapping later/higher
        // sibling wins; only once nothing below matches does this widget
        // check itself - true if it has an onClick or onScroll handler
        // set (see setOnClick()/setOnScroll()). CButtonWidget/
        // CInputWidget/CCheckboxWidget override this entirely for their
        // own always-a-target/click-to-focus/toggle semantics.
        virtual CWidget* hitTest(const Vector2D& origin, const Vector2D& point, const Vector2D& scale = {1, 1}) {
            if (!m_visible)
                return nullptr;

            const Vector2D basePos     = origin + (m_position + styleOffset()) * scale;
            const auto     local       = popinTransform();
            const Vector2D childOrigin = basePos + local.offset * scale;
            const Vector2D childScale  = scale * local.scale;
            auto           order       = paintOrder();
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                if (auto* hit = (*it)->hitTest(childOrigin, point, childScale))
                    return hit;
            }

            if ((m_onClick || m_onScroll) && !m_disabled && boxAt(origin, scale).containsPoint(point))
                return this;
            return nullptr;
        }

        // Whether this widget is ever a real hitTest() match - purely
        // descriptive, used by the debug overlay's hit-target highlight.
        // Default mirrors hitTest()'s own default (onClick or onScroll
        // set); CButtonWidget/CInputWidget/CCheckboxWidget override to
        // unconditional `true`.
        virtual bool isInteractive() const {
            return static_cast<bool>(m_onClick) || static_cast<bool>(m_onScroll);
        }

        // A plain no-argument click callback, available on any widget, not
        // just Button - see hitTest()'s default for what makes this
        // actually functional. Checkbox does not use this - its click()
        // toggles state and fires onChange(bool) instead.
        void setOnClick(std::function<void()> fn) {
            m_onClick = std::move(fn);
        }

        // Invokes the onClick handler, if any, and reports whether one was
        // set - called once a press and its matching release both land on
        // this same widget.
        bool fireClick() {
            if (!m_onClick)
                return false;
            m_onClick();
            return true;
        }

        // Interactive state, shared across every interactive widget type
        // instead of each reinventing its own hover/disabled bookkeeping.
        // Only meaningful for a widget whose isInteractive() is true.
        //
        // `disabled`: excludes this widget from hitTest() entirely -
        // click-through/unfocusable, while still rendering. A disabled
        // widget can never become hovered either.
        void setDisabled(bool disabled) {
            m_disabled = disabled;
        }
        bool disabled() const {
            return m_disabled;
        }

        // `hovered`: current hover state, set by CUIManager (via
        // setHovered()) as the pointer moves. Fires onHoverStart/
        // onHoverEnd on the transition.
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

        // Picks which color a leaf should fill with this frame:
        // disabledColor if disabled and set, else hoverColor if hovered
        // and set, else `base`. No precedence conflict is possible - a
        // disabled widget is never the hovered one.
        const CHyprColor& effectiveFillColor(const CHyprColor& base) const {
            if (m_disabled && m_disabledColor)
                return *m_disabledColor;
            if (m_hovered && m_hoverColor)
                return *m_hoverColor;
            return base;
        }

        // Stays completely inert unless a handler is explicitly set.
        // `vertical` is true for the common mouse-wheel axis, false for
        // horizontal scroll; `delta` is the raw axis-event value
        // forwarded as-is, not normalized.
        void setOnScroll(std::function<void(double delta, bool vertical)> fn) {
            m_onScroll = std::move(fn);
        }

        // Invokes the onScroll handler, if any, and reports whether one
        // was set - InputHook.cpp only cancels the underlying scroll
        // event when this returns true.
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
        // widget's own animationIn/animationOut table field. nullptr (the
        // default) means "use the global config for this leaf." A non-null
        // override completely replaces the global one for this widget
        // (including enabled/disabled), it does not merge with it.
        void setAnimationInOverride(SP<Hyprutils::Animation::SAnimationPropertyConfig> config) {
            m_animationInOverride = std::move(config);
        }
        void setAnimationOutOverride(SP<Hyprutils::Animation::SAnimationPropertyConfig> config) {
            m_animationOutOverride = std::move(config);
        }

        // Instant by default - only animates if the relevant leaf (IN when
        // becoming visible, OUT when becoming hidden) resolves enabled
        // (this widget's own override if it has one, else the global
        // config) - purely opt-in. Used identically regardless of why
        // visibility is changing (an explicit call, or this being a
        // window's root and the whole window opening/closing). Hiding:
        // m_visible doesn't flip false until the animation finishes.
        // Showing: m_visible flips true immediately, only opacity ramps.
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

        // Forces this widget fully hidden with no animation/end-callback -
        // used only by hyprlui.window()'s creation path to seed a
        // brand-new root into a "just built, about to animate in" state
        // before calling setVisible(true), so that call has something to
        // animate from. Not for general use - an ordinary hide should
        // always go through setVisible(false) instead.
        void primeHidden() {
            m_visible = false;
            m_visibilityAnim.reset();
        }

        // Like setVisible(false), but the caller intends to actually erase
        // this widget afterward - `onDone` fires once that's safe
        // (immediately if OUT isn't enabled, else once the animation
        // finishes). This widget doesn't know its own parent, so the
        // caller does the actual erase from `onDone`.
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
        // is actively interpolating - used by CCanvas::render() to know
        // whether to keep damaging every frame while a fade is in flight.
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
        // derive it from content/children (size-to-content is the
        // default).
        void setFixedSize(std::optional<double> w, std::optional<double> h) {
            m_fixedW = w;
            m_fixedH = h;
        }

        // Cross-axis stretch - "match my parent's available size instead
        // of sizing from my own content" (CSS align-self: stretch, not
        // flex-grow). Applied during arrange(), after the whole tree's
        // measure() pass, so it never feeds back into sizing, only
        // overrides the laid-out result:
        //   - Row/Column: stretches to the full cross-axis space; the
        //     main axis stays sized from this widget's own content.
        //   - Stack: matches the stack's own full measured size at (0,0),
        //     ignoring the stack's padding.
        //   - A window's root: matches its canvas's size, but only on axes
        //     where the canvas has a determinate size (explicit w/h).
        // A widget with nothing establishing a real size on some axis
        // (e.g. a Stack whose only child is also `fill`) stays at its own
        // natural size there - expected, matches CSS stretch against an
        // "auto" parent.
        void setFill(bool fill) {
            m_fill = fill;
        }
        bool fill() const {
            return m_fill;
        }

        // Clamps measure()'s result to [min, max] on each axis
        // independently (either bound may be omitted), applied after
        // setFixedSize()'s override - same precedence CSS gives min/max-
        // width over an explicit width.
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
        // unused by design).
        void setPadding(const SEdgeInsets& padding) {
            m_padding = padding;
        }
        const SEdgeInsets& padding() const {
            return m_padding;
        }

        // A widget's own requested space around itself, read by whichever
        // container lays it out - only CFlexWidget currently reads a
        // child's margin; CStackWidget leaves it unused, same as padding.
        void setMargin(const SEdgeInsets& margin) {
            m_margin = margin;
        }
        const SEdgeInsets& margin() const {
            return m_margin;
        }

        // Own opacity in [0, 1], multiplied with every ancestor's own
        // opacity (CSS/Qt convention) so a semi-transparent container
        // naturally fades its children too.
        void setOpacity(double opacity) {
            m_opacity = opacity;
        }
        double opacity() const {
            return m_opacity;
        }

        // Combines `parentOpacity` with this widget's own opacity and any
        // in-flight visibility-fade progress - every render() override
        // multiplies through this rather than inlining the composition
        // directly.
        float composedOpacity(float parentOpacity) const {
            float o = parentOpacity * static_cast<float>(m_opacity);
            if (m_visibilityAnim)
                o *= m_visibilityAnim->value();
            return o;
        }

        // Paint-order override among this widget's own siblings - higher
        // paints later/on top. Ties keep insertion order. A sibling-local
        // reorder only, not a full CSS stacking-context system.
        void setZIndex(int zIndex) {
            m_zIndex = zIndex;
        }
        int zIndex() const {
            return m_zIndex;
        }

        // `style`'s position offset, if this widget's currently-active
        // visibility animation has one set. Reuses m_visibilityAnim
        // directly - opacity and slide progress are the same 0..1 goal,
        // matching how Hyprland's own windowsIn/windowsOut couples them.
        //
        // Syntax matches Hyprland's own style string - "slide" or "slide
        // left|right|top|bottom" - minus its "no direction -> auto-pick
        // nearest monitor edge" behavior (needs monitor geometry a plain
        // widget doesn't have; "left" is the fixed default here instead).
        // Only "slide" is handled here - "popin"/"gnome" produce a scale,
        // not a translation, so popinTransform() below handles those; a
        // non-slide style just returns {0,0}. The distance slid is always
        // this widget's own current size along that axis.
        Vector2D styleOffset() const {
            if (!m_visibilityAnim)
                return {0, 0};
            const std::string& style = m_visibilityAnim->getStyle();
            if (!style.starts_with("slide"))
                return {0, 0};

            std::string direction = "left";
            if (const auto space = style.find(' '); space != std::string::npos && space + 1 < style.size())
                direction = style.substr(space + 1);

            Vector2D magnitude{0, 0};
            if (direction == "right")
                magnitude = {m_size.x, 0.0};
            else if (direction == "top")
                magnitude = {0.0, -m_size.y};
            else if (direction == "bottom")
                magnitude = {0.0, m_size.y};
            else
                magnitude = {-m_size.x, 0.0}; // "left", also the fallback for an unrecognized word

            return magnitude * (1.0 - m_visibilityAnim->value());
        }

        // This widget's own popin/gnome contribution - `scale` (1 = full
        // size, per axis) and `offset` (keeps the shrink centered on this
        // widget's own box). {1,1}/{0,0} for "slide" or no style at all.
        struct SPopinTransform {
            Vector2D scale{1, 1};
            Vector2D offset{0, 0};
        };

        // Any widget with style = "popin"/"popin N%"/"gnome"/"gnomed" set
        // shrinks itself and its whole subtree around its own center -
        // applies to any widget, not just a window's root (a window's
        // root doing this IS the whole window shrinking). Composes
        // multiplicatively with whatever its ancestors already
        // contributed (see render()/hitTest()) - a widget nested inside
        // an already-popin-ing ancestor shrinks further, relative to its
        // own center within that already-shrunk space.
        //
        // Math mirrors Hyprland's own WindowAnimationController.cpp:
        // popin's scale = minPerc + (1 - minPerc) * progress (uniform on
        // both axes, minPerc from an optional trailing "N%", default 0);
        // gnome's scale = {1, progress} (Y squashes to a horizontal line,
        // X untouched). Either way, offset = size/2 * (1 - scale)
        // componentwise, keeping the shrink centered.
        SPopinTransform popinTransform() const {
            if (!m_visibilityAnim)
                return {};
            const std::string& style = m_visibilityAnim->getStyle();
            Vector2D           scale{1, 1};
            if (style == "gnome" || style == "gnomed") {
                scale = {1.0, m_visibilityAnim->value()};
            } else if (style == "popin" || style.starts_with("popin ")) {
                double minPerc = 0.0;
                if (const auto space = style.find(' '); space != std::string::npos) {
                    const auto pct = style.substr(space + 1);
                    try {
                        minPerc = std::stod(pct.substr(0, pct.size() - 1)) / 100.0; // trailing '%' already validated by LuaBridge.cpp's optStyleField()
                    } catch (...) {}
                }
                const double s = minPerc + (1.0 - minPerc) * m_visibilityAnim->value();
                scale          = {s, s};
            } else {
                return {}; // "slide" or no style
            }
            return {scale, m_size * 0.5 * (Vector2D{1.0, 1.0} - scale)};
        }

        // `scale` here is the accumulated scale from every ancestor's own
        // popinTransform() (NOT including this widget's own, applied on
        // top via popinTransform() below).
        CBox boxAt(const Vector2D& origin, const Vector2D& scale = {1, 1}) const {
            const Vector2D basePos = origin + (m_position + styleOffset()) * scale;
            const auto     local   = popinTransform();
            return {basePos + local.offset * scale, m_size * scale * local.scale};
        }

        // This widget's own debug-overlay request - see SDebugSpec for
        // what each field means and how it merges with ancestors.
        void setDebug(const SDebugSpec& debug) {
            m_debugSpec = debug;
        }
        const SDebugSpec& debugSpec() const {
            return m_debugSpec;
        }

        // Whether this widget's resolved debug config becomes the seed its
        // children inherit from (the default, `true`), or they instead
        // start completely fresh (`false`) - lets a subtree be debugged in
        // isolation, or left alone while debug is on above it.
        void setDebugCascade(bool cascade) {
            m_debugCascade = cascade;
        }
        bool debugCascade() const {
            return m_debugCascade;
        }

        // Debug-overlay tree walk - entirely separate from render()/
        // hitTest(), called once per frame from CCanvas::render() after
        // the real render() pass so it always paints on top. `inherited`
        // is the resolved SDebugSpec accumulated from every ancestor so
        // far (the root call passes a fresh SDebugSpec{}). Non-virtual,
        // implemented once for every widget type - the box-model info it
        // draws is entirely made of base CWidget fields.
        //
        // Returns the union of every pixel actually drawn (nullopt if
        // nothing has debug enabled) - some labels are deliberately drawn
        // just outside a widget's own box, so CCanvas needs this to know
        // how far beyond the normal content box to damage.
        std::optional<CBox> renderDebug(const Vector2D& origin, const SDebugSpec& inherited);

      public:
        // Snapshots this widget's current m_size as its own natural/
        // intrinsic content size, read back by the default
        // measureContent() below. Called once by buildWidget()
        // (LuaBridge.cpp) right after a widget is fully constructed, and
        // again by CImageWidget::reload() whenever setImage() changes the
        // decoded texture's size - the one leaf type whose natural size
        // can legitimately change after construction.
        void primeNaturalSize() {
            m_naturalSize = m_size;
        }

      protected:
        // Sets m_size from this widget's own content/children every
        // frame. Default: reset to this widget's own natural/intrinsic
        // size - correct for a leaf, whose size doesn't otherwise depend
        // on anything that changes frame to frame. Containers and
        // CTextNode override this entirely instead.
        //
        // This is what makes a leaf self-correcting after a `fill`
        // stretch: without resetting every frame, m_size would stay
        // pinned at whatever it was last stretched to (setFill(), applied
        // via setSize() during arrange()), since nothing else would ever
        // touch it again.
        virtual void measureContent() {
            m_size = m_naturalSize;
        }

        // Positions m_children based on this widget's own m_size. Default:
        // no-op - right for leaves and CStackWidget (children keep
        // whatever absolute position they were given). Flex containers
        // override this.
        virtual void                                       arrangeChildren() {}

        std::string                                        m_id;
        Vector2D                                           m_position;
        Vector2D                                           m_size;
        Vector2D                                           m_naturalSize; // see primeNaturalSize()
        bool                                               m_visible = true;
        std::optional<double>                              m_fixedW, m_fixedH;
        bool                                               m_fill = false; // see setFill()
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
        PHLANIMVAR<float>                                  m_visibilityAnim; // lazily created only once setVisible() actually animates, see CWidgetAnimations
        SP<Hyprutils::Animation::SAnimationPropertyConfig> m_animationInOverride,
            m_animationOutOverride; // per-widget animationIn/animationOut override, null = use the global config

      private:
        // Merges m_debugSpec into `inherited` ("mine wins per-field if
        // set, else keep theirs"). Defined in Widget.cpp alongside
        // renderDebug()/drawDebugOverlay(), its only callers.
        SDebugSpec resolveDebugSpec(const SDebugSpec& inherited) const;

        // Draws this widget's box-model overlay per `resolved`'s already-
        // merged show* decisions - auto (nullopt) categories are decided
        // here, from this widget's own size. `origin` is the parent's
        // already-accumulated absolute position. Returns the union of
        // every pixel actually drawn (at least this widget's own box).
        CBox drawDebugOverlay(const Vector2D& origin, const SDebugSpec& resolved) const;

        // render()/hitTest()'s shared paint-order: a stable sort of
        // m_children by zIndex() (ascending - lower paints first/behind).
        // Recomputed every call rather than cached - cheap at HUD-sized
        // child counts.
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
