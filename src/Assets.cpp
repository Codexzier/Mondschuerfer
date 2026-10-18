// ---------------------------------------------------------------------------
// Assets.cpp – Laden, Umfärben und Zeichnen der 3D-Modelle
// (Schnittstelle und Überblick: Assets.hpp).
//
// Inhalt dieser Datei:
//   - LoadFilePatched: repariert die GLB-Dateien des Kits beim Einlesen.
//   - MoonTone: färbt das orange Gestein des Kits grau oder blau.
//   - kVertexShader / kFragmentShader: einfache Beleuchtung
//     (Sonne + Himmel + Randlicht + Leuchten).
//   - TRS: Welt-Matrizen bauen.
//   - Assets: Init/Shutdown, Modelle bei Bedarf laden (Ensure), zeichnen.
//
// Benutzt von: siehe Assets.hpp (vor allem src/Render.cpp). Nur im Client;
// der Server wird ohne diese Datei gebaut.
//
// Abhängigkeiten: raylib (Modelle, Shader, Dateizugriff), raymath.h,
// shared/Models.hpp (ModelName/ModelCount), die GLB-Dateien in
// assets/models/ (Kenney Space Kit).
//
// Annahme: Alles läuft im Haupt-Thread mit dem OpenGL-Kontext, Init() erst
// nach InitWindow().
// ---------------------------------------------------------------------------
#include "Assets.hpp"
#include "Models.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

Assets g_assets;

namespace {

// ---------------------------------------------------------------------------
// Die GLB-Dateien des Space Kits enthalten einen Hilfsknoten "tmpParent"
// (Knoten 0), der den eigentlichen Modellknoten (Knoten 1) als Kind einträgt.
// Die Szene führt aber nur Knoten 1 als Wurzel auf, "tmpParent" gar nicht.
// Der in raylib eingebaute glTF-Parser (cgltf) lehnt das ab. Beim Laden
// entfernen wir deshalb nur im Speicher den Eintrag  "children":[1],  (durch
// Leerzeichen gleicher Länge ersetzt). Die Dateien selbst bleiben unverändert.
//
// Warum Leerzeichen statt Löschen? Leerzeichen sind in JSON zwischen zwei
// Einträgen erlaubt, und alle Längenangaben im Dateikopf bleiben gültig.
//
// Aufbau einer GLB-Datei (binäres glTF), soweit hier gebraucht:
//   Byte  0..3   "glTF"  (Kennung)
//   Byte  4..7   Version
//   Byte  8..11  Gesamtlänge
//   Byte 12..15  Länge des ersten Blocks (immer der JSON-Block)
//   Byte 16..19  Blocktyp "JSON"
//   ab Byte 20   JSON-Text (Szene, Knoten, Materialien ...)
//   danach       BIN-Block mit den Eckpunktdaten
//
// Eingebunden wird die Funktion mit SetLoadFileDataCallback() in Init(). Ab
// dann liest raylib JEDE Datei über diese Funktion (auch Töne und Schriften);
// alles, was nicht mit "glTF" beginnt, wird unverändert durchgereicht. Der
// Speicher muss mit MemAlloc() kommen, weil raylib ihn später selbst mit
// UnloadFileData() (MemFree) freigibt.
// ---------------------------------------------------------------------------
unsigned char* LoadFilePatched(const char* fileName, int* dataSize) {
    *dataSize = 0;
    FILE* f = fopen(fileName, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return nullptr; }
    auto* data = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(size)));
    size_t read = fread(data, 1, static_cast<size_t>(size), f);
    fclose(f);
    *dataSize = static_cast<int>(read);

    if (read > 20 && memcmp(data, "glTF", 4) == 0) {
        // Gesucht wird der genaue Text, den der Exporter des Kits schreibt
        // (ohne Leerzeichen). Ersetzt wird nur der Teil bis einschließlich
        // des Kommas; der Name bleibt stehen. Fehlt das Muster, bleibt die
        // Datei unverändert.
        const char* pattern = "\"children\":[1],\"name\":\"tmpParent\"";
        const size_t patLen = strlen(pattern);
        const size_t replaceLen = strlen("\"children\":[1],");
        // Nur im JSON-Block suchen (Länge steht in Byte 12..15, siehe oben),
        // damit nie zufällig Bytes im Binärteil verändert werden.
        unsigned int jsonLen = 0;
        memcpy(&jsonLen, data + 12, 4);
        size_t end = 20 + static_cast<size_t>(jsonLen);
        if (end > read) end = read;
        for (size_t i = 20; i + patLen <= end; ++i) {
            if (memcmp(data + i, pattern, patLen) == 0) {
                memset(data + i, ' ', replaceLen);
                break;
            }
        }
    }
    return data;
}

// Die Gesteinsfarben des Kits sind orange (Mars). Für die Mondoberfläche
// werden sie beim Laden umgefärbt: grau (Standard), blau oder gar nicht
// (Orange = Originalfarbe). Die Originalfarben merkt sich jedes Modell, damit
// die Farbe später (Mehrspieler: Servereinstellung) gewechselt werden kann.
//
// Erkannt werden die Gesteinsmaterialien an ihrer Farbe. Toleranz < 6 je
// Kanal, weil die Farben in der Datei als Kommazahlen (0..1) stehen und beim
// Umrechnen auf 0..255 leicht abweichen können.
bool Near(Color a, int r, int g, int b) {
    return std::abs(a.r - r) < 6 && std::abs(a.g - g) < 6 && std::abs(a.b - b) < 6;
}

// Nur die drei Gesteinsfarben des Kits werden ersetzt; Metall, Glas,
// Kristalle usw. behalten ihre Farbe.
//   rock      Hauptfarbe des Bodens (terrain, Felsen)
//   rockDark  dunkleres Gestein (Krater, Rampen, Klippen)
//   rockTrack helleres Gestein (Wege, Meteoriten, Felsbrocken)
// Die Grautöne sind leicht bläulich, damit der Mond kühl wirkt. Die Abstufung
// hell/dunkel des Originals bleibt erhalten, damit Formen erkennbar bleiben.
Color MoonTone(Color original, MoonColor moon) {
    if (moon == MoonColor::Orange) return original;
    const bool grey = moon == MoonColor::Grey;
    if (Near(original, 232, 133, 99))  return grey ? Color{172, 174, 184, 255} : Color{128, 150, 205, 255};  // rock
    if (Near(original, 179, 94, 66))   return grey ? Color{118, 120, 132, 255} : Color{82, 100, 150, 255};   // rockDark
    if (Near(original, 250, 148, 115)) return grey ? Color{196, 197, 204, 255} : Color{160, 180, 225, 255};  // rockTrack
    return original;
}

// ---------------------------------------------------------------------------
// Beleuchtungs-Shader (GLSL 3.30, läuft auf der Grafikkarte)
//
// Ein Shader ist ein kleines Programm für die Grafikkarte:
//   - Der Vertex-Shader läuft einmal pro Eckpunkt. Er rechnet die Position
//     auf den Bildschirm um (mvp = Projektion · Kamera · Modell) und reicht
//     Weltposition, Normale (Richtung, in die die Fläche zeigt) und Farbe an
//     den Fragment-Shader weiter.
//   - Der Fragment-Shader läuft einmal pro Pixel und berechnet dessen Farbe.
//
// Die Namen der Eingänge (vertexPosition, vertexNormal, vertexColor ...) und
// der Uniforms mvp, matModel, matNormal und colDiffuse sind die Standardnamen
// von raylib. raylib findet sie beim Laden selbst und füllt sie bei jedem
// DrawModel(): colDiffuse = Materialfarbe · tint, matNormal = Matrix für die
// Normalen (hält sie auch bei Skalierung senkrecht zur Fläche). Die
// Kit-Modelle haben keine Vertexfarben; raylib liefert dann Weiß.
// Die eigenen Uniforms lightDir, viewPos und emissive setzt diese Datei mit
// SetShaderValue().
//
// Farbe eines Pixels (Fragment-Shader):
//   Grundfarbe = Materialfarbe · Vertexfarbe
//   Helligkeit = 0.36           Grundlicht: Schattenseiten werden nicht schwarz
//              + 0.16 · sky     Himmelslicht: sky = 0 für Unterseiten, 1 für
//                               Oberseiten (0.5 + 0.5 · Normale.y)
//              + 0.62 · diff    Sonnenlicht nach Lambert: Kosinus des Winkels
//                               zwischen Normale und Richtung zur Sonne
//                               (0 auf der abgewandten Seite)
//   Voll beleuchtete Oberseiten kommen auf 1.14, also leicht überstrahlt –
//   das wirkt hell und sonnig.
//   + Randlicht ("rim light"): Flächen, die fast seitlich zur Kamera stehen
//     (Normale fast senkrecht zur Blickrichtung), bekommen einen kühlen,
//     bläulichen Schimmer. Dadurch heben sich Umrisse vom Boden ab. Die
//     dritte Potenz beschränkt den Effekt auf einen schmalen Rand.
//   + emissive: eigenes Leuchten, unabhängig vom Licht (Kristalle, Treffer).
//   Alpha = Alpha von Materialfarbe/tint · Vertexfarbe.
//
// Die Erklärungen stehen hier außerhalb: Alles zwischen R"( und )" ist Teil
// des Shader-Textes (C++-Rohstring) und wird so an die Grafikkarte übergeben.
// ---------------------------------------------------------------------------
const char* kVertexShader = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPosition;
out vec3 fragNormal;
out vec4 fragColor;
void main() {
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kFragmentShader = R"(
#version 330
in vec3 fragPosition;
in vec3 fragNormal;
in vec4 fragColor;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 viewPos;
uniform vec3 emissive;
out vec4 finalColor;
void main() {
    vec3 n = normalize(fragNormal);
    vec3 l = normalize(-lightDir);
    float diff = max(dot(n, l), 0.0);
    float sky = 0.5 + 0.5 * n.y;
    vec3 base = colDiffuse.rgb * fragColor.rgb;
    vec3 col = base * (0.36 + 0.16 * sky + 0.62 * diff);
    vec3 v = normalize(viewPos - fragPosition);
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    col += vec3(0.10, 0.12, 0.16) * rim;
    col += emissive;
    finalColor = vec4(col, colDiffuse.a * fragColor.a);
}
)";

} // namespace

// Welt-Matrix: erst skalieren, dann um Y drehen, dann verschieben
// (Erklärung der Reihenfolge in Assets.hpp).
Matrix TRS(Vector3 pos, float yaw, float scale) {
    Matrix m = MatrixMultiply(MatrixScale(scale, scale, scale), MatrixRotateY(yaw));
    return MatrixMultiply(m, MatrixTranslate(pos.x, pos.y, pos.z));
}

// Wie oben, mit Neigungen. Drehreihenfolge: erst roll (Längsachse), dann pitch
// (Querachse), zuletzt yaw (Kurs). So neigt sich das Schiff um seine eigenen
// Achsen, und der Kurs dreht das schon geneigte Schiff als Ganzes.
Matrix TRS(Vector3 pos, float yaw, float pitch, float roll, float scale) {
    Matrix m = MatrixScale(scale, scale, scale);
    m = MatrixMultiply(m, MatrixRotateZ(roll));
    m = MatrixMultiply(m, MatrixRotateX(pitch));
    m = MatrixMultiply(m, MatrixRotateY(yaw));
    return MatrixMultiply(m, MatrixTranslate(pos.x, pos.y, pos.z));
}

bool Assets::Init() {
    // Muss vor dem ersten LoadModel() eingetragen sein (siehe LoadFilePatched).
    SetLoadFileDataCallback(LoadFilePatched);

    // Assets-Ordner suchen: zuerst neben der ausführbaren Datei (funktioniert
    // unabhängig vom Arbeitsordner), dann relativ zum Arbeitsordner, auch ein
    // oder zwei Ebenen höher (z. B. Start aus build/ oder build/Release/).
    // Als Erkennungszeichen dient eine Modelldatei, die es immer gibt.
    const std::string app = GetApplicationDirectory();
    const char* candidates[] = {"assets/", "../assets/", "../../assets/"};
    std::vector<std::string> tries = {app + "assets/"};
    for (const char* c : candidates) tries.emplace_back(c);
    for (const auto& dir : tries) {
        if (FileExists((dir + "models/craft_speederA.glb").c_str())) {
            assetDir_ = dir;
            break;
        }
    }
    if (assetDir_.empty()) {
        TraceLog(LOG_ERROR, "Assets-Ordner nicht gefunden (erwartet: assets/models/*.glb)");
        return false;
    }

    // Shader übersetzen (passiert im Grafiktreiber). Schlägt das fehl (z. B.
    // kein OpenGL 3.3), nimmt raylib seinen Standard-Shader ohne Licht; die
    // Locations sind dann -1 und das Spiel läuft trotzdem, nur flacher.
    lit_ = LoadShaderFromMemory(kVertexShader, kFragmentShader);
    locLightDir_ = GetShaderLocation(lit_, "lightDir");
    locViewPos_ = GetShaderLocation(lit_, "viewPos");
    locEmissive_ = GetShaderLocation(lit_, "emissive");
    // Richtung, in die das Sonnenlicht fällt: steil von oben, leicht schräg.
    // Die Sonne steht damit oben, etwas rechts (+X) und auf der Seite der
    // Spielkamera (+Z; die Kamera schaut von +Z nach -Z, siehe main.cpp).
    // So sind die Seiten, die man sieht, beleuchtet. Ein Uniform behält
    // seinen Wert, bis man ihn ändert – einmal setzen reicht.
    Vector3 light = Vector3Normalize({-0.45f, -1.0f, -0.35f});
    SetShaderValue(lit_, locLightDir_, &light, SHADER_UNIFORM_VEC3);
    ready_ = true;
    return true;
}

void Assets::Shutdown() {
    for (auto& e : entries_)
        if (e.loaded) UnloadModel(e.model);
    entries_.clear();
    // UnloadModel gibt Meshes und Material-Daten frei, aber absichtlich nicht
    // Shader und Texturen der Materialien (die könnten mehrere Modelle teilen).
    // Den gemeinsamen Shader geben wir deshalb hier einmal selbst frei.
    if (ready_) UnloadShader(lit_);
    ready_ = false;
}

int Assets::Id(const std::string& name) { return ModelId(name); }

// Lädt das Modell zu einer ID beim ersten Bedarf ("lazy loading") und liefert
// den Eintrag. nullptr bei ungültiger ID oder vor Init().
Assets::Entry* Assets::Ensure(int id) {
    if (!ready_ || id < 0 || id >= ModelCount()) return nullptr;
    // Das Modell-Verzeichnis vergibt neue IDs, sobald jemand einen neuen Namen
    // abfragt. entries_ wächst deshalb einfach mit.
    if (id >= static_cast<int>(entries_.size())) entries_.resize(static_cast<size_t>(id) + 1);
    Entry& e = entries_[static_cast<size_t>(id)];
    if (e.loaded) return &e;

    const std::string path = assetDir_ + "models/" + ModelName(id) + ".glb";
    e.model = LoadModel(path.c_str());
    e.loaded = true;   // auch bei Fehler: nicht jedes Frame erneut versuchen
    if (e.model.meshCount == 0 || e.model.meshes[0].vertexCount == 0) {
        TraceLog(LOG_WARNING, "Modell konnte nicht geladen werden: %s", path.c_str());
    }
    // Jedes Material bekommt unseren Beleuchtungs-Shader. Die Diffusfarbe
    // (MATERIAL_MAP_DIFFUSE.color, aus "baseColorFactor" der GLB-Datei) ist
    // die Grundfarbe des Materials; sie wird für das Umfärben gesichert.
    for (int i = 0; i < e.model.materialCount; ++i) {
        e.original.push_back(e.model.materials[i].maps[MATERIAL_MAP_DIFFUSE].color);
        e.model.materials[i].shader = lit_;
    }
    ApplyPalette(e);

    // Alle Kit-Modelle haben in ihrem Modellknoten (der Wurzel der Szene) eine
    // Verschiebung um (2, 0, 1.5). raylib rechnet sie beim Laden fest in die
    // Eckpunkte ein. base schiebt sie wieder zurück, sodass der Ursprung des
    // Modells an seinem Bezugspunkt liegt (Mitte, auf dem Boden): Position im
    // Spiel = Position des Modells. Die Grenzen in shared/ModelBounds.inc
    // gelten ebenfalls schon für die korrigierte Lage.
    e.base = MatrixTranslate(-2.0f, 0.0f, -1.5f);
    BoundingBox b = GetModelBoundingBox(e.model);
    b.min = Vector3Add(b.min, {-2.0f, 0.0f, -1.5f});
    b.max = Vector3Add(b.max, {-2.0f, 0.0f, -1.5f});
    e.bounds = b;
    return &e;
}

void Assets::ApplyPalette(Entry& e) {
    for (int i = 0; i < e.model.materialCount && i < static_cast<int>(e.original.size()); ++i)
        e.model.materials[i].maps[MATERIAL_MAP_DIFFUSE].color = MoonTone(e.original[static_cast<size_t>(i)], moon_);
}

// Nur schon geladene Modelle müssen umgefärbt werden; später geladene
// bekommen die Farbe in Ensure() über ApplyPalette().
void Assets::SetMoonColor(MoonColor c) {
    if (c == moon_) return;
    moon_ = c;
    for (auto& e : entries_)
        if (e.loaded) ApplyPalette(e);
}

BoundingBox Assets::Bounds(int id) {
    Entry* e = Ensure(id);
    return e ? e->bounds : BoundingBox{{0, 0, 0}, {0, 0, 0}};
}

// Kameraposition für das Randlicht (viewPos im Fragment-Shader).
void Assets::SetCamera(Vector3 camPos) {
    SetShaderValue(lit_, locViewPos_, &camPos, SHADER_UNIFORM_VEC3);
}

// Zeichnet ein Modell. Dasselbe Model wird oft mehrmals pro Frame an
// verschiedenen Stellen gezeichnet (z. B. viele gleiche Felsen): Die
// Geometrie liegt nur einmal im Grafikspeicher, nur transform wird vor jedem
// Zeichnen neu gesetzt. Das geht, weil DrawModel sofort zeichnet (eigener
// Zeichenaufruf je Mesh) und nicht in raylibs Sammelpuffer wartet.
void Assets::Draw(int id, const Matrix& world, Color tint, Vector3 emissive) {
    Entry* ep = Ensure(id);
    if (!ep) return;
    Entry& e = *ep;
    // Erst den Kit-Versatz entfernen (im Maßstab des Modells), dann die
    // Welt-Matrix anwenden.
    e.model.transform = MatrixMultiply(e.base, world);
    // Uniforms gelten für alle folgenden Zeichenaufrufe mit diesem Shader, bis
    // man sie ändert. Da alle Modelle denselben Shader teilen, wird emissive
    // vor JEDEM Modell gesetzt – sonst würde z. B. ein Schiff nach einem
    // leuchtenden Kristall ebenfalls grün leuchten.
    SetShaderValue(lit_, locEmissive_, &emissive, SHADER_UNIFORM_VEC3);
    // Position 0 und Maßstab 1: Die ganze Platzierung steckt schon in
    // model.transform. raylib multipliziert tint mit der Materialfarbe und
    // gibt das Ergebnis als colDiffuse an den Shader.
    DrawModel(e.model, {0, 0, 0}, 1.0f, tint);
}

void Assets::Draw(int id, Vector3 pos, float yaw, float scale, Color tint, Vector3 emissive) {
    Draw(id, TRS(pos, yaw, scale), tint, emissive);
}
