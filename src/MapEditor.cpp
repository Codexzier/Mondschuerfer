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
#include "ControlsMath.hpp"
#include "Models.hpp"
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

// Rechteck am Boden (Belegung eines Segments), knapp über dem Raster
void GroundRect(float cx, float cz, float w, float d, Color c) {
    const float y = 0.1f, x0 = cx - w * 0.5f, x1 = cx + w * 0.5f, z0 = cz - d * 0.5f, z1 = cz + d * 0.5f;
    DrawLine3D({x0, y, z0}, {x1, y, z0}, c);
    DrawLine3D({x1, y, z0}, {x1, y, z1}, c);
    DrawLine3D({x1, y, z1}, {x0, y, z1}, c);
    DrawLine3D({x0, y, z1}, {x0, y, z0}, c);
}

} // namespace

// ===========================================================================
// MapListView
// ===========================================================================
void MapListView::Refresh() {
    maps_ = ListMaps();
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
        const std::string label = (hasFirst_ && row == 0) ? std::string(firstEntry) : maps_[static_cast<size_t>(row - (hasFirst_ ? 1 : 0))].name;
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
    list_.Refresh();
}

void MapEditorScreen::Update(float dt) {
    world_.time += dt;   // Zug fährt (Render.cpp), Stationen drehen ihre Antennen
    if (msgT_ > 0.0f) msgT_ -= dt;
    if (backArmed_ > 0.0f) backArmed_ -= dt;

    if (mode_ == Mode::List) {
        // Kartenliste: die Kamera zieht langsam im Kreis über die Karte
        camHeight_ = 95.0f;
        camTarget_ = {sinf(world_.time * 0.05f) * 80.0f, 0.0f, cosf(world_.time * 0.05f) * 80.0f};
    } else {
        // Verschieben mit der Tastatur, schneller, je weiter die Kamera weg ist.
        // Nicht während im Namensfeld getippt wird (dann sind WASD/R Buchstaben).
        if (!TextInputActive()) {
            const float v = camHeight_ * 1.2f * dt;
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) camTarget_.z -= v;
            if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) camTarget_.z += v;
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) camTarget_.x -= v;
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) camTarget_.x += v;
            if (IsKeyPressed(KEY_R)) rot_ = (rot_ + 1) % 4;
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
        if (wheel != 0.0f && !PointerOverPanel(PointerPos()))
            camHeight_ = Clamp(camHeight_ * (1.0f - wheel * 0.12f), 18.0f, 260.0f);
        camTarget_.x = Clamp(camTarget_.x, -cfg::WORLD_HALF, cfg::WORLD_HALF);
        camTarget_.z = Clamp(camTarget_.z, -cfg::WORLD_HALF, cfg::WORLD_HALF);
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
    groundOk_ = controls::RayToGround(GetScreenToWorldRay(PointerPos(), cam_), &ground_);

    if (mode_ == Mode::Edit) {
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
    painting_ = erasing_ = false;
    backArmed_ = 0.0f;
    camTarget_ = {0, 0, 0};
    camHeight_ = 80.0f;
    mode_ = Mode::Edit;
    Rebuild();
}

void MapEditorScreen::Rebuild() {
    world_.ApplyMap(map_);
    trackPieces_ = 0;
    for (const MapPiece& p : map_.pieces)
        if (IsTrack(p.model)) ++trackPieces_;
    loopPieces_ = 0;
    for (const MonorailLoop& l : world_.monorails) loopPieces_ += l.pieces;
}

MapEditorScreen::Placement MapEditorScreen::PlacementAt(Vector3 g) const {
    const MapSegment& seg = MapSegments()[static_cast<size_t>(segment_)];
    const BoundingBox b = ModelBounds(ModelId(seg.model));
    const float C = mapgrid::CELL;
    Placement p;
    p.scale = seg.scale;
    p.yaw = rot_ * PI * 0.5f;
    // Belegte Felder; 0,05 Spielraum, damit z. B. genau 1,0 Felder nicht zu 2 werden
    p.fw = std::max(1, static_cast<int>(ceilf((b.max.x - b.min.x) * seg.scale / C - 0.05f)));
    p.fd = std::max(1, static_cast<int>(ceilf((b.max.z - b.min.z) * seg.scale / C - 0.05f)));
    if (rot_ % 2 == 1) std::swap(p.fw, p.fd);
    // Bereich am Raster ausrichten: linke Kante = gerundet (Zeiger - halbe Breite)
    p.cx = (roundf(g.x / C - p.fw * 0.5f) + p.fw * 0.5f) * C;
    p.cz = (roundf(g.z / C - p.fd * 0.5f) + p.fd * 0.5f) * C;
    Vector2 off{0, 0};
    if (!IsTrack(seg.model))
        off = MapRotate((b.min.x + b.max.x) * 0.5f * seg.scale, (b.min.z + b.max.z) * 0.5f * seg.scale, p.yaw);
    p.x = p.cx - off.x;
    p.z = p.cz - off.y;
    p.valid = fabsf(p.cx) + p.fw * C * 0.5f <= cfg::WORLD_HALF + 0.01f &&
              fabsf(p.cz) + p.fd * C * 0.5f <= cfg::WORLD_HALF + 0.01f;
    return p;
}

// Teil unter dem Zeiger: Abstand zur Mitte seiner Box im Verhältnis zu ihrer
// Größe. So gewinnt ein kleiner Fels direkt unter dem Zeiger gegen den
// großen Krater, in dem er liegt.
int MapEditorScreen::PieceAt(Vector3 g) const {
    int best = -1;
    float bestRel = 1e9f;
    for (int i = 0; i < static_cast<int>(map_.pieces.size()); ++i) {
        const MapPiece& q = map_.pieces[static_cast<size_t>(i)];
        const BoundingBox b = ModelBounds(ModelId(q.model));
        const Vector2 c = MapRotate((b.min.x + b.max.x) * 0.5f * q.scale, (b.min.z + b.max.z) * 0.5f * q.scale, q.yaw);
        const float r = std::max(1.2f, std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * q.scale);
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
    // Ein Teil mit demselben Bezugspunkt wird ersetzt – sonst stapeln sich
    // beim Malen Teile übereinander (und Schienen bekämen Abzweigungen).
    map_.pieces.erase(std::remove_if(map_.pieces.begin(), map_.pieces.end(),
                                     [&](const MapPiece& q) { return fabsf(q.x - p.x) < 0.01f && fabsf(q.z - p.z) < 0.01f; }),
                      map_.pieces.end());
    if (static_cast<int>(map_.pieces.size()) >= mapgrid::MAX_PIECES) {
        Message(Fmt("Eine Karte hat höchstens %d Teile.", mapgrid::MAX_PIECES), BAD);
        g_audio.Play(Sfx::Denied);
        painting_ = false;
        return;
    }
    map_.pieces.push_back({seg.model, p.x, p.z, p.yaw, p.scale});
    dirty_ = true;
    Rebuild();
    g_audio.Play(Sfx::UiMove, 0.3f);
}

// "Zufall generieren": dieselbe Mondoberfläche wie in einer Zufallswelt
// (World::GenerateTerrain: Krater, Felsen, Meteoriten, Fundstücke), mit
// Abstand zu allen Bauplätzen. Ersetzt die Teile der Gruppen Gelände,
// Felsen und Sonstiges; Straßen, Monorail und Gebäude bleiben stehen.
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
        if (!FindSegment(name) || static_cast<int>(pieces.size()) >= mapgrid::MAX_PIECES) continue;
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
    list_.Refresh();
    g_audio.Play(Sfx::UiBack);
}

void MapEditorScreen::Message(const std::string& text, Color c) {
    msg_ = text;
    msgColor_ = c;
    msgT_ = 3.5f;
}

// ---------------------------------------------------------------------------
// Zeichnen
// ---------------------------------------------------------------------------
void MapEditorScreen::DrawScene() {
    world_.Draw(cam_);
    if (mode_ != Mode::Edit) return;

    // Raster: feine Linien nur, wenn die Kamera nah genug ist; jede zehnte
    // Linie (alle 30 m) immer. Nur um den Blickpunkt herum, nicht über die
    // ganze Karte.
    const float C = mapgrid::CELL, H = cfg::WORLD_HALF, y = 0.06f;
    const float reach = std::min(2.0f * H, camHeight_ * 1.6f + 20.0f);
    const bool fine = camHeight_ < 140.0f;
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
        GroundRect(p.cx, p.cz, p.fw * C, p.fd * C, p.valid ? Color{120, 255, 150, 220} : Color{255, 90, 80, 220});
        if (p.valid) {
            const MapPiece q{MapSegments()[static_cast<size_t>(segment_)].model, p.x, p.z, p.yaw, p.scale};
            g_assets.Draw(ModelId(q.model), {q.x, MapPieceY(q), q.z}, q.yaw, q.scale, Color{150, 255, 170, 255});
        }
    }
    // Ohne gewähltes Segment (oder beim Radieren): das Teil markieren, das
    // ein Rechtsklick löschen würde
    if (segment_ < 0 || erasing_) {
        const int i = PieceAt(ground_);
        if (i >= 0) {
            const MapPiece& q = map_.pieces[static_cast<size_t>(i)];
            const BoundingBox b = ModelBounds(ModelId(q.model));
            const Vector2 c = MapRotate((b.min.x + b.max.x) * 0.5f * q.scale, (b.min.z + b.max.z) * 0.5f * q.scale, q.yaw);
            const float r = std::max(1.2f, std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f * q.scale);
            DrawCircle3D({q.x + c.x, 0.12f, q.z + c.y}, r, {1, 0, 0}, 90.0f, BAD);
        }
    }
}

void MapEditorScreen::DrawList() {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const float pw = 620 * s, ph = 560 * s;
    const Rectangle panel{(W - pw) * 0.5f, (H - ph) * 0.5f, pw, ph};
    Panel(panel);
    const float x = panel.x + 28 * s, y = panel.y + 22 * s;
    Text("KARTENEDITOR", x, y, 40 * s, ACCENT);
    Text("Gespeicherte Karten", x, y + 52 * s, 22 * s, MUTED);
    const float bh = 54 * s, by = panel.y + ph - 24 * s - bh;
    const Rectangle lr{x, y + 86 * s, pw - 56 * s, by - 18 * s - (y + 86 * s)};
    const bool open = list_.Draw(lr, s);

    const float gap = 14 * s, bw = (pw - 56 * s - 2 * gap) / 3.0f;
    const bool load = Button({x, by, bw, bh}, "Karte laden", 24 * s, false, list_.HasSelection());
    const bool fresh = Button({x + bw + gap, by, bw, bh}, "Neu", 24 * s, false);
    const bool back = Button({x + 2 * (bw + gap), by, bw, bh}, "Zurück", 24 * s, false);

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
    } else if (fresh) {
        // Leere Karte mit einem freien Namen ("Neue Karte", "Neue Karte 2" ...)
        GameMap m;
        m.name = "Neue Karte";
        for (int n = 2; MapFileExists(MapFileStem(m.name)); ++n) m.name = Fmt("Neue Karte %d", n);
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
    if (Button({x, y, w, 46 * s}, "Zufall generieren", 22 * s, false)) RandomFill();
    y += 52 * s;
    Text("Krater, Felsen und Fundstücke", x, y, 16 * s, MUTED);
    y += 30 * s;

    // Reiter der Gruppen (zwei Reihen à drei)
    const int groups = static_cast<int>(SegmentGroup::Count);
    const float tg = 6 * s, tw = (w - 2 * tg) / 3.0f, th = 34 * s;
    for (int g = 0; g < groups; ++g) {
        const Rectangle tr{x + (g % 3) * (tw + tg), y + (g / 3) * (th + tg), tw, th};
        if (Button(tr, SegmentGroupName(static_cast<SegmentGroup>(g)), 17 * s, g == group_) && g != group_) {
            group_ = g;
            g_audio.Play(Sfx::UiMove);
        }
    }
    y += 2 * (th + tg) + 8 * s;

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
    Text(Fmt("Drehung: %d°   [R]", rot_ * 90), x, by - 22 * s, 18 * s, TEXT);
    if (segment_ < 0) Text("Segment wählen, dann auf die Karte klicken", x, by - 48 * s, 15 * s, MUTED);
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
    const float backW = 140 * s, saveW = 170 * s, gap = 12 * s;
    const float rx = bar.x + bar.width - 16 * s;
    Text("Name", bar.x + 16 * s, fy + 10 * s, 22 * s, MUTED);
    const float fx = bar.x + 86 * s;
    const float fw = std::max(120 * s, std::min(360 * s, rx - backW - gap - saveW - gap - fx));
    if (nameField_.Draw({fx, fy, fw, fh}, 22 * s)) nameField_.focused = false;   // Enter beendet die Eingabe
    const bool dirty = Dirty();
    const bool back = Button({rx - backW, fy, backW, fh}, "Zurück", 22 * s, false);
    const bool save = Button({rx - backW - gap - saveW, fy, saveW, fh}, dirty ? "Speichern *" : "Speichern", 22 * s, dirty);
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (save || (ctrl && IsKeyPressed(KEY_S))) Save();

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
    Text(Fmt("Teile: %d  ·  ", static_cast<int>(map_.pieces.size())) + mono, bx, ty - 28 * s, 20 * s, loops > 0 ? GOOD : TEXT);
    Text("Linksklick setzen (halten: malen)  ·  Rechtsklick löschen  ·  R drehen  ·  WASD/Pfeile/Mitteltaste verschieben  ·  "
         "Mausrad zoomen  ·  Strg+S speichern",
         bx, ty, 16 * s, MUTED);

    // ESC: erst die Eingabe bzw. Auswahl beenden, dann zurück zur Kartenliste
    if (BackPressed()) {
        if (nameField_.focused) nameField_.focused = false;
        else if (segment_ >= 0) segment_ = -1;
        else Back();
    } else if (back) {
        Back();
    }
}
