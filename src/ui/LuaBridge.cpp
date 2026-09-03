#include "LuaBridge.hpp"
#include "UIManager.hpp"
#include "ContainerWidget.hpp"
#include "RectNode.hpp"
#include "TextNode.hpp"

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include <hyprland/src/state/MonitorState.hpp>

// This Lua build's headers don't guard their declarations with
// `extern "C"` themselves (only LUAMOD_API does) - without wrapping the
// include here, every lua_*/luaL_* call site gets C++-mangled, which then
// fails to resolve against liblua's plain C exports at plugin load time.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cstdint>
#include <optional>
#include <string>

namespace HyprLUI::Lua {

    namespace {

        // --- lua_State argument helpers ---------------------------------
        // Widget-spec tables (Stack{}/Row{}/Column{}/Text{}/Box{}/window{})
        // all take a single table argument, mirroring hl.notification.create
        // so optional fields stay readable at the call site. These helpers
        // read fields off the table at `idx` and raise a Lua error (via
        // luaL_error, which longjmps - never returns) on a type mismatch.

        double fieldNumber(lua_State* L, int idx, const char* key, double def) {
            lua_getfield(L, idx, key);
            double value = def;
            if (!lua_isnil(L, -1))
                value = luaL_checknumber(L, -1);
            lua_pop(L, 1);
            return value;
        }

        std::optional<double> optFixedField(lua_State* L, int idx, const char* key) {
            lua_getfield(L, idx, key);
            std::optional<double> value;
            if (!lua_isnil(L, -1))
                value = luaL_checknumber(L, -1);
            lua_pop(L, 1);
            return value;
        }

        double requireFieldNumber(lua_State* L, int idx, const char* key, const char* fnName) {
            lua_getfield(L, idx, key);
            if (lua_isnil(L, -1))
                luaL_error(L, "%s: missing required field '%s'", fnName, key);
            const double value = luaL_checknumber(L, -1);
            lua_pop(L, 1);
            return value;
        }

        std::string requireFieldString(lua_State* L, int idx, const char* key, const char* fnName) {
            lua_getfield(L, idx, key);
            if (lua_isnil(L, -1))
                luaL_error(L, "%s: missing required field '%s'", fnName, key);
            std::string value = luaL_checkstring(L, -1);
            lua_pop(L, 1);
            return value;
        }

        std::string optFieldString(lua_State* L, int idx, const char* key, const std::string& def) {
            lua_getfield(L, idx, key);
            std::string value = def;
            if (!lua_isnil(L, -1))
                value = luaL_checkstring(L, -1);
            lua_pop(L, 1);
            return value;
        }

        bool optFieldBool(lua_State* L, int idx, const char* key, bool def) {
            lua_getfield(L, idx, key);
            bool value = def;
            if (!lua_isnil(L, -1))
                value = lua_toboolean(L, -1);
            lua_pop(L, 1);
            return value;
        }

        // `color` fields accept either a packed 0xAARRGGBB integer
        // (matching hl.notification.create's convention) or a table
        // { r, g, b, a } with components in [0, 1].
        CHyprColor parseColorField(lua_State* L, int idx, const char* key, const CHyprColor& def, const char* fnName) {
            lua_getfield(L, idx, key);

            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                return def;
            }

            if (lua_isnumber(L, -1)) {
                const CHyprColor result(static_cast<uint64_t>(lua_tointeger(L, -1)));
                lua_pop(L, 1);
                return result;
            }

            if (lua_istable(L, -1)) {
                const int  colorIdx = lua_gettop(L);
                const auto r        = fieldNumber(L, colorIdx, "r", 1.0);
                const auto g        = fieldNumber(L, colorIdx, "g", 1.0);
                const auto b        = fieldNumber(L, colorIdx, "b", 1.0);
                const auto a        = fieldNumber(L, colorIdx, "a", 1.0);
                lua_pop(L, 1);
                return CHyprColor(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a));
            }

            luaL_error(L, "%s: field '%s' must be a number or table {r, g, b, a}", fnName, key);
            return def; // unreachable - silences -Wreturn-type
        }

        // --- widget constructors: Stack/Row/Column/Text/Box ---------------
        // Each just tags its table argument with a __type field and hands
        // it straight back - the actual tree gets built later, once, when
        // the whole thing reaches window{}. This is what makes
        // `Column{ gap = 8, Text{...} }` work: Text{} evaluates first and
        // its (tagged) return value becomes positional entry [1] of the
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

        // --- tree builder ------------------------------------------------
        // Recursively converts a tagged widget-spec table (already on the
        // Lua stack at `idx`) into a real CWidget subtree. `autoId` is a
        // per-window counter used to synthesize ids for widgets that don't
        // set one explicitly - they're unreachable by set_text/remove_widget
        // but still valid tree nodes (e.g. a decorative Box).

        PWidget buildWidget(lua_State* L, int idx, int& autoId) {
            idx = lua_absindex(L, idx);
            luaL_checktype(L, idx, LUA_TTABLE);

            lua_getfield(L, idx, "__type");
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                luaL_error(L, "hyprlui: expected a widget table (Stack{}/Row{}/Column{}/Text{}/Box{}), got a plain table");
            }
            const std::string type = lua_tostring(L, -1);
            lua_pop(L, 1);

            std::string id = optFieldString(L, idx, "id", "");
            if (id.empty())
                id = "__auto" + std::to_string(autoId++);

            const double x       = fieldNumber(L, idx, "x", 0);
            const double y       = fieldNumber(L, idx, "y", 0);
            const bool   visible = optFieldBool(L, idx, "visible", true);

            PWidget      widget;

            if (type == "box") {
                const double w        = requireFieldNumber(L, idx, "w", "hyprlui.Box");
                const double h        = requireFieldNumber(L, idx, "h", "hyprlui.Box");
                const auto   color    = parseColorField(L, idx, "color", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Box");
                const int    rounding = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
                widget                = std::make_shared<CRectNode>(id, Vector2D{x, y}, Vector2D{w, h}, color, rounding);

            } else if (type == "text") {
                const auto text  = requireFieldString(L, idx, "text", "hyprlui.Text");
                const int  size  = static_cast<int>(fieldNumber(L, idx, "size", 16));
                const auto color = parseColorField(L, idx, "color", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Text");
                const auto font  = optFieldString(L, idx, "font", "sans");
                widget           = std::make_shared<CTextNode>(id, Vector2D{x, y}, text, size, color, font);

            } else if (type == "stack") {
                widget = std::make_shared<CStackWidget>(id, Vector2D{x, y});

            } else if (type == "row" || type == "column") {
                const double gap      = fieldNumber(L, idx, "gap", 0);
                const double padding  = fieldNumber(L, idx, "padding", 0);
                const auto   alignStr = optFieldString(L, idx, "align", "start");
                EAlign       align    = EAlign::Start;
                if (alignStr == "center")
                    align = EAlign::Center;
                else if (alignStr == "end")
                    align = EAlign::End;
                else if (alignStr != "start")
                    luaL_error(L, "hyprlui.%s: 'align' must be 'start', 'center', or 'end', got '%s'", type.c_str(), alignStr.c_str());
                widget = std::make_shared<CFlexWidget>(id, Vector2D{x, y}, type == "row" ? EFlexDirection::Row : EFlexDirection::Column, gap, padding, align);

            } else {
                luaL_error(L, "hyprlui: unknown widget type '%s'", type.c_str());
            }

            widget->setVisible(visible);

            // Fixed-size override, meaningful only for containers - Box's
            // w/h above are its actual (required) dimensions, not an
            // override, and Text derives its size from rasterization.
            if (type == "stack" || type == "row" || type == "column")
                widget->setFixedSize(optFixedField(L, idx, "w"), optFixedField(L, idx, "h"));

            // Children: positional (ipairs-style) table entries.
            const auto n = lua_rawlen(L, idx);
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
                lua_rawgeti(L, idx, i);
                if (lua_istable(L, -1))
                    widget->addChild(buildWidget(L, lua_gettop(L), autoId));
                lua_pop(L, 1);
            }

            return widget;
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

        // --- hl.plugin.hyprlui.* implementations -------------------------

        int luaWindow(lua_State* L) {
            luaL_checktype(L, 1, LUA_TTABLE);

            const auto name       = requireFieldString(L, 1, "name", "hyprlui.window");
            const auto x          = fieldNumber(L, 1, "x", 0);
            const auto y          = fieldNumber(L, 1, "y", 0);
            const auto fw         = optFixedField(L, 1, "w");
            const auto fh         = optFixedField(L, 1, "h");
            const auto zStr       = optFieldString(L, 1, "zorder", "overlay");
            const auto anchorStr  = optFieldString(L, 1, "anchor", "");
            const auto monitorStr = optFieldString(L, 1, "monitor", "");

            EZOrder    zorder = EZOrder::Overlay;
            if (zStr == "background")
                zorder = EZOrder::Background;
            else if (zStr != "overlay")
                return luaL_error(L, "hyprlui.window: 'zorder' must be 'overlay' or 'background', got '%s'", zStr.c_str());

            auto& mgr = CUIManager::get();
            if (mgr.hasCanvas(name))
                return luaL_error(L, "hyprlui.window: a window named '%s' already exists", name.c_str());

            PWidget    root;
            int        autoId = 0;
            const auto n      = lua_rawlen(L, 1);
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
                lua_rawgeti(L, 1, i);
                if (lua_istable(L, -1)) {
                    root = buildWidget(L, lua_gettop(L), autoId);
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

            // No anchor: unchanged Phase 1 behavior - x/y are a raw global
            // position, same escape hatch a Stack's children already use.
            if (anchorStr.empty()) {
                auto canvas = mgr.createCanvas(name, {x, y}, size, zorder);
                canvas->setRoot(root);
                canvas->damage();
                return 0;
            }

            // Anchor given: x/y are reinterpreted as an offset from the
            // anchor point (same "relative to parent" convention a
            // widget's x/y already has relative to its parent widget),
            // not a global position. `monitor` is optional and uses
            // Hyprland's own monitor-selector syntax (same as window-rule
            // "mon:" fields - direction chars/+N/-N/numeric id/static
            // selector/output name), resolved relative to the currently
            // focused monitor so e.g. "+1" means "one past focused". Note
            // there's deliberately no "current"/"focused" keyword here -
            // Hyprland's own selector parser treats the literal string
            // "current" as a magic alias for whatever `.relativeTo()` was
            // given (MonitorQueryCore.cpp's fromConfigString()), which
            // would shadow an *actual* monitor a user has genuinely named
            // "current" in their own monitor rules. So: omit `monitor`
            // entirely to mean "the focused monitor", full stop - and if a
            // given selector doesn't match anything (typo, output
            // unplugged), fall back to the focused monitor too rather than
            // erroring, since "couldn't find that exact spot, use the
            // sensible default" is more useful here than failing the whole
            // window. Resolved ONCE here, by name - see
            // CCanvas::recomputeAnchorPosition().
            const EAnchor anchor  = parseAnchor(L, anchorStr);
            auto          monitor = monitorStr.empty() ? Desktop::focusState()->monitor() :
                                                         State::CMonitorQuery{*State::monitorState()}.relativeTo(Desktop::focusState()->monitor()).configString(monitorStr).run();
            if (!monitor)
                monitor = Desktop::focusState()->monitor();
            if (!monitor)
                return luaL_error(L, "hyprlui.window: no monitor available to anchor '%s' against", name.c_str());

            auto canvas = mgr.createCanvas(name, {0, 0}, size, zorder);
            canvas->setAnchor(anchor, std::string{monitor->name()}, {x, y});
            canvas->recomputeAnchorPosition();
            canvas->setRoot(root);
            canvas->damage();
            return 0;
        }

        int luaRemoveCanvas(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            CUIManager::get().removeCanvas(name);
            return 0;
        }

        int luaSetCanvasVisible(lua_State* L) {
            const std::string name    = luaL_checkstring(L, 1);
            const bool        visible = lua_toboolean(L, 2);

            auto              canvas = CUIManager::get().getCanvas(name);
            if (!canvas)
                return luaL_error(L, "hyprlui.set_canvas_visible: no window named '%s'", name.c_str());

            canvas->setVisible(visible);
            canvas->damage();
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

        int luaRemoveWidget(lua_State* L) {
            const std::string canvasName = luaL_checkstring(L, 1);
            const std::string id         = luaL_checkstring(L, 2);

            auto              canvas = CUIManager::get().getCanvas(canvasName);
            if (!canvas || !canvas->root())
                return luaL_error(L, "hyprlui.remove_widget: no window named '%s'", canvasName.c_str());

            canvas->root()->removeChild(id);
            canvas->damage();
            return 0;
        }

    } // namespace

    void registerFunctions(HANDLE handle) {
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Stack", &luaStack);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Row", &luaRow);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Column", &luaColumn);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Text", &luaText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "Box", &luaBox);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "window", &luaWindow);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "remove_canvas", &luaRemoveCanvas);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_canvas_visible", &luaSetCanvasVisible);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_text", &luaSetText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "remove_widget", &luaRemoveWidget);
    }

    void unregisterFunctions(HANDLE handle) {
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Stack");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Row");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Column");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "Box");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "window");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "remove_canvas");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_canvas_visible");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "remove_widget");
    }

} // namespace HyprLUI::Lua
