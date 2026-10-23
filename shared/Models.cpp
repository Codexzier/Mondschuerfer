// ---------------------------------------------------------------------------
// Models.cpp – Umsetzung des Modell-Verzeichnisses (Schnittstelle und
// Erklärung: Models.hpp).
//
// Aufbau:
//   - kBounds: feste Tabelle "Name -> Abmessungen", beim Kompilieren aus
//     ModelBounds.inc eingefügt. Liegt im Programm selbst, es wird also zur
//     Laufzeit keine Datei gelesen (der Server hat die GLB-Dateien gar nicht).
//   - Registry: das eigentliche Verzeichnis, das zur Laufzeit wächst
//     (Name <-> ID, dazu die Abmessungen je ID).
//
// Ablauf: Wer ModelId("rock") zum ersten Mal fragt, bekommt die nächste freie
// Nummer; dabei werden einmalig die Abmessungen in kBounds gesucht und neben
// der ID abgelegt. Spätere Abfragen sind nur noch ein Nachschlagen.
//
// Hinweis zu ModelBounds.inc: Sie entsteht mit
// "Mondschuerfer --dump-model-bounds > shared/ModelBounds.inc" (DumpModelBounds
// in src/main.cpp). Das Spiel lädt dazu jedes GLB mit raylib, misst es mit
// GetModelBoundingBox und verschiebt die Box um (-2, 0, -1.5), genau wie beim
// normalen Laden (Assets::Ensure, src/Assets.cpp). Kommen neue Modelle hinzu,
// die Tabelle so neu erzeugen.
//
// Abhängigkeiten: Standardbibliothek (mutex, unordered_map), raylib.h nur
// für die Datentypen Vector3/BoundingBox.
// ---------------------------------------------------------------------------
#include "Models.hpp"
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

// Anonymer Namensraum: alles darin ist nur in dieser Datei sichtbar (wie
// "static"). So kann keine andere Datei das Verzeichnis an den Funktionen
// vorbei verändern.
namespace {

struct BoundsEntry {
    const char* name;
    Vector3 min;
    Vector3 max;
};

// Die Tabelle wird beim Kompilieren aus ModelBounds.inc eingefügt.
// Das #include mitten in der Initialisierung kopiert den Dateiinhalt (Zeilen
// der Form {"name", {min}, {max}},) wörtlich an diese Stelle.
const BoundsEntry kBounds[] = {
#include "ModelBounds.inc"
};

struct Registry {
    std::vector<std::string> names;              // ID -> Name
    std::unordered_map<std::string, int> ids;    // Name -> ID
    std::vector<BoundingBox> bounds;             // ID -> Abmessungen
    std::mutex lock;                             // Schutz, falls mehrere Threads gleichzeitig fragen
};

// Das Verzeichnis als lokale static-Variable ("Meyers-Singleton"): Sie wird
// beim ersten Aufruf angelegt, garantiert nur einmal und auch bei mehreren
// Threads sicher. Eine globale Variable wäre riskanter: Globale Objekte
// verschiedener .cpp-Dateien werden in unbestimmter Reihenfolge angelegt,
// und ein anderes globales Objekt könnte ModelId() schon vorher aufrufen.
Registry& Reg() {
    static Registry r;  // wird beim ersten Aufruf angelegt
    return r;
}

// Lineare Suche über die ~150 Einträge. Das passiert nur einmal je neuem
// Namen (beim Weltaufbau), deshalb lohnt sich keine schnellere Struktur.
BoundingBox LookupBounds(const std::string& name) {
    for (const auto& b : kBounds)
        if (name == b.name) return BoundingBox{b.min, b.max};
    fprintf(stderr, "Warnung: keine Abmessungen für Modell '%s' (shared/ModelBounds.inc neu erzeugen)\n",
            name.c_str());
    return BoundingBox{{0, 0, 0}, {0, 0, 0}};
}

} // namespace

// Alle vier Funktionen sperren den Mutex für die Dauer des Zugriffs
// (std::lock_guard gibt ihn am Ende der Funktion automatisch wieder frei).
int ModelId(const std::string& name) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> guard(r.lock);
    auto it = r.ids.find(name);
    if (it != r.ids.end()) return it->second;
    // Neuer Name: nächste freie Nummer = bisherige Anzahl
    int id = static_cast<int>(r.names.size());
    r.names.push_back(name);
    r.bounds.push_back(LookupBounds(name));
    r.ids[name] = id;
    return id;
}

const std::string& ModelName(int id) {
    // Für ungültige IDs eine Referenz auf einen dauerhaft lebenden leeren
    // String (eine Referenz auf ein lokales Objekt wäre nach dem Rücksprung
    // ungültig).
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

const std::vector<std::string>& ShipModelNames() {
    // Einmal beim ersten Aufruf füllen (thread-sicher wie Reg() oben)
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& b : kBounds)
            if (std::string(b.name).rfind("craft", 0) == 0) out.push_back(b.name);
        return out;
    }();
    return names;
}
