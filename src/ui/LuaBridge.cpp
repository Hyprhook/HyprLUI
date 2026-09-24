#include "LuaBridge.hpp"
#include "UIManager.hpp"
#include "WidgetBuilders.hpp"
#include "TextNode.hpp"
#include "InputWidget.hpp"
#include "ImageWidget.hpp"
#include "CheckboxWidget.hpp"
#include "ComponentRegistry.hpp"
#include "parser/ValueParsers.hpp"
#include "parser/ShorthandParsers.hpp"
#include "parser/AnimationParsers.hpp"
#include "../reactive/Watcher.hpp"
#include "../reserved/ReservedAreaComposer.hpp"
#include "../persistence/PersistenceStore.hpp"
#include "../services/NativeServices.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

// This Lua build's headers don't guard their declarations with
// `extern "C"` themselves (only LUAMOD_API does) - without wrapping the
// include here, every lua_*/luaL_* call site gets C++-mangled, which then
// fails to resolve against liblua's plain C exports at plugin load time.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace HyprLUI::Lua {

    namespace {

        // Tags a widget's table argument with a __type field and hands it
        // straight back - the actual tree gets built later, once the
        // whole thing reaches window{}. This is what makes
        // `Column{ gap = 8, Text{...} }` work: Text{} evaluates first and
        // its tagged return value becomes positional entry [1] of the
        // Column table via ordinary Lua table-constructor semantics.
        int tagWidget(lua_State* L, const char* type) {
            luaL_checktype(L, 1, LUA_TTABLE);
            lua_pushstring(L, type);
            lua_setfield(L, 1, "__type");
            lua_pushvalue(L, 1);
            return 1;
        }

        int luaStack(lua_State* L) {
            return tagWidget(L, "stack");
        }
        int luaRow(lua_State* L) {
            return tagWidget(L, "row");
        }
        int luaColumn(lua_State* L) {
            return tagWidget(L, "column");
        }
        int luaText(lua_State* L) {
            return tagWidget(L, "text");
        }
        int luaBox(lua_State* L) {
            return tagWidget(L, "box");
        }
        int luaButton(lua_State* L) {
            return tagWidget(L, "button");
        }
        int luaInput(lua_State* L) {
            return tagWidget(L, "input");
        }
        int luaImage(lua_State* L) {
            return tagWidget(L, "image");
        }
        int luaDivider(lua_State* L) {
            return tagWidget(L, "divider");
        }
        int luaCheckbox(lua_State* L) {
            return tagWidget(L, "checkbox");
        }

        // hyprlui.Bind(name) takes a plain string, not a table - wraps it
        // into a {__bind = name} marker table so buildWidget() (via
        // fieldBindName()) can tell "track watcher `name`" apart from "the
        // field is literally the string `name`".
        int luaBind(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            lua_newtable(L);
            lua_pushstring(L, name.c_str());
            lua_setfield(L, -2, "__bind");
            return 1;
        }

        // Maps a Lua-facing anchor string onto EAnchor. luaL_error (never
        // returns) on anything else.
        EAnchor parseAnchor(lua_State* L, const std::string& anchorStr) {
            if (anchorStr == "top-left")
                return EAnchor::TopLeft;
            if (anchorStr == "top")
                return EAnchor::Top;
            if (anchorStr == "top-right")
                return EAnchor::TopRight;
            if (anchorStr == "left")
                return EAnchor::Left;
            if (anchorStr == "center")
                return EAnchor::Center;
            if (anchorStr == "right")
                return EAnchor::Right;
            if (anchorStr == "bottom-left")
                return EAnchor::BottomLeft;
            if (anchorStr == "bottom")
                return EAnchor::Bottom;
            if (anchorStr == "bottom-right")
                return EAnchor::BottomRight;

            luaL_error(L, "hyprlui.window: 'anchor' must be one of top-left/top/top-right/left/center/right/bottom-left/bottom/bottom-right, got '%s'", anchorStr.c_str());
            return EAnchor::TopLeft; // unreachable - silences -Wreturn-type
        }

        // Maps a Lua-facing edge string onto EEdge for `exclusive`.
        // luaL_error (never returns) on anything else.
        EEdge parseEdge(lua_State* L, const std::string& edgeStr) {
            if (edgeStr == "top")
                return EEdge::Top;
            if (edgeStr == "right")
                return EEdge::Right;
            if (edgeStr == "bottom")
                return EEdge::Bottom;
            if (edgeStr == "left")
                return EEdge::Left;

            luaL_error(L, "hyprlui.window: 'exclusive' must be one of top/right/bottom/left, got '%s'", edgeStr.c_str());
            return EEdge::Top; // unreachable - silences -Wreturn-type
        }

        // Whether reserving `edge` makes sense for a window anchored at
        // `anchor` (e.g. anchor="top" + exclusive="left" is nonsensical -
        // the window isn't near the left edge). A corner anchor touches
        // two edges, so either is allowed; "center" allows neither.
        bool anchorAllowsExclusiveEdge(EAnchor anchor, EEdge edge) {
            switch (anchor) {
                case EAnchor::Top: return edge == EEdge::Top;
                case EAnchor::TopRight: return edge == EEdge::Top || edge == EEdge::Right;
                case EAnchor::Right: return edge == EEdge::Right;
                case EAnchor::BottomRight: return edge == EEdge::Bottom || edge == EEdge::Right;
                case EAnchor::Bottom: return edge == EEdge::Bottom;
                case EAnchor::BottomLeft: return edge == EEdge::Bottom || edge == EEdge::Left;
                case EAnchor::Left: return edge == EEdge::Left;
                case EAnchor::TopLeft: return edge == EEdge::Top || edge == EEdge::Left;
                case EAnchor::Center: return false;
            }
            return false; // unreachable
        }

        int luaWindow(lua_State* L) {
            luaL_checktype(L, 1, LUA_TTABLE);

            // Auto-generated if omitted, like a widget's own id.
            static int s_nextWindowId = 0;
            auto       name           = optFieldString(L, 1, "name", "");
            if (name.empty())
                name = "__window" + std::to_string(s_nextWindowId++);
            lua_pushstring(L, name.c_str());
            lua_setfield(L, 1, "name");

            const auto x              = fieldNumber(L, 1, "x", 0);
            const auto y              = fieldNumber(L, 1, "y", 0);
            const auto fw             = optFixedField(L, 1, "w");
            const auto fh             = optFixedField(L, 1, "h");
            const bool hotReload      = optFieldBool(L, 1, "hotReload", false);
            const auto zStr           = optFieldString(L, 1, "zorder", "overlay");
            const auto anchorStr      = optFieldString(L, 1, "anchor", "");
            const auto monitorStr     = optFieldString(L, 1, "monitor", "");
            const auto exclusiveStr   = optFieldString(L, 1, "exclusive", "");
            const bool spanWidth      = optFieldBool(L, 1, "spanWidth", false);
            const bool spanHeight     = optFieldBool(L, 1, "spanHeight", false);
            const auto monitorPadding = optInsetsField(L, 1, "monitorPadding", "hyprlui.window").value_or(SEdgeInsets{});

            if (!exclusiveStr.empty() && anchorStr.empty())
                return luaL_error(L,
                                  "hyprlui.window: 'exclusive' requires 'anchor' - a reserved zone needs a resolved target monitor, and anchor is currently the only "
                                  "thing that gives us one");

            if ((spanWidth || spanHeight) && anchorStr.empty())
                return luaL_error(L, "hyprlui.window: 'spanWidth'/'spanHeight' require 'anchor' - spanning needs a resolved target monitor to span against");

            EZOrder zorder = EZOrder::Overlay;
            if (zStr == "background")
                zorder = EZOrder::Background;
            else if (zStr != "overlay")
                return luaL_error(L, "hyprlui.window: 'zorder' must be 'overlay' or 'background', got '%s'", zStr.c_str());

            auto& mgr = CUIManager::get();
            if (mgr.hasCanvas(name))
                return luaL_error(L, "hyprlui.window: a window named '%s' already exists", name.c_str());

            PWidget                            root;
            int                                autoId = 0;
            std::vector<std::function<void()>> bindings;
            std::unordered_set<std::string>    seenIds;
            const auto                         n = lua_rawlen(L, 1);
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
                lua_rawgeti(L, 1, i);
                if (lua_istable(L, -1)) {
                    root = buildWidget(L, lua_gettop(L), autoId, bindings, seenIds);
                    lua_pop(L, 1);
                    break;
                }
                lua_pop(L, 1);
            }

            if (!root)
                return luaL_error(L, "hyprlui.window: needs exactly one root widget (Stack/Row/Column/Text/Box)");

            root->setFixedSize(fw, fh);
            root->measure();

            const Vector2D size{fw ? *fw : root->size().x, fh ? *fh : root->size().y};

            // hotReload restores the last recorded visibility instead of
            // always opening visible - seeds `true` on a first-ever run.
            const auto resolveInitialVisible = [&]() -> bool {
                if (!hotReload)
                    return true;
                const bool v = mgr.hotReloadVisibility(name).value_or(true);
                mgr.setHotReloadVisibility(name, v);
                return v;
            };

            // No anchor: x/y are a raw global position.
            if (anchorStr.empty()) {
                auto canvas = mgr.createCanvas(name, {x, y}, size, zorder);
                canvas->setFixedSize(fw, fh);
                canvas->setHotReload(hotReload);
                for (auto& binding : bindings)
                    canvas->addBinding(std::move(binding));
                canvas->setRoot(root);
                // Primed hidden first - a plain setVisible(true) alone
                // would have nothing to animate FROM.
                root->primeHidden();
                root->setVisible(resolveInitialVisible());
                canvas->damage();
                lua_pushvalue(L, 1);
                return 1;
            }

            // With `anchor`, x/y are reinterpreted as an offset from the
            // anchor point, not a global position (see docs/api.md).
            // `monitor` is resolved once here, by name - see
            // CCanvas::recomputeAnchorPosition() for the per-frame
            // re-read of that monitor's live box/reserved area.
            const EAnchor anchor = parseAnchor(L, anchorStr);

            // Validated BEFORE createCanvas() below - erroring after the
            // canvas already exists would leave an orphaned, empty,
            // undamaged entry registered under `name` forever (hasCanvas()
            // would keep saying "taken", with no root ever shown).
            std::optional<EEdge> exclusiveEdge;
            if (!exclusiveStr.empty()) {
                exclusiveEdge = parseEdge(L, exclusiveStr);
                if (!anchorAllowsExclusiveEdge(anchor, *exclusiveEdge))
                    return luaL_error(L,
                                      "hyprlui.window: anchor '%s' with exclusive '%s' doesn't make sense - a window has to actually be at (or in a corner "
                                      "touching) the edge it reserves",
                                      anchorStr.c_str(), exclusiveStr.c_str());
            }

            auto monitor = monitorStr.empty() ? Desktop::focusState()->monitor() :
                                                State::CMonitorQuery{*State::monitorState()}.relativeTo(Desktop::focusState()->monitor()).configString(monitorStr).run();
            if (!monitor)
                monitor = Desktop::focusState()->monitor();
            if (!monitor)
                return luaL_error(L, "hyprlui.window: no monitor available to anchor '%s' against", name.c_str());

            // Applied here too (not just every render() frame via
            // CCanvas::resolveSpan()) so exclusive-zone seeding below sees
            // the correct size from frame 1, not one frame late.
            Vector2D spannedSize = size;
            if (spanWidth)
                spannedSize.x = monitor->logicalBox().size().x - monitorPadding.left - monitorPadding.right;
            if (spanHeight)
                spannedSize.y = monitor->logicalBox().size().y - monitorPadding.top - monitorPadding.bottom;

            auto canvas = mgr.createCanvas(name, {0, 0}, spannedSize, zorder);
            canvas->setFixedSize(fw, fh);
            canvas->setHotReload(hotReload);
            canvas->setAnchor(anchor, std::string{monitor->name()}, {x, y});
            canvas->setSpan(spanWidth, spanHeight, monitorPadding);

            // Must happen BEFORE recomputeAnchorPosition() below, so this
            // window positions itself correctly from the very first frame.
            if (exclusiveEdge)
                canvas->setExclusive(*exclusiveEdge);

            canvas->recomputeAnchorPosition();

            for (auto& binding : bindings)
                canvas->addBinding(std::move(binding));
            canvas->setRoot(root);

            if (exclusiveEdge) {
                const EEdge       edge        = *exclusiveEdge;
                const std::string monitorName = std::string{monitor->name()};

                // Tracks this window's LIVE size (e.g. Bind()ed content
                // growing/shrinking) - CCanvas itself stays unaware
                // exclusive zones even exist.
                auto sizeAlong = [edge](const Vector2D& sz) -> double { return (edge == EEdge::Top || edge == EEdge::Bottom) ? sz.y : sz.x; };

                CReservedAreaComposer::get().setContribution(name, monitorName, edge, sizeAlong(canvas->size()));
                canvas->setOnSizeChanged(
                    [name, monitorName, edge, sizeAlong](const Vector2D& newSize) { CReservedAreaComposer::get().setContribution(name, monitorName, edge, sizeAlong(newSize)); });
            }

            root->primeHidden();
            root->setVisible(resolveInitialVisible());
            canvas->damage();
            lua_pushvalue(L, 1);
            return 1;
        }

        int luaRemoveCanvas(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            auto&             mgr  = CUIManager::get();
            // Unlike a reload's own automatic wipe, an explicit close should stick.
            if (auto canvas = mgr.getCanvas(name); canvas && canvas->hotReload())
                mgr.setHotReloadVisibility(name, false);
            mgr.removeCanvas(name);
            CReservedAreaComposer::get().removeContribution(name); // no-op if `name` never had one
            return 0;
        }

        int luaSetCanvasVisible(lua_State* L) {
            const std::string name    = luaL_checkstring(L, 1);
            const bool        visible = lua_toboolean(L, 2);

            auto&             mgr    = CUIManager::get();
            auto              canvas = mgr.getCanvas(name);
            if (!canvas)
                return luaL_error(L, "hyprlui.set_canvas_visible: no window named '%s'", name.c_str());

            // A hidden Input can't visibly be typed into, so it shouldn't
            // silently keep keyboard focus either.
            if (!visible && mgr.isCanvasFocused(name))
                mgr.blurFocusedInput();

            canvas->setVisible(visible);
            canvas->damage();
            CReservedAreaComposer::get().setActive(name, visible); // a hidden exclusive window reserves nothing
            if (canvas->hotReload())
                mgr.setHotReloadVisibility(name, visible);
            return 0;
        }

        // Repositions an already-created window to an explicit global
        // position - clears any anchor first (an explicit position and an
        // anchor are mutually exclusive), so calling this on an anchored
        // window makes it stop tracking that anchor from here on.
        int luaSetCanvasPosition(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            const double      x    = luaL_checknumber(L, 2);
            const double      y    = luaL_checknumber(L, 3);

            auto              canvas = CUIManager::get().getCanvas(name);
            if (!canvas)
                return luaL_error(L, "hyprlui.set_canvas_position: no window named '%s'", name.c_str());

            canvas->moveTo({x, y});
            return 0;
        }

        // Resizes an already-created window; nil for either axis lets it
        // size-to-content again. No explicit damage() call needed -
        // CCanvas::render()'s content-size sync picks this up next frame.
        int luaSetCanvasSize(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);

            auto              canvas = CUIManager::get().getCanvas(name);
            if (!canvas)
                return luaL_error(L, "hyprlui.set_canvas_size: no window named '%s'", name.c_str());

            std::optional<double> w, h;
            if (!lua_isnil(L, 2))
                w = luaL_checknumber(L, 2);
            if (!lua_isnil(L, 3))
                h = luaL_checknumber(L, 3);

            if ((w && *w <= 0) || (h && *h <= 0))
                return luaL_error(L, "hyprlui.set_canvas_size: w/h must be greater than 0");

            canvas->setFixedSize(w, h);
            return 0;
        }

        int luaSetText(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            const std::string text       = luaL_checkstring(L, 3);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_text: no window named '%s'", canvasName.c_str());

            auto* textNode = dynamic_cast<CTextNode*>(canvas->root()->findWidget(id));
            if (!textNode)
                return luaL_error(L, "hyprlui.set_text: no text widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            textNode->setText(text);
            canvas->damage();
            return 0;
        }

        int luaSetWidgetVisible(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            const bool        visible    = lua_toboolean(L, 3);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_widget_visible: no window named '%s'", canvasName.c_str());

            auto* widget = canvas->root()->findWidget(id);
            if (!widget)
                return luaL_error(L, "hyprlui.set_widget_visible: no widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            // Hiding a focused/hovered widget blurs/un-hovers it first,
            // rather than leaving it invisible but still silently focused.
            if (!visible) {
                if (CUIManager::get().isFocused(canvasName, id))
                    CUIManager::get().blurFocusedInput();
                if (CUIManager::get().isHovered(canvasName, id))
                    CUIManager::get().updateHover({});
            }

            widget->setVisible(visible);
            canvas->damage();
            return 0;
        }

        int luaSetWidgetDisabled(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            const bool        disabled   = lua_toboolean(L, 3);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_widget_disabled: no window named '%s'", canvasName.c_str());

            auto* widget = canvas->root()->findWidget(id);
            if (!widget)
                return luaL_error(L, "hyprlui.set_widget_disabled: no widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            // A widget that becomes disabled can't stay focused/hovered -
            // hitTest() excludes it from here on, so drop that state now.
            if (disabled) {
                if (CUIManager::get().isFocused(canvasName, id))
                    CUIManager::get().blurFocusedInput();
                if (CUIManager::get().isHovered(canvasName, id))
                    CUIManager::get().updateHover({});
            }

            widget->setDisabled(disabled);
            canvas->damage();
            return 0;
        }

        // Resizes a single widget within an existing window; nil for
        // either axis lets it size-to-content again - the same mechanism
        // every fixed-size-capable widget already exposes at construction,
        // one level down from hyprlui.set_canvas_size().
        int luaSetWidgetSize(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_widget_size: no window named '%s'", canvasName.c_str());

            auto* widget = canvas->root()->findWidget(id);
            if (!widget)
                return luaL_error(L, "hyprlui.set_widget_size: no widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            std::optional<double> w, h;
            if (!lua_isnil(L, 3))
                w = luaL_checknumber(L, 3);
            if (!lua_isnil(L, 4))
                h = luaL_checknumber(L, 4);

            if ((w && *w <= 0) || (h && *h <= 0))
                return luaL_error(L, "hyprlui.set_widget_size: w/h must be greater than 0");

            widget->setFixedSize(w, h);
            canvas->damage();
            return 0;
        }

        int luaSetInputText(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            const std::string text       = luaL_checkstring(L, 3);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_input_text: no window named '%s'", canvasName.c_str());

            auto* input = dynamic_cast<CInputWidget*>(canvas->root()->findWidget(id));
            if (!input)
                return luaL_error(L, "hyprlui.set_input_text: no Input widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            input->setText(text);
            canvas->damage();
            return 0;
        }

        int luaGetInputText(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.get_input_text: no window named '%s'", canvasName.c_str());

            auto* input = dynamic_cast<CInputWidget*>(canvas->root()->findWidget(id));
            if (!input)
                return luaL_error(L, "hyprlui.get_input_text: no Input widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            lua_pushlstring(L, input->text().data(), input->text().size());
            return 1;
        }

        int luaSetImage(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            const std::string path       = luaL_checkstring(L, 3);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_image: no window named '%s'", canvasName.c_str());

            auto* image = dynamic_cast<CImageWidget*>(canvas->root()->findWidget(id));
            if (!image)
                return luaL_error(L, "hyprlui.set_image: no Image widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            image->setImage(path);
            if (!image->loaded())
                Log::logger->log(Log::WARN, "[hyprlui] set_image: failed to load '{}' for widget '{}' - drawing nothing", path, id);
            canvas->damage();
            return 0;
        }

        int luaSetCheckboxChecked(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            const bool        checked    = lua_toboolean(L, 3);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.set_checkbox_checked: no window named '%s'", canvasName.c_str());

            auto* checkbox = dynamic_cast<CCheckboxWidget*>(canvas->root()->findWidget(id));
            if (!checkbox)
                return luaL_error(L, "hyprlui.set_checkbox_checked: no Checkbox widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            checkbox->setChecked(checked);
            canvas->damage();
            return 0;
        }

        int luaGetCheckboxChecked(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.get_checkbox_checked: no window named '%s'", canvasName.c_str());

            auto* checkbox = dynamic_cast<CCheckboxWidget*>(canvas->root()->findWidget(id));
            if (!checkbox)
                return luaL_error(L, "hyprlui.get_checkbox_checked: no Checkbox widget '%s' in window '%s'", id.c_str(), canvasName.c_str());

            lua_pushboolean(L, checkbox->checked());
            return 1;
        }

        int luaRemoveWidget(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.remove_widget: no window named '%s'", canvasName.c_str());

            // Blur BEFORE the widget is actually torn down below, if it
            // currently holds keyboard focus.
            if (CUIManager::get().isFocused(canvasName, id))
                CUIManager::get().blurFocusedInput();

            auto* widget = canvas->root()->findWidget(id);
            if (!widget) {
                canvas->damage(); // matches removeChild()'s own prior no-op-if-missing behavior
                return 0;
            }

            // Animates the widget out first (if "out" is enabled) and only
            // erases it from the tree once that finishes; immediate
            // otherwise. `canvas` (shared ownership) is captured so it
            // can't be destroyed out from under the deferred callback.
            widget->animateOutThenRemove([canvas, id]() {
                if (canvas->root())
                    canvas->root()->removeChild(id);
                canvas->damage();
            });
            canvas->damage();
            return 0;
        }

        // Adds one widget (built the same way window{}'s own root/children
        // are, via buildWidget()) to an existing window's tree at runtime,
        // without rebuilding the whole thing. Appends only - paints/lays
        // out after whatever the parent already has, matching addChild()'s
        // own append-only semantics; no insert-at-index/prepend yet, add
        // if something actually needs it.
        int luaAddWidget(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string parentId   = luaL_checkstring(L, 2);
            luaL_checktype(L, 3, LUA_TTABLE);

            auto canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.add_widget: no window named '%s'", canvasName.c_str());

            auto* parent = canvas->root()->findWidget(parentId);
            if (!parent)
                return luaL_error(L, "hyprlui.add_widget: no widget named '%s' on window '%s'", parentId.c_str(), canvasName.c_str());

            // Seeded with every id already in the tree, not just the new
            // subtree - buildWidget()'s own duplicate-id check then also
            // catches a collision against an EXISTING widget for free (the
            // same error a static duplicate-id-within-one-window{} call
            // already gives).
            std::unordered_set<std::string> seenIds;
            canvas->root()->collectIds(seenIds);

            int                                autoId = 0;
            std::vector<std::function<void()>> bindings;
            auto                               child = buildWidget(L, 3, autoId, bindings, seenIds);

            for (auto& b : bindings)
                canvas->addBinding(std::move(b));

            // Primed hidden first, same as window{}'s own root - a plain
            // setVisible(true) alone would have nothing to animate FROM,
            // so animationIn would never fire (child built via buildWidget()
            // starts m_visible=true already, unlike a fresh window root).
            child->primeHidden();
            child->setVisible(true);

            parent->addChild(std::move(child));
            canvas->damage();
            return 0;
        }

        int luaWatch(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            luaL_checktype(L, 2, LUA_TFUNCTION);

            std::optional<int> intervalMs;
            if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
                luaL_checktype(L, 3, LUA_TTABLE);
                lua_getfield(L, 3, "interval");
                if (!lua_isnil(L, -1))
                    intervalMs = static_cast<int>(luaL_checknumber(L, -1));
                lua_pop(L, 1);
            }

            lua_pushvalue(L, 2);
            const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);

            CWatcherManager::get().registerWatcher(L, name, fnRef, intervalMs);
            return 0;
        }

        int luaNotify(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            if (!CWatcherManager::get().notify(name))
                return luaL_error(L, "hyprlui.notify: no watcher named '%s'", name.c_str());
            return 0;
        }

        // Reads a number/string/boolean argument at `idx` for
        // hyprlui.persistent()/its wrapper's :set(). Uses lua_type()
        // (exact tag), NOT lua_isnumber()/lua_isstring() - those are
        // coercion-aware (a numeric-looking string like "123" satisfies
        // lua_isnumber() too), which would misclassify a string value.
        PersistentValue readPersistentValueArg(lua_State* L, int idx, const char* fnName) {
            switch (lua_type(L, idx)) {
                case LUA_TBOOLEAN: return static_cast<bool>(lua_toboolean(L, idx));
                case LUA_TNUMBER: return static_cast<double>(lua_tonumber(L, idx));
                case LUA_TSTRING: return std::string(lua_tostring(L, idx));
                default: luaL_error(L, "%s: value must be a number, string, or boolean", fnName); return false; // unreachable
            }
        }

        void pushPersistentValue(lua_State* L, const PersistentValue& val) {
            std::visit(
                [L](auto&& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, double>)
                        lua_pushnumber(L, v);
                    else if constexpr (std::is_same_v<T, std::string>)
                        lua_pushlstring(L, v.data(), v.size());
                    else if constexpr (std::is_same_v<T, bool>)
                        lua_pushboolean(L, v);
                },
                val);
        }

        // `key` is set via lua_pushcclosure() at the table-construction
        // site in luaPersistent() - `store:get()`/`:set(v)`'s Lua `:`
        // sugar passes `store` as arg 1, which these ignore entirely.
        int luaPersistentGet(lua_State* L) {
            const std::string key = lua_tostring(L, lua_upvalueindex(1));
            pushPersistentValue(L, CPersistenceStore::get().getRaw(key));
            return 1;
        }

        int luaPersistentSet(lua_State* L) {
            const std::string key   = lua_tostring(L, lua_upvalueindex(1));
            const auto        value = readPersistentValueArg(L, 2, "hyprlui.persistent:set");
            CPersistenceStore::get().set(key, value);
            return 0;
        }

        int luaPersistent(lua_State* L) {
            const std::string key = luaL_checkstring(L, 1);
            const auto        def = readPersistentValueArg(L, 2, "hyprlui.persistent");
            CPersistenceStore::get().getOrInit(key, def); // seeds it if absent, warns (doesn't error) on a stored-type mismatch

            lua_newtable(L);
            const int tblIdx = lua_gettop(L);

            lua_pushstring(L, key.c_str());
            lua_pushcclosure(L, luaPersistentGet, 1);
            lua_setfield(L, tblIdx, "get");

            lua_pushstring(L, key.c_str());
            lua_pushcclosure(L, luaPersistentSet, 1);
            lua_setfield(L, tblIdx, "set");

            return 1;
        }

        // Parses arguments and delegates - the actual spawn/poll machinery
        // lives in CNativeServices (see its own header for why it polls
        // instead of CEventLoopManager::doOnReadable()).
        int luaRunCmd(lua_State* L) {
            const std::string cmd = luaL_checkstring(L, 1);
            if (cmd.empty())
                return luaL_error(L, "hyprlui.run_cmd: cmd must not be empty");
            luaL_checktype(L, 2, LUA_TFUNCTION);
            lua_pushvalue(L, 2);
            const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
            CNativeServices::get().runCmd(L, cmd, fnRef);
            return 0;
        }

        int luaOpenSocket(lua_State* L) {
            const std::string path = luaL_checkstring(L, 1);
            if (path.empty())
                return luaL_error(L, "hyprlui.open_socket: path must not be empty");
            luaL_checktype(L, 2, LUA_TFUNCTION);
            lua_pushvalue(L, 2);
            const int fnRef = luaL_ref(L, LUA_REGISTRYINDEX);
            CNativeServices::get().openSocket(L, path, fnRef);
            return 0;
        }

        // hyprlui.animation({leaf="in"|"out", enabled=true, speed, bezier,
        // style}) - the GLOBAL default every widget/window uses unless it
        // sets its own animationIn/animationOut override. Shaped to look
        // like hl.animation()'s own table call, but self-contained -
        // HyprLUI isn't hooked into Hyprland's own animation tree (no
        // public API to register a new leaf node there). `speed` is in
        // deciseconds, same unit as hl.animation().
        int luaAnimation(lua_State* L) {
            luaL_checktype(L, 1, LUA_TTABLE);

            const auto               leaf = requireFieldString(L, 1, "leaf", "hyprlui.animation");

            CWidgetAnimations::EKind kind;
            if (leaf == "in")
                kind = CWidgetAnimations::IN;
            else if (leaf == "out")
                kind = CWidgetAnimations::OUT;
            else
                return luaL_error(L, R"(hyprlui.animation: leaf must be "in" or "out", got "%s")", leaf.c_str());

            const bool enabled = optFieldBool(L, 1, "enabled", true);

            if (!enabled) {
                CWidgetAnimations::get().configure(kind, false, 1.f, "default");
                return 0;
            }

            const double speed = requireFieldNumber(L, 1, "speed", "hyprlui.animation");
            if (speed <= 0)
                return luaL_error(L, "hyprlui.animation(\"%s\"): speed must be greater than 0", leaf.c_str());

            const auto curve = resolveCurveField(L, 1, "hyprlui.animation(\"" + leaf + "\")");
            const auto style = optStyleField(L, 1, "hyprlui.animation(\"" + leaf + "\")");

            CWidgetAnimations::get().configure(kind, true, static_cast<float>(speed), curve, style);
            return 0;
        }

        int luaFocusWidget(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);
            if (!CUIManager::get().focusWidget(canvasName, id))
                return luaL_error(L, "hyprlui.focus_widget: no Input widget '%s' in window '%s'", id.c_str(), canvasName.c_str());
            return 0;
        }

        int luaBlurWidget(lua_State* L) {
            CUIManager::get().blurFocusedInput();
            return 0;
        }

        int luaDefineComponent(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            luaL_checktype(L, 2, LUA_TTABLE);

            lua_getfield(L, 2, "props");
            int schemaIdx = 0;
            if (lua_istable(L, -1))
                schemaIdx = lua_gettop(L); // left on the stack - defineComponent() reads it directly
            else if (!lua_isnil(L, -1))
                return luaL_error(L, "hyprlui.defineComponent('%s'): 'props' must be a table", name.c_str());
            else
                lua_pop(L, 1);

            lua_getfield(L, 2, "render");
            if (!lua_isfunction(L, -1))
                return luaL_error(L, "hyprlui.defineComponent('%s'): missing required field 'render' (a function)", name.c_str());
            const int renderFnRef = luaL_ref(L, LUA_REGISTRYINDEX); // pops the function

            CComponentRegistry::get().defineComponent(L, name, schemaIdx, renderFnRef);

            if (schemaIdx != 0)
                lua_pop(L, 1); // pop the props table we left on the stack above
            return 0;
        }

        int luaComponent(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);

            int               propsIdx = 0;
            if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
                luaL_checktype(L, 2, LUA_TTABLE);
                propsIdx = 2;
            }

            int optsIdx = 0;
            if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
                luaL_checktype(L, 3, LUA_TTABLE);
                optsIdx = 3;
            }

            CComponentRegistry::get().instantiate(L, name, propsIdx, optsIdx); // leaves exactly one widget-spec table on the stack
            return 1;
        }

    } // namespace

    void registerFunctions(HANDLE handle) {
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Stack", &luaStack);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Row", &luaRow);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Column", &luaColumn);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Text", &luaText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Box", &luaBox);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Button", &luaButton);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Input", &luaInput);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Image", &luaImage);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Divider", &luaDivider);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Checkbox", &luaCheckbox);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Bind", &luaBind);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "window", &luaWindow);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "remove_canvas", &luaRemoveCanvas);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_canvas_visible", &luaSetCanvasVisible);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_canvas_position", &luaSetCanvasPosition);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_canvas_size", &luaSetCanvasSize);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_widget_visible", &luaSetWidgetVisible);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_widget_disabled", &luaSetWidgetDisabled);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_widget_size", &luaSetWidgetSize);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_text", &luaSetText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_input_text", &luaSetInputText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "get_input_text", &luaGetInputText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_image", &luaSetImage);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_checkbox_checked", &luaSetCheckboxChecked);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "get_checkbox_checked", &luaGetCheckboxChecked);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "remove_widget", &luaRemoveWidget);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "add_widget", &luaAddWidget);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "watch", &luaWatch);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "notify", &luaNotify);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "persistent", &luaPersistent);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "run_cmd", &luaRunCmd);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "open_socket", &luaOpenSocket);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "animation", &luaAnimation);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "focus_widget", &luaFocusWidget);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "blur_widget", &luaBlurWidget);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "defineComponent", &luaDefineComponent);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Component", &luaComponent);
    }

    void unregisterFunctions(HANDLE handle) {
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Stack");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Row");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Column");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Box");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Button");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Input");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Image");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Divider");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Checkbox");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Bind");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "window");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "remove_canvas");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_canvas_visible");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_canvas_position");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_canvas_size");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_widget_visible");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_widget_disabled");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_widget_size");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_input_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "get_input_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_image");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_checkbox_checked");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "get_checkbox_checked");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "remove_widget");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "add_widget");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "watch");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "notify");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "persistent");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "run_cmd");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "open_socket");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "animation");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "focus_widget");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "blur_widget");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "defineComponent");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Component");
    }

} // namespace HyprLUI::Lua
