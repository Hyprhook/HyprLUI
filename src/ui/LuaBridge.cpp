#include "LuaBridge.hpp"
#include "UIManager.hpp"
#include "ContainerWidget.hpp"
#include "RectNode.hpp"
#include "TextNode.hpp"
#include "ButtonWidget.hpp"
#include "InputWidget.hpp"
#include "ImageWidget.hpp"
#include "CheckboxWidget.hpp"
#include "ComponentRegistry.hpp"
#include "../reactive/Watcher.hpp"
#include "../reserved/ReservedAreaComposer.hpp"
#include "../persistence/PersistenceStore.hpp"
#include "../services/NativeServices.hpp"

#include <hyprland/src/helpers/Color.hpp>
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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

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

        // Generic "this numeric field, if the caller actually gave one" -
        // used for setFixedSize()'s w/h originally, and reused as-is for
        // every other optional numeric widget property Phase 7 added
        // (minW/minH/maxW/maxH, opacity, zIndex) - nullopt (as opposed to
        // some baked-in numeric default) is what lets buildWidget() below
        // leave a widget's own constructor-chosen default alone when the
        // Lua spec doesn't mention the field at all.
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

        // Tri-state boolean read - nullopt (as opposed to some baked-in
        // default) when the field is absent, for fields where "the caller
        // didn't say anything" is itself a meaningful, distinct state
        // (e.g. the debug-overlay fields below, where nullopt means
        // "inherit"/"auto" rather than any concrete true/false).
        std::optional<bool> optFieldBoolOpt(lua_State* L, int idx, const char* key) {
            lua_getfield(L, idx, key);
            std::optional<bool> value;
            if (!lua_isnil(L, -1))
                value = static_cast<bool>(lua_toboolean(L, -1));
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

        // Same accepted shapes as parseColorField() above, but for
        // optional color fields (Phase 10's hoverColor/disabledColor)
        // where absence is itself meaningful (no override) rather than
        // falling back to some baked-in default color.
        std::optional<CHyprColor> optColorField(lua_State* L, int idx, const char* key, const char* fnName) {
            lua_getfield(L, idx, key);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                return std::nullopt;
            }
            lua_pop(L, 1);
            return parseColorField(L, idx, key, CHyprColor{}, fnName);
        }

        // Phase 13 follow-up: a widget's own `animationIn`/`animationOut`
        // field (see buildWidget()'s common tail) - a per-widget override
        // of hyprlui.animation()'s global "in"/"out" config, self-
        // contained like the global one's own table shape (leaf implied
        // by which field this is), not a partial merge with it. Returns
        // nullptr if the field isn't present at all (the common case -
        // "use the global config for this widget"). `speed`/`bezier` are
        // only required when the table doesn't explicitly set `enabled =
        // false` - same conditional-requirement shape as hl.animation()/
        // hyprlui.animation() themselves.
        SP<Hyprutils::Animation::SAnimationPropertyConfig> optAnimationOverrideField(lua_State* L, int idx, const char* key, const char* fnName) {
            lua_getfield(L, idx, key);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                return nullptr;
            }
            if (!lua_istable(L, -1))
                luaL_error(L, "%s: field '%s' must be a table", fnName, key);

            const int                                          tblIdx  = lua_gettop(L);
            const bool                                         enabled = optFieldBool(L, tblIdx, "enabled", true);

            SP<Hyprutils::Animation::SAnimationPropertyConfig> cfg;
            if (!enabled) {
                cfg = makeAnimationConfig(false, 1.f, "default");
            } else {
                const double speed = requireFieldNumber(L, tblIdx, "speed", fnName);
                if (speed <= 0)
                    luaL_error(L, "%s: field '%s': speed must be greater than 0", fnName, key);
                const auto bezier = optFieldString(L, tblIdx, "bezier", "default");
                if (!Animation::mgr()->bezierExists(bezier))
                    luaL_error(L, "%s: field '%s': no such bezier \"%s\"", fnName, key, bezier.c_str());
                cfg = makeAnimationConfig(true, static_cast<float>(speed), bezier);
            }

            lua_pop(L, 1); // the animationIn/animationOut table itself
            return cfg;
        }

        // `padding`/`margin` fields (Phase 7) accept either a single
        // number (applied uniformly to all four sides, the common case)
        // or a table { top, right, bottom, left } (each defaulting to 0
        // when only some sides are given - CSS shorthand-table
        // convention, not the widget's own overall default). Returns
        // nullopt if the field is absent entirely, so buildWidget() below
        // can leave a widget's own constructor-chosen default (e.g.
        // CInputWidget's built-in left padding) alone rather than
        // clobbering it with an all-zero SEdgeInsets{} just because the
        // Lua spec didn't mention the field.
        std::optional<SEdgeInsets> optInsetsField(lua_State* L, int idx, const char* key, const char* fnName) {
            lua_getfield(L, idx, key);

            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                return std::nullopt;
            }

            if (lua_isnumber(L, -1)) {
                const auto v = static_cast<double>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                return SEdgeInsets{v, v, v, v};
            }

            if (lua_istable(L, -1)) {
                const int   tblIdx = lua_gettop(L);
                SEdgeInsets result;
                result.top    = fieldNumber(L, tblIdx, "top", 0);
                result.right  = fieldNumber(L, tblIdx, "right", 0);
                result.bottom = fieldNumber(L, tblIdx, "bottom", 0);
                result.left   = fieldNumber(L, tblIdx, "left", 0);
                lua_pop(L, 1);
                return result;
            }

            luaL_error(L, "%s: field '%s' must be a number or table {top, right, bottom, left}", fnName, key);
            return std::nullopt; // unreachable - silences -Wreturn-type
        }

        // If the table at `idx`.`key` is a hyprlui.Bind(name) marker table
        // ({__bind = name} - see luaBind() below), returns that watcher
        // name. Otherwise (plain string, missing field, etc.) returns
        // nullopt so the caller falls back to reading the field normally.
        std::optional<std::string> fieldBindName(lua_State* L, int idx, const char* key) {
            lua_getfield(L, idx, key);
            std::optional<std::string> result;
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "__bind");
                if (lua_isstring(L, -1))
                    result = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            return result;
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

        // Unlike the other constructors, hyprlui.Bind(name) takes a plain
        // string, not a table - it just wraps it into a {__bind = name}
        // marker table so buildWidget() (via fieldBindName() above) can
        // tell "this field should track watcher `name`" apart from "this
        // field is literally the string `name`".
        int luaBind(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            lua_newtable(L);
            lua_pushstring(L, name.c_str());
            lua_setfield(L, -2, "__bind");
            return 1;
        }

        // Wraps a LUA_REGISTRYINDEX reference so the Lua function it
        // points to gets released exactly once, whenever the last copy of
        // the owning std::function (and thus this) is destroyed - i.e.
        // when the CButtonWidget holding it is torn down (its window/
        // widget removed). Held via shared_ptr in fieldOnClick()'s
        // returned closure since std::function requires its target type
        // to be copyable, which a bare move-only RAII guard wouldn't be.
        struct SLuaFnRef {
            lua_State* L   = nullptr;
            int        ref = LUA_NOREF;

            ~SLuaFnRef() {
                if (L && ref != LUA_NOREF)
                    luaL_unref(L, LUA_REGISTRYINDEX, ref);
            }
        };

        // Reads a zero-argument Lua-function field (onClick, onFocus,
        // onBlur, ...) off the table at `idx`, if present, and returns a
        // std::function that invokes it via lua_pcall, logging (not
        // propagating) any error - these fire from InputHook.cpp's mouse/
        // keyboard callbacks, which have no pcall of their own to catch a
        // mistake in the handler, same reasoning as CWatcherManager's
        // callWatcherFn(). Returns an empty std::function (falsy) if
        // `fieldName` is absent.
        std::function<void()> fieldZeroArgFn(lua_State* L, int idx, const char* fieldName) {
            lua_getfield(L, idx, fieldName);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return {};
            }

            const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops the function value
            // Constructed in place (C++20 aggregate-init-via-parens), NOT
            // via std::make_shared<SLuaFnRef>(SLuaFnRef{L, ref}) - that
            // form builds a temporary SLuaFnRef first and copies it in,
            // and the temporary's destructor then unrefs the slot
            // immediately, before the callback is ever invoked. Caught
            // live on onClick: the registry slot got reused for something
            // else by click time, so lua_rawgeti() below pushed whatever
            // now occupied it instead of the callback.
            auto fnRef = std::make_shared<SLuaFnRef>(L, ref);

            return [L, fnRef, fieldName]() {
                lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
                if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    Log::logger->log(Log::ERR, "[hyprlui] error in {} handler: {}", fieldName, err ? err : "<error object is not a string>");
                    lua_pop(L, 1);
                }
            };
        }

        // Same shape as fieldZeroArgFn() above, but for `onKey` - the one
        // callback that actually takes arguments (keysym, pressed).
        // `keysym` is an xkb_keysym_t (already resolved, layout/shift-
        // aware) - see InputHook.cpp's onKeyboardKey().
        std::function<void(uint32_t, bool)> fieldOnKey(lua_State* L, int idx) {
            lua_getfield(L, idx, "onKey");
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return {};
            }

            const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
            auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

            return [L, fnRef](uint32_t keysym, bool pressed) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
                lua_pushinteger(L, keysym);
                lua_pushboolean(L, pressed);
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    Log::logger->log(Log::ERR, "[hyprlui] error in onKey handler: {}", err ? err : "<error object is not a string>");
                    lua_pop(L, 1);
                }
            };
        }

        // Same shape again, but for `onChange` - fires with the Input's
        // current text after it changes from typing/Backspace (not from
        // a programmatic set_input_text() call - see CInputWidget::
        // setText()'s doc comment).
        std::function<void(const std::string&)> fieldOnChange(lua_State* L, int idx) {
            lua_getfield(L, idx, "onChange");
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return {};
            }

            const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
            auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

            return [L, fnRef](const std::string& text) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
                lua_pushlstring(L, text.data(), text.size());
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    Log::logger->log(Log::ERR, "[hyprlui] error in onChange handler: {}", err ? err : "<error object is not a string>");
                    lua_pop(L, 1);
                }
            };
        }

        // Same shape again, but for a Checkbox's `onChange` - fires with
        // the NEW checked state after a real click toggles it (not from a
        // programmatic set_checkbox_checked() call, same "no invocation on
        // load/programmatic set" convention as Input's onChange above).
        std::function<void(bool)> fieldOnChangeBool(lua_State* L, int idx) {
            lua_getfield(L, idx, "onChange");
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return {};
            }

            const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
            auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

            return [L, fnRef](bool checked) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
                lua_pushboolean(L, checked);
                if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    Log::logger->log(Log::ERR, "[hyprlui] error in onChange handler: {}", err ? err : "<error object is not a string>");
                    lua_pop(L, 1);
                }
            };
        }

        // Same shape again, but for `onScroll` (Phase 10) - fires with the
        // raw IPointer::SAxisEvent delta and whether the axis was vertical
        // (the common mouse-wheel case) or horizontal, forwarded as-is, no
        // attempt to normalize/invert it into a "lines scrolled" unit.
        std::function<void(double, bool)> fieldOnScroll(lua_State* L, int idx) {
            lua_getfield(L, idx, "onScroll");
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return {};
            }

            const int ref   = luaL_ref(L, LUA_REGISTRYINDEX);
            auto      fnRef = std::make_shared<SLuaFnRef>(L, ref);

            return [L, fnRef](double delta, bool vertical) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, fnRef->ref);
                lua_pushnumber(L, delta);
                lua_pushboolean(L, vertical);
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                    const char* err = lua_tostring(L, -1);
                    Log::logger->log(Log::ERR, "[hyprlui] error in onScroll handler: {}", err ? err : "<error object is not a string>");
                    lua_pop(L, 1);
                }
            };
        }

        // --- tree builder ------------------------------------------------
        // Recursively converts a tagged widget-spec table (already on the
        // Lua stack at `idx`) into a real CWidget subtree. `autoId` is a
        // per-window counter used to synthesize ids for widgets that don't
        // set one explicitly - they're unreachable by set_text/remove_widget
        // but still valid tree nodes (e.g. a decorative Box). `bindings`
        // collects one closure per hyprlui.Bind()-tagged field found -
        // luaWindow() attaches them to the finished CCanvas once it exists
        // (a widget is built long before its owning canvas is). `seenIds`
        // is a per-window set checked against every resolved id (explicit
        // or auto-generated) - a duplicate is a hard error (Phase 9,
        // DESIGN.md): previously silent (findWidget() just returns the
        // first match, so set_text()/remove_widget()/etc. on the second
        // widget silently landed on the first one instead), which is
        // exactly the failure mode hyprlui.Component()'s own id-rewriting
        // exists to avoid for its own instances - this closes the same
        // gap for hand-written duplicate ids too, not just component ones.

        PWidget buildWidget(lua_State* L, int idx, int& autoId, std::vector<std::function<void()>>& bindings, std::unordered_set<std::string>& seenIds) {
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

            } else if (type == "image") {
                const auto path     = requireFieldString(L, idx, "path", "hyprlui.Image");
                const int  rounding = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
                auto       image    = std::make_shared<CImageWidget>(id, Vector2D{x, y}, path, rounding);
                if (!image->loaded())
                    Log::logger->log(Log::WARN, "[hyprlui] Image '{}': failed to load '{}' - drawing nothing", id, path);
                widget = image;

            } else if (type == "divider") {
                // Sugar over a plain CRectNode with a computed w/h - a
                // Divider has no behavior a Box doesn't already have, this
                // exists purely so a caller doesn't have to remember "just
                // make one axis 1px" by hand. `length` is the dimension
                // along the divider's own axis (w if horizontal, h if
                // vertical); `thickness` is the perpendicular one.
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

                widget = std::make_shared<CRectNode>(id, Vector2D{x, y}, size, color, 0);

            } else if (type == "button") {
                const double w        = requireFieldNumber(L, idx, "w", "hyprlui.Button");
                const double h        = requireFieldNumber(L, idx, "h", "hyprlui.Button");
                const auto   color    = parseColorField(L, idx, "color", CHyprColor{0.2, 0.2, 0.2, 1.0}, "hyprlui.Button");
                const int    rounding = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
                // onClick is parsed generically below (Phase 10 follow-up
                // - CWidget's own field now, not Button-specific).
                widget = std::make_shared<CButtonWidget>(id, Vector2D{x, y}, Vector2D{w, h}, color, rounding);

            } else if (type == "input") {
                const double w         = requireFieldNumber(L, idx, "w", "hyprlui.Input");
                const double h         = requireFieldNumber(L, idx, "h", "hyprlui.Input");
                const auto   color     = parseColorField(L, idx, "color", CHyprColor{0.15, 0.15, 0.15, 1.0}, "hyprlui.Input");
                const int    rounding  = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
                const auto   text      = optFieldString(L, idx, "text", "");
                const auto   textColor = parseColorField(L, idx, "textColor", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Input");
                const int    textSize  = static_cast<int>(fieldNumber(L, idx, "textSize", 14));
                const auto   textFont  = optFieldString(L, idx, "textFont", "sans");
                auto         onKey     = fieldOnKey(L, idx);
                auto         onChange  = fieldOnChange(L, idx);
                auto         onFocus   = fieldZeroArgFn(L, idx, "onFocus");
                auto         onBlur    = fieldZeroArgFn(L, idx, "onBlur");

                auto         input = std::make_shared<CInputWidget>(id, Vector2D{x, y}, Vector2D{w, h}, color, rounding, text, textColor, textSize, textFont);
                if (onKey)
                    input->setOnKey(std::move(onKey));
                if (onChange)
                    input->setOnChange(std::move(onChange));
                if (onFocus)
                    input->setOnFocus(std::move(onFocus));
                if (onBlur)
                    input->setOnBlur(std::move(onBlur));
                widget = input;

            } else if (type == "checkbox") {
                const double w            = requireFieldNumber(L, idx, "w", "hyprlui.Checkbox");
                const double h            = requireFieldNumber(L, idx, "h", "hyprlui.Checkbox");
                const auto   color        = parseColorField(L, idx, "color", CHyprColor{0.2, 0.2, 0.2, 1.0}, "hyprlui.Checkbox");
                const auto   checkedColor = parseColorField(L, idx, "checkedColor", CHyprColor{0.3, 0.6, 1.0, 1.0}, "hyprlui.Checkbox");
                const int    rounding     = static_cast<int>(fieldNumber(L, idx, "rounding", 0));
                const bool   checked      = optFieldBool(L, idx, "checked", false);
                auto         onChange     = fieldOnChangeBool(L, idx);

                auto         checkbox = std::make_shared<CCheckboxWidget>(id, Vector2D{x, y}, Vector2D{w, h}, color, checkedColor, rounding, checked);
                if (onChange)
                    checkbox->setOnChange(std::move(onChange));
                widget = checkbox;

            } else if (type == "text") {
                const auto  bindName = fieldBindName(L, idx, "text");
                std::string text;
                if (bindName) {
                    if (!CWatcherManager::get().hasWatcher(*bindName))
                        luaL_error(L, "hyprlui.Text: text is bound to unknown watcher '%s' - call hyprlui.watch() before referencing it", bindName->c_str());
                    text = CWatcherManager::get().currentValue(*bindName);
                } else {
                    text = requireFieldString(L, idx, "text", "hyprlui.Text");
                }
                const int  size     = static_cast<int>(fieldNumber(L, idx, "size", 16));
                const auto color    = parseColorField(L, idx, "color", CHyprColor{1.0, 1.0, 1.0, 1.0}, "hyprlui.Text");
                const auto font     = optFieldString(L, idx, "font", "sans");
                auto       textNode = std::make_shared<CTextNode>(id, Vector2D{x, y}, text, size, color, font);
                widget              = textNode;

                if (bindName) {
                    // Captures the raw CTextNode* (not the shared_ptr) -
                    // the closure lives on the CCanvas, which itself owns
                    // the widget tree (via m_root) for at least as long,
                    // so the pointer stays valid for the closure's whole
                    // lifetime.
                    auto* rawNode = textNode.get();
                    bindings.emplace_back([rawNode, name = *bindName]() { rawNode->setText(CWatcherManager::get().currentValue(name)); });
                }

            } else if (type == "stack") {
                widget = std::make_shared<CStackWidget>(id, Vector2D{x, y});

            } else if (type == "row" || type == "column") {
                const double gap      = fieldNumber(L, idx, "gap", 0);
                const auto   alignStr = optFieldString(L, idx, "align", "start");
                EAlign       align    = EAlign::Start;
                if (alignStr == "center")
                    align = EAlign::Center;
                else if (alignStr == "end")
                    align = EAlign::End;
                else if (alignStr != "start")
                    luaL_error(L, "hyprlui.%s: 'align' must be 'start', 'center', or 'end', got '%s'", type.c_str(), alignStr.c_str());
                widget = std::make_shared<CFlexWidget>(id, Vector2D{x, y}, type == "row" ? EFlexDirection::Row : EFlexDirection::Column, gap, align);

            } else {
                luaL_error(L, "hyprlui: unknown widget type '%s'", type.c_str());
            }

            widget->setVisible(visible);

            // Base widget properties (Phase 7, DESIGN.md) - shared across
            // every widget type via CWidget itself, so parsed generically
            // here rather than per-type above. Each is only actually
            // applied if the Lua spec mentions it at all (see
            // optInsetsField()/optFixedField()'s doc comments) - so a
            // widget whose constructor already chose a sensible built-in
            // default (e.g. CInputWidget's left padding) keeps it instead
            // of getting silently zeroed out just because the spec didn't
            // repeat it.
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

            // Debug overlay (post-Phase-7 addition, DESIGN.md) - `debug`
            // is tri-state (absent = inherit from the nearest ancestor
            // that hasn't walled itself off, see debugCascade below);
            // `debugShow` forces individual detail categories on/off,
            // bypassing the size-based "auto" default (see Widget.cpp's
            // drawDebugOverlay()) - any category left out of the table
            // stays auto/inherited, same "only touch what's mentioned"
            // convention as every other field here.
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

            // Interactive state (Phase 10, DESIGN.md) - hoverColor/
            // disabledColor/onHoverStart/onHoverEnd/onScroll/disabled are
            // only meaningful for a widget whose isInteractive() is true,
            // but parsed generically here like everything else in this
            // tail; a decorative widget setting them does nothing (see
            // CWidget::setDisabled()'s doc comment). onClick is different
            // - it's what actually MAKES a plain Box/Text/Image/Row/
            // Column/Stack interactive in the first place (see Widget.
            // hpp's hitTest()/isInteractive() defaults) rather than only
            // doing something on a widget that already was.
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
            if (auto onClick = fieldZeroArgFn(L, idx, "onClick"))
                widget->setOnClick(std::move(onClick));

            // Per-widget animation override (Phase 13 follow-up) - see
            // optAnimationOverrideField()'s own doc comment. nullptr (the
            // common case) leaves this widget on the global
            // hyprlui.animation() config for that leaf.
            widget->setAnimationInOverride(optAnimationOverrideField(L, idx, "animationIn", "hyprlui"));
            widget->setAnimationOutOverride(optAnimationOverrideField(L, idx, "animationOut", "hyprlui"));

            // Fixed-size override - meaningful for containers (whose w/h
            // are genuinely optional) and Image (whose natural size comes
            // from the decoded texture, same "size-to-content unless
            // overridden" shape). Box's w/h above are its actual
            // (required) dimensions, not an override, and Text derives
            // its size from rasterization (no override exists for it).
            if (type == "stack" || type == "row" || type == "column" || type == "image")
                widget->setFixedSize(optFixedField(L, idx, "w"), optFixedField(L, idx, "h"));

            // Children: positional (ipairs-style) table entries.
            const auto n = lua_rawlen(L, idx);
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(n); ++i) {
                lua_rawgeti(L, idx, i);
                if (lua_istable(L, -1))
                    widget->addChild(buildWidget(L, lua_gettop(L), autoId, bindings, seenIds));
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
        // `anchor` - e.g. anchor="top" + exclusive="top" is a sensible
        // top bar; anchor="top" + exclusive="left" is nonsensical (the
        // window sits at the top, nowhere near the left edge, so
        // "reserving left space" wouldn't even visually correspond to
        // where the window actually is). Corner anchors (top-left etc.)
        // touch two edges at once, so either is allowed - a corner-docked
        // window could sensibly be a horizontal or a vertical bar.
        // "center" allows neither - a centered window isn't at any edge.
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

        // --- hl.plugin.hyprlui.* implementations -------------------------

        int luaWindow(lua_State* L) {
            luaL_checktype(L, 1, LUA_TTABLE);

            const auto name         = requireFieldString(L, 1, "name", "hyprlui.window");
            const auto x            = fieldNumber(L, 1, "x", 0);
            const auto y            = fieldNumber(L, 1, "y", 0);
            const auto fw           = optFixedField(L, 1, "w");
            const auto fh           = optFixedField(L, 1, "h");
            const auto zStr         = optFieldString(L, 1, "zorder", "overlay");
            const auto anchorStr    = optFieldString(L, 1, "anchor", "");
            const auto monitorStr   = optFieldString(L, 1, "monitor", "");
            const auto exclusiveStr = optFieldString(L, 1, "exclusive", "");

            if (!exclusiveStr.empty() && anchorStr.empty())
                return luaL_error(L,
                                  "hyprlui.window: 'exclusive' requires 'anchor' - a reserved zone needs a resolved target monitor, and anchor is currently the only "
                                  "thing that gives us one");

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

            // No anchor: unchanged Phase 1 behavior - x/y are a raw global
            // position, same escape hatch a Stack's children already use.
            if (anchorStr.empty()) {
                auto canvas = mgr.createCanvas(name, {x, y}, size, zorder);
                canvas->setFixedSize(fw, fh);
                for (auto& binding : bindings)
                    canvas->addBinding(std::move(binding));
                canvas->setRoot(root);
                // Same mechanism as any other setVisible(true) - see
                // CWidget::primeHidden()'s doc comment for why the root
                // needs to be primed hidden first: a plain setVisible(true)
                // alone would have nothing to animate FROM.
                root->primeHidden();
                root->setVisible(true);
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
            const EAnchor anchor = parseAnchor(L, anchorStr);

            // `exclusive`: this window reserves screen-edge space along
            // one of the four edges - the amount reserved is its own
            // current size along the perpendicular axis (top/bottom ->
            // height, left/right -> width), matching eww's `exclusive`
            // flag. Parsed and validated here, BEFORE createCanvas()
            // below - erroring after the canvas already exists would
            // leave an orphaned, empty, undamaged entry registered under
            // `name` in CUIManager forever (no root, never shown, but
            // permanently blocking that name from ever being reused,
            // since hasCanvas(name) would keep saying "taken").
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

            auto canvas = mgr.createCanvas(name, {0, 0}, size, zorder);
            canvas->setFixedSize(fw, fh);
            canvas->setAnchor(anchor, std::string{monitor->name()}, {x, y});

            // setExclusive() must happen BEFORE recomputeAnchorPosition()
            // below, so this window positions itself correctly (excluding
            // only its own contribution, see CCanvas::setExclusive()'s
            // doc comment) from the very first frame.
            if (exclusiveEdge)
                canvas->setExclusive(*exclusiveEdge);

            canvas->recomputeAnchorPosition();

            for (auto& binding : bindings)
                canvas->addBinding(std::move(binding));
            canvas->setRoot(root);

            if (exclusiveEdge) {
                const EEdge       edge        = *exclusiveEdge;
                const std::string monitorName = std::string{monitor->name()};

                // Reserved amount tracks this window's LIVE size (e.g. if
                // its content is Bind()ed and grows/shrinks) - registered
                // via the same generic "size actually changed" hook
                // addBinding() uses for reactivity, keeping CCanvas itself
                // unaware exclusive zones even exist.
                auto sizeAlong = [edge](const Vector2D& sz) -> double { return (edge == EEdge::Top || edge == EEdge::Bottom) ? sz.y : sz.x; };

                CReservedAreaComposer::get().setContribution(name, monitorName, edge, sizeAlong(canvas->size()));
                canvas->setOnSizeChanged(
                    [name, monitorName, edge, sizeAlong](const Vector2D& newSize) { CReservedAreaComposer::get().setContribution(name, monitorName, edge, sizeAlong(newSize)); });
            }

            root->primeHidden();
            root->setVisible(true);
            canvas->damage();
            return 0;
        }

        int luaRemoveCanvas(lua_State* L) {
            const std::string name = luaL_checkstring(L, 1);
            CUIManager::get().removeCanvas(name);
            // No-op if `name` never had an exclusive contribution.
            CReservedAreaComposer::get().removeContribution(name);
            return 0;
        }

        int luaSetCanvasVisible(lua_State* L) {
            const std::string name    = luaL_checkstring(L, 1);
            const bool        visible = lua_toboolean(L, 2);

            auto              canvas = CUIManager::get().getCanvas(name);
            if (!canvas)
                return luaL_error(L, "hyprlui.set_canvas_visible: no window named '%s'", name.c_str());

            // A hidden Input can't visibly be typed into, so it shouldn't
            // silently keep HyprLUI's keyboard focus either - matches the
            // exclusive-zone precedent below ("hidden reserves nothing").
            // No-op if this canvas isn't the one holding focus.
            if (!visible && CUIManager::get().isCanvasFocused(name))
                CUIManager::get().blurFocusedInput();

            canvas->setVisible(visible);
            canvas->damage();
            // A hidden exclusive window reserves nothing (matches eww) -
            // no-op if `name` never had an exclusive contribution.
            CReservedAreaComposer::get().setActive(name, visible);
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

            // Same reasoning as set_canvas_visible() - hiding a widget
            // that happens to be the focused Input blurs it first, rather
            // than leaving it invisible yet still silently holding
            // HyprLUI's keyboard focus. Same for hover (Phase 10) - a
            // hidden widget shouldn't silently keep firing onHoverEnd-
            // pending state either.
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

            // A widget that becomes disabled while focused/hovered can't
            // stay that way - hitTest() excluding it from here on means
            // it could never have BECOME focused/hovered in the first
            // place, so drop that state now rather than leave it stale
            // (same "keep C++ state honest" precedent as set_widget_visible).
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

            // Blur BEFORE the widget is actually torn down below, if it's
            // the one currently holding HyprLUI's keyboard focus - same
            // reasoning as CUIManager::removeCanvas()'s own check.
            if (CUIManager::get().isFocused(canvasName, id))
                CUIManager::get().blurFocusedInput();

            auto* widget = canvas->root()->findWidget(id);
            if (!widget) {
                canvas->damage(); // matches removeChild()'s own prior no-op-if-missing behavior
                return 0;
            }

            // Phase 13: animates this widget out first (if "out" is
            // enabled) and only actually erases it from the tree once
            // that finishes - immediately, same as before Phase 13, if
            // it isn't. `canvas` (a PCanvas, shared ownership) is
            // captured by the deferred callback so the canvas itself
            // can't have been destroyed out from under it in the
            // meantime.
            widget->animateOutThenRemove([canvas, id]() {
                if (canvas->root())
                    canvas->root()->removeChild(id);
                canvas->damage();
            });
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

        // Phase 11 (DESIGN.md) - reads a number/string/boolean argument at
        // `idx` for hyprlui.persistent()/its wrapper's :set(). Uses
        // lua_type() (exact tag), NOT lua_isnumber()/lua_isstring() -
        // those two are coercion-aware in the Lua C API (a numeric-
        // looking string like "123" satisfies lua_isnumber() too), which
        // would silently misclassify a string default/value as a number.
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

        // Both `key` upvalues below are set via lua_pushcclosure() at the
        // table-construction site in luaPersistent() - `store:get()`/
        // `store:set(v)`'s Lua `:` sugar passes `store` itself as arg 1,
        // which these ignore entirely (the actual key lives in the
        // closure's upvalue, not in any argument).
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
            CPersistenceStore::get().getOrInit(key, def); // seeds it if absent, warns (not errors) on a stored-type mismatch otherwise

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

        // Phase 12 (DESIGN.md) - see NativeServices.hpp for the full
        // design rationale (why this polls instead of using
        // CEventLoopManager::doOnReadable(), why no exit code, etc.).
        // Both of these just parse arguments and delegate - the actual
        // spawn/socket/poll machinery lives in CNativeServices.
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

        // Phase 13 (DESIGN.md) - hyprlui.animation({leaf="in"|"out",
        // enabled=true, speed, bezier}). Deliberately only "in"/"out" -
        // not hooked into Hyprland's own animation tree at all (verified:
        // no public API to register a new leaf node there - see
        // CWidgetAnimations' doc comment in Widget.hpp), so this is a
        // self-contained, separate config surface, shaped to LOOK like
        // hl.animation()'s own table call for familiarity, but scoped to
        // exactly the two leaves HyprLUI actually supports. `speed` is in
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

            const auto bezier = optFieldString(L, 1, "bezier", "default");
            if (!Animation::mgr()->bezierExists(bezier))
                return luaL_error(L, R"(hyprlui.animation("%s"): no such bezier "%s")", leaf.c_str(), bezier.c_str());

            CWidgetAnimations::get().configure(kind, true, static_cast<float>(speed), bezier);
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
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_widget_visible", &luaSetWidgetVisible);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_widget_disabled", &luaSetWidgetDisabled);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_text", &luaSetText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_input_text", &luaSetInputText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "get_input_text", &luaGetInputText);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_image", &luaSetImage);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "set_checkbox_checked", &luaSetCheckboxChecked);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "get_checkbox_checked", &luaGetCheckboxChecked);
        HyprlandAPI::addLuaFunction(handle, "hyprlui", "remove_widget", &luaRemoveWidget);
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
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_widget_visible");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_widget_disabled");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_input_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "get_input_text");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_image");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "set_checkbox_checked");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "get_checkbox_checked");
        HyprlandAPI::removeLuaFunction(handle, "hyprlui", "remove_widget");
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
