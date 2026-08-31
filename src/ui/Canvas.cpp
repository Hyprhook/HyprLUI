#include "Canvas.hpp"
#include "../render/gfx.hpp"

#include <algorithm>

namespace HyprLUI {

    void CCanvas::render() {
        if (!m_visible)
            return;

        for (const auto& node : m_nodes)
            node->render(m_position);
    }

    void CCanvas::damage() const {
        gfx::damageBox(box());
    }

    PNode CCanvas::addNode(PNode node) {
        m_nodes.push_back(node);
        damage();
        return node;
    }

    void CCanvas::removeNode(const std::string& id) {
        std::erase_if(m_nodes, [&id](const PNode& n) { return n->id() == id; });
        damage();
    }

    PNode CCanvas::findNode(const std::string& id) const {
        auto it = std::find_if(m_nodes.begin(), m_nodes.end(), [&id](const PNode& n) { return n->id() == id; });
        return it == m_nodes.end() ? nullptr : *it;
    }

} // namespace HyprLUI
