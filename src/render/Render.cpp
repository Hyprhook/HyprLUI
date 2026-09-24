#include "Render.hpp"
#include "../ui/UIManager.hpp"

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>

namespace HyprLUI::RenderHook {

    namespace {
        CHyprSignalListener g_renderListener;

        void                onRenderStage(eRenderStage stage) {
            // eRenderStage is Hyprland's own enum describing where in the
            // frame we are (declared in src/SharedDefs.hpp - values are
            // typically RENDER_PRE_WINDOWS, RENDER_PRE_WINDOW,
            // RENDER_POST_WINDOW, RENDER_POST_WINDOWS, RENDER_LAST_MOMENT,
            // RENDER_POST_RENDER). If your headers name these differently,
            // this is the only place that needs updating.
            switch (stage) {
                case RENDER_PRE_WINDOWS:
                    // Drawn first, so windows/layers can render on top of it.
                    HyprLUI::CUIManager::get().renderBackground();
                    break;
                case RENDER_POST_WINDOWS:
                    // NOT RENDER_LAST_MOMENT - that fires after Hyprland's
                    // own cursor render, so we'd draw over the cursor
                    // (confirmed live). This is the latest stage before
                    // the cursor, traded off against firing before the
                    // top/overlay layer-shell surfaces too - see
                    // DESIGN.md's own note on the upstream fix this needs.
                    HyprLUI::CUIManager::get().renderOverlay();
                    break;
                default: break;
            }
        }
    } // namespace

    void registerHooks(HANDLE handle) {
        g_renderListener = Event::bus()->m_events.render.stage.listen(onRenderStage);
    }

    void unregisterHooks(HANDLE handle) {
        // Hyprland automatically drops a plugin's callbacks on unload, but
        // releasing our own reference here is cheap and explicit.
        g_renderListener.reset();
    }

} // namespace HyprLUI::RenderHook
