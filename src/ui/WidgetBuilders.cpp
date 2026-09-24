#include "WidgetBuilders.hpp"
#include "ContainerWidget.hpp"
#include "RectNode.hpp"
#include "TextNode.hpp"
#include "ButtonWidget.hpp"
#include "InputWidget.hpp"
#include "ImageWidget.hpp"
#include "CheckboxWidget.hpp"
#include "parser/ValueParsers.hpp"
#include "parser/ShorthandParsers.hpp"
#include "parser/AnimationParsers.hpp"
#include "parser/CallbackParsers.hpp"
#include "../reactive/Watcher.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Color.hpp>

namespace HyprLUI::Lua {

    namespace {

        // Unsized (no w/h) defaults to 0x0 rather than erroring - almost
        // always paired with `fill` in practice. Warns (debug-gated) when
        // w/h are both fully omitted (not explicitly 0) and fill isn't
        // set either, since that combination is silently invisible.
        PWidget buildBoxWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos, bool debugEnabled) {
            const auto wOpt        = optFixedField(L, idx, "w");
            const auto hOpt        = optFixedField(L, idx, "h");
            const auto color       = parseColorField(L, idx, "color", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Box");
            const int  rounding    = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
            const auto borderColor = parseGradientField(L, idx, "borderColor", CHyprColor{}, "hyprlui.Box");
            const int  borderWidth = static_cast<int>(fieldNumber(L, idx, "borderWidth", 0));
            const bool fill        = optFieldBool(L, idx, "fill", false);

            if (!wOpt && !hOpt && !fill && debugEnabled)
                Log::logger->log(Log::WARN, "[hyprlui] Box '{}': no w/h and no fill - will be invisible (0x0)", id);

            return std::make_shared<CRectNode>(id, pos, Vector2D{wOpt.value_or(0.0), hOpt.value_or(0.0)}, color, rounding, borderColor, borderWidth);
        }

        PWidget buildImageWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos) {
            const auto path        = requireFieldString(L, idx, "path", "hyprlui.Image");
            const auto color       = parseColorField(L, idx, "color", CHyprColor{0.0, 0.0, 0.0, 0.0}, "hyprlui.Image");
            const int  rounding    = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
            const auto borderColor = parseGradientField(L, idx, "borderColor", CHyprColor{}, "hyprlui.Image");
            const int  borderWidth = static_cast<int>(fieldNumber(L, idx, "borderWidth", 0));
            auto       image       = std::make_shared<CImageWidget>(id, pos, path, color, rounding, borderColor, borderWidth);
            if (!image->loaded())
                Log::logger->log(Log::WARN, "[hyprlui] Image '{}': failed to load '{}' - drawing fill/border only", id, path);
            return image;
        }

        // Sugar over a plain CRectNode with a computed w/h.
        PWidget buildDividerWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos) {
            const double thickness   = fieldNumber(L, idx, "thickness", 1);
            const double length      = requireFieldNumber(L, idx, "length", "hyprlui.Divider");
            const auto   orientation = optFieldString(L, idx, "orientation", "horizontal");
            const auto   color       = parseColorField(L, idx, "color", CHyprColor{0.5, 0.5, 0.5, 0.5}, "hyprlui.Divider");

            Vector2D     size;
            if (orientation == "horizontal")
                size = Vector2D{length, thickness};
            else if (orientation == "vertical")
                size = Vector2D{thickness, length};
            else
                luaL_error(L, "hyprlui.Divider: 'orientation' must be 'horizontal' or 'vertical', got '%s'", orientation.c_str());

            return std::make_shared<CRectNode>(id, pos, size, color, 0);
        }

        PWidget buildButtonWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos) {
            const double w           = requireFieldNumber(L, idx, "w", "hyprlui.Button");
            const double h           = requireFieldNumber(L, idx, "h", "hyprlui.Button");
            const auto   color       = parseColorField(L, idx, "color", CHyprColor{0.2, 0.2, 0.2, 1.0}, "hyprlui.Button");
            const int    rounding    = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
            const auto   borderColor = parseGradientField(L, idx, "borderColor", CHyprColor{}, "hyprlui.Button");
            const int    borderWidth = static_cast<int>(fieldNumber(L, idx, "borderWidth", 0));
            // onClick is parsed generically in applyCommonWidgetProperties()
            // below - CWidget's own field, not Button-specific.
            return std::make_shared<CButtonWidget>(id, pos, Vector2D{w, h}, color, rounding, borderColor, borderWidth);
        }

        PWidget buildInputWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos) {
            const double w           = requireFieldNumber(L, idx, "w", "hyprlui.Input");
            const double h           = requireFieldNumber(L, idx, "h", "hyprlui.Input");
            const auto   color       = parseColorField(L, idx, "color", CHyprColor{0.15, 0.15, 0.15, 1.0}, "hyprlui.Input");
            const int    rounding    = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
            const auto   borderColor = parseGradientField(L, idx, "borderColor", CHyprColor{}, "hyprlui.Input");
            const int    borderWidth = static_cast<int>(fieldNumber(L, idx, "borderWidth", 0));
            const auto   text        = optFieldString(L, idx, "text", "");
            const auto   textColor   = parseColorField(L, idx, "textColor", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Input");
            const int    textSize    = static_cast<int>(fieldNumber(L, idx, "textSize", 14));
            const auto   textFont    = optFieldString(L, idx, "textFont", "sans");
            auto         onKey       = fieldOnKey(L, idx);
            auto         onChange    = fieldOnChange(L, idx);
            auto         onFocus     = fieldZeroArgFn(L, idx, "onFocus");
            auto         onBlur      = fieldZeroArgFn(L, idx, "onBlur");

            auto         input = std::make_shared<CInputWidget>(id, pos, Vector2D{w, h}, color, rounding, text, textColor, textSize, textFont, borderColor, borderWidth);
            if (onKey)
                input->setOnKey(std::move(onKey));
            if (onChange)
                input->setOnChange(std::move(onChange));
            if (onFocus)
                input->setOnFocus(std::move(onFocus));
            if (onBlur)
                input->setOnBlur(std::move(onBlur));
            return input;
        }

        PWidget buildCheckboxWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos) {
            const double w            = requireFieldNumber(L, idx, "w", "hyprlui.Checkbox");
            const double h            = requireFieldNumber(L, idx, "h", "hyprlui.Checkbox");
            const auto   color        = parseColorField(L, idx, "color", CHyprColor{0.2, 0.2, 0.2, 1.0}, "hyprlui.Checkbox");
            const auto   checkedColor = parseColorField(L, idx, "checkedColor", CHyprColor{0.3, 0.6, 1.0, 1.0}, "hyprlui.Checkbox");
            const int    rounding     = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
            const auto   borderColor  = parseGradientField(L, idx, "borderColor", CHyprColor{}, "hyprlui.Checkbox");
            const int    borderWidth  = static_cast<int>(fieldNumber(L, idx, "borderWidth", 0));
            const bool   checked      = optFieldBool(L, idx, "checked", false);
            auto         onChange     = fieldOnChangeBool(L, idx);

            auto         checkbox = std::make_shared<CCheckboxWidget>(id, pos, Vector2D{w, h}, color, checkedColor, rounding, checked, borderColor, borderWidth);
            if (onChange)
                checkbox->setOnChange(std::move(onChange));
            return checkbox;
        }

        PWidget buildTextWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos, std::vector<std::function<void()>>& bindings) {
            const auto  bindName = fieldBindName(L, idx, "text");
            std::string text;
            if (bindName) {
                if (!CWatcherManager::get().hasWatcher(*bindName))
                    luaL_error(L, "hyprlui.Text: text is bound to unknown watcher '%s' - call hyprlui.watch() before referencing it", bindName->c_str());
                text = CWatcherManager::get().currentValue(*bindName);
            } else {
                text = requireFieldString(L, idx, "text", "hyprlui.Text");
            }
            const int     size        = static_cast<int>(fieldNumber(L, idx, "size", 16));
            const auto    color       = parseColorField(L, idx, "color", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Text");
            const auto    font        = optFieldString(L, idx, "font", "sans");
            const auto    overflowStr = optFieldString(L, idx, "overflow", "ellipsis");
            ETextOverflow overflow    = ETextOverflow::Ellipsis;
            if (overflowStr == "clip")
                overflow = ETextOverflow::Clip;
            else if (overflowStr != "ellipsis")
                luaL_error(L, "hyprlui.Text: 'overflow' must be 'ellipsis' or 'clip', got '%s'", overflowStr.c_str());

            // `marquee = true` for defaults, or a table to override
            // pauseMs/speed - same boolean-or-table shape debugShow uses
            // above. `bezier`/`spring`, if given, are resolved like
            // animationIn/animationOut's own; omitted means linear motion.
            std::optional<SMarqueeSpec> marquee;
            lua_getfield(L, idx, "marquee");
            if (lua_istable(L, -1)) {
                const int    marqueeIdx = lua_gettop(L);
                SMarqueeSpec spec;
                spec.pauseMs       = fieldNumber(L, marqueeIdx, "pauseMs", spec.pauseMs);
                spec.speedPxPerSec = fieldNumber(L, marqueeIdx, "speed", spec.speedPxPerSec);

                lua_getfield(L, marqueeIdx, "bezier");
                const bool hasBezier = !lua_isnil(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, marqueeIdx, "spring");
                const bool hasSpring = !lua_isnil(L, -1);
                lua_pop(L, 1);
                if (hasBezier || hasSpring)
                    spec.curve = resolveCurveField(L, marqueeIdx, "hyprlui.Text: field 'marquee'");

                marquee = spec;
            } else if (lua_isboolean(L, -1)) {
                if (lua_toboolean(L, -1))
                    marquee = SMarqueeSpec{};
            } else if (!lua_isnil(L, -1)) {
                luaL_error(L, "hyprlui.Text: 'marquee' must be a boolean or a table");
            }
            lua_pop(L, 1);

            auto textNode = std::make_shared<CTextNode>(id, pos, text, size, color, font, overflow, marquee);

            if (bindName) {
                // Captures the raw CTextNode* (not the shared_ptr) - the
                // closure lives on the CCanvas, which itself owns the
                // widget tree for at least as long, so the pointer stays
                // valid for the closure's whole lifetime.
                auto* rawNode = textNode.get();
                bindings.emplace_back([rawNode, name = *bindName]() { rawNode->setText(CWatcherManager::get().currentValue(name)); });
            }

            return textNode;
        }

        PWidget buildFlexWidget(lua_State* L, int idx, const std::string& id, const Vector2D& pos, const std::string& type) {
            const double gap      = fieldNumber(L, idx, "gap", 0);
            const auto   alignStr = optFieldString(L, idx, "align", "start");
            EAlign       align    = EAlign::Start;
            if (alignStr == "center")
                align = EAlign::Center;
            else if (alignStr == "end")
                align = EAlign::End;
            else if (alignStr != "start")
                luaL_error(L, "hyprlui.%s: 'align' must be 'start', 'center', or 'end', got '%s'", type.c_str(), alignStr.c_str());
            return std::make_shared<CFlexWidget>(id, pos, type == "row" ? EFlexDirection::Row : EFlexDirection::Column, gap, align);
        }

        // Base widget properties, shared across every widget type via
        // CWidget itself - parsed generically here rather than per-type
        // above. Each is only applied if the Lua spec actually mentions it
        // (see optInsetsField()/optFixedField()), so a widget's own
        // constructor-chosen default isn't silently zeroed out.
        void applyCommonWidgetProperties(lua_State* L, int idx, const PWidget& widget, const std::string& type) {
            if (auto padding = optInsetsField(L, idx, "padding", "hyprlui"))
                widget->setPadding(*padding);
            if (auto margin = optInsetsField(L, idx, "margin", "hyprlui"))
                widget->setMargin(*margin);

            const auto minW = optFixedField(L, idx, "minW");
            const auto minH = optFixedField(L, idx, "minH");
            if (minW || minH)
                widget->setMinSize(minW, minH);

            const auto maxW = optFixedField(L, idx, "maxW");
            const auto maxH = optFixedField(L, idx, "maxH");
            if (maxW || maxH)
                widget->setMaxSize(maxW, maxH);

            if (auto opacity = optFixedField(L, idx, "opacity"))
                widget->setOpacity(*opacity);
            if (auto zIndex = optFixedField(L, idx, "zIndex"))
                widget->setZIndex(static_cast<int>(*zIndex));

            // Debug overlay - `debug` is tri-state (absent = inherit from
            // the nearest ancestor that hasn't walled itself off via
            // debugCascade below); `debugShow` force-overrides individual
            // detail categories, bypassing the size-based "auto" default.
            SDebugSpec debugSpec;
            debugSpec.enabled = optFieldBoolOpt(L, idx, "debug");
            lua_getfield(L, idx, "debugShow");
            if (lua_istable(L, -1)) {
                const int showIdx       = lua_gettop(L);
                debugSpec.showBox       = optFieldBoolOpt(L, showIdx, "box");
                debugSpec.showPadding   = optFieldBoolOpt(L, showIdx, "padding");
                debugSpec.showMargin    = optFieldBoolOpt(L, showIdx, "margin");
                debugSpec.showId        = optFieldBoolOpt(L, showIdx, "id");
                debugSpec.showSize      = optFieldBoolOpt(L, showIdx, "size");
                debugSpec.showZOpacity  = optFieldBoolOpt(L, showIdx, "zOpacity");
                debugSpec.showHitTarget = optFieldBoolOpt(L, showIdx, "hitTarget");
            } else if (!lua_isnil(L, -1)) {
                luaL_error(L, "hyprlui: field 'debugShow' must be a table");
            }
            lua_pop(L, 1);
            if (auto fontSize = optFixedField(L, idx, "debugFontSize"))
                debugSpec.fontSize = static_cast<int>(*fontSize);
            widget->setDebug(debugSpec);
            widget->setDebugCascade(optFieldBool(L, idx, "debugCascade", true));

            // Interactive state - hoverColor/disabledColor/onHoverStart/
            // onHoverEnd/onScroll/disabled only matter for a widget whose
            // isInteractive() is true, but are parsed generically here; a
            // decorative widget setting them does nothing. onClick is
            // different - it's what actually MAKES a plain widget
            // interactive in the first place.
            widget->setDisabled(optFieldBool(L, idx, "disabled", false));
            if (auto hoverColor = optColorField(L, idx, "hoverColor", "hyprlui"))
                widget->setHoverColor(hoverColor);
            if (auto disabledColor = optColorField(L, idx, "disabledColor", "hyprlui"))
                widget->setDisabledColor(disabledColor);
            if (auto onHoverStart = fieldZeroArgFn(L, idx, "onHoverStart"))
                widget->setOnHoverStart(std::move(onHoverStart));
            if (auto onHoverEnd = fieldZeroArgFn(L, idx, "onHoverEnd"))
                widget->setOnHoverEnd(std::move(onHoverEnd));
            if (auto onScroll = fieldOnScroll(L, idx))
                widget->setOnScroll(std::move(onScroll));
            if (auto onClick = fieldOnClick(L, idx))
                widget->setOnClick(std::move(onClick));

            // nullptr (the common case) leaves this widget on the global
            // hyprlui.animation() config for that leaf.
            widget->setAnimationInOverride(optAnimationOverrideField(L, idx, "animationIn", "hyprlui"));
            widget->setAnimationOutOverride(optAnimationOverrideField(L, idx, "animationOut", "hyprlui"));
            widget->setLayoutAnimation(optAnimationOverrideField(L, idx, "animationLayout", "hyprlui"));

            // Fixed-size override - meaningful for containers (whose w/h
            // are genuinely optional) and Image (size-to-content unless
            // overridden). Box's w/h are its actual required dimensions,
            // not an override; Text derives its size from rasterization.
            if (type == "stack" || type == "row" || type == "column" || type == "image")
                widget->setFixedSize(optFixedField(L, idx, "w"), optFixedField(L, idx, "h"));

            widget->setFill(optFieldBool(L, idx, "fill", false));

            // Must run AFTER every size-affecting step above, so a leaf's
            // default measureContent() has the right value to self-correct
            // to every frame regardless of any later `fill` stretch.
            widget->primeNaturalSize();
        }

    } // namespace

    PWidget buildWidget(lua_State* L, int idx, int& autoId, std::vector<std::function<void()>>& bindings, std::unordered_set<std::string>& seenIds, bool inheritedDebug) {
        idx = lua_absindex(L, idx);
        luaL_checktype(L, idx, LUA_TTABLE);

        lua_getfield(L, idx, "__type");
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "hyprlui: expected a widget table (Stack{}/Row{}/Column{}/Text{}/Box{}/Button{}), got a plain table");
        }
        const std::string type = lua_tostring(L, -1);
        lua_pop(L, 1);

        std::string id = optFieldString(L, idx, "id", "");
        if (id.empty())
            id = "__auto" + std::to_string(autoId++);

        if (!seenIds.insert(id).second)
            luaL_error(L,
                       "hyprlui: duplicate widget id '%s' in the same window - ids must be unique within a window (set_text/remove_widget/etc. address a widget by id and only "
                       "ever find the first match)",
                       id.c_str());

        const Vector2D pos{fieldNumber(L, idx, "x", 0), fieldNumber(L, idx, "y", 0)};
        const bool     visible = optFieldBool(L, idx, "visible", true);

        // Mirrors resolveDebugSpec()'s per-frame cascade (Widget.cpp), but
        // computed once here at construction time, for buildBoxWidget()'s
        // warning below - not a substitute for the real per-frame resolve,
        // which still separately drives the debug overlay itself.
        const bool debugEnabled = optFieldBoolOpt(L, idx, "debug").value_or(inheritedDebug);
        const bool childDebug   = optFieldBool(L, idx, "debugCascade", true) ? debugEnabled : false;

        PWidget    widget;

        if (type == "box")
            widget = buildBoxWidget(L, idx, id, pos, debugEnabled);
        else if (type == "image")
            widget = buildImageWidget(L, idx, id, pos);
        else if (type == "divider")
            widget = buildDividerWidget(L, idx, id, pos);
        else if (type == "button")
            widget = buildButtonWidget(L, idx, id, pos);
        else if (type == "input")
            widget = buildInputWidget(L, idx, id, pos);
        else if (type == "checkbox")
            widget = buildCheckboxWidget(L, idx, id, pos);
        else if (type == "text")
            widget = buildTextWidget(L, idx, id, pos, bindings);
        else if (type == "stack")
            widget = std::make_shared<CStackWidget>(id, pos);
        else if (type == "row" || type == "column")
            widget = buildFlexWidget(L, idx, id, pos, type);
        else
            luaL_error(L, "hyprlui: unknown widget type '%s'", type.c_str());

        widget->setVisible(visible);
        applyCommonWidgetProperties(L, idx, widget, type);

        // Children: positional (ipairs-style) table entries.
        const auto n = lua_rawlen(L, idx);
        for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
            lua_rawgeti(L, idx, i);
            if (lua_istable(L, -1))
                widget->addChild(buildWidget(L, lua_gettop(L), autoId, bindings, seenIds, childDebug));
            lua_pop(L, 1);
        }

        return widget;
    }

} // namespace HyprLUI::Lua
