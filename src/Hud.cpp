// ---------------------------------------------------------------------------
// Hud.cpp – HUD, Minikarte, Stationsfenster und Endbildschirme
// (Schnittstelle: Hud.hpp).
//
// Wofür ist die Datei da?
//   Sie zeichnet die 2D-Oberfläche über dem 3D-Bild der Welt. Alle Werte
//   (Kontostand, Schild, Lager, Meldungen ...) werden in jedem Frame frisch
//   aus der World gelesen; das HUD speichert selbst fast nichts (nur die
//   "Reserve" rechts oben, siehe SetHudRightReserve).
//
// Wer benutzt sie?
//   src/main.cpp (Einzelspieler, Pause, Simulation) und src/Multiplayer.cpp
//   (Mehrspieler). Im Mehrspieler ist die World eine vom Server gefüllte
//   Spiegelwelt (NetClient.cpp); der eigene Spieler steht dort genauso in
//   players[0] und das eigene Schiff in ships[0]. Deshalb funktioniert jede
//   Funktion hier unverändert in beiden Spielarten.
//
// Abhängigkeiten:
//   - shared/World.hpp (Daten), shared/Config.hpp (Balancing-Werte für
//     Anzeigen wie "Tempo 16 -> 19"), shared/Util.hpp (Dist2D, ForwardFromYaw).
//   - Ui.hpp: alle Bausteine (Text, Panel, Bar, Button) und Farben.
//   - Controls.hpp: Tastennamen passend zum zuletzt benutzten Gerät
//     ("[F] Andocken" bzw. "[Y] Andocken" / "[Dreieck] Andocken") und die
//     Zeilen der Tastenhilfe.
//   - Audio.hpp: g_audio für den Ton "Abgelehnt" (Sfx::Denied), wenn etwas
//     zu teuer ist. Der Ton entsteht direkt hier im Client, auch im
//     Mehrspieler – es ist reine Bedien-Rückmeldung, der Server weiß davon nichts.
//   - Assets.hpp wird hier nicht direkt gebraucht (liefert nur nebenbei
//     raylib.h/raymath.h, die ohnehin über World.hpp kommen).
//   - raylib: 2D-Zeichnen (DrawRectangle..., DrawTriangle, Farbverläufe) und
//     GetWorldToScreen (3D-Weltpunkt -> Bildschirmpixel).
//
// Größen und Koordinaten:
//   Alle Maße sind für ein 1500 x 900 Pixel großes Fenster ausgelegt und
//   werden mit s = Scale() multipliziert (siehe Ui.cpp). "360 * s" heißt also
//   "360 Pixel bei Referenzgröße". Bildschirmkoordinaten: (0,0) links oben,
//   x nach rechts, y nach unten.
//
// Ablauf von DrawHud (Zeichenreihenfolge = was später kommt, liegt oben):
//   1. roter Warnrand (Treffer oder Schild < 30 %)
//   2. Stationsnamen bzw. Randpfeile
//   3. Statusfenster oben links
//   4. Minikarte oben rechts
//   5. Mission oben in der Mitte, Meldungen darunter
//   6. Kontext-Hinweis unten ("[F] Andocken ...", Abbau-Fortschritt)
//   7. Funkspruch der Mission unten rechts
//   8. Tastenhilfe unten links
// ---------------------------------------------------------------------------
#include "Hud.hpp"
#include "Assets.hpp"
#include "Audio.hpp"
#include "ClipPlayer.hpp"
#include "Controls.hpp"
#include "Ui.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

using namespace ui;

namespace {

// Rechte obere Ecke, die Randpfeile meiden (siehe SetHudRightReserve). 0 = Standard.
float g_rightReserveW = 0.0f;
float g_rightReserveBottom = 0.0f;

// Eine Zeile des Statusfensters: Beschriftung links (grau), Wert rechts,
// darunter ein Balken mit dem Füllstand frac (0..1). Belegt 36 * s Pixel Höhe.
void StatBar(float x, float y, float w, const char* label, const std::string& value, float frac, Color col) {
    const float s = Scale();
    Text(label, x, y, 20 * s, MUTED);
    TextRight(value, x + w, y, 20 * s, TEXT);
    Bar({x, y + 24 * s, w, 12 * s}, frac, col);
}

// Name über einer Station. Liegt sie außerhalb des Bildes, zeigt ein Pfeil am
// Bildrand in ihre Richtung (nur für die nächste Station jeder Art: edgeArrow).
//
// GetWorldToScreen(punkt, kamera) rechnet einen 3D-Weltpunkt mit denselben
// Matrizen wie BeginMode3D (View- und Projektionsmatrix der Kamera) in
// Bildschirmpixel um. Das Ergebnis kann auch außerhalb des Fensters liegen
// (negativ oder größer als Breite/Höhe) – genau das nutzt der Randpfeil.
// Vorsicht: Für Punkte HINTER der Kamera (hinter ihrer Bildebene) liefert
// die Projektion gespiegelte Koordinaten, weil dabei durch einen negativen
// Wert geteilt wird. Mit der Spielkamera (30 hoch, 27 nach +z, Zoom 1)
// betrifft das Stationen, die mehr als etwa 50 Einheiten in +z-Richtung
// ("unten" im Bild) liegen. Für sie wird die Richtung des Randpfeils deshalb
// aus der Lage am Boden berechnet (ArrowDirection).
// Richtung (in Bildschirmkoordinaten, y nach unten) von der Bildmitte zu
// einem Weltpunkt, der nicht im Bild ist. Liegt er vor der Kamera, ist das
// einfach der projizierte Punkt minus Bildmitte. Liegt er dahinter, wäre die
// Projektion gespiegelt; dann zählt nur die Lage am Boden (x/z) relativ zum
// Kameraziel: "vorwärts" ist die Blickrichtung der Kamera am Boden (im Bild
// oben), "rechts" steht senkrecht dazu (im Bild rechts).
Vector2 ArrowDirection(const Camera3D& cam, Vector3 point, Vector2 projected, Vector2 center) {
    const Vector3 look = Vector3Subtract(cam.target, cam.position);
    const bool behind = Vector3DotProduct(Vector3Subtract(point, cam.position), look) <= 0.0f;
    if (!behind) return Vector2Subtract(projected, center);
    Vector2 fwd{look.x, look.z};
    if (Vector2Length(fwd) < 1e-4f) fwd = {0.0f, -1.0f};   // Kamera genau senkrecht: -z gilt als oben
    fwd = Vector2Normalize(fwd);
    const Vector2 right{-fwd.y, fwd.x};
    const Vector2 delta{point.x - cam.target.x, point.z - cam.target.z};
    // Bildschirm: x = Anteil nach rechts, y = Anteil nach hinten (y wächst nach unten)
    return {Vector2DotProduct(delta, right), -Vector2DotProduct(delta, fwd)};
}

// Farbe einer Stationsart (Namen, Randpfeile, Minikarte):
// Handel = Orange (ACCENT), Werft = Hellblau, Raumschiffsverkäufer = Lime-Grün
constexpr Color YARD_COLOR{120, 210, 255, 255};
constexpr Color DEALER_COLOR{150, 255, 90, 255};
Color StationColor(StationType t) {
    return t == StationType::Trade ? ACCENT : (t == StationType::Shipyard ? YARD_COLOR : DEALER_COLOR);
}

void StationLabel(const World& w, const Camera3D& cam, int idx, bool edgeArrow) {
    const float s = Scale();
    const Station& st = w.stations[idx];
    const bool trade = st.type == StationType::Trade;
    // Der Name steht 10 Einheiten über dem Stationsmittelpunkt, also etwa
    // über dem Dach der Station statt mitten im Modell.
    Vector3 top = Vector3Add(st.pos, {0, 10.0f, 0});
    Vector2 p = GetWorldToScreen(top, cam);
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    // Farben wie auf der Minikarte
    Color col = StationColor(st.type);
    // In Großbuchstaben. toupper wirkt nur auf einzelne Bytes, d. h. nur auf
    // ASCII; Umlaute (UTF-8, zwei Bytes) bleiben klein. Das unsigned-char-
    // Umwandeln ist nötig, weil toupper mit negativen char-Werten (Bytes über
    // 127) undefiniert ist.
    std::string upper = st.name;
    for (char& c : upper) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    // Handelsstationen wachsen gemeinsam mit allen Lieferungen (World::StationStage,
    // im Mehrspieler die Lieferungen ALLER Spieler) – darum steht die Stufe dabei.
    std::string name = trade ? TextFormat("%s  Stufe %d", upper.c_str(), w.StationStage()) : upper;
    float margin = 40 * s;
    // Zwei Prüfungen: Ist der Namenspunkt (über dem Dach) mit 40 px Abstand
    // zum Rand im Bild, oder wenigstens der Fußpunkt der Station? Steht die
    // Station am oberen Bildrand, liegt der Namenspunkt schon draußen, die
    // Station selbst ist aber zu sehen – dann soll kein Randpfeil erscheinen.
    Vector2 base = GetWorldToScreen(st.pos, cam);
    bool baseOnScreen = base.x > 0 && base.x < W && base.y > 0 && base.y < H;
    bool onScreen = p.x > margin && p.x < W - margin && p.y > margin && p.y < H - margin;
    if (onScreen || baseOnScreen) {
        // Name nicht über den oberen Rand rutschen lassen und nicht unter das
        // Statusfenster oben links (360 x 300 bei Position 16,16 -> mit Rand
        // ungefähr bis x = 400, y = 330).
        float ly = std::max(p.y, 50 * s);
        if (p.x < 400 * s && ly < 360 * s) ly = 360 * s;
        TextCentered(name, p.x, ly, 20 * s, col);
    } else if (edgeArrow) {
        // Randmarkierung in Richtung der Station.
        // Idee: Strahl von der Bildmitte c zum (unsichtbaren) Bildpunkt p.
        // Er wird so verkürzt (Faktor k), dass er genau auf einem Rechteck
        // endet, das 40 px innerhalb des Bildrands liegt. kx/ky sind die
        // Faktoren, mit denen der Strahl den linken/rechten bzw. oberen/unteren
        // Rand träfe; der kleinere davon wird zuerst erreicht.
        // max(1, ...) verhindert eine Division durch 0, wenn d genau
        // waagerecht oder senkrecht ist.
        Vector2 c{W * 0.5f, H * 0.5f};
        Vector2 d = ArrowDirection(cam, top, p, c);
        float kx = (W * 0.5f - margin) / std::max(1.0f, fabsf(d.x));
        float ky = (H * 0.5f - margin) / std::max(1.0f, fabsf(d.y));
        float k = std::min(kx, ky);
        Vector2 e = Vector2Add(c, Vector2Scale(d, k));
        if (e.x < 400 * s && e.y < 370 * s) e.y = 370 * s;              // nicht unter das Statusfenster
        // Rechts oben: Minikarte (230 px + Legende -> ca. 260 breit, 300 hoch)
        // oder im Mehrspieler zusätzlich das Panel "Beste Piloten".
        const float rw = g_rightReserveW > 0.0f ? g_rightReserveW : 260 * s;
        const float rb = g_rightReserveBottom > 0.0f ? g_rightReserveBottom : 300 * s;
        if (e.x > W - rw && e.y < rb) e.y = rb;                          // nicht unter die Minikarte
        // Pfeil als Dreieck: Spitze 14 px in Strahlrichtung, die beiden
        // hinteren Ecken bei +-2,5 rad (ca. +-143°) – ergibt eine schlanke
        // Pfeilspitze.
        float ang = atan2f(d.y, d.x);
        Vector2 tip{e.x + cosf(ang) * 14 * s, e.y + sinf(ang) * 14 * s};
        Vector2 l{e.x + cosf(ang + 2.5f) * 12 * s, e.y + sinf(ang + 2.5f) * 12 * s};
        Vector2 r{e.x + cosf(ang - 2.5f) * 12 * s, e.y + sinf(ang - 2.5f) * 12 * s};
        // raylib zeichnet ein Dreieck nur, wenn die Ecken gegen den
        // Uhrzeigersinn angegeben sind (die andere Seite gilt als "Rückseite"
        // und wird weggelassen). Welche Reihenfolge das hier ist, hängt vom
        // Winkel ab – also einfach beide Reihenfolgen zeichnen.
        DrawTriangle(tip, r, l, col);
        DrawTriangle(tip, l, r, col);
        // Entfernung am Boden (x/z, ohne Höhe) in Welteinheiten, als "m" angezeigt.
        // Der Text steht 34 px vom Pfeil Richtung Bildmitte, damit er nicht
        // über den Bildrand hinausragt.
        int dist = static_cast<int>(Dist2D(w.Player().pos, st.pos));
        std::string t = TextFormat("%s %d m", trade ? "Handel" : (st.type == StationType::Shipyard ? "Werft" : "Schiffe"), dist);
        Vector2 tp = Vector2Subtract(e, Vector2Scale(Vector2Normalize(d), 34 * s));
        TextCentered(t, tp.x, tp.y - 8 * s, 18 * s, col);
    }
}

// Minikarte ("Radar") oben rechts: die ganze Karte (-WORLD_HALF..+WORLD_HALF
// in x und z, also 300 x 300 Einheiten) verkleinert auf 230 x 230 Pixel.
// Gezeigt werden: Stationen und Wracks immer, entdeckte Lagerstätten immer,
// andere Schiffe nur im Radarkreis von 90 Einheiten um das eigene Schiff,
// Ziele der eigenen Mission überall (pulsierend).
void Minimap(const World& w) {
    const float s = Scale();
    const float size = 230 * s;
    Rectangle r{GetScreenWidth() - size - 16 * s, 16 * s, size, size};
    Panel(r, Color{18, 22, 30, 215});
    // Weltposition -> Punkt auf der Karte. Welt-x wird Karten-x, Welt-z wird
    // Karten-y (die Höhe y spielt keine Rolle). -z liegt oben, so wie im
    // 3D-Bild: Die Kamera schaut immer von +z her auf das Schiff (sie dreht
    // nicht mit), "vorwärts" bei Drehwinkel 0 ist -z (ForwardFromYaw).
    auto map = [&](Vector3 p) {
        return Vector2{r.x + (p.x + cfg::WORLD_HALF) / (2 * cfg::WORLD_HALF) * r.width,
                       r.y + (p.z + cfg::WORLD_HALF) / (2 * cfg::WORLD_HALF) * r.height};
    };
    // Gitter 6 x 6 (jedes Feld 50 x 50 Welteinheiten), fast durchsichtig (Alpha 18)
    for (int i = 1; i < 6; ++i) {
        float x = r.x + r.width * i / 6.0f, y = r.y + r.height * i / 6.0f;
        DrawLineEx({x, r.y}, {x, r.y + r.height}, 1, Color{255, 255, 255, 18});
        DrawLineEx({r.x, y}, {r.x + r.width, y}, 1, Color{255, 255, 255, 18});
    }
    const Ship& p = w.Player();
    // Radarreichweite: Kreis mit 90 Welteinheiten Radius, umgerechnet in
    // Kartenpixel. Dieselbe Zahl steht unten als Filter für fremde Schiffe
    // (kein eigener cfg-Wert).
    Vector2 pp = map(p.pos);
    DrawCircleLinesV(pp, 90.0f / (2 * cfg::WORLD_HALF) * r.width, Color{255, 255, 255, 35});

    // Lagerstätten nur, wenn der eigene Spieler sie entdeckt hat (Scanner).
    // Im Mehrspieler schickt der Server ohnehin nur die Lagerstätten, die
    // dieser Spieler sehen darf; NetClient setzt dort revealed = true.
    for (const auto& d : w.deposits) {
        if (!d.revealed) continue;
        Vector2 m = map(d.pos);
        DrawRectangleV({m.x - 2 * s, m.y - 2 * s}, {4 * s, 4 * s}, GOOD);
    }
    for (const auto& wr : w.wrecks) {
        Vector2 m = map(wr.pos);
        DrawRectangleV({m.x - 2 * s, m.y - 2 * s}, {4 * s, 4 * s}, Color{255, 170, 60, 255});
    }
    // Stationen: gefülltes Quadrat mit Rahmen, damit sie größer als alles andere wirken
    for (const Station& st : w.stations) {
        Vector2 m = map(st.pos);
        Color c = StationColor(st.type);
        DrawRectangleV({m.x - 5 * s, m.y - 5 * s}, {10 * s, 10 * s}, c);
        DrawRectangleLinesEx({m.x - 7 * s, m.y - 7 * s, 14 * s, 14 * s}, 1, c);
    }
    for (const auto& sh : w.ships) {
        // Das eigene Schiff zeichnet unten der weiße Pfeil. (Früher wurde hier jedes
        // Spielerschiff übersprungen – im Mehrspieler fehlten dadurch die anderen
        // Spieler; im Einzelspieler gibt es nur das eigene, dort ändert sich nichts.)
        if (sh.dead || sh.hidden || &sh == &p) continue;
        if (Dist2D(sh.pos, p.pos) > 90.0f) continue;
        Vector2 m = map(sh.pos);
        // Rot: Piraten und Händler/Miner, die man angegriffen hat (sie wehren
        // sich). Grün: andere Spieler. Hellblau: friedliche Miner/Händler.
        Color c = (sh.faction == Faction::Pirate || sh.hostileToPlayer) ? BAD
                  : sh.kind == ShipKind::Player ? GOOD                  // andere Spieler (Mehrspieler)
                                                : Color{140, 200, 255, 255};
        DrawCircleV(m, 2.6f * s, c);
    }
    // Ziele der eigenen Mission, auch außerhalb des Radarkreises (Markierungen
    // aus World::UpdateMissions, im Mehrspieler vom Server): Personen gelb,
    // Piraten rot, Konvoi-Frachter grün. Sie pulsieren (Größe und Deckkraft),
    // damit man sie auf der großen Karte schnell findet.
    const float pulse = 0.5f + 0.5f * sinf(w.time * 5.0f);
    for (const MissionMarker& mk : w.Me().markers) {
        const Vector2 m = map(mk.pos);
        const Color c = mk.kind == 0 ? Color{255, 225, 60, 255} : (mk.kind == 1 ? BAD : Color{150, 255, 120, 255});
        DrawCircleV(m, (5.0f + 3.0f * pulse) * s, Fade(c, 0.25f + 0.25f * pulse));
        DrawCircleV(m, 3.0f * s, c);
    }
    // Eigenes Schiff als Pfeil in Flugrichtung. fd = Vorwärtsrichtung auf der
    // Karte (Welt-x/z), sd = fd um 90° gedreht (seitlich). Spitze 8 px nach
    // vorn, die beiden hinteren Ecken 5 px zurück und 5 px zur Seite.
    if (!p.dead && !p.hidden) {
        Vector3 f = ForwardFromYaw(p.yaw);
        Vector2 fd{f.x, f.z};
        Vector2 sd{-fd.y, fd.x};
        Vector2 tip = Vector2Add(pp, Vector2Scale(fd, 8 * s));
        Vector2 a = Vector2Add(pp, Vector2Add(Vector2Scale(fd, -5 * s), Vector2Scale(sd, 5 * s)));
        Vector2 b = Vector2Add(pp, Vector2Add(Vector2Scale(fd, -5 * s), Vector2Scale(sd, -5 * s)));
        // beide Eckenreihenfolgen, siehe StationLabel
        DrawTriangle(tip, a, b, WHITE);
        DrawTriangle(tip, b, a, WHITE);
    }
    Text("RADAR", r.x + 8 * s, r.y + 6 * s, 16 * s, MUTED);
    // Legende unter der Karte: 2 x 2 Einträge (i % 2 = Spalte, i / 2 = Zeile).
    // Andere Spieler (grün) und friedliche Schiffe (hellblau) stehen nicht darin.
    // Sie ist kein Panel; damit ein Klick darauf trotzdem kein Flugziel setzt
    // (Maussteuerung, Controls.cpp), wird ihre Fläche eigens angemeldet.
    BlockPointer({r.x, r.y + r.height, r.width, 50 * s});
    struct L { const char* t; Color c; } legend[4] = {{"Handel", ACCENT}, {"Werft", Color{120, 210, 255, 255}},
                                                   {"Kristall", GOOD}, {"Gegner", BAD}};
    for (int i = 0; i < 4; ++i) {
        float lx = r.x + (i % 2) * r.width * 0.5f + 4 * s;
        float ly = r.y + r.height + 8 * s + (i / 2) * 20 * s;
        DrawRectangleV({lx, ly + 3 * s}, {9 * s, 9 * s}, legend[i].c);
        Text(legend[i].t, lx + 14 * s, ly, 15 * s, MUTED);
    }
}

// Meldungen kommen aus der Spiellogik (shared/World.cpp) oder vom Server und
// nennen fest die Tasten der Tastatur, z. B. "Finde Kristalle mit dem Scanner
// [E].". Dort ändern wir nichts – Server und Einzelspieler teilen sich den
// Text. Stattdessen tauscht das HUD beim Zeichnen "[E]" und "[F]" gegen die
// Taste des gerade benutzten Geräts aus (KeyHint, z. B. "[B]" oder "[Kreis]").
// Mit Tastatur liefert KeyHint wieder "[E]" bzw. "[F]" – dann bleibt alles gleich.
std::string WithDeviceKeys(std::string t) {
    const std::pair<const char*, controls::Act> keys[] = {{"[E]", controls::Act::Scan}, {"[F]", controls::Act::Interact}};
    for (const auto& [key, act] : keys) {
        const std::string hint = controls::KeyHint(act);
        for (size_t at = t.find(key); at != std::string::npos; at = t.find(key, at + hint.size()))
            t.replace(at, strlen(key), hint);
    }
    return t;
}

// Fortschritt einer Mission als kurzer Text (HUD oben, Stationsfenster)
std::string MissionProgress(const Mission& m) {
    switch (m.type) {
        case MissionType::Persons:
            return m.progress >= m.goal ? std::string("Alle an Bord: zur Handelsstation!")
                                        : TextFormat("Personen %d / %d", m.progress, m.goal);
        case MissionType::Pirates: return TextFormat("Abschüsse %d / %d", m.progress, m.goal);
        case MissionType::Convoy: return TextFormat("Frachter unterwegs %d, angekommen %d", m.alive, m.progress);
    }
    return std::string();
}

// Text in Zeilen zerlegen, die höchstens maxW Pixel breit sind (an
// Leerzeichen; ein einzelnes zu langes Wort bekommt seine eigene Zeile).
std::vector<std::string> WrapLines(const std::string& text, float maxW, float size) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&]() {
        if (word.empty()) return;
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && Width(tryLine, size) > maxW) {
            lines.push_back(line);
            line = word;
        } else {
            line = tryLine;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') flush();
        else word += c;
    }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

} // namespace

void SetHudRightReserve(float width, float bottom) {
    g_rightReserveW = width;
    g_rightReserveBottom = bottom;
}

// Die Zeilen der Tastenhilfe liefert controls::HelpRows() (Controls.cpp) –
// je nach zuletzt benutztem Gerät die Tastatur/Maus-Belegung (9 Zeilen) oder
// die des Controllers (7 Zeilen, Xbox- oder PlayStation-Namen). Dort steht
// auch die Auswertung der Tasten; beides passt so immer zusammen.
// ControlsHeight rechnet mit derselben Zeilenzahl (aktives Gerät). DrawHud
// fragt sie, statt eine Zeilenzahl anzunehmen: Wechselt der Spieler das
// Gerät, passt sich die Hilfe unten links im nächsten Frame an.
// ControlsMaxHeight nimmt die längste Liste aller Geräte. Die Pausenmenüs
// rechnen damit, damit ihre Knöpfe beim Gerätewechsel nicht verrutschen.
float ControlsHeight(float size) { return static_cast<float>(controls::HelpRows().size()) * size * 1.35f; }
float ControlsMaxHeight(float size) { return static_cast<float>(controls::MaxHelpRows()) * size * 1.35f; }

// Spalte 2 beginnt hinter dem breitesten Tastennamen plus einer
// Schrifthöhe Abstand (gemessen mit Width, weil "Rechter Stick" breiter ist
// als "E").
void DrawControls(float x, float y, float size) {
    const std::vector<controls::HelpRow> rows = controls::HelpRows();
    float keyW = 0.0f;
    for (const auto& r : rows) keyW = std::max(keyW, Width(r.keys, size));
    for (size_t i = 0; i < rows.size(); ++i) {
        const float ry = y + static_cast<float>(i) * size * 1.35f;
        Text(rows[i].keys, x, ry, size, Color{255, 200, 140, 210});
        Text(rows[i].what, x + keyW + size, ry, size, Color{220, 225, 235, 200});
    }
}

// ===========================================================================
// HUD während des Fluges (Ablauf siehe Dateikopf)
// ===========================================================================
void DrawHud(const World& w, const Camera3D& cam, bool simulation) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    const Ship& p = w.Player();
    const PlayerState& me = w.Me();

    // Treffer-/Warnrand: rote Farbverläufe von allen vier Rändern nach innen
    // (120 px breit, außen kräftig, innen durchsichtig).
    // - Treffer: Ship::hitFlash springt beim Treffer auf 1 und klingt in der
    //   Welt mit 3 pro Sekunde ab (~0,33 s). Mal 0,6 -> höchstens Alpha 90.
    // - Schild unter 30 %: pulsiert zwischen 0,05 und 0,45 (sin mit 6 rad/s,
    //   also knapp einmal pro Sekunde) – ein Dauer-Warnsignal.
    // Der stärkere der beiden Werte gewinnt; 1,0 entspricht Alpha 150.
    // DrawRectangleGradientV/H: senkrechter bzw. waagerechter Farbverlauf
    // zwischen zwei Farben (hier gleiche Farbe, verschiedene Deckkraft).
    float danger = p.hitFlash * 0.6f;
    if (!p.crashing && !p.dead && p.hp / p.maxHp < 0.3f) danger = std::max(danger, 0.25f + 0.2f * sinf(w.time * 6.0f));
    if (danger > 0.01f) {
        unsigned char a = static_cast<unsigned char>(std::min(1.0f, danger) * 150);
        float t = 120 * s;
        DrawRectangleGradientV(0, 0, static_cast<int>(W), static_cast<int>(t), Color{255, 40, 30, a}, Color{255, 40, 30, 0});
        DrawRectangleGradientV(0, static_cast<int>(H - t), static_cast<int>(W), static_cast<int>(t), Color{255, 40, 30, 0}, Color{255, 40, 30, a});
        DrawRectangleGradientH(0, 0, static_cast<int>(t), static_cast<int>(H), Color{255, 40, 30, a}, Color{255, 40, 30, 0});
        DrawRectangleGradientH(static_cast<int>(W - t), 0, static_cast<int>(t), static_cast<int>(H), Color{255, 40, 30, 0}, Color{255, 40, 30, a});
    }

    // Namen aller Stationen; Randpfeile nur für die jeweils nächste Handels-
    // station, die nächste Werft und den Raumschiffsverkäufer (sonst wäre der
    // Rand voller Pfeile).
    const int nearTrade = w.NearestStation(p.pos, StationType::Trade);
    const int nearYard = w.NearestStation(p.pos, StationType::Shipyard);
    const int nearDealer = w.NearestStation(p.pos, StationType::Dealer);
    for (int i = 0; i < static_cast<int>(w.stations.size()); ++i)
        StationLabel(w, cam, i, i == nearTrade || i == nearYard || i == nearDealer);

    // --- Statuspanel oben links ------------------------------------------------
    // 360 x 330 bei (16, 16). StationLabel (oben) und Multiplayer.cpp
    // (DrawShipNames) halten diesen Bereich frei – bei Größenänderungen dort
    // mit anpassen. y wandert von oben nach unten durch die Zeilen.
    Rectangle pr{16 * s, 16 * s, 360 * s, 330 * s};
    Panel(pr);
    float x = pr.x + 18 * s, y = pr.y + 14 * s, bw = pr.width - 36 * s;
    // Kontostand groß, rechts daneben das Spielziel (cfg::GOAL_POINTS = 100.000)
    Text("KONTOSTAND", x, y, 20 * s, MUTED);
    y += 24 * s;
    Text(Num(me.balance), x, y, 40 * s, ACCENT);
    TextRight("/ " + Num(cfg::GOAL_POINTS), x + bw, y + 16 * s, 20 * s, MUTED);
    y += 46 * s;
    Bar({x, y, bw, 8 * s}, static_cast<float>(me.balance) / cfg::GOAL_POINTS, ACCENT);
    y += 22 * s;
    // Schild: aufgerundet, damit "0" erst erscheint, wenn wirklich nichts mehr
    // übrig ist. Unter 30 % rot (gleiche Schwelle wie der Warnrand).
    StatBar(x, y, bw, "SCHILD", TextFormat("%d / %d", static_cast<int>(ceilf(p.hp)), static_cast<int>(p.maxHp)),
            p.hp / p.maxHp, p.hp / p.maxHp < 0.3f ? BAD : SHIELD);
    y += 48 * s;
    // Scanner: Die Energie ist zugleich die Restlaufzeit in Sekunden, weil sie
    // mit 1 pro Sekunde verbraucht und wieder aufgeladen wird (SCANNER_DRAIN/
    // SCANNER_REGEN = 1, max. 10). "bereit" erst bei (fast) voller Ladung;
    // die 0,01 fängt Rundungsfehler der Gleitkommarechnung ab.
    // TextFormat (raylib) ist wie sprintf, liefert aber einen Zeiger in einen
    // internen, rotierenden Puffer – daher sofort in std::string übernehmen.
    // KeyHint: "[E]", mit Controller "[B]" bzw. "[Kreis]" (Controls.hpp).
    std::string scanText = me.scanOn ? TextFormat("AKTIV  %.1f s", me.scanEnergy)
                                     : (me.scanEnergy >= cfg::SCANNER_MAX_ENERGY - 0.01f ? "bereit " + controls::KeyHint(controls::Act::Scan)
                                                                                         : TextFormat("lädt  %.1f s", me.scanEnergy));
    StatBar(x, y, bw, "SCANNER", scanText, me.scanEnergy / cfg::SCANNER_MAX_ENERGY,
            me.scanOn ? Color{120, 255, 210, 255} : SCAN);
    y += 48 * s;
    // Lager: grün, sobald es voll ist (Signal: "jetzt verkaufen"). max(1, ...)
    // schützt vor Division durch 0, falls cargoMax noch 0 ist (z. B. im
    // Mehrspieler, bevor der erste Snapshot das Schiff gefüllt hat).
    StatBar(x, y, bw, "LAGER", TextFormat("%s / %s", Num(p.cargo).c_str(), Num(p.cargoMax).c_str()),
            static_cast<float>(p.cargo) / std::max(1, p.cargoMax), p.cargo >= p.cargoMax ? GOOD : ACCENT);
    y += 50 * s;
    // Aufrüstungsstufen: vier Spalten (Reihenfolge wie cfg::Upgrade), je fünf
    // kleine Kästchen; gefüllte Kästchen = gekaufte Stufen.
    const char* names[4] = {"Antrieb", "Panzer", "Waffen", "Lager"};
    float cx = x;
    for (int i = 0; i < 4; ++i) {
        Text(names[i], cx, y, 16 * s, MUTED);
        for (int k = 0; k < cfg::UPGRADE_LEVELS; ++k) {
            Color c = k < me.upgrades[i] ? ACCENT : Color{70, 76, 90, 255};
            DrawRectangle(static_cast<int>(cx + k * 13 * s), static_cast<int>(y + 20 * s), static_cast<int>(10 * s),
                          static_cast<int>(6 * s), c);
        }
        cx += bw / 4.0f;
    }
    // Kanonen (gleichzeitige Schüsse) und verbleibende Leben
    y += 38 * s;
    Text(TextFormat("KANONEN  %d", cfg::GUN_COUNT[std::clamp(me.upgrades[cfg::UP_GUNS], 0, cfg::UPGRADE_LEVELS)]), x, y, 18 * s, TEXT);
    TextRight(TextFormat("LEBEN  %d", me.lives), x + bw, y, 18 * s, me.lives <= 1 ? BAD : TEXT);

    Minimap(w);

    // --- Laufende Mission oben in der Mitte -------------------------------------------
    // Name und Fortschritt ("Personen 2 / 3", "Abschüsse 1 / 3" ...) in einem
    // schmalen Panel über den Meldungen.
    if (const Mission* mi = w.FindMission(me.mission)) {
        const std::string t = std::string("MISSION  ") + cfg::MISSIONS[static_cast<int>(mi->type)].name + "    " + MissionProgress(*mi);
        const float tw = Width(t, 22 * s) + 40 * s;
        const Rectangle mr{W * 0.5f - tw * 0.5f, 16 * s, tw, 42 * s};
        Panel(mr);
        TextCentered(t, W * 0.5f, mr.y + 10 * s, 22 * s, Color{255, 230, 120, 255});
    }

    // --- Meldungen ---------------------------------------------------------------------
    // me.notices füllt World::Notify (im Mehrspieler kommen sie vom Server).
    // Jede Meldung lebt 4 s (Notice::time zählt in der Welt herunter), höchstens
    // 5 gleichzeitig; eine wiederholte gleiche Meldung zählt nur count hoch
    // ("(3x)") und beginnt ihre 4 s neu. Die letzten 0,6 s wird sie ausgeblendet:
    // Deckkraft a = time / 0,6, begrenzt auf 0..1.
    float ny = H * 0.15f;
    for (const auto& n : me.notices) {
        float a = std::clamp(n.time / 0.6f, 0.0f, 1.0f);
        Color c = n.color;
        c.a = static_cast<unsigned char>(255 * a);
        std::string t = WithDeviceKeys(n.count > 1 ? n.text + TextFormat("  (%dx)", n.count) : n.text);
        float tw = Width(t, 24 * s);
        // dunkler Hintergrundstreifen, genau so breit wie der Text plus Rand
        DrawRectangle(static_cast<int>(W * 0.5f - tw * 0.5f - 12 * s), static_cast<int>(ny - 6 * s),
                      static_cast<int>(tw + 24 * s), static_cast<int>(36 * s), Color{15, 18, 26, static_cast<unsigned char>(170 * a)});
        TextCentered(t, W * 0.5f, ny, 24 * s, c);
        ny += 42 * s;
    }

    // --- Kontext-Hinweis unten ---------------------------------------------------------
    // Sagt, was die Taste F gerade tun würde (mit Controller: Y bzw. Dreieck,
    // key = "[F]", "[Y]" oder "[Dreieck]"). Welches Ziel in Reichweite ist,
    // entscheidet dieselbe Funktion, die auch die Spiellogik benutzt
    // (World::InteractTarget) – Anzeige und Wirkung passen so immer zusammen.
    // kind: 0 nichts, 1 Lagerstätte, 2 Wrack, 3 Handelsstation, 4 Werft, 5 Raumschiffsverkäufer.
    if (p.crashing) {
        TextCentered("SCHIFF STÜRZT AB!", W * 0.5f, H * 0.42f, 50 * s, BAD);
    } else if (!p.dead && !p.hidden && me.dockedAt < 0) {
        std::string hint;
        Color hc = TEXT;
        int kind = 0, id = -1;
        w.InteractTarget(&kind, &id);
        Rectangle hr{W * 0.5f - 300 * s, H - 110 * s, 600 * s, 80 * s};
        const std::string key = controls::KeyHint(controls::Act::Interact);
        if (me.mining) {
            // Abbau läuft: Fortschrittsbalken über MINING_TIME (5 s)
            Panel(hr);
            float f = me.mineT / cfg::MINING_TIME;
            TextCentered(TextFormat("ABBAU LÄUFT ...  %.1f / %.0f s", me.mineT, cfg::MINING_TIME), W * 0.5f, hr.y + 12 * s, 22 * s, GOOD);
            Bar({hr.x + 20 * s, hr.y + 44 * s, hr.width - 40 * s, 16 * s}, f, GOOD);
            TextRight(key + " abbrechen", hr.x + hr.width - 14 * s, hr.y + hr.height + 4 * s, 16 * s, MUTED);
        } else {
            if (kind == 3) hint = p.cargo > 0 ? key + TextFormat(" Andocken: Handelsstation  (%s verkaufen)", Num(p.cargo).c_str())
                                              : key + " Andocken: Handelsstation";
            else if (kind == 4) hint = key + " Andocken: Werft (Upgrades)";
            else if (kind == 5) hint = key + " Andocken: Raumschiffsverkäufer";
            else if (kind == 6) { hint = key + " Person einsammeln"; hc = Color{255, 230, 120, 255}; }
            else if (kind == 1 || kind == 2) {
                // InteractTarget liefert nur die ID; die Restmenge suchen wir
                // in der passenden Liste (Lagerstätten bzw. Wracks).
                int amount = 0;
                if (kind == 1) { for (const auto& d : w.deposits) if (d.id == id) amount = d.amount; }
                else { for (const auto& wr : w.wrecks) if (wr.id == id) amount = wr.amount; }
                if (p.cargo >= p.cargoMax) { hint = "Lager voll! Zur Handelsstation fliegen."; hc = Color{255, 200, 120, 255}; }
                else hint = key + TextFormat(" %s abbauen  (%s vorhanden)", kind == 1 ? "Kristalle" : "Wrack", Num(amount).c_str());
            } else if (p.cargo >= p.cargoMax) {
                hint = "Lager voll! Zur Handelsstation fliegen.";
                hc = Color{255, 200, 120, 255};
            }
            if (!hint.empty()) {
                // Panel passt sich der Textbreite an (20 px Rand je Seite)
                float tw = Width(hint, 26 * s) + 40 * s;
                Rectangle r{W * 0.5f - tw * 0.5f, H - 90 * s, tw, 50 * s};
                Panel(r);
                TextCentered(hint, W * 0.5f, r.y + 13 * s, 26 * s, hc);
            }
        }
    }

    // --- Funkspruch der Mission unten rechts --------------------------------------------
    // Avatar-Clip (links, abgespielt von ClipPlayer) mit Untertitel (Start,
    // Ende, Fehlschlag; Dateien und Texte in cfg::MISSIONS). Fehlt der Clip,
    // steht nur der Untertitel da. Die Restzeit läuft nur außerhalb der
    // Station (World::UpdateMissions), angedockt wird er nicht gezeigt. Die
    // letzten 0,6 s blendet er aus.
    if (me.cue >= 0 && me.cue < cfg::MISSION_TYPE_COUNT * 3 && me.cueT > 0.0f && me.dockedAt < 0) {
        const cfg::MissionDef& def = cfg::MISSIONS[me.cue / 3];
        const float a = std::clamp(me.cueT / 0.6f, 0.0f, 1.0f);
        const Texture2D* clip = g_clip.Frame(me.cue);
        const float vs = clip ? 160 * s : 0.0f;              // Kantenlänge des Clipbilds
        const float tx = clip ? vs + 32 * s : 16 * s;       // Textspalte, vom linken Rand
        const float bw2 = 430 * s + tx - 16 * s;
        const std::vector<std::string> lines = WrapLines(def.subtitle[me.cue % 3], 430 * s - 32 * s, 20 * s);
        const float bh = std::max(52 * s + static_cast<float>(lines.size()) * 26 * s, clip ? vs + 32 * s : 0.0f);
        const Rectangle cr{W - bw2 - 16 * s, H - bh - 16 * s, bw2, bh};
        DrawRectangleRec(cr, Color{15, 18, 26, static_cast<unsigned char>(210 * a)});
        DrawRectangleLinesEx(cr, 1.0f, Fade(Color{255, 230, 120, 255}, 0.6f * a));
        if (clip) {
            const Rectangle vr{cr.x + 16 * s, cr.y + 16 * s, vs, vs};
            DrawTexturePro(*clip, Rectangle{0, 0, static_cast<float>(clip->width), static_cast<float>(clip->height)}, vr,
                           Vector2{0, 0}, 0.0f, Fade(WHITE, a));
            DrawRectangleLinesEx(vr, 1.0f, Fade(Color{255, 230, 120, 255}, 0.4f * a));
        }
        Text(std::string("FUNKSPRUCH  ") + def.name, cr.x + tx, cr.y + 12 * s, 16 * s, Fade(Color{255, 230, 120, 255}, a));
        for (size_t i = 0; i < lines.size(); ++i)
            Text(lines[i], cr.x + tx, cr.y + 40 * s + static_cast<float>(i) * 26 * s, 20 * s, Fade(TEXT, a));
    }

    // Tastenhilfe: in den ersten 90 s mit einem Schiff ganz, danach nur ein
    // kurzer Hinweis; mit gehaltener Taste H wieder ganz. Im Mehrspieler ist
    // playTime die Zeit mit dem aktuellen Schiff, nach einem Absturz erscheint
    // die Hilfe also wieder. y = Unterkante minus Höhe der Liste
    // (ControlsHeight, bei 16 px Schrift) minus 16 px Rand.
    // Mit Controller gibt es keine Taste H; der Hinweis nennt dann die Pause,
    // in der die Hilfe immer steht.
    if (!simulation) {
        if (me.playTime < 90.0f || IsKeyDown(KEY_H)) DrawControls(18 * s, H - ControlsHeight(16 * s) - 16 * s, 16 * s);
        else if (controls::ActiveDevice() == controls::Device::KeyboardMouse)
            Text("[H] Steuerung anzeigen", 18 * s, H - 30 * s, 16 * s, Color{200, 205, 215, 170});
        else
            Text("Steuerung: in der Pause " + controls::KeyHint(controls::Act::Pause), 18 * s, H - 30 * s, 16 * s,
                 Color{200, 205, 215, 170});
    }
}

// ===========================================================================
// Stationsfenster
// ===========================================================================
// Kurzbeschreibung eines Schiffs mit seinen Aufrüstungen (Tab "Schiff",
// Raumschiffsverkäufer)
static std::string ShipStats(int type, const int* up) {
    const int t = std::clamp(type, 0, cfg::SHIP_TYPE_COUNT - 1);
    int lvl[cfg::UP_COUNT];
    for (int u = 0; u < cfg::UP_COUNT; ++u) lvl[u] = std::clamp(up[u], 0, cfg::UPGRADE_LEVELS);
    return TextFormat("Tempo %.0f   Schild %.0f   Schaden %.0f   Lager %d   Scanner %.0f m", cfg::ShipSpeed(t, lvl[cfg::UP_SPEED]),
                      cfg::ShipArmor(t, lvl[cfg::UP_ARMOR]), cfg::ShipDamage(t, lvl[cfg::UP_WEAPON]),
                      cfg::ShipCargo(t, lvl[cfg::UP_CARGO]), cfg::ShipScanner(t));
}

// Wird jedes Frame gezeichnet, solange der Spieler angedockt ist. Es liest nur
// die World und gibt zurück, was der Spieler angeklickt bzw. gedrückt hat
// (DockRequest). Ob eine Aktion möglich ist (genug Punkte?), prüft es nur für
// die Anzeige (Knopf grau, Ton "Abgelehnt"); die verbindliche Prüfung macht
// World::Repair/BuyUpgrade – im Mehrspieler auf dem Server, der dafür den
// eigenen, echten Kontostand benutzt.
// Bedienung: Maus auf die Knöpfe oder Tasten R (Reparieren), 1-4 (Aufrüsten,
// Schiff kaufen, mit Umschalt in Zahlung geben), 1-3 (Schiff wechseln bzw.
// Mission annehmen), Tab (Karteireiter der Handelsstation), F oder ESC (Abdocken). Controller: Zeiger mit dem linken Stick, A klickt;
// Y (wie F) oder B (wie ESC) docken ab. Die Tasten R und 1-4 stehen nur bei
// Tastaturbenutzung auf den Knöpfen.
DockRequest DrawDockPanel(const World& w, bool interactive) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    // Ganzen Bildschirm leicht abdunkeln (Alpha 120 von 255), damit das
    // Fenster sich von der weiterlaufenden Welt abhebt.
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 120});
    const Ship& p = w.Player();
    const PlayerState& me = w.Me();
    DockRequest req;
    const bool keyboard = controls::ActiveDevice() == controls::Device::KeyboardMouse;
    const std::string undockLabel = "Abdocken  " + controls::KeyHint(controls::Act::Interact);
    // Absicherung gegen einen ungültigen Stationsindex (dockedAt kommt im
    // Mehrspieler vom Server). Die Abdunkelung oben ist dann trotzdem schon
    // gezeichnet.
    if (me.dockedAt < 0 || me.dockedAt >= static_cast<int>(w.stations.size())) return req;
    const Station& station = w.stations[me.dockedAt];

    // Karteireiter der Handelsstation: 0 = "Station", 1 = "Schiff",
    // 2 = "Missionen". Wurde das Fenster im letzten Frame nicht gezeichnet
    // (neu angedockt), beginnt es wieder mit "Station".
    static int dockTab = 0;
    static unsigned lastFrame = 0;
    if (FrameIndex() > lastFrame + 1) dockTab = 0;
    lastFrame = FrameIndex();

    if (station.type == StationType::Trade) {
        // --- Handelsstation: Verkauf (passiert schon beim Andocken),
        //     Kontostand, Stationsausbau, Reparatur, Abdocken ------------------
        Rectangle r{W * 0.5f - 330 * s, H * 0.5f - 300 * s, 660 * s, 600 * s};
        Panel(r);
        float x = r.x + 30 * s, y = r.y + 24 * s, bw = r.width - 60 * s;
        std::string title = station.name;
        for (char& c : title) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        Text(title, x, y, 40 * s, ACCENT);
        y += 50 * s;

        // Karteireiter (wie im Optionsfenster): gewählter orange, sonst dunkel.
        // Tab-Taste oder RB am Controller: nächster, LB: voriger.
        constexpr int TABS = 3;
        if (interactive && (IsKeyPressed(KEY_TAB) || PadPressed(GAMEPAD_BUTTON_RIGHT_TRIGGER_1))) dockTab = (dockTab + 1) % TABS;
        if (interactive && PadPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_1)) dockTab = (dockTab + TABS - 1) % TABS;
        const char* tabNames[TABS] = {"Station", "Schiff", "Missionen"};
        for (int t = 0; t < TABS; ++t) {
            const Rectangle tr{x + t * 190 * s, y, 180 * s, 40 * s};
            const bool hover = CheckCollisionPointRec(PointerPos(), tr);
            if (interactive && hover && PointerPressed()) dockTab = t;
            const bool current = t == dockTab;
            DrawRectangleRec(tr, current ? ACCENT : (hover ? PANEL_LIGHT : Color{15, 18, 24, 200}));
            if (!current) DrawRectangleLinesEx(tr, 1.0f, ACCENT_DIM);
            TextCentered(tabNames[t], tr.x + tr.width * 0.5f, tr.y + 9 * s, 22 * s, current ? Color{26, 31, 41, 255} : TEXT, !current);
        }
        DrawRectangleRec({x, y + 40 * s, bw, std::max(2.0f, 3.0f * s)}, ACCENT);
        y += 58 * s;

        if (dockTab == 0) {
            // Stufen 0..7, also "von 7" (STATION_STAGES = 8)
            Text(TextFormat("Ausbaustufe %d von %d", w.StationStage(), cfg::STATION_STAGES - 1), x, y, 20 * s, MUTED);
            y += 34 * s;
            // lastSale: Erlös des Verkaufs bei diesem Andocken (World::Dock)
            Text("Ladung verkauft", x, y, 24 * s, MUTED);
            TextRight("+" + Num(me.lastSale), x + bw, y, 24 * s, me.lastSale > 0 ? GOOD : TEXT);
            y += 38 * s;
            Text("Kontostand", x, y, 24 * s, MUTED);
            TextRight(Num(me.balance) + " / " + Num(cfg::GOAL_POINTS), x + bw, y, 24 * s, TEXT);
            y += 32 * s;
            Bar({x, y, bw, 12 * s}, static_cast<float>(me.balance) / cfg::GOAL_POINTS, ACCENT);
            y += 34 * s;

            // Stationsausbau: Fortschritt innerhalb der aktuellen Stufe, also von
            // der Schwelle der aktuellen Stufe (cur) bis zur nächsten (next), z. B.
            // Stufe 2: 5.000 -> 12.000 (cfg::STATION_THRESHOLDS). next = -1 heißt
            // "letzte Stufe erreicht".
            int stage = w.StationStage();
            int next = w.NextStageThreshold();
            Text("Stationsausbau (abgelieferte Ressourcen)", x, y, 20 * s, MUTED);
            y += 26 * s;
            if (next > 0) {
                int cur = cfg::STATION_THRESHOLDS[stage];
                float f = static_cast<float>(w.totalDelivered - cur) / static_cast<float>(next - cur);
                Bar({x, y, bw, 12 * s}, f, Color{255, 210, 120, 255});
                y += 18 * s;
                Text(TextFormat("%s / %s bis Stufe %d", Num(w.totalDelivered).c_str(), Num(next).c_str(), stage + 1), x, y, 18 * s, TEXT);
            } else {
                Bar({x, y, bw, 12 * s}, 1.0f, Color{255, 210, 120, 255});
                y += 18 * s;
                Text("Die Station ist vollständig ausgebaut!", x, y, 18 * s, GOOD);
            }
            y += 40 * s;

            Text("Schild", x, y, 24 * s, MUTED);
            TextRight(TextFormat("%d / %d", static_cast<int>(ceilf(p.hp)), static_cast<int>(p.maxHp)), x + bw, y, 24 * s, TEXT);
            y += 30 * s;
            Bar({x, y, bw, 12 * s}, p.hp / p.maxHp, SHIELD);
            y += 32 * s;

            // Reparatur kostet REPAIR_COST_PER_POINT (2) Punkte je fehlendem
            // Schildpunkt. Reicht das Geld nicht für alles, repariert
            // World::Repair so viel, wie bezahlbar ist. Darum genügt als
            // Bedingung "Geld für mindestens einen Schildpunkt".
            int cost = w.RepairCost(me);
            std::string rl = cost > 0 ? TextFormat("Schild reparieren  (%s Pkt.)%s", Num(cost).c_str(), keyboard ? "  [R]" : "") : "Schild ist voll";
            bool canRepair = cost > 0 && me.balance >= cfg::REPAIR_COST_PER_POINT;
            Rectangle rb{x, y, bw, 50 * s};
            // Klick auf einen gesperrten Knopf liefert bei Button() kein true;
            // repairHover sagt uns trotzdem, ob die Maus darauf war -> "Abgelehnt"-Ton.
            bool repairHover = false;
            if (Button(rb, rl, 22 * s, false, canRepair && interactive, &repairHover) || (interactive && canRepair && IsKeyPressed(KEY_R)))
                req.repair = true;
            else if (interactive && !canRepair && (IsKeyPressed(KEY_R) || (repairHover && PointerPressed())))
                g_audio.Play(Sfx::Denied);
        } else if (dockTab == 1) {
            // --- Tab "Schiff": geflogenes Schiff und die geparkten (hangar),
            //     zu denen man wechseln kann (World::SwitchShip) ----------------
            Text("Aktuelles Schiff", x, y, 20 * s, MUTED);
            y += 26 * s;
            Rectangle cur{x, y, bw, 70 * s};
            DrawRectangleRec(cur, Color{36, 42, 54, 220});
            DrawRectangleLinesEx(cur, 1.0f, ACCENT_DIM);
            Text(cfg::SHIP_TYPES[std::clamp(me.shipType, 0, cfg::SHIP_TYPE_COUNT - 1)].name, cur.x + 16 * s, cur.y + 8 * s, 26 * s, TEXT);
            Text(ShipStats(me.shipType, me.upgrades), cur.x + 16 * s, cur.y + 42 * s, 18 * s, MUTED);
            y += 84 * s;
            Text("Geparkte Schiffe", x, y, 20 * s, MUTED);
            y += 26 * s;
            if (me.hangar.empty()) {
                Text("Keine. Weitere Schiffe gibt es beim Raumschiffsverkäufer.", x, y, 18 * s, TEXT);
                y += 76 * s;
            }
            for (int i = 0; i < static_cast<int>(me.hangar.size()) && i < cfg::SHIP_TYPE_COUNT - 1; ++i) {
                const OwnedShip& o = me.hangar[i];
                const int type = std::clamp(o.type, 0, cfg::SHIP_TYPE_COUNT - 1);
                Rectangle row{x, y, bw, 70 * s};
                DrawRectangleRec(row, Color{36, 42, 54, 220});
                Text(cfg::SHIP_TYPES[type].name, row.x + 16 * s, row.y + 8 * s, 26 * s, TEXT);
                Text(ShipStats(type, o.upgrades), row.x + 16 * s, row.y + 42 * s, 18 * s, MUTED);
                std::string bl = keyboard ? TextFormat("[%d]  Wechseln", i + 1) : std::string("Wechseln");
                Rectangle b{row.x + row.width - 190 * s, row.y + 12 * s, 174 * s, 46 * s};
                if ((Button(b, bl, 22 * s, false, interactive) ||
                     (interactive && IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + i)))))
                    req.switchShip = type;
                y += 80 * s;
            }
        } else {
            // --- Tab "Missionen": laufende Mission und offene Angebote
            //     (World::AcceptMission). Angebote derselben Art stehen in
            //     einer Zeile; angenommen wird über die Art. ---------------------
            const Mission* cur = w.FindMission(me.mission);
            if (cur) {
                const cfg::MissionDef& def = cfg::MISSIONS[static_cast<int>(cur->type)];
                Text("Laufende Mission", x, y, 20 * s, MUTED);
                y += 26 * s;
                Rectangle box{x, y, bw, 70 * s};
                DrawRectangleRec(box, Color{36, 42, 54, 220});
                DrawRectangleLinesEx(box, 1.0f, ACCENT_DIM);
                Text(def.name, box.x + 16 * s, box.y + 8 * s, 26 * s, Color{255, 230, 120, 255});
                Text(MissionProgress(*cur), box.x + 16 * s, box.y + 42 * s, 18 * s, TEXT);
                y += 84 * s;
            }
            Text("Angebote", x, y, 20 * s, MUTED);
            y += 26 * s;
            int count[cfg::MISSION_TYPE_COUNT] = {};
            int offers = 0;
            for (const Mission& m : w.missions)
                if (m.owner < 0) { ++count[static_cast<int>(m.type)]; ++offers; }
            if (offers == 0) {
                Text(w.settings.missions > 0 ? "Gerade keine Angebote. Neue kommen bald." : "Auf diesem Server gibt es keine Missionen.",
                     x, y, 18 * s, TEXT);
            }
            int row = 0;
            for (int t = 0; t < cfg::MISSION_TYPE_COUNT; ++t) {
                if (count[t] == 0) continue;
                const cfg::MissionDef& def = cfg::MISSIONS[t];
                Rectangle rr{x, y, bw, 78 * s};
                DrawRectangleRec(rr, Color{36, 42, 54, 220});
                std::string name = def.name;
                if (count[t] > 1) name += TextFormat("  (%dx)", count[t]);
                Text(name, rr.x + 16 * s, rr.y + 8 * s, 24 * s, TEXT);
                const std::vector<std::string> lines = WrapLines(def.text, bw - 240 * s, 15 * s);
                for (size_t i = 0; i < lines.size() && i < 2; ++i)
                    Text(lines[i], rr.x + 16 * s, rr.y + 38 * s + static_cast<float>(i) * 19 * s, 15 * s, MUTED);
                // Rechts: Belohnung und Knopf. Nur ohne laufende Mission annehmbar.
                TextRight("+" + Num(def.reward) + " Pkt.", rr.x + rr.width - 16 * s, rr.y + 8 * s, 20 * s, GOOD);
                const bool can = cur == nullptr;
                std::string bl = keyboard ? TextFormat("[%d]  Annehmen", row + 1) : std::string("Annehmen");
                Rectangle b{rr.x + rr.width - 196 * s, rr.y + 36 * s, 180 * s, 38 * s};
                bool hover = false;
                const bool click = Button(b, bl, 20 * s, false, can && interactive, &hover);
                const bool key = interactive && IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + row));
                if (interactive && can && (click || key)) req.acceptMission = t;
                else if (interactive && !can && (key || (hover && PointerPressed()))) g_audio.Play(Sfx::Denied);
                y += 86 * s;
                ++row;
            }
        }
        // "Abdocken" steht immer unten im Fenster und ist per Tastatur
        // vorausgewählt (focused = true), daher hervorgehoben
        Rectangle ub{x, r.y + r.height - 74 * s, bw, 50 * s};
        if (Button(ub, undockLabel, 22 * s, true, interactive)) req.undock = true;
    } else if (station.type == StationType::Dealer) {
        // --- Raumschiffsverkäufer: alle Schiffstypen, die man noch nicht
        //     besitzt, kaufen (altes Schiff parken) oder mit Inzahlunggabe
        //     des geflogenen Schiffs (World::BuyShip) -------------------------
        Rectangle r{W * 0.5f - 420 * s, H * 0.5f - 290 * s, 840 * s, 580 * s};
        Panel(r);
        float x = r.x + 30 * s, y = r.y + 24 * s, bw = r.width - 60 * s;
        std::string title = station.name;
        for (char& c : title) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        Text(title, x, y, 40 * s, DEALER_COLOR);
        TextRight("Kontostand  " + Num(me.balance), x + bw, y + 10 * s, 24 * s, ACCENT);
        y += 46 * s;
        Text(TextFormat("Kaufen: altes Schiff wird geparkt.  In Zahlung: +%s Pkt. für das alte Schiff.",
                        Num(w.TradeInValue(me)).c_str()),
             x, y, 18 * s, MUTED);
        y += 34 * s;
        const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        for (int t = 0; t < cfg::SHIP_TYPE_COUNT; ++t) {
            Rectangle row{x, y, bw, 88 * s};
            DrawRectangleRec(row, Color{36, 42, 54, 220});
            const int zero[cfg::UP_COUNT] = {0, 0, 0, 0};
            Text(cfg::SHIP_TYPES[t].name, row.x + 16 * s, row.y + 10 * s, 26 * s, TEXT);
            Text(ShipStats(t, zero), row.x + 16 * s, row.y + 50 * s, 18 * s, MUTED);
            const bool owned = w.OwnsShipType(me, t);
            if (owned) {
                TextRight(t == me.shipType ? "Wird geflogen" : "Im Besitz", row.x + row.width - 20 * s, row.y + 12 * s, 22 * s, GOOD);
            } else {
                // Zwei Knöpfe: Kaufen und In Zahlung geben. Der Preis mit
                // Inzahlunggabe kann negativ sein (Gutschrift).
                const int price = w.ShipPrice(me, t, false);
                const int tradePrice = w.ShipPrice(me, t, true);
                const bool canBuy = me.balance >= price;
                const bool canTrade = me.balance >= tradePrice;
                const bool keyHit = IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + t));
                std::string bl = (keyboard ? TextFormat("[%d] ", t + 1) : std::string()) + "Kaufen " + Num(price);
                std::string tl = (keyboard ? TextFormat("[Umschalt+%d] ", t + 1) : std::string()) + "In Zahlung " +
                                 Num(std::max(0, tradePrice));
                // Knöpfe rechts in der oberen Zeile, die Werte stehen darunter
                Rectangle b1{row.x + row.width - 486 * s, row.y + 8 * s, 200 * s, 36 * s};
                Rectangle b2{row.x + row.width - 276 * s, row.y + 8 * s, 260 * s, 36 * s};
                bool hover1 = false, hover2 = false;
                bool click1 = Button(b1, bl, 18 * s, false, canBuy && interactive, &hover1);
                bool click2 = Button(b2, tl, 18 * s, false, canTrade && interactive, &hover2);
                if (interactive && canBuy && (click1 || (keyHit && !shift))) {
                    req.buyShip = t;
                } else if (interactive && canTrade && (click2 || (keyHit && shift))) {
                    req.buyShip = t;
                    req.tradeIn = true;
                } else if (interactive && (keyHit || ((hover1 || hover2) && PointerPressed()))) {
                    g_audio.Play(Sfx::Denied);
                }
            }
            y += 98 * s;
        }
        Rectangle ub{x, r.y + r.height - 74 * s, bw, 50 * s};
        if (Button(ub, undockLabel, 22 * s, true, interactive)) req.undock = true;
    } else {
        // --- Werft: fünf Aufrüstungen, je eine Zeile mit Stufenkästchen,
        //     Wirkung (aktuell -> nächste Stufe) und Kaufknopf --------------------
        Rectangle r{W * 0.5f - 400 * s, H * 0.5f - 300 * s, 800 * s, 600 * s};
        Panel(r);
        float x = r.x + 30 * s, y = r.y + 24 * s, bw = r.width - 60 * s;
        std::string title = station.name;
        for (char& c : title) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        Text(title, x, y, 40 * s, Color{120, 210, 255, 255});
        TextRight("Kontostand  " + Num(me.balance), x + bw, y + 10 * s, 24 * s, ACCENT);
        y += 46 * s;
        Text("Rüste dein Schiff auf. Upgrades werden mit Ressourcenpunkten bezahlt.", x, y, 18 * s, MUTED);
        y += 34 * s;

        // t läuft in der Reihenfolge von cfg::Upgrade (Antrieb, Panzerung,
        // Bewaffnung, Lager, Kanonen). Die Werte-Tabellen in Config.hpp haben je
        // UPGRADE_LEVELS + 1 Einträge (Index 0 = Startwert), daher ist
        // [lvl + 1] gültig, solange die Stufe nicht maximal ist.
        // Die Werte hängen vom Schiffstyp ab (cfg::ShipSpeed ...).
        const char* names[cfg::UP_COUNT] = {"Antrieb", "Panzerung", "Bewaffnung", "Lager", "Kanonen"};
        const int st = std::clamp(me.shipType, 0, cfg::SHIP_TYPE_COUNT - 1);
        for (int t = 0; t < cfg::UP_COUNT; ++t) {
            int lvl = me.upgrades[t];
            bool maxed = lvl >= cfg::UPGRADE_MAX[t];
            std::string desc;
            switch (t) {
                case cfg::UP_SPEED:
                    desc = maxed ? TextFormat("Tempo %.0f", cfg::ShipSpeed(st, lvl))
                                 : TextFormat("Tempo %.0f  ->  %.0f", cfg::ShipSpeed(st, lvl), cfg::ShipSpeed(st, lvl + 1));
                    break;
                case cfg::UP_ARMOR:
                    desc = maxed ? TextFormat("Schild %.0f", cfg::ShipArmor(st, lvl))
                                 : TextFormat("Schild %.0f  ->  %.0f", cfg::ShipArmor(st, lvl), cfg::ShipArmor(st, lvl + 1));
                    break;
                case cfg::UP_WEAPON:
                    // WEAPON_RATE ist die Pause zwischen zwei Schüssen in
                    // Sekunden; 1 / Pause = Schüsse pro Sekunde.
                    desc = maxed ? TextFormat("Schaden %.0f, %.1f Schuss/s", cfg::ShipDamage(st, lvl), 1.0f / cfg::WEAPON_RATE[lvl])
                                 : TextFormat("Schaden %.0f -> %.0f,  %.1f -> %.1f Schuss/s", cfg::ShipDamage(st, lvl),
                                              cfg::ShipDamage(st, lvl + 1), 1.0f / cfg::WEAPON_RATE[lvl], 1.0f / cfg::WEAPON_RATE[lvl + 1]);
                    break;
                case cfg::UP_GUNS:
                    desc = maxed ? TextFormat("%d Schüsse gleichzeitig", cfg::GUN_COUNT[lvl])
                                 : TextFormat("%d  ->  %d Schüsse gleichzeitig", cfg::GUN_COUNT[lvl], cfg::GUN_COUNT[lvl + 1]);
                    break;
                default:
                    desc = maxed ? TextFormat("Kapazität %d", cfg::ShipCargo(st, lvl))
                                 : TextFormat("Kapazität %d  ->  %d", cfg::ShipCargo(st, lvl), cfg::ShipCargo(st, lvl + 1));
                    break;
            }
            Rectangle row{x, y, bw, 74 * s};
            DrawRectangleRec(row, Color{36, 42, 54, 220});
            Text(names[t], row.x + 16 * s, row.y + 10 * s, 26 * s, TEXT);
            for (int k = 0; k < cfg::UPGRADE_MAX[t]; ++k) {
                Color c = k < lvl ? ACCENT : Color{70, 76, 90, 255};
                DrawRectangle(static_cast<int>(row.x + 16 * s + k * 22 * s), static_cast<int>(row.y + 46 * s),
                              static_cast<int>(18 * s), static_cast<int>(10 * s), c);
            }
            Text(desc, row.x + 150 * s, row.y + 44 * s, 18 * s, MUTED);
            // UpgradeCost liefert -1 bei maximaler Stufe -> afford = false.
            int cost = w.UpgradeCost(me, t);
            bool afford = cost >= 0 && me.balance >= cost;
            std::string bl = maxed ? std::string("MAXIMUM")
                                   : keyboard ? TextFormat("[%d]  %s Pkt.", t + 1, Num(cost).c_str()) : Num(cost) + " Pkt.";
            Rectangle b{row.x + row.width - 230 * s, row.y + 14 * s, 214 * s, 50 * s};
            bool buyHover = false;
            bool click = Button(b, bl, 22 * s, false, afford && interactive, &buyHover);
            // Zifferntasten 1-5: In raylibs KeyboardKey liegen KEY_ONE..KEY_FIVE
            // direkt hintereinander (ASCII '1'..'5'), daher KEY_ONE + t.
            bool keyBuy = IsKeyPressed(static_cast<KeyboardKey>(KEY_ONE + t));
            if (interactive && afford && (click || keyBuy)) {
                req.buy = t;
            } else if (interactive && !afford && (keyBuy || (buyHover && PointerPressed()))) {
                g_audio.Play(Sfx::Denied);
            }
            y += 82 * s;
        }
        y += 6 * s;
        Rectangle ub{x, y, bw, 50 * s};
        if (Button(ub, undockLabel, 22 * s, true, interactive)) req.undock = true;
    }

    // F (dieselbe Taste wie zum Andocken) oder ESC verlassen die Station;
    // am Controller Y (dieselbe Taste wie zum Andocken) oder B bzw. die
    // Menü-Taste (BackPressed, wie ESC). Deshalb müssen die Aufrufer
    // interactive in den ersten Frames nach dem Andocken auf false lassen
    // (siehe Hud.hpp).
    if (interactive && (IsKeyPressed(KEY_F) || BackPressed() || PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_UP))) req.undock = true;
    return req;
}

// ===========================================================================
// Endbildschirme
// ===========================================================================
// Gemeinsamer Aufbau für Niederlage und Sieg (nur Einzelspieler, siehe
// Hud.hpp): Titel, Untertitel, Statistik, Knopf "Zum Hauptmenü".
// Liefert true, wenn der Spieler zurück ins Menü will.
static bool EndScreen(const World& w, const char* title, const char* sub, Color tc) {
    const float s = Scale();
    const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(W), static_cast<int>(H), Color{8, 10, 16, 160});
    Rectangle r{W * 0.5f - 340 * s, H * 0.5f - 230 * s, 680 * s, 460 * s};
    Panel(r);
    TextCentered(title, W * 0.5f, r.y + 30 * s, 50 * s, tc);
    TextCentered(sub, W * 0.5f, r.y + 96 * s, 22 * s, MUTED);
    float x = r.x + 60 * s, y = r.y + 150 * s, bw = r.width - 120 * s;
    // Hilfsfunktion für eine Statistikzeile "Bezeichnung ...... Wert"; sie
    // schiebt y (per Referenz eingefangen, [&]) jeweils eine Zeile weiter.
    auto row = [&](const char* k, const std::string& v) {
        Text(k, x, y, 24 * s, MUTED);
        TextRight(v, x + bw, y, 24 * s, TEXT);
        y += 38 * s;
    };
    row("Kontostand", Num(w.Me().balance));
    row("Insgesamt abgeliefert", Num(w.totalDelivered));
    row("Stationsstufe", TextFormat("%d / %d", w.StationStage(), cfg::STATION_STAGES - 1));
    row("Abschüsse", std::to_string(w.Me().kills));
    row("Spielzeit", Time(w.Me().playTime));
    Rectangle b{W * 0.5f - 170 * s, r.y + r.height - 76 * s, 340 * s, 52 * s};
    bool go = Button(b, "Zum Hauptmenü", 24 * s, true);
    // Tastatur erst nach 1 s: Wer beim Sieg/Absturz gerade Enter oder ESC
    // drückt, soll den Bildschirm nicht versehentlich sofort schließen.
    // Controller: A (der einzige Knopf hier) oder B wie ESC.
    if (w.outcomeT > 1.0f && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || BackPressed() ||
                              PadPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)))
        go = true;
    return go;
}

bool DrawGameOver(const World& w) {
    return EndScreen(w, "SCHIFF ZERSTÖRT", "Dein Schiff wurde abgeschossen. Das Spiel ist zu Ende.", BAD);
}

// Die Zahl im Untertitel ist fest eingetragen und muss zu cfg::GOAL_POINTS passen.
bool DrawVictory(const World& w) {
    return EndScreen(w, "ZIEL ERREICHT!", "Du hast 100.000 Ressourcenpunkte erwirtschaftet.", GOOD);
}
