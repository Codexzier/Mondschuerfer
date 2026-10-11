#include "Models.hpp"
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct BoundsEntry {
    const char* name;
    Vector3 min;
    Vector3 max;
};

// Die Tabelle wird beim Kompilieren aus ModelBounds.inc eingefügt.
const BoundsEntry kBounds[] = {
#include "ModelBounds.inc"
};

struct Registry {
    std::vector<std::string> names;              // ID -> Name
    std::unordered_map<std::string, int> ids;    // Name -> ID
    std::vector<BoundingBox> bounds;             // ID -> Abmessungen
    std::mutex lock;                             // Schutz, falls mehrere Threads gleichzeitig fragen
};

Registry& Reg() {
    static Registry r;  // wird beim ersten Aufruf angelegt
    return r;
}

BoundingBox LookupBounds(const std::string& name) {
    for (const auto& b : kBounds)
        if (name == b.name) return BoundingBox{b.min, b.max};
    fprintf(stderr, "Warnung: keine Abmessungen für Modell '%s' (shared/ModelBounds.inc neu erzeugen)\n",
            name.c_str());
    return BoundingBox{{0, 0, 0}, {0, 0, 0}};
}

} // namespace

int ModelId(const std::string& name) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> guard(r.lock);
    auto it = r.ids.find(name);
    if (it != r.ids.end()) return it->second;
    int id = static_cast<int>(r.names.size());
    r.names.push_back(name);
    r.bounds.push_back(LookupBounds(name));
    r.ids[name] = id;
    return id;
}

const std::string& ModelName(int id) {
    static const std::string empty;
    Registry& r = Reg();
    std::lock_guard<std::mutex> guard(r.lock);
    if (id < 0 || id >= static_cast<int>(r.names.size())) return empty;
    return r.names[static_cast<size_t>(id)];
}

int ModelCount() {
    Registry& r = Reg();
    std::lock_guard<std::mutex> guard(r.lock);
    return static_cast<int>(r.names.size());
}

BoundingBox ModelBounds(int id) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> guard(r.lock);
    if (id < 0 || id >= static_cast<int>(r.bounds.size())) return BoundingBox{{0, 0, 0}, {0, 0, 0}};
    return r.bounds[static_cast<size_t>(id)];
}
