// ---------------------------------------------------------------------------
// MapEditor.cpp – Karteneditor und Kartenliste (Schnittstelle und Bedienung:
// MapEditor.hpp).
//
// Aufbau:
//   - MapListView: Liste der Karten-Dateien (auch für die Kartenauswahl bei
//     "Neues Spiel" in main.cpp).
//   - MapEditorScreen: Kartenliste und Bearbeiten. Gezeichnet wird mit einer
//     eigenen World (world_): BuildMirror baut Boden, Felswall und Stationen,
//     ApplyMap setzt nach jeder Änderung die Teile der Karte ein. So sieht
//     die Karte im Editor genau so aus wie später im Spiel, und der
//     Monorail-Zug fährt mit derselben Zeichenfunktion (Render.cpp).
//     Einsatzkarten baut World::NewInstance nach jeder Änderung als eigene
//     Welt auf (Boden, Himmel, Tor mit Treppe, Kristalle), wie im Einsatz.
//
// Raster und Bezugspunkt (PlacementAt):
//   Jedes Segment belegt fw x fd Rasterfelder (aus seinen Abmessungen in
//   ModelBounds.inc mal Größe, aufgerundet). Unter dem Zeiger wird der
//   belegte Bereich am Raster ausgerichtet. Die Mitte der Modell-Box kommt
//   auf die Mitte dieses Bereichs – nur bei Schienen der Modellursprung, denn
//   deren Enden liegen genau auf Seitenmitten bzw. (große Kurve) auf
//   Rasterlinien (siehe GameMap.cpp, "Monorail-Kreise").
// ---------------------------------------------------------------------------
#include "MapEditor.hpp"
#include "Assets.hpp"
#include "Audio.hpp"
#include "ClientConfig.hpp"
#include "ControlsMath.hpp"
#include "Models.hpp"
#include "NetClient.hpp"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <ctime>

using namespace ui;

namespace {

// Kulisse des Editors: alle Bauplätze mit Stationen (im Mehrspieler kann es
// bis zu 5 Handelsstationen und 4 Werften geben), ganz ausgebaut. So sieht
// man, wo Stationen stehen können und wie viel Platz sie brauchen.
void BuildBackdrop(World& w, unsigned int seed) {
    GameSettings gs;
    gs.tradeStations = limits::MAX_TRADE_STATIONS;
    gs.shipyards = limits::MAX_SHIPYARDS;
    w.BuildMirror(seed, gs);
    w.totalDelivered = cfg::STATION_THRESHOLDS[cfg::STATION_STAGES - 1];
    w.lastStage = w.StationStage();
}

std::string Trim(const std::string& t) {
    size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
}

// Eingabefilter wie im Mehrspieler-Menü (Multiplayer.cpp)
bool NameChar(int cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '_' || cp == '-';
}
bool AddressChar(int cp) { return cp > 32 && cp < 127; }

void WipeText(std::string& t) {   // Passwort nicht länger als nötig im Speicher
    std::fill(t.begin(), t.end(), '\0');
    t.clear();
}

// Mehrzeiligen Text mit Umbruch an Leerzeichen zeichnen (wie TextBlock in
// Multiplayer.cpp); liefert die belegte Höhe
float TextBlock(const std::string& text, float x, float y, float size, float maxW, Color c) {
    float yy = y;
    std::string line, word;
    auto flush = [&](bool last) {
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && Width(candidate, size) > maxW) {
            Text(line, x, yy, size, c);
            yy += size * 1.3f;
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
        if (last && !line.empty()) {
            Text(line, x, yy, size, c);
            yy += size * 1.3f;
        }
    };
    for (char ch : text) {
        if (ch == ' ') flush(false);
        else word.push_back(ch);
    }
    flush(true);
    return yy - y;
}

// Rechteck am Boden (Belegung eines Segments), knapp über dem Raster
// (y: Höhe, Einsatzkarte: in der gewählten Ebene)
void GroundRect(float cx, float cz, float w, float d, Color c, float y = 0.1f) {
    const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, z0 = cz - d * 0.5f, z1 = cz + d * 0.5f;
    DrawLine3D({x0, y, z0}, {x1, y, z0}, c);
    DrawLine3D({x1, y, z0}, {x1, y, z1}, c);
    DrawLine3D({x1, y, z1}, {x0, y, z1}, c);
    DrawLine3D({x0, y, z1}, {x0, y, z0}, c);
}

// Gegner-Startplatz: ein Bodenpirat (Haut 4, wie in SpawnPirateGroup) mit
// rotem Ring
void EnemyFigure(Vector3 pos, float yaw, Color tint) {
    g_assets.DrawCharacter(4, "idle", 0.0f, true, TRS(pos, yaw + PI, cfg::TROOPER_HEIGHT / 3.77f), tint);
    DrawCircle3D({pos.x, 0.08f, pos.z}, 0.9f, {1, 0, 0}, 90.0f, Color{255, 90, 80, 220});
}

// Ein Teil zeichnen (Vorschau unter dem Zeiger). Markierungen haben kein
// eigenes Modell: Tor, Kristalle bzw. ein Bodenpirat stehen dafür.
void DrawPiece(const MapPiece& q, Color tint) {
    if (q.model == MARKER_GATE) g_assets.Draw(ModelId("gate_complex"), {q.x, 0.0f, q.z}, q.yaw, q.scale, tint);
    else if (q.model == MARKER_CRYSTAL) g_assets.Draw(ModelId("rock_crystals"), {q.x, 0.0f, q.z}, q.yaw, mapgrid::CRYSTAL_SCALE, tint);
    else if (q.model == MARKER_ENEMY) EnemyFigure({q.x, 0.0f, q.z}, q.yaw, tint);
    else g_assets.Draw(ModelId(q.model), {q.x, MapPieceY(q), q.z}, q.yaw, q.scale, tint);
}

// Nächster Wert eines enums mit Count am Ende (im Kreis)
template <typename E>
E NextValue(E v) {
    return static_cast<E>((static_cast<int>(v) + 1) % static_cast<int>(E::Count));
}

} // namespace

// ===========================================================================
// MapListView
// ===========================================================================
void MapListView::Refresh(bool moonOnly) {
    maps_ = ListMaps();
    if (moonOnly)
        maps_.erase(std::remove_if(maps_.begin(), maps_.end(), [](const MapFileInfo& m) { return m.kind != MapKind::Moon; }),
                    maps_.end());
    row_ = -1;
    scroll_ = 0.0f;
    fresh_ = true;
}

bool MapListView::Draw(Rectangle r, float s, const char* firstEntry) {
    hasFirst_ = firstEntry != nullptr;
    if (fresh_) {
        if (hasFirst_) row_ = 0;
        fresh_ = false;
    }
    const int rows = static_cast<int>(maps_.size()) + (hasFirst_ ? 1 : 0);
    const float rowH = 40 * s;
    const int visible = std::max(1, static_cast<int>((r.height - 8 * s) / rowH));
    Panel(r, Color{18, 22, 30, 235});

    // Pfeiltasten wählen, die Auswahl bleibt sichtbar
    bool keyMoved = false;
    if (rows > 0 && !TextInputActive()) {
        if (IsKeyPressed(KEY_DOWN)) { row_ = std::min(rows - 1, row_ + 1); keyMoved = true; }
        if (IsKeyPressed(KEY_UP)) { row_ = std::max(0, row_ - 1); keyMoved = true; }
        if (keyMoved) g_audio.Play(Sfx::UiMove);
    }
    const Vector2 m = PointerPos();
    if (CheckCollisionPointRec(m, r)) scroll_ -= GetMouseWheelMove();
    if (keyMoved) {
        if (row_ < scroll_) scroll_ = static_cast<float>(row_);
        if (row_ >= scroll_ + visible) scroll_ = static_cast<float>(row_ - visible + 1);
    }
    scroll_ = Clamp(scroll_, 0.0f, static_cast<float>(std::max(0, rows - visible)));

    bool open = false;
    const int first = static_cast<int>(scroll_);
    for (int i = 0; i < visible && first + i < rows; ++i) {
        const int row = first + i;
        Rectangle rr{r.x + 6 * s, r.y + 4 * s + i * rowH, r.width - 12 * s, rowH - 4 * s};
        const bool hover = CheckCollisionPointRec(m, rr);
        if (row == row_) DrawRectangleRec(rr, Color{90, 62, 28, 255});
        else if (hover) DrawRectangleRec(rr, PANEL_LIGHT);
        std::string label = firstEntry ? std::string(firstEntry) : std::string();
        if (!(hasFirst_ && row == 0)) {
            const MapFileInfo& info = maps_[static_cast<size_t>(row - (hasFirst_ ? 1 : 0))];
            label = info.name;
            if (info.kind == MapKind::Mission) label += Fmt("  (Einsatzkarte %s)", MapThemeName(info.theme));
        }
        Text(label, rr.x + 12 * s, rr.y + (rr.height - 22 * s) * 0.5f, 22 * s, row == row_ ? ACCENT : TEXT);
        if (hover && PointerPressed()) {
            // Zweiter Klick auf dieselbe Zeile innerhalb von 0,4 s = Doppelklick
            const double now = GetTime();
            if (row == lastClickRow_ && now - lastClick_ < 0.4) open = true;
            lastClick_ = now;
            lastClickRow_ = row;
            if (row != row_) g_audio.Play(Sfx::UiMove);
            row_ = row;
        }
    }
    if (rows == 0) TextCentered("Noch keine Karten gespeichert.", r.x + r.width * 0.5f, r.y + r.height * 0.5f - 11 * s, 22 * s, MUTED);
    // Bildlaufleiste, wenn nicht alles hineinpasst
    if (rows > visible) {
        const float h = r.height * visible / rows;
        const float y = r.y + (r.height - h) * scroll_ / std::max(1, rows - visible);
        DrawRectangleRec({r.x + r.width - 5 * s, y, 3 * s, h}, ACCENT_DIM);
    }
    if (row_ >= 0 && !TextInputActive() && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) open = true;
    return open;
}

// ===========================================================================
// MapEditorScreen
// ===========================================================================
MapEditorScreen::MapEditorScreen() {
    BuildBackdrop(world_, static_cast<unsigned int>(std::time(nullptr)));
    nameField_.maxChars = mapgrid::MAX_NAME;
    nameField_.placeholder = "Name der Karte";
    serverField_.placeholder = "z. B. localhost:7777";
    serverField_.maxChars = 120;
    serverField_.accept = AddressChar;
    playerField_.placeholder = "Spielername";
    playerField_.maxChars = net::MAX_NAME;
    playerField_.accept = NameChar;
    passField_.placeholder = "Passwort";
    passField_.maxChars = 64;
    passField_.password = true;
    list_.Refresh();
}

MapEditorScreen::~MapEditorScreen() {
    if (uploadNet_) uploadNet_->Disconnect();
    WipeText(passField_.text);
}

void MapEditorScreen::Update(float dt) {
    world_.time += dt;   // Züge fahren, Raketen starten (Render.cpp); Rover stehen hier still
    if (uploadNet_) UpdateUpload(dt);
    if (msgT_ > 0.0f) msgT_ -= dt;
    if (backArmed_ > 0.0f) backArmed_ -= dt;

    if (mode_ == Mode::List) {
        // Kartenliste: die Kamera zieht langsam im Kreis über die Karte
        camHeight_ = 95.0f;
        camTarget_ = {sinf(world_.time * 0.05f) * 80.0f, 0.0f, cosf(world_.time * 0.05f) * 80.0f};
    } else {
        // Verschieben mit der Tastatur, schneller, je weiter die Kamera weg ist.
        // Nicht während im Namensfeld getippt wird (dann sind WASD/R Buchstaben).
        if (!TextInputActive() && !uploadOpen_) {
            const float v = camHeight_ * 1.2f * dt;
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) camTarget_.z -= v;
            if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) camTarget_.z += v;
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) camTarget_.x -= v;
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) camTarget_.x += v;
            if (IsKeyPressed(KEY_R)) rot_ = (rot_ + 1) % 4;
            if (Mission() && IsKeyPressed(KEY_E)) level_ = std::min(mapgrid::MAX_LEVEL, level_ + 1);
            if (Mission() && IsKeyPressed(KEY_Q)) level_ = std::max(0, level_ - 1);
        }
        // Mittlere Maustaste ziehen: die Karte folgt dem Zeiger (ungefähr;
        // senkrecht im Bild gehört wegen der schrägen Sicht mehr Boden dazu)
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            const Vector2 d = GetMouseDelta();
            const float k = camHeight_ * 0.9f / static_cast<float>(std::max(1, GetScreenHeight()));
            camTarget_.x -= d.x * k;
            camTarget_.z -= d.y * k * 1.3f;
        }
        // Mausrad: zoomen (nicht über dem Werkzeugfenster)
        const float wheel = GetMouseWheelMove();
        // (Einsatzkarte: näher heran, das Raster ist feiner)
        if (wheel != 0.0f && !PointerOverPanel(PointerPos()))
            camHeight_ = Clamp(camHeight_ * (1.0f - wheel * 0.12f), Mission() ? 5.0f : 18.0f, Mission() ? 140.0f : 260.0f);
        const float half = MapHalf(map_);
        camTarget_.x = Clamp(camTarget_.x, -half, half);
        camTarget_.z = Clamp(camTarget_.z, -half, half);
    }
    // Schräg von oben, wie im Spiel, aber weiter weg und steiler
    cam_.up = {0, 1, 0};
    cam_.fovy = 45.0f;
    cam_.projection = CAMERA_PERSPECTIVE;
    cam_.target = camTarget_;
    cam_.position = {camTarget_.x, camHeight_, camTarget_.z + camHeight_ * 0.55f};
    world_.drawRange = camHeight_ * 1.8f + 40.0f;
}

void MapEditorScreen::Draw() {
    // Punkt unter dem Zeiger in der Höhe der gewählten Ebene (Strahl um die
    // Höhe nach unten verschoben, dann wie auf dem Boden)
    Ray ray = GetScreenToWorldRay(PointerPos(), cam_);
    if (mode_ == Mode::Edit) ray.position.y -= Lift();
    groundOk_ = controls::RayToGround(ray, &ground_);

    if (mode_ == Mode::Edit && !uploadOpen_) {
        // Setzen und Löschen vor dem Zeichnen, damit das Bild gleich den
        // neuen Stand zeigt. Ein Klick zählt nur, wenn er nicht auf einem
        // Fenster landet; danach wird gemalt bzw. radiert, solange die Taste
        // gehalten wird.
        const bool overUi = PointerOverPanel(PointerPos());
        if (PointerPressed() && !overUi) {
            painting_ = segment_ >= 0;
            lastX_ = lastZ_ = 1e9f;
        }
        if (!PointerDown()) painting_ = false;
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !overUi) erasing_ = true;
        if (!IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) erasing_ = false;
        if (painting_ && groundOk_ && segment_ >= 0) {
            const Placement p = PlacementAt(ground_);
            if (p.valid && (fabsf(p.x - lastX_) > 0.01f || fabsf(p.z - lastZ_) > 0.01f)) {
                Place(p);
                lastX_ = p.x;
                lastZ_ = p.z;
            }
        }
        if (erasing_ && groundOk_) {
            const int i = PieceAt(ground_);
            if (i >= 0) {
                map_.pieces.erase(map_.pieces.begin() + i);
                dirty_ = true;
                Rebuild();
            }
        }
    }

    BeginMode3D(cam_);
    DrawScene();
    EndMode3D();
    if (mode_ == Mode::List) DrawList();
    else DrawEdit();
}

// ---------------------------------------------------------------------------
// Karte öffnen, ändern, speichern
// ---------------------------------------------------------------------------
void MapEditorScreen::OpenMap(const GameMap& m, const std::string& stem) {
    map_ = m;
    stem_ = stem;
    nameField_.text = m.name;
    nameField_.focused = false;
    dirty_ = false;
    segment_ = -1;
    rot_ = 0;
    level_ = 0;
    painting_ = erasing_ = false;
    backArmed_ = 0.0f;
    camTarget_ = {0, 0, 0};
    camHeight_ = Mission() ? 45.0f : 80.0f;
    mode_ = Mode::Edit;
    // erster Reiter dieser Kartenart
    group_ = 0;
    while (group_ < static_cast<int>(SegmentGroup::Count) && !GroupForKind(static_cast<SegmentGroup>(group_), map_.kind)) ++group_;
    // Mondkarte nach einer Einsatzkarte: wieder die Kulisse mit den Stationen
    if (!Mission() && world_.instance) BuildBackdrop(world_, static_cast<unsigned int>(std::time(nullptr)));
    Rebuild();
}

void MapEditorScreen::Rebuild() {
    if (Mission()) {
        const float t = world_.time;   // Animationen laufen weiter
        world_.NewInstance(map_, 1);
        world_.time = t;
    } else {
        world_.ApplyMap(map_);
    }
    trackPieces_ = 0;
    for (const MapPiece& p : map_.pieces)
        if (IsTrack(p.model)) ++trackPieces_;
    loopPieces_ = 0;
    for (const MonorailLoop& l : world_.monorails) loopPieces_ += l.pieces;
}

MapEditorScreen::Placement MapEditorScreen::PlacementAt(Vector3 g) const {
    const MapSegment& seg = MapSegments()[static_cast<size_t>(segment_)];
    const BoundingBox b = SegmentBounds(seg.model);
    const float C = MapCell(map_.kind), H = MapHalf(map_);
    Placement p;
    p.scale = seg.scale;
    p.yaw = rot_ * PI * 0.5f;
    // Einsatzkarte: Die Teile der beiden Kits haben ihren Ursprung in der
    // Feldmitte, Mauern, Zäune und Hecken stehen an einer Feldkante. Deshalb
    // rastet dort der Ursprung ein (nicht die Mitte der Box), belegt sind die
    // Felder rund um den Ursprung; Überstände wie die Traufe eines Dachs
    // zählen nicht mit. Markierungen wie bisher.
    const bool origin = Mission() && !IsMarker(seg.model);
    if (origin) {
        const float hx = std::max(fabsf(b.min.x), fabsf(b.max.x)) * seg.scale;
        const float hz = std::max(fabsf(b.min.z), fabsf(b.max.z)) * seg.scale;
        p.fw = std::max(1, static_cast<int>(ceilf(2.0f * hx / C - 0.2f)));
        p.fd = std::max(1, static_cast<int>(ceilf(2.0f * hz / C - 0.2f)));
    } else {
        // Belegte Felder; 0,05 Spielraum, damit z. B. genau 1,0 Felder nicht zu 2 werden
        p.fw = std::max(1, static_cast<int>(ceilf((b.max.x - b.min.x) * seg.scale / C - 0.05f)));
        p.fd = std::max(1, static_cast<int>(ceilf((b.max.z - b.min.z) * seg.scale / C - 0.05f)));
    }
    if (rot_ % 2 == 1) std::swap(p.fw, p.fd);
    // Bereich am Raster ausrichten: linke Kante = gerundet (Zeiger - halbe Breite)
    p.cx = (roundf(g.x / C - p.fw * 0.5f) + p.fw * 0.5f) * C;
    p.cz = (roundf(g.z / C - p.fd * 0.5f) + p.fd * 0.5f) * C;
    Vector2 off{0, 0};
    if (!IsTrack(seg.model) && !origin)
        off = MapRotate((b.min.x + b.max.x) * 0.5f * seg.scale, (b.min.z + b.max.z) * 0.5f * seg.scale, p.yaw);
    p.x = p.cx - off.x;
    p.z = p.cz - off.y;
    p.valid = fabsf(p.cx) + p.fw * C * 0.5f <= H + 0.01f && fabsf(p.cz) + p.fd * C * 0.5f <= H + 0.01f;
    // Markierungen nur auf dem Boden
    p.lift = IsMarker(seg.model) ? 0.0f : Lift();
    return p;
}

// Teil unter dem Zeiger: Abstand zur Mitte seiner Box im Verhältnis zu ihrer
// Größe. So gewinnt ein kleiner Fels direkt unter dem Zeiger gegen den
// großen Krater, in dem er liegt. Einsatzkarte: nur Teile der gewählten
// Ebene (Markierungen liegen auf dem Boden).
int MapEditorScreen::PieceAt(Vector3 g) const {
    int best = -1;
    float bestRel = 1e9f;
    const float minR = Mission() ? 0.45f : 1.2f;
    for (int i = 0; i < static_cast<int>(map_.pieces.size()); ++i) {
        const MapPiece& q = map_.pieces[static_cast<size_t>(i)];
        if (Mission() && fabsf(q.lift - Lift()) > 0.01f) continue;
        const BoundingBox b = SegmentBounds(q.model);
        const Vector2 c = MapRotate((b.min.x + b.max.x) * 0.5f * q.scale, (b.min.z + b.max.z) * 0.5f * q.scale, q.yaw);
        const float r = std::max(minR, std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * q.scale);
        const float d = Vector2Distance({g.x, g.z}, {q.x + c.x, q.z + c.y});
        if (d <= r && d / r < bestRel) {
            bestRel = d / r;
            best = i;
        }
    }
    return best;
}

void MapEditorScreen::Place(const Placement& p) {
    const MapSegment& seg = MapSegments()[static_cast<size_t>(segment_)];
    // Ein Teil mit demselben Bezugspunkt (und derselben Ebene) wird ersetzt –
    // sonst stapeln sich beim Malen Teile übereinander (und Schienen bekämen
    // Abzweigungen). Das Stargate gibt es nur einmal: Es zieht um.
    // Einsatzkarte: nur bei gleicher Drehung, denn in einem Feld stehen oft
    // mehrere Mauern an verschiedenen Kanten (alle mit demselben Ursprung).
    const bool gate = seg.model == std::string(MARKER_GATE);
    const bool mission = Mission();
    map_.pieces.erase(std::remove_if(map_.pieces.begin(), map_.pieces.end(),
                                     [&](const MapPiece& q) {
                                         return (gate && q.model == MARKER_GATE) ||
                                                (fabsf(q.x - p.x) < 0.01f && fabsf(q.z - p.z) < 0.01f && fabsf(q.lift - p.lift) < 0.01f &&
                                                 (!mission || fabsf(AngleDiff(q.yaw, p.yaw)) < 0.01f));
                                     }),
                      map_.pieces.end());
    if (static_cast<int>(map_.pieces.size()) >= mapgrid::MAX_PIECES) {
        Message(Fmt("Eine Karte hat höchstens %d Teile.", mapgrid::MAX_PIECES), BAD);
        g_audio.Play(Sfx::Denied);
        painting_ = false;
        return;
    }
    map_.pieces.push_back({seg.model, p.x, p.z, p.yaw, p.scale, p.lift});
    dirty_ = true;
    Rebuild();
    g_audio.Play(Sfx::UiMove, 0.3f);
}

// "Zufall generieren": dieselbe Mondoberfläche wie in einer Zufallswelt
// (World::GenerateTerrain: Krater, Felsen, Meteoriten, Fundstücke, Rover), mit
// Abstand zu allen Bauplätzen und den Schienen zwischen ihnen. Ersetzt die
// Teile der Gruppen Gelände, Felsen und Sonstiges; Straßen, Monorail und
// Gebäude bleiben stehen. Die Tore der Zufallswelt (Gruppe Gebäude) kommen
// nicht mit, sonst würden es bei jedem Klick mehr.
void MapEditorScreen::RandomFill() {
    static unsigned int counter = 0;
    World tmp;
    BuildBackdrop(tmp, static_cast<unsigned int>(std::time(nullptr)) ^ (++counter * 7919u));
    std::vector<MapPiece> pieces;
    for (const MapPiece& p : map_.pieces) {
        const MapSegment* seg = FindSegment(p.model);
        if (seg && (seg->group == SegmentGroup::Roads || seg->group == SegmentGroup::Monorail || seg->group == SegmentGroup::Buildings))
            pieces.push_back(p);
    }
    for (const Deco& d : tmp.decos) {
        if (fabsf(d.pos.x) > cfg::WORLD_HALF || fabsf(d.pos.z) > cfg::WORLD_HALF) continue;   // Felswall
        const std::string name = ModelName(d.model);
        const MapSegment* seg = FindSegment(name);
        if (!seg || seg->group == SegmentGroup::Buildings || static_cast<int>(pieces.size()) >= mapgrid::MAX_PIECES) continue;
        pieces.push_back({name, d.pos.x, d.pos.z, d.yaw, d.scale});
    }
    map_.pieces = std::move(pieces);
    dirty_ = true;
    Rebuild();
    Message("Zufällige Mondoberfläche erzeugt.", GOOD);
    g_audio.Play(Sfx::UiConfirm);
}

void MapEditorScreen::Save() {
    const std::string name = Trim(nameField_.text);
    if (name.empty()) {
        Message("Bitte zuerst einen Namen eingeben.", BAD);
        g_audio.Play(Sfx::Denied);
        return;
    }
    std::string error;
    if (Mission() && !CheckMission(error)) {
        Message(error, BAD);
        g_audio.Play(Sfx::Denied);
        return;
    }
    nameField_.text = name;
    map_.name = name;
    const std::string stem = MapFileStem(name);
    const bool replaced = stem != stem_ && MapFileExists(stem);
    if (!SaveMapFile(map_)) {
        Message("Speichern fehlgeschlagen (Ordner \"karten\" schreibgeschützt?).", BAD);
        g_audio.Play(Sfx::Denied);
        return;
    }
    stem_ = stem;
    dirty_ = false;
    Message(replaced ? "Gespeichert, die vorhandene Karte \"" + name + "\" wurde ersetzt." : "Gespeichert: karten/" + stem + ".karte",
            GOOD);
    g_audio.Play(Sfx::UiConfirm);
}

// Zurück: aus der Kartenliste ins Hauptmenü, beim Bearbeiten zur
// Kartenliste. Ungespeicherte Änderungen verwirft erst ein zweiter Druck
// innerhalb von 3 Sekunden.
void MapEditorScreen::Back() {
    if (mode_ == Mode::List) {
        wantsMenu_ = true;
        return;
    }
    if (Dirty() && backArmed_ <= 0.0f) {
        backArmed_ = 3.0f;
        Message("Nicht gespeichert! Nochmal \"Zurück\" verwirft die Änderungen.", ACCENT);
        g_audio.Play(Sfx::Denied);
        return;
    }
    backArmed_ = 0.0f;
    mode_ = Mode::List;
    if (world_.instance) BuildBackdrop(world_, static_cast<unsigned int>(std::time(nullptr)));   // Liste: über dem Mond
    list_.Refresh();
    g_audio.Play(Sfx::UiBack);
}

// Genau ein Stargate (Ankunft und Rückkehr) und genug Kristallplätze für die
// Proben eines Einsatzes (cfg::SAMPLE_GOAL).
bool MapEditorScreen::CheckMission(std::string& error) const {
    int gates = 0, crystals = 0;
    for (const MapPiece& q : map_.pieces) {
        if (q.model == MARKER_GATE) ++gates;
        else if (q.model == MARKER_CRYSTAL) ++crystals;
    }
    if (gates != 1) error = "Eine Einsatzkarte braucht genau ein Stargate (Reiter \"Markierungen\").";
    else if (crystals < cfg::SAMPLE_GOAL) error = Fmt("Eine Einsatzkarte braucht mindestens %d Kristallplätze (jetzt %d).", cfg::SAMPLE_GOAL, crystals);
    return error.empty();
}

void MapEditorScreen::Message(const std::string& text, Color c) {
    msg_ = text;
    msgColor_ = c;
    msgT_ = 3.5f;
}

// ---------------------------------------------------------------------------
// An Server senden
// ---------------------------------------------------------------------------
void MapEditorScreen::OpenUpload() {
    if (Trim(nameField_.text).empty()) {
        Message("Bitte zuerst einen Namen eingeben.", BAD);
        g_audio.Play(Sfx::Denied);
        return;
    }
    uploadOpen_ = true;
    nameField_.focused = false;
    if (serverField_.text.empty()) serverField_.text = g_clientConfig.server;
    if (playerField_.text.empty()) playerField_.text = g_clientConfig.name;   // zuletzt benutzter Spielername
    serverField_.focused = false;
    playerField_.focused = playerField_.text.empty();
    passField_.focused = !playerField_.focused;
    uploadMsg_.clear();
    g_audio.Play(Sfx::UiConfirm);
}

void MapEditorScreen::CloseUpload() {
    if (uploadNet_) {
        uploadNet_->Disconnect();
        uploadNet_.reset();
    }
    WipeText(passField_.text);
    uploadOpen_ = false;
    g_audio.Play(Sfx::UiBack);
}

void MapEditorScreen::StartUpload() {
    if (uploadNet_) return;
    const std::string addr = Trim(serverField_.text), player = Trim(playerField_.text);
    std::string error;
    if (addr.empty()) error = "Bitte die Serveradresse eingeben.";
    else if (!net::ValidPlayerName(player)) error = net::ErrorText(net::ErrorCode::NameInvalid);
    else if (Utf8Length(passField_.text) < net::MIN_PASSWORD) error = Fmt("Das Passwort hat mindestens %d Zeichen.", net::MIN_PASSWORD);
    if (!error.empty()) {
        uploadMsg_ = error;
        uploadMsgColor_ = BAD;
        g_audio.Play(Sfx::Denied);
        return;
    }
    uploadMap_ = map_;
    uploadMap_.name = Trim(nameField_.text);
    uploadNet_ = std::make_unique<NetClient>();
    uploadSent_ = false;
    if (!uploadNet_->Connect(addr)) {
        FinishUpload(uploadNet_->TakeError(), false);
        return;
    }
    uploadMsg_ = "Verbinde mit " + addr + " ...";
    uploadMsgColor_ = MUTED;
}

// Jedes Frame, solange die Verbindung besteht: Sobald die Lobby erreicht ist
// (ServerInfo mit dem Salz für den Schlüssel), Karte schicken (UploadMap),
// dann auf die Antwort warten.
void MapEditorScreen::UpdateUpload(float dt) {
    NetClient& n = *uploadNet_;
    n.Update(dt, ShipInput{}, false, false);
    const std::string err = n.TakeError();
    bool ok = false;
    std::string text;
    if (!err.empty()) {
        FinishUpload(err, false);
    } else if (n.TakeUploadResult(ok, text)) {
        if (ok) {
            g_clientConfig.server = Trim(serverField_.text);
            g_clientConfig.name = Trim(playerField_.text);
            g_clientConfig.Save();
        }
        FinishUpload(text, ok);
    } else if (!uploadSent_ && n.State() == NetState::Lobby && n.HasInfo()) {
        n.UploadMap(Trim(playerField_.text), passField_.text, uploadMap_);
        WipeText(passField_.text);
        uploadSent_ = true;
        uploadMsg_ = Fmt("Sende Karte (%d Teile) ...", static_cast<int>(uploadMap_.pieces.size()));
    }
}

void MapEditorScreen::FinishUpload(const std::string& text, bool ok) {
    uploadMsg_ = text;
    uploadMsgColor_ = ok ? GOOD : BAD;
    if (uploadNet_) {
        uploadNet_->Disconnect();
        uploadNet_.reset();
    }
    g_audio.Play(ok ? Sfx::UiConfirm : Sfx::Denied);
}

// ---------------------------------------------------------------------------
// Zeichnen
// ---------------------------------------------------------------------------
void MapEditorScreen::DrawScene() {
    world_.Draw(cam_);
    if (mode_ != Mode::Edit) return;
    const bool mission = Mission();
    for (const MapPiece& q : map_.pieces)
        if (q.model == MARKER_ENEMY) EnemyFigure({q.x, 0.0f, q.z}, q.yaw, WHITE);

    // Raster: feine Linien nur, wenn die Kamera nah genug ist; jede zehnte
    // Linie (alle 30 m, Einsatzkarte alle 10 m) immer. Nur um den Blickpunkt
    // herum, nicht über die ganze Karte. Einsatzkarte: in der Höhe der Ebene.
    const float C = MapCell(map_.kind), H = MapHalf(map_), y = 0.06f + Lift();
    const float reach = std::min(2.0f * H, camHeight_ * 1.6f + 20.0f);
    const bool fine = camHeight_ < (mission ? 60.0f : 140.0f);
    const float x0 = std::max(-H, floorf((camTarget_.x - reach) / C) * C), x1 = std::min(H, camTarget_.x + reach);
    const float z0 = std::max(-H, floorf((camTarget_.z - reach) / C) * C), z1 = std::min(H, camTarget_.z + reach);
    for (float x = x0; x <= x1; x += C) {
        const bool major = static_cast<int>(lroundf(x / C)) % 10 == 0;
        if (fine || major) DrawLine3D({x, y, z0}, {x, y, z1}, major ? Color{255, 255, 255, 70} : Color{255, 255, 255, 24});
    }
    for (float z = z0; z <= z1; z += C) {
        const bool major = static_cast<int>(lroundf(z / C)) % 10 == 0;
        if (fine || major) DrawLine3D({x0, y, z}, {x1, y, z}, major ? Color{255, 255, 255, 70} : Color{255, 255, 255, 24});
    }
    GroundRect(0, 0, 2 * H, 2 * H, ACCENT);   // Spielfeldrand

    if (!groundOk_ || PointerOverPanel(PointerPos())) return;
    // Vorschau: belegter Bereich (grün = passt, rot = ragt aus dem Spielfeld)
    // und das Modell selbst, leicht grün eingefärbt
    if (segment_ >= 0) {
        const Placement p = PlacementAt(ground_);
        GroundRect(p.cx, p.cz, p.fw * C, p.fd * C, p.valid ? Color{120, 255, 150, 220} : Color{255, 90, 80, 220}, 0.1f + p.lift);
        if (p.valid) DrawPiece({MapSegments()[static_cast<size_t>(segment_)].model, p.x, p.z, p.yaw, p.scale, p.lift}, Color{150, 255, 170, 255});
    }
    // Ohne gewähltes Segment (oder beim Radieren): das Teil markieren, das
    // ein Rechtsklick löschen würde
    if (segment_ < 0 || erasing_) {
        const int i = PieceAt(ground_);
        if (i >= 0) {
            const MapPiece& q = map_.pieces[static_cast<size_t>(i)];
            const BoundingBox b = SegmentBounds(q.model);
            const Vector2 c = MapRotate((b.min.x + b.max.x) * 0.5f * q.scale, (b.min.z + b.max.z) * 0.5f * q.scale, q.yaw);
            const float r = std::max(mission ? 0.45f : 1.2f, std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * q.scale);
            DrawCircle3D({q.x + c.x, 0.12f + q.lift, q.z + c.y}, r, {1, 0, 0}, 90.0f, BAD);
        }
    }
}

void MapEditorScreen::DrawList() {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const float pw = 780 * s, ph = 560 * s;
    const Rectangle panel{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    Panel(panel);
    const float x = panel.x + 28 * s, y = panel.y + 22 * s;
    Text("KARTENEDITOR", x, y, 40 * s, ACCENT);
    Text("Gespeicherte Karten", x, y + 52 * s, 22 * s, MUTED);
    const float bh = 54 * s, by = panel.y + ph - 24 * s - bh;
    const Rectangle lr{x, y + 86 * s, pw - 56 * s, by - 18 * s - (y + 86 * s)};
    const bool open = list_.Draw(lr, s);

    const float gap = 14 * s, bw = (pw - 56 * s - 3 * gap) / 4.0f;
    const bool load = Button({x, by, bw, bh}, "Karte laden", 20 * s, false, list_.HasSelection());
    const bool fresh = Button({x + bw + gap, by, bw, bh}, "Neue Mondkarte", 20 * s, false);
    const bool freshMission = Button({x + 2 * (bw + gap), by, bw, bh}, "Neue Einsatzkarte", 20 * s, false);
    const bool back = Button({x + 3 * (bw + gap), by, bw, bh}, "Zurück", 20 * s, false);

    if ((open || load) && list_.HasSelection()) {
        const MapFileInfo info = list_.Maps()[static_cast<size_t>(list_.SelectedMap())];
        GameMap m;
        if (LoadMapFile(info.stem, m)) {
            g_audio.Play(Sfx::UiConfirm);
            OpenMap(m, info.stem);
        } else {
            g_audio.Play(Sfx::Denied);
            list_.Refresh();
        }
    } else if (fresh || freshMission) {
        // Leere Karte mit einem freien Namen ("Neue Karte", "Neue Karte 2" ...)
        GameMap m;
        m.kind = freshMission ? MapKind::Mission : MapKind::Moon;
        const char* base = freshMission ? "Neue Einsatzkarte" : "Neue Karte";
        m.name = base;
        for (int n = 2; MapFileExists(MapFileStem(m.name)); ++n) m.name = Fmt("%s %d", base, n);
        g_audio.Play(Sfx::UiConfirm);
        OpenMap(m, "");
    } else if (back || BackPressed()) {
        Back();
    }
}

void MapEditorScreen::DrawToolPanel(Rectangle r, float s) {
    Panel(r);
    const float x = r.x + 14 * s, w = r.width - 28 * s;
    float y = r.y + 14 * s;
    const bool mission = Mission();
    if (!mission) {
        if (Button({x, y, w, 46 * s}, "Zufall generieren", 22 * s, false)) RandomFill();
        y += 52 * s;
        Text("Krater, Felsen und Fundstücke", x, y, 16 * s, MUTED);
        y += 30 * s;
    } else {
        // Einsatzkarte: Thema, Größe, Boden und Himmel, ein Klick wählt den
        // nächsten Wert. Wird das Feld kleiner, fallen Teile außerhalb weg.
        const float g = 6 * s, bw = (w - g) * 0.5f, bh = 34 * s;
        const std::string labels[4] = {std::string("Thema: ") + MapThemeName(map_.theme), Fmt("Größe: %d m", map_.size),
                                       std::string("Boden: ") + MapGroundName(map_.ground),
                                       std::string("Himmel: ") + MapSkyName(map_.sky)};
        for (int i = 0; i < 4; ++i) {
            if (!Button({x + (i % 2) * (bw + g), y + (i / 2) * (bh + g), bw, bh}, labels[i], 16 * s, false)) continue;
            if (i == 0) map_.theme = NextValue(map_.theme);
            else if (i == 2) map_.ground = NextValue(map_.ground);
            else if (i == 3) map_.sky = NextValue(map_.sky);
            else {
                int k = 0;
                while (k < 4 && mapgrid::MISSION_SIZES[k] != map_.size) ++k;
                map_.size = mapgrid::MISSION_SIZES[(k + 1) % 4];
                const float half = MapHalf(map_);
                const size_t before = map_.pieces.size();
                map_.pieces.erase(std::remove_if(map_.pieces.begin(), map_.pieces.end(),
                                                 [&](const MapPiece& q) { return fabsf(q.x) > half || fabsf(q.z) > half; }),
                                  map_.pieces.end());
                const int gone = static_cast<int>(before - map_.pieces.size());
                if (gone > 0) Message(Fmt("%d Teile außerhalb des kleineren Felds entfernt.", gone), ACCENT);
            }
            dirty_ = true;
            Rebuild();
            g_audio.Play(Sfx::UiMove);
        }
        y += 2 * (bh + g) + 10 * s;
    }

    // Reiter der Gruppen dieser Kartenart (Mondkarte: zwei Reihen à drei,
    // Einsatzkarte: fünf Reihen à zwei, ihre Namen sind länger)
    const int groups = static_cast<int>(SegmentGroup::Count);
    const int cols = mission ? 2 : 3;
    const float tg = 6 * s, tw = (w - (cols - 1) * tg) / cols, th = 34 * s;
    int tab = 0;
    for (int g = 0; g < groups; ++g) {
        if (!GroupForKind(static_cast<SegmentGroup>(g), map_.kind)) continue;
        const Rectangle tr{x + (tab % cols) * (tw + tg), y + (tab / cols) * (th + tg), tw, th};
        ++tab;
        if (Button(tr, SegmentGroupName(static_cast<SegmentGroup>(g)), 17 * s, g == group_) && g != group_) {
            group_ = g;
            g_audio.Play(Sfx::UiMove);
        }
    }
    y += ((tab + cols - 1) / cols) * (th + tg) + 8 * s;

    // Segmente des offenen Reiters; noch einmal anklicken hebt die Auswahl auf
    const auto& segs = MapSegments();
    for (int i = 0; i < static_cast<int>(segs.size()); ++i) {
        if (static_cast<int>(segs[static_cast<size_t>(i)].group) != group_) continue;
        if (Button({x, y, w, 34 * s}, segs[static_cast<size_t>(i)].label, 18 * s, i == segment_)) {
            segment_ = segment_ == i ? -1 : i;
            g_audio.Play(Sfx::UiMove);
        }
        y += 39 * s;
    }

    const float by = r.y + r.height - 14 * s;
    if (mission) Text(Fmt("Drehung: %d° [R]   Ebene: %d [Q/E]", rot_ * 90, level_), x, by - 22 * s, 18 * s, TEXT);
    else Text(Fmt("Drehung: %d°   [R]", rot_ * 90), x, by - 22 * s, 18 * s, TEXT);
    if (segment_ < 0) Text("Segment wählen, dann auf die Karte klicken", x, by - 48 * s, 15 * s, MUTED);
}

// Fenster "An Server senden" in der Bildmitte
void MapEditorScreen::DrawUpload(float s) {
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const float pw = 640 * s, ph = 560 * s;
    const Rectangle r{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    Panel(r, Color{18, 22, 30, 245});
    const bool busy = uploadNet_ != nullptr;
    const float x = r.x + 28 * s, fw = pw - 56 * s, fh = 46 * s;
    float y = r.y + 22 * s;
    Text("KARTE AN SERVER SENDEN", x, y, 30 * s, ACCENT);
    y += 42 * s;
    Text(Fmt("\"%s\"  ·  %d Teile", Trim(nameField_.text).c_str(), static_cast<int>(map_.pieces.size())), x, y, 20 * s, TEXT);
    y += 34 * s;

    CycleFocus({&serverField_, &playerField_, &passField_});   // Tab wechselt das Feld
    bool submit = false;
    const char* labels[3] = {"Server", "Spielername", "Passwort"};
    TextField* fields[3] = {&serverField_, &playerField_, &passField_};
    for (int i = 0; i < 3; ++i) {
        Text(labels[i], x, y, 18 * s, MUTED);
        y += 22 * s;
        submit = fields[i]->Draw({x, y, fw, fh}, 22 * s, !busy) || submit;
        y += fh + 10 * s;
    }
    y += TextBlock("Anmeldung wie im Mehrspieler. Der Betreiber muss dir das Hochladen auf der Verwaltungsseite erlauben und "
                   "kann die Karte dort für alle aktivieren. Gleicher Kartenname ersetzt deine ältere Fassung.",
                   x, y, 16 * s, fw, MUTED);
    y += 8 * s;
    if (!uploadMsg_.empty()) TextBlock(uploadMsg_, x, y, 18 * s, fw, uploadMsgColor_);

    const float bw = 200 * s, bh = 48 * s, by = r.y + ph - bh - 20 * s;
    const bool send = Button({x, by, bw, bh}, busy ? "Sende ..." : "Senden", 22 * s, false, !busy);
    const bool close = Button({r.x + pw - 28 * s - bw, by, bw, bh}, busy ? "Abbrechen" : "Schließen", 22 * s, false);
    if ((send || submit) && !busy) StartUpload();
    else if (close) CloseUpload();
}

void MapEditorScreen::DrawEdit() {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());

    // Namen der Stationen über der Karte (alle Bauplätze, siehe BuildBackdrop)
    for (const Station& st : world_.stations) {
        if (Dist2D(st.pos, camTarget_) > world_.drawRange) continue;
        const Vector2 p = GetWorldToScreen({st.pos.x, 8.0f, st.pos.z}, cam_);
        TextCentered(st.name, p.x, p.y, 18 * s, Color{255, 255, 255, 210});
    }

    const float m = 12 * s, pw = 300 * s;
    DrawToolPanel({m, m, pw, H - 2 * m}, s);

    // Obere Leiste: Name, Speichern, Zurück
    const float bx = m + pw + m;
    const Rectangle bar{bx, m, W - bx - m, 66 * s};
    Panel(bar);
    const float fh = 42 * s, fy = bar.y + (bar.height - fh) * 0.5f;
    const float backW = 140 * s, saveW = 170 * s, sendW = 230 * s, gap = 12 * s;
    const float rx = bar.x + bar.width - 16 * s;
    Text("Name", bar.x + 16 * s, fy + 10 * s, 22 * s, MUTED);
    const float fx = bar.x + 86 * s;
    const float fw = std::max(120 * s, std::min(360 * s, rx - backW - gap - saveW - gap - sendW - gap - fx));
    if (nameField_.Draw({fx, fy, fw, fh}, 22 * s, !uploadOpen_)) nameField_.focused = false;   // Enter beendet die Eingabe
    const bool dirty = Dirty();
    const bool back = Button({rx - backW, fy, backW, fh}, "Zurück", 22 * s, false, !uploadOpen_);
    const bool save = Button({rx - backW - gap - saveW, fy, saveW, fh}, dirty ? "Speichern *" : "Speichern", 22 * s, dirty, !uploadOpen_);
    const bool send = Button({rx - backW - gap - saveW - gap - sendW, fy, sendW, fh}, "An Server senden", 22 * s, false, !uploadOpen_);
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (save || (ctrl && IsKeyPressed(KEY_S) && !uploadOpen_)) Save();
    if (send) OpenUpload();

    if (msgT_ > 0.0f) {
        Color c = msgColor_;
        c.a = static_cast<unsigned char>(255 * std::min(1.0f, msgT_));
        TextCentered(msg_, bar.x + bar.width * 0.5f, bar.y + bar.height + 14 * s, 22 * s, c);
    }

    // Unten: Zustand der Karte und Tastenhilfe
    const int loops = static_cast<int>(world_.monorails.size());
    std::string mono;
    if (trackPieces_ == 0) {
        mono = "Monorail: keine Schienen";
    } else {
        if (loops == 0) mono = "Monorail: kein geschlossener Kreis";
        else if (loops == 1) mono = "Monorail: 1 geschlossener Kreis, der Zug fährt";
        else mono = Fmt("Monorail: %d geschlossene Kreise, die Züge fahren", loops);
        const int open = trackPieces_ - loopPieces_;
        if (open > 0) mono += Fmt("  ·  %d Schiene%s ohne Kreis", open, open == 1 ? "" : "n");
    }
    const float ty = H - m - 24 * s;
    if (Mission()) {
        // Einsatzkarte: die Markierungen (zum Speichern: 1 Stargate, genug Kristallplätze)
        int gates = 0, crystals = 0, enemies = 0;
        for (const MapPiece& q : map_.pieces) {
            if (q.model == MARKER_GATE) ++gates;
            else if (q.model == MARKER_CRYSTAL) ++crystals;
            else if (q.model == MARKER_ENEMY) ++enemies;
        }
        std::string error;
        Text(Fmt("Teile: %d  ·  Stargate: %d  ·  Kristallplätze: %d (mind. %d)  ·  Gegner-Startplätze: %d",
                 static_cast<int>(map_.pieces.size()), gates, crystals, cfg::SAMPLE_GOAL, enemies),
             bx, ty - 28 * s, 20 * s, CheckMission(error) ? GOOD : TEXT);
        Text("Linksklick setzen (halten: malen)  ·  Rechtsklick löschen  ·  R drehen  ·  Q/E Ebene  ·  "
             "WASD/Pfeile/Mitteltaste verschieben  ·  Mausrad zoomen  ·  Strg+S speichern",
             bx, ty, 16 * s, MUTED);
    } else {
        Text(Fmt("Teile: %d  ·  ", static_cast<int>(map_.pieces.size())) + mono, bx, ty - 28 * s, 20 * s, loops > 0 ? GOOD : TEXT);
        Text("Linksklick setzen (halten: malen)  ·  Rechtsklick löschen  ·  R drehen  ·  WASD/Pfeile/Mitteltaste verschieben  ·  "
             "Mausrad zoomen  ·  Strg+S speichern",
             bx, ty, 16 * s, MUTED);
    }

    if (uploadOpen_) DrawUpload(s);

    // ESC: erst das Fenster "An Server senden" schließen bzw. die Eingabe oder
    // Auswahl beenden, dann zurück zur Kartenliste
    if (BackPressed()) {
        if (uploadOpen_) CloseUpload();
        else if (nameField_.focused) nameField_.focused = false;
        else if (segment_ >= 0) segment_ = -1;
        else Back();
    } else if (back) {
        Back();
    }
}
