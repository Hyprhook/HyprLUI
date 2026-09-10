#include "PersistenceStore.hpp"

#include <hyprland/src/debug/log/Logger.hpp>

namespace HyprLUI {

    CPersistenceStore& CPersistenceStore::get() {
        static CPersistenceStore instance;
        return instance;
    }

    PersistentValue CPersistenceStore::getOrInit(const std::string& key, const PersistentValue& def) {
        auto it = m_values.find(key);
        if (it == m_values.end()) {
            m_values.emplace(key, def);
            return def;
        }

        if (it->second.index() != def.index())
            Log::logger->log(Log::WARN, "[hyprlui] persistent('{}'): stored value's type differs from this call's default - keeping the existing stored value", key);

        return it->second;
    }

    PersistentValue CPersistenceStore::getRaw(const std::string& key) const {
        auto it = m_values.find(key);
        return it == m_values.end() ? PersistentValue{false} : it->second;
    }

    void CPersistenceStore::set(const std::string& key, const PersistentValue& value) {
        m_values[key] = value;
    }

    void CPersistenceStore::clear() {
        m_values.clear();
    }

} // namespace HyprLUI
