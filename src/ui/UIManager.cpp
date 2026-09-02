#include "UIManager.hpp"

namespace HyprLUI {

    CUIManager& CUIManager::get() {
        static CUIManager instance;
        return instance;
    }

    PCanvas CUIManager::createCanvas(const std::string& name, const Vector2D& position, const Vector2D& size, EZOrder zorder) {
        auto canvas          = std::make_shared<CCanvas>(name, position, size, zorder);
        m_canvases[name]     = canvas;
        return canvas;
    }

    void CUIManager::removeCanvas(const std::string& name) {
        auto it = m_canvases.find(name);
        if (it == m_canvases.end())
            return;

        // Damage before erasing - Hyprland needs a repaint of this box to
        // redraw over it now that our content is gone (drawing/erasing
        // internal state alone doesn't repaint anything on its own).
        it->second->damage();
        m_canvases.erase(it);
    }

    bool CUIManager::hasCanvas(const std::string& name) const {
        return m_canvases.contains(name);
    }

    PCanvas CUIManager::getCanvas(const std::string& name) const {
        auto it = m_canvases.find(name);
        return it == m_canvases.end() ? nullptr : it->second;
    }

    void CUIManager::renderOverlay() {
        for (const auto& [name, canvas] : m_canvases) {
            if (canvas->zorder() == EZOrder::Overlay)
                canvas->render();
        }
    }

    void CUIManager::renderBackground() {
        for (const auto& [name, canvas] : m_canvases) {
            if (canvas->zorder() == EZOrder::Background)
                canvas->render();
        }
    }

    void CUIManager::clear() {
        m_canvases.clear();
    }

} // namespace HyprLUI
