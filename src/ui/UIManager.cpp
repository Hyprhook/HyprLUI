#include "UIManager.hpp"
#include "InputWidget.hpp"
#include "CheckboxWidget.hpp"

#include <algorithm>

namespace HyprLUI {

    CUIManager& CUIManager::get() {
        static CUIManager instance;
        return instance;
    }

    PCanvas CUIManager::createCanvas(const std::string& name, const Vector2D& position, const Vector2D& size, EZOrder zorder) {
        auto canvas = std::make_shared<CCanvas>(name, position, size, zorder);
        canvas->setSequence(++m_nextSequence);
        m_canvases[name] = canvas;
        return canvas;
    }

    void CUIManager::removeCanvas(const std::string& name) {
        auto it = m_canvases.find(name);
        if (it == m_canvases.end())
            return;

        // Blur BEFORE the widget tree is torn down below, so onBlur still
        // fires against a live CInputWidget - otherwise Lua's own idea of
        // "is this focused" would silently go stale.
        if (m_focusedInput.canvasName == name)
            blurFocusedInput();
        // Same reasoning for hover - onHoverEnd should still fire against
        // a live widget rather than this canvas's hover tracking silently
        // going stale.
        if (m_hoveredWidget.canvasName == name)
            updateHover({});

        auto canvas = std::move(it->second);
        m_canvases.erase(it);

        // Hide + damage so Hyprland repaints over the now-gone content,
        // then keep it alive in m_pendingRemoval just long enough for
        // render()'s redamage countdown to finish - see the header doc
        // comment. It's already gone from m_canvases, so it draws nothing
        // and a new canvas can reuse this name immediately.
        canvas->setVisible(false);
        canvas->damage();
        m_pendingRemoval.push_back(std::move(canvas));
    }

    bool CUIManager::hasCanvas(const std::string& name) const {
        return m_canvases.contains(name);
    }

    PCanvas CUIManager::getCanvas(const std::string& name) const {
        auto it = m_canvases.find(name);
        return it == m_canvases.end() ? nullptr : it->second;
    }

    void CUIManager::damageAll() {
        for (const auto& [name, canvas] : m_canvases)
            canvas->damage();
    }

    SWidgetHit CUIManager::hitTestWidget(const Vector2D& pt) const {
        std::vector<const CCanvas*> candidates;
        for (const auto& [name, canvas] : m_canvases) {
            if (canvas->zorder() == EZOrder::Overlay && canvas->visible())
                candidates.push_back(canvas.get());
        }

        // Newest-created first - CWidget::hitTest() only ever returns a
        // match that's actually interactive (see its default no-op
        // implementation vs. CButtonWidget's/CInputWidget's overrides),
        // so the first candidate that hits anything at all is the answer.
        std::sort(candidates.begin(), candidates.end(), [](const CCanvas* a, const CCanvas* b) { return a->sequence() > b->sequence(); });

        for (const auto* canvas : candidates) {
            if (auto* hit = canvas->hitTest(pt))
                return {canvas->name(), hit->id()};
        }

        return {};
    }

    bool CUIManager::clickWidget(const std::string& canvasName, const std::string& widgetId) {
        auto canvas = getCanvas(canvasName);
        if (!canvas || !canvas->root())
            return false;

        auto* widget = canvas->root()->findWidget(widgetId);
        if (!widget)
            return false;

        // Checkbox keeps its own dedicated click() (toggles state, fires
        // onChange(bool) - a different shape from the generic no-arg
        // onClick). Every other widget type falls through to the generic
        // fireClick().
        if (auto* checkbox = dynamic_cast<CCheckboxWidget*>(widget)) {
            checkbox->click();
            return true;
        }
        return widget->fireClick();
    }

    bool CUIManager::focusWidget(const std::string& canvasName, const std::string& widgetId) {
        if (isFocused(canvasName, widgetId))
            return true; // already this exact widget - don't re-fire onFocus

        auto canvas = getCanvas(canvasName);
        if (!canvas || !canvas->root())
            return false;

        auto* input = dynamic_cast<CInputWidget*>(canvas->root()->findWidget(widgetId));
        if (!input || input->disabled())
            return false;

        blurFocusedInput(); // no-op if nothing was focused

        input->focus();
        m_focusedInput = {canvasName, widgetId};
        return true;
    }

    void CUIManager::blurFocusedInput() {
        if (m_focusedInput.empty())
            return;

        auto canvas = getCanvas(m_focusedInput.canvasName);
        if (canvas && canvas->root()) {
            if (auto* input = dynamic_cast<CInputWidget*>(canvas->root()->findWidget(m_focusedInput.widgetId)))
                input->blur();
        }

        m_focusedInput = {};
    }

    bool CUIManager::dispatchKey(uint32_t keysym, bool pressed) {
        if (m_focusedInput.empty())
            return false;

        auto canvas = getCanvas(m_focusedInput.canvasName);
        if (!canvas || !canvas->root()) {
            m_focusedInput = {};
            return false;
        }

        auto* input = dynamic_cast<CInputWidget*>(canvas->root()->findWidget(m_focusedInput.widgetId));
        if (!input) {
            m_focusedInput = {};
            return false;
        }

        input->handleKey(keysym, pressed);

        // Text content changing (built-in typing/Backspace capture) doesn't
        // repaint on its own, same as set_text(). Damaged unconditionally
        // per key rather than having handleKey() report back whether text
        // actually changed - key events are rare relative to frame rate.
        canvas->damage();
        return true;
    }

    void CUIManager::handlePressFocus(const SWidgetHit& hit) {
        if (!hit.empty() && hit == m_focusedInput)
            return; // clicking the already-focused Input again - no-op

        blurFocusedInput(); // no-op if nothing was focused

        if (!hit.empty())
            focusWidget(hit.canvasName, hit.widgetId); // no-op (false) if hit isn't actually an Input, e.g. a Button
    }

    void CUIManager::updateHover(const SWidgetHit& hit) {
        if (hit == m_hoveredWidget)
            return;

        if (!m_hoveredWidget.empty()) {
            auto canvas = getCanvas(m_hoveredWidget.canvasName);
            if (canvas && canvas->root()) {
                if (auto* widget = canvas->root()->findWidget(m_hoveredWidget.widgetId))
                    widget->setHovered(false);
            }
        }

        m_hoveredWidget = hit;

        if (!m_hoveredWidget.empty()) {
            auto canvas = getCanvas(m_hoveredWidget.canvasName);
            if (canvas && canvas->root()) {
                if (auto* widget = canvas->root()->findWidget(m_hoveredWidget.widgetId))
                    widget->setHovered(true);
            }
        }
    }

    bool CUIManager::dispatchScroll(const std::string& canvasName, const std::string& widgetId, double delta, bool vertical) {
        auto canvas = getCanvas(canvasName);
        if (!canvas || !canvas->root())
            return false;

        auto* widget = canvas->root()->findWidget(widgetId);
        if (!widget)
            return false;

        return widget->fireScroll(delta, vertical);
    }

    void CUIManager::renderOverlay() {
        for (const auto& [name, canvas] : m_canvases) {
            if (canvas->zorder() == EZOrder::Overlay)
                canvas->render();
        }

        // Ticked once per real frame here. A rootless pending-removal
        // canvas's render() only runs its redamage countdown and draws
        // nothing. A canvas removed while a fade-out is enabled is
        // different: removeCanvas() calls setVisible(false) on it, which
        // keeps m_visible true (still actually rendering, fading down) for
        // the whole animation - render()'s own continuous self-damaging
        // while animating keeps hasPendingRedamage() true for exactly
        // that long, so this sweep doesn't drop it early.
        for (const auto& canvas : m_pendingRemoval)
            canvas->render();
        std::erase_if(m_pendingRemoval, [](const PCanvas& c) { return !c->hasPendingRedamage(); });
    }

    void CUIManager::renderBackground() {
        for (const auto& [name, canvas] : m_canvases) {
            if (canvas->zorder() == EZOrder::Background)
                canvas->render();
        }
    }

    void CUIManager::clear() {
        m_canvases.clear();
        m_pendingRemoval.clear();
        // Every widget just got destroyed above - no live CInputWidget
        // left to call blur() on, so just drop the tracking state rather
        // than routing through blurFocusedInput() (which would try to
        // look it up and find nothing anyway). Same reasoning for hover.
        m_focusedInput  = {};
        m_hoveredWidget = {};
    }

} // namespace HyprLUI
