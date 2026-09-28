#include "zlong/service/registry.h"

#include <utility>

namespace zlong::service {

bool ServiceRegistry::Register(std::string name, std::unique_ptr<IService> service) {
    if (name.empty() || service == nullptr) {
        return false;
    }
    for (const Entry& entry : services_) {
        if (entry.name == name) {
            return false;  // a duplicate would shadow, not replace
        }
    }
    Entry entry;
    entry.name = std::move(name);
    entry.service = std::move(service);
    services_.push_back(std::move(entry));
    return true;
}

IService* ServiceRegistry::Find(std::string_view name) const {
    for (const Entry& entry : services_) {
        if (entry.name == name) {
            return entry.service.get();
        }
    }
    return nullptr;
}

std::vector<std::string> ServiceRegistry::names() const {
    std::vector<std::string> names;
    names.reserve(services_.size());
    for (const Entry& entry : services_) {
        names.push_back(entry.name);
    }
    return names;
}

}  // namespace zlong::service
