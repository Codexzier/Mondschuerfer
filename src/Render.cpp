// ---------------------------------------------------------------------------
// Render.cpp – zeichnet die Spielwelt in 3D (Definition von World::Draw).
//
// Wofür ist die Datei da?
//   World (shared/World.hpp) enthält nur Daten und Regeln und zeichnet
//   nichts, damit der Server ohne Grafik auskommt. World::Draw ist dort zwar
//   deklariert, wird aber erst hier definiert. Nur das Spiel (Client) wird mit
//   dieser Datei gebaut, der Server nicht.
//
// Wer benutzt sie?
//   - src/main.cpp: Einzelspieler, Hintergrund des Hauptmenüs, Simulation.
//   - src/Multiplayer.cpp: die Spiegelwelt des Mehrspieler-Clients
//     (NetClient) beim Spielen und Zuschauen sowie die Hintergrundwelt der
//     Mehrspieler-Menüs.
//   Aufgerufen wird World::Draw immer zwischen BeginMode3D(cam) und
//   EndMode3D(). BeginMode3D stellt die Kamera ein: Aus position (wo die
//   Kamera steht), target (wohin sie schaut), up und fovy (Öffnungswinkel)
//   berechnet raylib die Blick- und die Projektionsmatrix. Danach sind alle
//   Koordinaten Weltkoordinaten (1 Einheit = 1 Meter, Y zeigt nach oben,
//   der Boden liegt bei y = 0). EndMode3D schaltet zurück auf 2D (für das HUD).
//
// Woher kommen die Daten?
//   Einzelspieler: World::Update() rechnet alles selbst aus.
//   Mehrspieler: NetClient füllt die Welt aus den Snapshots des Servers
//   (fremde Schiffe interpoliert, das eigene Schiff vorhergesagt). Für diese
//   Datei macht das keinen Unterschied – sie zeichnet einfach, was in der
//   Welt steht. Das eigene Schiff ist immer ships[0] (Player()), der eigene
//   Spielerzustand players[0] (Me()).
//
// Abhängigkeiten:
//   - raylib: DrawCylinder/DrawCylinderEx/DrawCube (einfache Formen),
//     BeginBlendMode (Mischmodus für Transparenz).
//   - rlgl: raylibs Schicht direkt über OpenGL. Hier für eigene Dreiecke
//     (rlBegin/rlVertex3f/rlEnd), Tiefenpuffer und Backface-Culling.
//   - Assets.hpp: 3D-Modelle zeichnen (g_assets), TRS-Matrizen.
//   - World.hpp / Config.hpp: Spielzustand und Balancing-Werte.
//
// Ablauf von World::Draw (die Reihenfolge ist wichtig):
//   0. Grafikeffekte für dieses Bild einstellen (Effects.hpp): Sandsturm,
//      Licht der Explosionen und – wenn die Option an ist – die
//      Schattenkarte: die bewegten Objekte (DrawMovingModels) einmal aus
//      Sicht der Sonne zeichnen.
//   1. Undurchsichtige Modelle: Boden, Dekoration, Schienen, Startplätze,
//      Stationen, Lagerstätten, Wracks und die bewegten Objekte (Züge,
//      Raketen, Rover, Personen, Figuren zu Fuß, Schiffe). Sie füllen den Tiefenpuffer.
//   2. Halbtransparente Fahrspuren, Bodenmarkierungen und Schatten (normales
//      Mischen; die runden "Blob-Schatten" nur ohne Schattenkarte).
//   3. Leuchteffekte mit additivem Mischen: Scanner, Lichtsäulen, Glimmen
//      der Kristalle, Tore, Raketenflammen, Abbaustrahlen, Triebwerke,
//      Triebwerksschweif, Geschosse, leuchtende Partikel, Explosionsblitze.
//   4. Rauch (auch der Raketen), Trümmer und Sandkörner (normales Mischen).
//   Durchsichtiges kommt nach dem Undurchsichtigen, weil es mit dem schon
//   fertigen Bild dahinter gemischt wird. Im Sandsturm verblasst alles
//   Durchsichtige mit dem Abstand zum Schiff (fx::BeginEffects).
//
// Sichtweite ("Culling"): Alles, was weiter als drawRange (Standard 115 m,
// Zuschauer: ganze Karte) waagerecht vom Blickpunkt der Kamera (cam.target)
// entfernt ist, wird übersprungen. Jedes Modell kostet mindestens einen
// eigenen Zeichenaufruf; Unsichtbares wegzulassen spart viel Zeit.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Assets.hpp"
#include "ClientConfig.hpp"
#include "Effects.hpp"
#include "Models.hpp"
#include "TiltShift.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// Flacher Ring auf dem Boden (beidseitig sichtbar)
//
// Kreisring zwischen rIn und rOut aus einzelnen Dreiecken, direkt über rlgl:
// rlBegin(RL_TRIANGLES) – je drei rlVertex3f ergeben ein Dreieck, die Farbe
// aus rlColor4ub gilt für alle folgenden Eckpunkte. Pro Segment entsteht ein
// Viereck (zwei Dreiecke) zwischen innerem und äußerem Kreis; mehr Segmente =
// rundere Kante (große Ringe bekommen deshalb mehr).
// rlgl sammelt die Eckpunkte im "Render-Batch" (siehe World::Draw) und zeichnet
// sie mit raylibs Standard-Shader, also ohne Beleuchtung in genau dieser Farbe.
// "Beidseitig sichtbar" gilt nur, weil World::Draw vorher das Backface-Culling
// abschaltet.
// y = 0.12: knapp über dem Boden (y = 0) und den Schatten (0.06), sonst
// flackern die Flächen gegeneinander ("Z-Fighting": der Tiefenpuffer kann
// gleich hohe Flächen nicht sauber unterscheiden).
void DrawGroundRing(Vector3 c, float rIn, float rOut, Color col, int segments = 64, float y = 0.12f) {
    rlBegin(RL_TRIANGLES);
    rlColor4ub(col.r, col.g, col.b, col.a);
    for (int i = 0; i < segments; ++i) {
        float a0 = 2 * PI * i / segments;
        float a1 = 2 * PI * (i + 1) / segments;
        Vector3 i0{c.x + cosf(a0) * rIn, y, c.z + sinf(a0) * rIn};
        Vector3 o0{c.x + cosf(a0) * rOut, y, c.z + sinf(a0) * rOut};
        Vector3 i1{c.x + cosf(a1) * rIn, y, c.z + sinf(a1) * rIn};
        Vector3 o1{c.x + cosf(a1) * rOut, y, c.z + sinf(a1) * rOut};
        rlVertex3f(i0.x, i0.y, i0.z); rlVertex3f(o0.x, o0.y, o0.z); rlVertex3f(o1.x, o1.y, o1.z);
        rlVertex3f(i0.x, i0.y, i0.z); rlVertex3f(o1.x, o1.y, o1.z); rlVertex3f(i1.x, i1.y, i1.z);
    }
    rlEnd();
}

// Gestrichelter Ring
//
// dashes Striche gleichmäßig verteilt; jeder Strich belegt 55 % seines
// Abschnitts, der Rest ist Lücke. Jeder Strich besteht aus 4 Teilstücken,
// damit er der Rundung folgt. phase verschiebt alle Striche (1.0 = um genau
// einen Abschnitt); wächst phase mit der Zeit, dreht sich der Ring langsam.
// Gleiche Bauweise und Höhe wie DrawGroundRing.
void DrawDashedRing(Vector3 c, float r, float width, Color col, int dashes, float phase) {
    rlBegin(RL_TRIANGLES);
    rlColor4ub(col.r, col.g, col.b, col.a);
    const int sub = 4;
    for (int d = 0; d < dashes; ++d) {
        float start = (d + phase) * 2 * PI / dashes;
        float len = 2 * PI / dashes * 0.55f;
        for (int s = 0; s < sub; ++s) {
            float a0 = start + len * s / sub;
            float a1 = start + len * (s + 1) / sub;
            float rIn = r - width * 0.5f, rOut = r + width * 0.5f;
            rlVertex3f(c.x + cosf(a0) * rIn, 0.12f, c.z + sinf(a0) * rIn);
            rlVertex3f(c.x + cosf(a0) * rOut, 0.12f, c.z + sinf(a0) * rOut);
            rlVertex3f(c.x + cosf(a1) * rOut, 0.12f, c.z + sinf(a1) * rOut);
            rlVertex3f(c.x + cosf(a0) * rIn, 0.12f, c.z + sinf(a0) * rIn);
            rlVertex3f(c.x + cosf(a1) * rOut, 0.12f, c.z + sinf(a1) * rOut);
            rlVertex3f(c.x + cosf(a1) * rIn, 0.12f, c.z + sinf(a1) * rIn);
        }
    }
    rlEnd();
}

// Senkrechter Lichtstrahl als Markierung
//
// Kegelstumpf ab 0.2 m Höhe, oben schmaler (60 % des unteren Radius).
// DrawCylinder(Mittelpunkt unten, Radius oben, Radius unten, Höhe, Segmente,
// Farbe) ist eine raylib-Form aus dem Render-Batch. Gedacht für additives
// Mischen: Dann wirkt die Säule wie ein Lichtschein.
void DrawBeam(Vector3 base, float height, float radius, Color col) {
    DrawCylinder({base.x, 0.2f, base.z}, radius * 0.6f, radius, height, 8, col);
}

// Einfacher "Blob-Schatten": eine flache, halbdurchsichtige schwarze Scheibe
// unter dem Schiff. Nur wenn die Grafikoption "Schatten" aus ist; sonst gibt
// es echte Schatten (Schattenkarte, Effects.hpp). Die Scheibe zeigt trotzdem
// gut, wo ein schwebendes Schiff über dem Boden steht.
void DrawShadow(Vector3 pos, float radius, float alpha) {
    DrawCylinder({pos.x, 0.06f, pos.z}, radius, radius, 0.02f, 18, Fade(BLACK, alpha));
}

// Weicher Lichtfleck: Scheibe mit der Farbe col in der Mitte, die zum Rand
// hin durchsichtig wird (Alpha 0). Jedes Dreieck bekommt eigene Farben je
// Eckpunkt; OpenGL verläuft dazwischen. Für additives Mischen gedacht
// (Schein der Kristalle und Tore auf dem Boden).
void DrawGlowDisc(Vector3 c, float r, Color col, int segments = 24) {
    rlBegin(RL_TRIANGLES);
    for (int i = 0; i < segments; ++i) {
        const float a0 = 2 * PI * i / segments, a1 = 2 * PI * (i + 1) / segments;
        rlColor4ub(col.r, col.g, col.b, col.a);
        rlVertex3f(c.x, c.y, c.z);
        rlColor4ub(col.r, col.g, col.b, 0);
        rlVertex3f(c.x + cosf(a0) * r, c.y, c.z + sinf(a0) * r);
        rlVertex3f(c.x + cosf(a1) * r, c.y, c.z + sinf(a1) * r);
    }
    rlEnd();
}

// Pseudozufall 0..1 aus einer Zahl (gleiche Eingabe -> gleiches Ergebnis),
// z. B. für die Stelle, an der ein Kristall gerade aufblitzt.
float Hash01(float n) {
    const float x = sinf(n * 12.9898f) * 43758.5453f;
    return x - floorf(x);
}

// Tore (Kartenanimation, MapAnim.cpp): Jedes Tor öffnet einmal pro Minute
// für GATE_OPEN Sekunden ein "Wurmloch". Wann, hängt von seiner Lage ab, damit
// nicht alle gleichzeitig aufleuchten. GateTime = Sekunden seit dem letzten
// Öffnen (0..60), GateGlow = Leuchtstärke 0..1 (in 0,3 s an, in der letzten
// Sekunde aus).
constexpr float GATE_PERIOD = 60.0f;
constexpr float GATE_OPEN = 6.0f;
float GateTime(const Deco& d, float time) { return fmodf(time + fabsf(d.pos.x * 0.37f + d.pos.z * 0.61f), GATE_PERIOD); }
float GateGlow(float u) {
    if (u >= GATE_OPEN) return 0.0f;
    return std::min(1.0f, u / 0.3f) * std::clamp(GATE_OPEN - u, 0.0f, 1.0f);
}

// Bauteile der Raketen auf den Startplätzen: wie am Raketenplatz der
// Handelsstation (WorldGen.cpp) übereinandergestapelt, y = Höhe im Kit-Raster.
struct RocketPart {
    const char* model;
    float y;
};
const RocketPart kRocketA[4] = {{"rocket_baseA", 0.0f}, {"rocket_fuelA", 1.0f}, {"rocket_sidesA", 1.5f}, {"rocket_topA", 2.5f}};
const RocketPart kRocketB[4] = {{"rocket_baseB", 0.0f}, {"rocket_fuelB", 1.0f}, {"rocket_sidesB", 2.0f}, {"rocket_topB", 3.0f}};
constexpr float PAD_SCALE = 1.8f;   // Startplatz und Rakete in Stationsgröße

// Easing-Funktion "easeOutBack" (bekannte Formel, z. B. easings.net):
// t = 0..1 ergibt 0..1, schießt aber kurz vor dem Ende um etwa 10 % über 1
// hinaus und federt zurück. c1 = 1.70158 ist der übliche Wert für genau
// dieses Überschwingen. Für Dinge, die "aus dem Boden ploppen" sollen
// (Lagerstätten, neue Stationsteile).
float EaseOutBack(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    float x = t - 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

// Bewegte Modelle: Monorail-Züge, Raketen, Rover, Personen, Figuren zu Fuß und Schiffe.
// World::Draw zeichnet sie zweimal: in die Schattenkarte (Grafikoption
// "Schatten": nur sie werfen Schatten) und ins eigentliche Bild. So passen
// Schatten und Modell immer genau zusammen. hitFlash = Stärke des
// Treffer-Aufblitzens aus den Optionen (0..1).
void DrawMovingModels(const World& w, Vector3 focus, float cullR, float hitFlash) {
    const float time = w.time;
    // Modell-IDs nur einmal nachschlagen (static, siehe Geschütztürme in World::Draw)
    static int trainCars[3] = {-1, -1, -1}, trainMid[3] = {-1, -1, -1}, rocketIds[2][4], roverModel = -1;
    if (trainCars[0] < 0) {
        trainCars[0] = g_assets.Id("monorail_trainFront");
        trainCars[1] = g_assets.Id("monorail_trainPassenger");
        trainCars[2] = g_assets.Id("monorail_trainEnd");
        trainMid[0] = g_assets.Id("monorail_trainPassenger");
        trainMid[1] = g_assets.Id("monorail_trainCargo");
        trainMid[2] = g_assets.Id("monorail_trainBox");
        for (int k = 0; k < 4; ++k) {
            rocketIds[0][k] = g_assets.Id(kRocketA[k].model);
            rocketIds[1][k] = g_assets.Id(kRocketB[k].model);
        }
        roverModel = g_assets.Id("rover");
    }

    // --- Monorail-Züge (eigene Karte, shared/GameMap.hpp) ------------------------
    // Auf jedem geschlossenen Schienenkreis fährt ein Zug aus Triebwagen,
    // Personenwagen und Schlusswagen. Die Lage ergibt sich nur aus der
    // Weltzeit, es muss also nichts weitergerechnet oder gespeichert werden.
    // Die Wagen schauen wie die Schiffe mit der Nase nach -z (YawTowards).
    for (const MonorailLoop& loop : w.monorails) {
        for (int c = 0; c < 3; ++c) {
            Vector3 dir;
            const Vector3 p = MonorailPoint(loop, time * mapgrid::TRAIN_SPEED - c * mapgrid::TRAIN_CAR_GAP, &dir);
            if (Dist2D(p, focus) > cullR + 5.0f) continue;
            g_assets.Draw(trainCars[c], p, YawTowards({0, 0, 0}, dir), mapgrid::CELL);
        }
    }

    // --- Züge zwischen den Stationen (Kartenanimation, MapAnim.cpp) ---------------
    // Der Zug pendelt: vorne und hinten je ein Triebwagen (der hintere
    // umgedreht), so braucht er an den Enden nicht zu wenden. Die Wagen stehen
    // auf der Oberkante der Schiene (0,13 im Kit, wie kTrackTop in GameMap.cpp).
    for (const StationRail& r : w.rails) {
        const float c = RailTrainPos(r, time);
        if (c < 0.0f) continue;
        Vector3 dir = Vector3Subtract(r.b, r.a);
        dir.y = 0.0f;
        dir = Vector3Normalize(dir);
        for (int car = 0; car < 3; ++car) {
            Vector3 p = Vector3Add(r.a, Vector3Scale(dir, c + (car - 1) * mapgrid::TRAIN_CAR_GAP));
            p.y = 0.13f * mapgrid::CELL;
            if (Dist2D(p, focus) > cullR + 5.0f) continue;
            const int model = car == 1 ? trainMid[r.cargo] : trainCars[0];
            const float carYaw = YawTowards({0, 0, 0}, car == 0 ? Vector3Negate(dir) : dir);
            g_assets.Draw(model, p, carYaw, mapgrid::CELL);
        }
    }

    // --- Raketen auf den Startplätzen (Kartenanimation) ------------------------------
    // RocketAt: steht, startet, ist unterwegs oder landet. Beim Aufstieg kippt
    // die ganze Rakete um die waagerechte Achse quer zu "out" (up x out); erst
    // skalieren, dann das Bauteil auf seine Höhe schieben, kippen und an den
    // Fußpunkt verschieben.
    for (const RocketPad& pad : w.pads) {
        if (Dist2D(pad.pos, focus) > cullR + 10.0f) continue;
        const RocketPose rp = RocketAt(pad, time);
        if (!rp.visible || rp.size < 0.01f) continue;
        const float sc = PAD_SCALE * rp.size;
        const Vector3 axis{pad.out.z, 0.0f, -pad.out.x};
        for (int k = 0; k < 4; ++k) {
            const RocketPart& part = pad.typeB ? kRocketB[k] : kRocketA[k];
            Matrix m = MatrixMultiply(MatrixScale(sc, sc, sc), MatrixTranslate(0.0f, part.y * sc, 0.0f));
            if (rp.tilt > 0.0f) m = MatrixMultiply(m, MatrixRotate(axis, rp.tilt));
            m = MatrixMultiply(m, MatrixTranslate(rp.pos.x, rp.pos.y, rp.pos.z));
            g_assets.Draw(rocketIds[pad.typeB ? 1 : 0][k], m);
        }
    }

    // --- Rover (Kartenanimation) ----------------------------------------------------
    // Beim Fahren schaukeln sie leicht (kleine Nick- und Rollwinkel, mit dem
    // Index als Phasenversatz), im Stand nicht.
    for (size_t i = 0; i < w.rovers.size(); ++i) {
        const Rover& r = w.rovers[i];
        if (Dist2D(r.pos, focus) > cullR + 2.0f) continue;
        const float bump = std::min(1.0f, r.speed * 0.5f);
        const float pitch = 0.035f * bump * sinf(time * 9.0f + i * 1.7f);
        const float roll = 0.03f * bump * sinf(time * 7.0f + i * 2.3f);
        g_assets.Draw(roverModel, TRS(r.pos, r.yaw, pitch, roll, r.scale));
    }

    // --- Personen (Mission "Personen einsammeln") --------------------------------
    // Nur die eigenen (im Mehrspieler schickt der Server ohnehin nur diese).
    // 1,8-fach vergrößert (das Modell ist nur 0,8 m hoch), damit man sie von
    // oben erkennt; sie hüpfen leicht, als würden sie auf sich aufmerksam machen.
    const PlayerState& me = w.Me();
    for (const auto& pe : w.persons) {
        if (pe.owner != me.id || Dist2D(pe.pos, focus) > cullR) continue;
        const float hop = 0.12f * fabsf(sinf(time * 4.0f + static_cast<float>(pe.id)));
        g_assets.Draw(pe.model, Vector3{pe.pos.x, hop, pe.pos.z}, pe.yaw, 1.8f);
    }

    // --- Figuren zu Fuß (FPS-Modus) -------------------------------------------------
    // Kameraden und Bodenpiraten als animierte Figur (Assets::DrawCharacter),
    // die eigene Figur nicht (Ego-Ansicht). Das Modell ist 3,77 Einheiten hoch
    // und wird auf TROOPER_HEIGHT verkleinert; es schaut nach +z, deshalb
    // yaw + PI. Animation nach Zustand: gefallen "death" (einmal), in der Luft
    // "jump", ab 0,8 m/s "run", langsamer "walk", im Kampf "shoot", sonst "idle".
    // Damit die Füße nicht über den Boden rutschen, läuft walk/run nach dem
    // zurückgelegten Weg (Trooper::walked): Die Animationen bewegen die Füße
    // mit 1,5 bzw. 7,1 Modelleinheiten pro Sekunde, nach dem Verkleinern also
    // mit 0,24 bzw. 1,13 m/s. Die Waffe (Blaster wie in der Ego-Ansicht) sitzt
    // in der rechten Hand und zeigt in die Blickrichtung der Figur.
    static const int blaster = g_assets.Id("blaster/blaster-a");
    for (const Trooper& t : w.troopers) {
        if (t.id == me.trooperId || Dist2D(t.pos, focus) > cullR) continue;
        const float speed = Vector2Length({t.vel.x, t.vel.z});
        const float scale = cfg::TROOPER_HEIGHT / 3.77f;
        const char* anim = "idle";
        float at = t.age;
        if (t.down) {
            anim = "death";
            at = t.downT;
        } else if (!t.onGround) {
            anim = "jump";
        } else if (speed > 0.8f) {
            anim = "run";
            at = t.walked / (7.1f * scale);
        } else if (speed > 0.1f) {
            anim = "walk";
            at = t.walked / (1.5f * scale);
        } else if (t.targetId >= 0 || t.fireCd > 0.0f) {
            anim = "shoot";
        }
        const float hf = t.hitFlash * hitFlash;
        const Vector3 hand = g_assets.DrawCharacter(t.skin, anim, at, !t.down, TRS(t.pos, t.yaw + PI, scale),
                                                    WHITE, Vector3{0.9f * hf, 0.45f * hf, 0.3f * hf});
        if (!t.down) g_assets.Draw(blaster, TRS(hand, t.yaw, t.pitch, 0.0f, 0.183f));
    }

    // --- Schiffe -------------------------------------------------------------------
    // dead = wird gerade entfernt, hidden = angedockt (steht unsichtbar in der
    // Station) bzw. Platzhalter ohne Schiff im Mehrspieler.
    for (const auto& s : w.ships) {
        if (s.dead || s.hidden) continue;
        if (Dist2D(s.pos, focus) > cullR) continue;
        // Farbton je Schiffsart: Piraten rötlich, Elite-Piraten violett.
        // Spieler (auch fremde im Mehrspieler) und Händler/Miner bleiben unverändert.
        Color tint = WHITE;
        if (s.kind == ShipKind::Pirate) tint = Color{255, 150, 145, 255};
        else if (s.kind == ShipKind::Elite) tint = Color{190, 150, 255, 255};
        // Treffer-Aufblitzen: hitFlash springt beim Treffer auf 1 und klingt
        // in ca. 0,3 s ab (World.cpp); warmweißes Eigenleuchten, so stark wie
        // in den Optionen eingestellt.
        const float hf = s.hitFlash * hitFlash;
        Vector3 em{hf * 0.7f, hf * 0.5f, hf * 0.4f};
        // Nicken: Beim Absturz zeigt die Nase deutlich nach unten (0,5 rad,
        // ca. 29°); beim Beschleunigen neigt sich das Schiff leicht nach vorn
        // (bis ca. 3,4° bei vollem Schub). bank = Rollwinkel in Kurven (bzw.
        // Trudeln beim Absturz), berechnet in World.cpp.
        float pitch = s.crashing ? -0.5f : -0.06f * s.thrustVisual;
        g_assets.Draw(s.model, TRS(s.pos, s.yaw, pitch, s.bank, 1.0f), tint, em);
    }
}

// Lage eines Triebwerks: Zwei Triebwerke am Heck jedes Schiffs (k = -1 links,
// +1 rechts). fwd = Flugrichtung aus dem Kurs, side = fwd um 90° in der
// Waagerechten gedreht (zeigt zur Seite). back Meter hinter der Schiffsmitte
// (das Modell der Händler/Miner, ShipKind::Miner, ist länger: 1.4 statt
// 1.15), 0.45 m zur Seite, 0.35 m hoch.
Vector3 EnginePos(const Ship& s, int k) {
    const Vector3 fwd = ForwardFromYaw(s.yaw);
    const Vector3 side{fwd.z, 0, -fwd.x};
    const float back = s.kind == ShipKind::Miner ? 1.4f : 1.15f;
    Vector3 base = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, -back), Vector3Scale(side, 0.45f * k)));
    base.y += 0.35f;
    return base;
}

// Triebwerksschweif (Grafikoption, Stufen kurz/mittel/lang): kleine
// leuchtende Würfel, die hinter den Triebwerken entstehen, zurückbleiben,
// schrumpfen und verblassen. Sie gehören nicht zur Welt (World::Draw ist
// const und die Spiellogik braucht sie nicht), deshalb merkt sich diese
// Datei sie selbst. Eine Wolke ist durch Startpunkt, Drift und
// Entstehungszeit festgelegt; ihre Lage folgt daraus, ohne Update.
struct TrailPuff {
    Vector3 pos{};          // Ort beim Entstehen
    Vector3 vel{};          // Drift in m/s
    float born = 0.0f;      // Weltzeit beim Entstehen
    float power = 1.0f;     // Schub beim Entstehen 0..1 (Helligkeit)
    bool pirate = false;    // Piraten: rötliche Flamme
};
struct TrailState {
    const World* world = nullptr;   // Welt, zu der die Wolken gehören (Menü-Hintergrund, Spiel ...)
    float last = 0.0f;              // Weltzeit beim letzten Zeichnen
    std::vector<TrailPuff> puffs;
};
TrailState g_trail;
// Lebensdauer einer Wolke in Sekunden je Stufe (0 = aus)
constexpr float kTrailLife[4] = {0.0f, 0.3f, 0.55f, 0.9f};
constexpr float kTrailStep = 1.0f / 50.0f;   // je Triebwerk 50 Wolken pro Sekunde

// Gelbe Variante eines Kristallmodells (FPS-Modus): Kopien der drei
// Kristallmodelle mit gelbem statt grünem Kristallmaterial
// (assets/models/rock_crystals*Yellow.glb).
int YellowCrystal(const World& w, int model) {
    static const int small = g_assets.Id("rock_crystalsYellow"), largeA = g_assets.Id("rock_crystalsLargeAYellow"),
                     largeB = g_assets.Id("rock_crystalsLargeBYellow");
    return model == w.mdl.crystalA ? largeA : model == w.mdl.crystalB ? largeB : small;
}

// Waffe der eigenen Figur in der Ego-Ansicht: Blaster aus dem Blaster Kit
// (Lauf zeigt im Modell nach -z wie die Schiffsnasen), rechts unten vor der
// Kamera und mit dem Blick geneigt. Dazu ein leichtes Wippen beim Laufen,
// ein kleiner Rückstoß nach jedem Schuss und beim Nachladen ein Absenken.
// Alle Maße passen zur Figurengröße (ein Drittel, siehe Config.hpp); main.cpp
// stellt dafür die nahe Clip-Ebene auf 2 cm.
void DrawTrooperWeapon(const Trooper& t, float time) {
    static const int model = g_assets.Id("blaster/blaster-a");
    const Vector3 dir = TrooperAim(t);
    const Vector3 fwd = ForwardFromYaw(t.yaw);
    const Vector3 right{-fwd.z, 0.0f, fwd.x};
    const Vector3 up = Vector3CrossProduct(right, dir);
    const float speed = std::min(1.0f, Vector2Length({t.vel.x, t.vel.z}) / cfg::TROOPER_WALK);
    const float bob = t.onGround ? sinf(time * 9.0f) * 0.004f * speed : 0.0f;
    const float kick = std::max(0.0f, t.fireCd) / cfg::TROOPER_FIRE_INTERVAL * 0.013f;
    const float reload = t.reloadT > 0.0f ? sinf(PI * (1.0f - t.reloadT / cfg::TROOPER_RELOAD)) : 0.0f;
    Vector3 pos = Vector3Add(TrooperEye(t), Vector3Scale(dir, 0.113f - kick));
    pos = Vector3Add(pos, Vector3Scale(right, 0.053f));
    pos = Vector3Add(pos, Vector3Scale(up, -0.05f + bob - 0.027f * reload));
    g_assets.Draw(model, TRS(pos, t.yaw, t.pitch - 0.6f * reload, 0.0f, 0.183f));
}

// ---------------------------------------------------------------------------
// Bodensteine (Optionen → Grafik → Bodensteine)
// ---------------------------------------------------------------------------
// Viele kleine, kantige Steine auf dem Boden um den Blickpunkt, damit man zu
// Fuß nicht über eine glatte Fläche läuft. Reine Optik ohne Kollision: Auch
// die größten sind flacher als TROOPER_STEP.
//   - Raster aus Zellen von 1 m. Ob in einer Zelle ein Stein liegt, wo und
//     wie groß, folgt aus einer Hash-Zahl der Zellennummer (StoneHash) und
//     daraus weitergezählten Zufallszahlen (StoneRand). Es wird nichts
//     gespeichert; dieselbe Stelle hat immer dieselben Steine.
//   - Meist kleine Steine (3–5 cm Halbmesser), selten bis 18 cm. Ein Stein
//     wird nur gezeichnet, solange sein Halbmesser über 0,2 % des Abstands zur
//     Kamera liegt (weiter weg wäre er nur ein Bildpunkt). An dieser Grenze
//     und am Rand des Kreises wächst er langsam aus dem Boden, damit beim
//     Laufen nichts aufpoppt.
//   - Keine Steine in der Grundfläche von Deko (außer Kratern und Rovern),
//     Stationsteilen und Startplätzen, sonst stächen sie durch Bodenplatten.
//   - Form: Fünfeck am Boden, darüber ein kleineres, gedrehtes Fünfeck und eine
//     Spitze, alles leicht verwackelt: 15 flache Dreiecke (Low-Poly wie die
//     Kenney-Modelle), beleuchtet wie die Modelle (Assets::BeginLit).
struct GroundStone {
    float x = 0.0f, z = 0.0f;
    float r = 0.0f;          // Halbmesser; 0 = kein Stein in dieser Zelle
    uint32_t seed = 0;       // weitere Zufallszahlen für Form und Farbe
};

uint32_t StoneHash(int x, int z) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(z) * 0xd8163841u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h | 1u;   // nie 0, sonst bliebe StoneRand bei 0 stehen
}

// Nächste Zufallszahl 0..1 (Xorshift); zählt seed weiter.
float StoneRand(uint32_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<float>(seed & 0xFFFFFFu) / 16777216.0f;
}

// Ein Dreieck mit Normale nach außen (weg von mid, der Mitte des Steins):
// Liegen die Ecken andersherum, werden zwei getauscht, damit die Vorderseite
// außen ist (Backface-Culling).
void StoneFace(Vector3 a, Vector3 b, Vector3 c, Vector3 mid) {
    Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
    const Vector3 g = Vector3Scale(Vector3Add(Vector3Add(a, b), c), 1.0f / 3.0f);
    if (Vector3DotProduct(n, Vector3Subtract(g, mid)) < 0.0f) {
        std::swap(b, c);
        n = Vector3Negate(n);
    }
    rlNormal3f(n.x, n.y, n.z);
    rlVertex3f(a.x, a.y, a.z);
    rlVertex3f(b.x, b.y, b.z);
    rlVertex3f(c.x, c.y, c.z);
}

// center = Mitte des Kreises (Blickpunkt), radius in Metern.
void DrawGroundStones(const World& w, Vector3 center, Vector3 camPos, float radius) {
    constexpr float CELL = 1.0f, DENSITY = 0.6f, LOD = 0.002f, EDGE = 4.0f;
    const int x0 = static_cast<int>(floorf((center.x - radius) / CELL));
    const int z0 = static_cast<int>(floorf((center.z - radius) / CELL));
    const int n = static_cast<int>(2.0f * radius / CELL) + 2;
    static std::vector<GroundStone> grid;   // nur wiederverwendeter Speicher
    grid.assign(static_cast<size_t>(n * n), GroundStone{});
    for (int iz = 0; iz < n; ++iz) {
        for (int ix = 0; ix < n; ++ix) {
            uint32_t seed = StoneHash(x0 + ix, z0 + iz);
            if (StoneRand(seed) > DENSITY) continue;
            const float x = (x0 + ix + StoneRand(seed)) * CELL, z = (z0 + iz + StoneRand(seed)) * CELL;
            const float size = StoneRand(seed);
            float r = 0.03f + 0.15f * size * size * size;
            const float d = Dist2D({x, 0.0f, z}, center);
            const float minR = LOD * Vector3Distance({x, 0.0f, z}, camPos);
            if (d > radius || r <= minR) continue;
            r *= std::min(std::clamp((radius - d) / EDGE, 0.0f, 1.0f), std::clamp((r - minR) / (0.3f * r), 0.0f, 1.0f));
            grid[static_cast<size_t>(iz * n + ix)] = GroundStone{x, z, r, seed};
        }
    }

    // Grundflächen freiräumen: Stein in Modellkoordinaten umrechnen (Drehung
    // zurück, durch scale teilen) und gegen die Grenzen aus ModelBounds prüfen.
    // Teile, die erst 0,3 m über dem Boden anfangen, dürfen Steine unter sich haben.
    auto clear = [&](int model, Vector3 pos, float yaw, float scale) {
        const BoundingBox b = ModelBounds(model);
        if (pos.y + b.min.y * scale > 0.3f) return;
        const float ex = std::max(fabsf(b.min.x), fabsf(b.max.x)), ez = std::max(fabsf(b.min.z), fabsf(b.max.z));
        const float reach = scale * sqrtf(ex * ex + ez * ez) + 0.2f;
        if (Dist2D(pos, center) > radius + reach) return;
        const float c = cosf(yaw), sn = sinf(yaw);
        const int ax = std::max(0, static_cast<int>(floorf((pos.x - reach) / CELL)) - x0);
        const int bx = std::min(n - 1, static_cast<int>(floorf((pos.x + reach) / CELL)) - x0);
        const int az = std::max(0, static_cast<int>(floorf((pos.z - reach) / CELL)) - z0);
        const int bz = std::min(n - 1, static_cast<int>(floorf((pos.z + reach) / CELL)) - z0);
        for (int iz = az; iz <= bz; ++iz) {
            for (int ix = ax; ix <= bx; ++ix) {
                GroundStone& st = grid[static_cast<size_t>(iz * n + ix)];
                if (st.r <= 0.0f) continue;
                const float dx = st.x - pos.x, dz = st.z - pos.z, m = st.r / scale;
                const float lx = (c * dx - sn * dz) / scale, lz = (sn * dx + c * dz) / scale;
                if (lx > b.min.x - m && lx < b.max.x + m && lz > b.min.z - m && lz < b.max.z + m) st.r = 0.0f;
            }
        }
    };
    static const int rover = g_assets.Id("rover"), craterA = g_assets.Id("crater"), craterB = g_assets.Id("craterLarge"),
                     padPlatform = g_assets.Id("platform_large");
    for (const Deco& d : w.decos)
        if (d.model != rover && d.model != craterA && d.model != craterB) clear(d.model, d.pos, d.yaw, d.scale);
    const int stage = w.StationStage();
    for (const Station& st : w.stations) {
        if (Dist2D(st.pos, center) > radius + 40.0f) continue;
        const int stStage = st.type == StationType::Trade ? stage : 0;
        for (const StationPart& p : st.parts)
            if (p.stage <= stStage) clear(p.model, Vector3Add(st.pos, p.offset), p.yaw, p.scale);
    }
    for (const RocketPad& pad : w.pads) clear(padPlatform, pad.pos, 0.0f, PAD_SCALE);

    // Farben: zwischen dunklem und hellem Gestein der Mondfarbe (wie MoonTone
    // in Assets.cpp), damit sich die Steine vom Boden abheben.
    Color dark{104, 106, 116, 255}, light{170, 172, 182, 255};
    if (g_assets.GetMoonColor() == MoonColor::Blue) {
        dark = Color{72, 88, 132, 255};
        light = Color{128, 148, 200, 255};
    } else if (g_assets.GetMoonColor() == MoonColor::Orange) {
        dark = Color{150, 82, 58, 255};
        light = Color{214, 124, 92, 255};
    }
    g_assets.BeginLit();
    rlBegin(RL_TRIANGLES);
    for (const GroundStone& st : grid) {
        if (st.r <= 0.0f) continue;
        uint32_t seed = st.seed;
        const Color col = ColorLerp(dark, light, StoneRand(seed));
        rlColor4ub(col.r, col.g, col.b, 255);
        const float h = st.r * (0.45f + 0.35f * StoneRand(seed));
        const float yaw = StoneRand(seed) * 2.0f * PI;
        Vector3 bottom[5], top[5];
        for (int i = 0; i < 5; ++i) {
            const float a = yaw + i * 2.0f * PI / 5.0f + (StoneRand(seed) - 0.5f) * 0.5f;
            const float rb = st.r * (0.8f + 0.3f * StoneRand(seed));
            const float rt = st.r * (0.4f + 0.3f * StoneRand(seed));
            bottom[i] = Vector3{st.x + cosf(a) * rb, -0.02f, st.z + sinf(a) * rb};
            top[i] = Vector3{st.x + cosf(a + PI / 5.0f) * rt, h * (0.55f + 0.3f * StoneRand(seed)), st.z + sinf(a + PI / 5.0f) * rt};
        }
        const Vector3 apex{st.x + (StoneRand(seed) - 0.5f) * 0.3f * st.r, h, st.z + (StoneRand(seed) - 0.5f) * 0.3f * st.r};
        const Vector3 mid{st.x, h * 0.4f, st.z};
        for (int i = 0; i < 5; ++i) {
            const int j = (i + 1) % 5;
            StoneFace(bottom[i], bottom[j], top[i], mid);
            StoneFace(top[i], bottom[j], top[j], mid);
            StoneFace(top[i], top[j], apex, mid);
        }
    }
    rlEnd();
    g_assets.EndLit();
}

} // namespace

// Zeichnet die ganze Welt aus Sicht von cam. Muss zwischen BeginMode3D(cam)
// und EndMode3D() aufgerufen werden (siehe Dateikopf). Ändert die Welt nicht
// (const); Animationen ergeben sich aus world.time und den Animationswerten,
// die Update()/UpdateCosmetics() weiterzählen.
void World::Draw(const Camera3D& cam) const {
    // Kameraposition für das Randlicht des Modell-Shaders (Assets.cpp)
    g_assets.SetCamera(cam.position);
    // Mittelpunkt für das Culling: der Punkt, auf den die Kamera schaut
    // (eigenes Schiff, im Menü die Station, als Zuschauer der Kartenausschnitt).
    const Vector3 focus = cam.target;
    const float cullR = drawRange;
    // Eigenes Schiff und eigener Spielerzustand. Im Mehrspieler sind das die
    // Platzhalter, die NetClient füllt; ohne Schiff (Zuschauer, nach einem
    // Absturz) ist das Schiff hidden bzw. dead und wird nicht gezeichnet.
    const Ship& player = Player();
    const PlayerState& me = Me();
    const bool orangeMoon = g_assets.GetMoonColor() == MoonColor::Orange;

    // --- Grafikeffekte dieses Bildes (Optionen → Grafik, Effects.hpp) ------------
    // Sandsturm nur mit den Kameraeffekten (nicht im Karteneditor) und nicht in
    // der Übersicht des Zuschauers (dort wäre die ganze Karte im Staub).
    const float sand = tiltshift::Active() && cullR < 200.0f ? g_clientConfig.sandstorm : 0.0f;
    fx::SetFog(focus, sand, time);
    // Treffer-Aufblitzen: Jede Explosion leuchtet kurz als Punktlicht (die
    // vier neuesten in Sichtweite), das Schiffe, Boden und Stationen in der
    // Nähe anstrahlt. Helligkeit k² fällt in 0,45 s von 1 auf 0. Explodiert
    // etwas nahe am Blickpunkt (unter 30 m), blitzt das ganze Bild kurz auf.
    const float flashK = g_clientConfig.hitFlash;
    Vector4 lightPos[4];
    Vector3 lightCol[4];
    int lights = 0;
    float screenFlash = 0.0f;
    if (flashK > 0.0f && cullR < 200.0f) {
        for (size_t i = flashes.size(); i-- > 0 && lights < 4;) {
            const Flash& f = flashes[i];
            const float d = Dist2D(f.pos, focus);
            if (d > cullR + 20.0f) continue;
            const float k = f.life / f.maxLife, bright = k * k * flashK;
            lightPos[lights] = Vector4{f.pos.x, f.pos.y + 1.5f, f.pos.z, 16.0f * f.size};
            lightCol[lights] = Vector3{3.0f * bright, 2.15f * bright, 1.25f * bright};
            ++lights;
            screenFlash = std::max(screenFlash, 0.35f * bright * std::clamp(1.0f - d / 30.0f, 0.0f, 1.0f));
        }
    }
    g_assets.SetFlashes(lightPos, lightCol, lights);
    fx::SetScreenFlash(screenFlash);
    // Schattenkarte: die bewegten Objekte zuerst aus Sicht der Sonne zeichnen
    // (nur ihre Tiefe zählt, siehe Effects.hpp). Ist die Option aus, gibt es
    // stattdessen weiter unten die einfachen runden Schatten.
    const bool shadowMap = fx::BeginShadowPass(cam, cullR);
    if (shadowMap) {
        DrawMovingModels(*this, focus, cullR, 0.0f);
        fx::EndShadowPass();
    }

    // --- Boden --------------------------------------------------------------
    // Das Modell "terrain" ist eine flache Platte von 1 m x 1 m. Zwei Platten:
    //   1. 1400 m groß als Umgebung bis zum Horizont, etwas dunkler und 5 cm
    //      tiefer (sonst Z-Fighting mit der zweiten Platte).
    //   2. das Spielfeld: 2 * WORLD_HALF + 30 = 630 m (Feld ±300 m plus 15 m
    //      Rand), heller, damit man sieht, wo das Spielfeld endet.
    // Der tint wird mit der (umgefärbten) Gesteinsfarbe multipliziert.
    // Beim orangefarbenen Mond (Originalfarbe) den Boden kaum abdunkeln
    g_assets.Draw(mdl.terrain, TRS({0, -0.05f, 0}, 0, 1400.0f), orangeMoon ? Color{150, 140, 136, 255} : Color{120, 120, 128, 255});
    g_assets.Draw(mdl.terrain, TRS({0, 0, 0}, 0, cfg::WORLD_HALF * 2.0f + 30.0f),
                  orangeMoon ? Color{225, 218, 214, 255} : Color{176, 176, 184, 255});
    // Bodensteine (Optionen → Grafik): Stufe 1 nur zu Fuß (28 m um den
    // Blickpunkt), Stufe 2 auch aus dem Schiff (70 m; aber wie der Sandsturm
    // nicht im Karteneditor und nicht in der Übersicht des Zuschauers).
    const int stoneLevel = g_clientConfig.groundStones;
    if (stoneLevel >= 1 && me.role == PlayerRole::Trooper)
        DrawGroundStones(*this, focus, cam.position, 28.0f);
    else if (stoneLevel >= 2 && tiltshift::Active() && cullR < 200.0f)
        DrawGroundStones(*this, focus, cam.position, 70.0f);

    // --- Dekoration -----------------------------------------------------------
    // Felsen, Krater usw. aus WorldGen.cpp. Große Objekte ragen weit über ihren
    // Mittelpunkt hinaus, deshalb wächst der Culling-Radius mit der Größe.
    // Rover und Tore (Kartenanimation): die Modell-IDs nur einmal nachschlagen
    // (static, siehe Geschütztürme unten). Rover stehen nur als Startplatz in
    // decos; gezeichnet werden sie weiter unten aus rovers.
    static int roverModel = -1, gateA = -1, gateB = -1;
    if (roverModel < 0) {
        roverModel = g_assets.Id("rover");
        gateA = g_assets.Id("gate_simple");
        gateB = g_assets.Id("gate_complex");
    }
    for (const auto& d : decos) {
        if (d.model == roverModel) continue;
        if (Dist2D(d.pos, focus) > cullR + d.scale * 0.6f) continue;
        if (d.model == gateA || d.model == gateB) {
            // Tor: leuchtet beim Öffnen des Wurmlochs bläulich (Effekt unten, additiv)
            const float g = GateGlow(GateTime(d, time));
            g_assets.Draw(d.model, d.pos, d.yaw, d.scale, WHITE, Vector3{0.10f * g, 0.25f * g, 0.55f * g});
        } else if (d.yScale > 0.0f) {
            // Flache, große Krater: in der Höhe anders skaliert als in der
            // Breite. TRS kann nur gleichmäßig skalieren, deshalb wird die
            // Matrix hier von Hand gebaut (gleiche Reihenfolge: skalieren,
            // drehen, verschieben). Leicht abgedunkelt, damit sie nicht grell
            // wirken.
            Matrix m = MatrixMultiply(MatrixScale(d.scale, d.yScale, d.scale), MatrixRotateY(d.yaw));
            m = MatrixMultiply(m, MatrixTranslate(d.pos.x, d.pos.y, d.pos.z));
            g_assets.Draw(d.model, m, Color{225, 225, 232, 255});
        } else {
            g_assets.Draw(d.model, d.pos, d.yaw, d.scale);
        }
    }

    // --- Monorail zwischen den Stationen (Kartenanimation, MapAnim.cpp) -----------
    // Gerade Schienen aus Rasterfeld-langen Stücken, mittig zwischen den
    // Enden. Das Kit-Stück liegt längs der z-Achse; mit yaw = atan2(dx, dz)
    // zeigt es in Streckenrichtung (Drehung wie MapRotate). Die Züge zeichnet
    // DrawMovingModels.
    static int railPiece = -1;
    if (railPiece < 0) railPiece = g_assets.Id("monorail_trackStraight");
    for (const StationRail& r : rails) {
        Vector3 dir = Vector3Subtract(r.b, r.a);
        dir.y = 0.0f;
        const float len = Vector3Length(dir);
        dir = Vector3Scale(dir, 1.0f / len);
        const int pieces = static_cast<int>(len / mapgrid::CELL);
        const float start = (len - pieces * mapgrid::CELL) * 0.5f;
        const float yaw = atan2f(dir.x, dir.z);
        for (int k = 0; k < pieces; ++k) {
            const Vector3 p = Vector3Add(r.a, Vector3Scale(dir, start + (k + 0.5f) * mapgrid::CELL));
            if (Dist2D(p, focus) > cullR + 3.0f) continue;
            g_assets.Draw(railPiece, p, yaw, mapgrid::CELL);
        }
    }

    // --- Raketenstartplätze (Kartenanimation) ---------------------------------------
    // Plattform mit Gerüst daneben; die Rakete darauf zeichnet DrawMovingModels.
    static int padPlatform = -1, padTower = -1, padTowerTop = -1;
    if (padPlatform < 0) {
        padPlatform = g_assets.Id("platform_large");
        padTower = g_assets.Id("structure_detailed");
        padTowerTop = g_assets.Id("structure");
    }
    for (const RocketPad& pad : pads) {
        if (Dist2D(pad.pos, focus) > cullR + 10.0f) continue;
        g_assets.Draw(padPlatform, pad.pos, 0.0f, PAD_SCALE);
        const Vector3 tower = Vector3Add(pad.pos, Vector3Scale({-pad.out.z, 0.0f, pad.out.x}, 1.5f * PAD_SCALE));
        const float towerYaw = atan2f(pad.out.x, pad.out.z);
        g_assets.Draw(padTower, tower, towerYaw, PAD_SCALE);
        g_assets.Draw(padTowerTop, Vector3Add(tower, {0, 1.0f * PAD_SCALE, 0}), towerYaw, PAD_SCALE);
        g_assets.Draw(padTowerTop, Vector3Add(tower, {0, 2.0f * PAD_SCALE, 0}), towerYaw, PAD_SCALE);
    }

    // --- Stationen ------------------------------------------------------------
    // Eine Station besteht aus vielen Kit-Teilen (parts) und Geschütztürmen.
    // Ihre Teile reichen weit um den Mittelpunkt herum, daher +30 m beim
    // Culling.
    for (const Station& st : stations) {
        if (Dist2D(st.pos, focus) > cullR + 30.0f) continue;
        // Handelsstationen wachsen mit der insgesamt abgelieferten Menge
        // (StationStage aus totalDelivered, Schwellen in Config.hpp; im
        // Mehrspieler kommt totalDelivered vom Server). Werften wachsen nicht:
        // von ihnen werden nur Teile der Stufe 0 gezeichnet.
        const bool trade = st.type == StationType::Trade;
        int stage = trade ? StationStage() : 0;
        // Raumschiffsverkäufer: alle Teile lime-grün eingefärbt
        const Color tint = st.type == StationType::Dealer ? Color{170, 255, 90, 255} : WHITE;
        for (const auto& p : st.parts) {
            if (p.stage > stage) continue;
            float scale = p.scale;
            Vector3 off = p.offset;
            // Teile der gerade erreichten Ausbaustufe "wachsen" aus dem Boden.
            // stageAnim fällt nach dem Ausbau von 1 auf 0 (0.6 pro Sekunde,
            // also ca. 1,7 s). Mit dem Faktor 1.4 bleibt t in den ersten
            // ca. 0,5 s bei 0 (kurze Pause nach dem Ausbau-Ton), dann läuft t
            // in ca. 1,2 s auf 1. EaseOutBack lässt die Teile dabei kurz
            // überschwingen. Größe und Höhe wachsen gemeinsam, damit Teile, die
            // auf anderen sitzen, mit nach oben kommen.
            // Mindestens 0.001 statt 0: Eine Matrix mit Maßstab 0 lässt sich
            // nicht umkehren, raylib braucht die Umkehrung für die Normalen.
            if (trade && p.stage == lastStage && stageAnim > 0.0f && p.stage > 0) {
                float t = std::clamp(1.0f - stageAnim * 1.4f, 0.0f, 1.0f);
                float k = EaseOutBack(t);
                scale *= std::max(0.001f, k);
                off.y *= std::max(0.0f, k);
            }
            Vector3 wp = Vector3Add(st.pos, off);
            g_assets.Draw(p.model, wp, p.yaw, scale, tint);
        }
        // Modell-IDs der Geschütztürme nur einmal nachschlagen (static). Das
        // ist sicher, weil sich eine Modell-ID während des Programmlaufs nie
        // ändert (shared/Models.hpp), auch nicht bei einem Neuaufbau der Welt.
        static int turretDouble = -1, turretSingle = -1;
        if (turretDouble < 0) {
            turretDouble = g_assets.Id("turret_double");
            turretSingle = g_assets.Id("turret_single");
        }
        // Geschütztürme erscheinen wie die Teile erst ab ihrer Ausbaustufe.
        // Maßstab 2.2: Die Turm-Modelle des Kits sind im Vergleich zu den
        // Stationsteilen klein.
        for (const auto& tu : st.turrets) {
            if (tu.stage > stage) continue;
            Vector3 wp = Vector3Add(st.pos, tu.offset);
            g_assets.Draw(tu.isDouble ? turretDouble : turretSingle, wp, tu.yaw, 2.2f);
        }
    }

    // --- Lagerstätten -----------------------------------------------------------
    // Nur entdeckte Lagerstätten sind sichtbar. Einzelspieler: der eigene
    // Scanner setzt revealed. Mehrspieler: Der Server schickt nur die
    // Lagerstätten, die dieser Spieler entdeckt hat; NetClient setzt dafür
    // revealed = true.
    for (const auto& d : deposits) {
        if (!d.revealed) continue;
        if (Dist2D(d.pos, focus) > cullR) continue;
        // Pulsieren 0..1; d.id als Phasenversatz, damit nicht alle Kristalle
        // im gleichen Takt leuchten.
        float pulse = 0.5f + 0.5f * sinf(time * 3.0f + d.id);
        // Einblenden: revealT läuft nach der Entdeckung in 0,5 s von 0 auf 1
        // (World::Update/UpdateCosmetics), EaseOutBack lässt den Kristall
        // aus dem Boden ploppen.
        float appear = EaseOutBack(std::clamp(d.revealT, 0.0f, 1.0f));
        // Beim Abbauen schrumpft die Lagerstätte von 100 % auf 55 % ihrer
        // Größe: man sieht den Fortschritt, sie verschwindet aber nicht, bevor
        // sie leer ist.
        float fill = 0.55f + 0.45f * static_cast<float>(d.amount) / std::max(1, d.initial);
        // emissive: grünliches Eigenleuchten im Takt von pulse; nie ganz aus,
        // damit die Kristalle auch im dunklen Moment noch glimmen. Gelbe
        // Kristalle (Probe nötig) mit gelbem Modell und gelblichem Leuchten.
        if (d.sample == Sample::Yellow)
            g_assets.Draw(YellowCrystal(*this, d.model), d.pos, d.yaw, d.scale * appear * fill, WHITE,
                          Vector3{0.08f + 0.22f * pulse, 0.07f + 0.17f * pulse, 0.02f});
        else
            g_assets.Draw(d.model, d.pos, d.yaw, d.scale * appear * fill, WHITE,
                          Vector3{0.06f, 0.08f + 0.26f * pulse, 0.05f + 0.16f * pulse});
    }

    // --- Wracks ------------------------------------------------------------------
    // Schief im Boden liegend (tilt als Nick- und, etwas schwächer, als
    // Rollwinkel) und dunkelgrau wie ausgebrannt.
    for (const auto& w : wrecks) {
        if (Dist2D(w.pos, focus) > cullR) continue;
        g_assets.Draw(w.model, TRS(w.pos, w.yaw, w.tilt, w.tilt * 0.7f, 1.0f), Color{95, 92, 92, 255});
    }

    // --- Züge, Raketen, Rover, Personen, Figuren und Schiffe -----------------------------------
    DrawMovingModels(*this, focus, cullR, flashK);

    // ===== Ab hier transparente / leuchtende Effekte ====================================
    // raylib sammelt einfache Formen (DrawCylinder, DrawCube, rlBegin/rlVertex)
    // in einem Puffer, dem "Render-Batch", und schickt sie gesammelt an die
    // Grafikkarte – erst beim nächsten Leeren (z. B. EndMode3D, Wechsel des
    // Mischmodus oder rlDrawRenderBatchActive). Zustände wie Tiefenmaske oder
    // Culling wirken dagegen sofort in OpenGL. Deshalb wird der Puffer vor
    // jeder solchen Umstellung geleert, damit bisher Gesammeltes noch mit dem
    // alten Zustand gezeichnet wird.
    rlDrawRenderBatchActive();
    // Backface-Culling aus: Normalerweise lässt OpenGL Dreiecke weg, deren
    // Rückseite zur Kamera zeigt (spart Zeit bei geschlossenen Modellen). Die
    // flachen Ringe und Effekte sollen aber von beiden Seiten sichtbar sein.
    rlDisableBackfaceCulling();
    // Tiefenmaske aus: Die Effekte werden weiterhin hinter festen Objekten
    // verdeckt (Tiefentest bleibt an), schreiben aber selbst nichts in den
    // Tiefenpuffer. Sonst würde ein zuerst gezeichneter durchsichtiger Effekt
    // einen später gezeichneten dahinter komplett verdecken. So muss nicht
    // nach Entfernung sortiert werden.
    rlDisableDepthMask();
    // Im Sandsturm verblassen alle folgenden Effekte mit dem Abstand zum Schiff
    fx::BeginEffects();

    // Fahrspuren der Rover (Kartenanimation)
    // Je Radseite ein schmaler, dunkler Streifen von Spurpunkt zu Spurpunkt
    // (MapAnim.cpp legt alle 0,6 m einen an), 5 cm über dem Boden und damit
    // unter den Schatten. Die Räder liegen im Kit bei x = +-0,125 und sind
    // 0,05 breit. Jeder Punkt verblasst
    // mit seinem Alter und ist nach der in den Optionen eingestellten Zeit
    // (g_clientConfig.trackTime, 0 = aus) weg. Die Schleife läuft vom neuesten
    // Punkt rückwärts und hört beim ersten zu alten auf. Erster Abschnitt:
    // vom Rover selbst zum neuesten Punkt. In der Übersicht (Zuschauer, ganze
    // Karte) wären die Streifen nur Bruchteile eines Pixels: dort keine Spuren.
    const float trackLife = static_cast<float>(g_clientConfig.trackTime);
    if (trackLife > 0.0f && cullR < 200.0f) {
        auto alphaAt = [&](float t) {
            return static_cast<unsigned char>(95.0f * std::clamp(1.0f - (time - t) / trackLife, 0.0f, 1.0f));
        };
        for (const Rover& r : rovers) {
            if (r.track.empty()) continue;
            const float off = 0.125f * r.scale, half = 0.028f * r.scale, y = 0.05f;
            const Vector3 f = ForwardFromYaw(r.yaw);
            TrackPoint prev{r.pos.x, r.pos.z, -f.z, f.x, time};
            rlBegin(RL_TRIANGLES);
            for (size_t i = r.track.size(); i-- > 0;) {
                const TrackPoint& q = r.track[i];
                if (time - q.t >= trackLife) break;
                const bool inView = Vector2Distance({q.x, q.z}, {focus.x, focus.z}) < cullR;
                if (inView) {
                    const unsigned char a0 = alphaAt(prev.t), a1 = alphaAt(q.t);
                    for (int k = -1; k <= 1; k += 2) {
                        const float i0 = k * off - half, o0 = k * off + half;
                        const Vector3 p0{prev.x + prev.sx * i0, y, prev.z + prev.sz * i0};
                        const Vector3 p1{prev.x + prev.sx * o0, y, prev.z + prev.sz * o0};
                        const Vector3 q0{q.x + q.sx * i0, y, q.z + q.sz * i0};
                        const Vector3 q1{q.x + q.sx * o0, y, q.z + q.sz * o0};
                        rlColor4ub(35, 33, 32, a0); rlVertex3f(p0.x, p0.y, p0.z); rlVertex3f(p1.x, p1.y, p1.z);
                        rlColor4ub(35, 33, 32, a1); rlVertex3f(q1.x, q1.y, q1.z);
                        rlColor4ub(35, 33, 32, a0); rlVertex3f(p0.x, p0.y, p0.z);
                        rlColor4ub(35, 33, 32, a1); rlVertex3f(q1.x, q1.y, q1.z); rlVertex3f(q0.x, q0.y, q0.z);
                    }
                }
                prev = q;
            }
            rlEnd();
        }
    }

    // Schatten (nur ohne Schattenkarte, die zeichnet echte Schatten)
    // Alpha 0.16 bis 2,5 m Flughöhe, darüber schwächer, ab 12,5 m nur noch
    // 20 % davon: je höher das Schiff, desto blasser der Schatten.
    for (const auto& s : ships) {
        if (shadowMap) break;
        if (s.dead || s.hidden || Dist2D(s.pos, focus) > cullR) continue;
        float h = std::max(0.0f, s.pos.y);
        DrawShadow(s.pos, 1.1f, 0.16f * std::clamp(1.2f - h * 0.08f, 0.2f, 1.0f));
    }
    for (const Rover& r : rovers) {
        if (shadowMap) break;
        if (Dist2D(r.pos, focus) <= cullR) DrawShadow(r.pos, 0.22f * r.scale, 0.18f);
    }

    // Markierungen unter Schiffen
    // Farbcode: eigenes Schiff orange, andere Spieler grün, feindlich rot
    // (Piraten oder Händler/Miner, die man angegriffen hat), neutral blau
    // (friedliche Händler/Miner).
    if (!player.dead && !player.hidden && !player.crashing)
        DrawGroundRing(player.pos, 1.7f, 1.95f, Color{255, 170, 60, 130}, 40);
    for (const auto& s : ships) {
        // &s == &player: das eigene Schiff (ships[0]) hat oben schon seinen Ring.
        if (s.dead || s.hidden || s.crashing || &s == &player) continue;
        if (Dist2D(s.pos, focus) > cullR) continue;
        if (s.kind == ShipKind::Player)   // andere Spieler (Mehrspieler)
            DrawGroundRing(s.pos, 1.7f, 1.95f, Color{110, 240, 170, 130}, 32);
        else if (s.faction == Faction::Pirate || s.hostileToPlayer)
            DrawGroundRing(s.pos, 1.7f, 1.95f, Color{255, 70, 60, 150}, 32);
        else if (s.kind == ShipKind::Convoy)   // Frachter des Konvois (Mission)
            DrawGroundRing(s.pos, 1.7f, 1.95f, Color{150, 255, 120, 140}, 32);
        else
            DrawGroundRing(s.pos, 1.7f, 1.9f, Color{120, 200, 255, 100}, 32);
    }

    // Andockbereich der Stationen
    // Gestrichelter Kreis mit dem Radius DOCK_RANGE (in diesem Abstand kann
    // man andocken). Er erscheint ab 55 m Entfernung und ist ab 30 m voll
    // sichtbar ((55 - d) / 25). Handelsstation orange, Werft blau, Verkäufer grün. Er dreht
    // sich langsam (ein Strichabstand in ca. 6,7 s). Angedockt (hidden):
    // keine Ringe.
    for (const Station& st : stations) {
        if (player.hidden) break;
        float d = Dist2D(player.pos, st.pos);
        if (d < 55.0f) {
            float a = std::clamp((55.0f - d) / 25.0f, 0.0f, 1.0f);
            Color c = st.type == StationType::Trade      ? Color{255, 190, 80, static_cast<unsigned char>(120 * a)}
                      : st.type == StationType::Shipyard ? Color{120, 210, 255, static_cast<unsigned char>(120 * a)}
                                                         : Color{150, 255, 90, static_cast<unsigned char>(120 * a)};
            DrawDashedRing(st.pos, cfg::DOCK_RANGE, 0.35f, c, 28, time * 0.15f);
        }
    }

    // Additives Mischen: Die Farbe wird zum vorhandenen Bild addiert
    // (Bild + Farbe · Alpha), statt es zu überdecken. Überlagerungen werden
    // dadurch heller, nie dunkler – Strahlen und Funken wirken wie Licht.
    // Alpha regelt hier die Helligkeit; dunkle Farben wären fast unsichtbar.
    // (BeginBlendMode leert den Render-Batch selbst.)
    BeginBlendMode(BLEND_ADDITIVE);

    // Scanner
    // Ein Ring läuft in SCANNER_PULSE_TIME (1,25 s) vom Schiff bis
    // Scannerradius des Schiffstyps (World::ScannerRadius) nach außen und wird dabei blasser (Alpha 200 -> 60).
    // World::Update deckt Lagerstätten mit derselben Formel auf, wenn der Ring
    // sie erreicht – Bild und Spiellogik passen also zusammen. Im Mehrspieler
    // zählt NetClient scanPulse lokal weiter. Der zweite, feste Ring zeigt
    // die Reichweite des Scanners.
    if (me.scanOn && !player.crashing && !player.hidden) {
        float f = fmodf(me.scanPulse, cfg::SCANNER_PULSE_TIME) / cfg::SCANNER_PULSE_TIME;
        const float radius = ScannerRadius(me);
        float r = radius * f;
        unsigned char a = static_cast<unsigned char>(200 * (1.0f - f * 0.7f));
        DrawGroundRing(player.pos, std::max(0.0f, r - 0.9f), r, Color{60, 255, 180, a}, 96);
        DrawGroundRing(player.pos, radius - 0.25f, radius, Color{60, 220, 160, 90}, 96);
    }

    // Leuchtmarkierungen für entdeckte Lagerstätten und Wracks
    // Lichtsäule (9 m bzw. 8 m hoch) und Bodenring, pulsierend mit der id als
    // Phasenversatz. Von oben weithin sichtbar, damit man Fundstellen schnell
    // wiederfindet. Grün = Kristalle, gelb = gelbe Kristalle (Probe nötig),
    // orange = Wrack (Schrott).
    for (const auto& d : deposits) {
        if (!d.revealed || Dist2D(d.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 2.5f + d.id);
        const bool yellow = d.sample == Sample::Yellow;
        DrawBeam(d.pos, 9.0f, 0.35f, yellow ? Color{220, 190, 40, static_cast<unsigned char>(70 * pulse)}
                                            : Color{40, 200, 120, static_cast<unsigned char>(70 * pulse)});
        DrawGroundRing(d.pos, 3.2f, 3.6f, yellow ? Color{255, 225, 80, static_cast<unsigned char>(120 * pulse)}
                                                 : Color{60, 255, 170, static_cast<unsigned char>(120 * pulse)}, 40);
    }
    for (const auto& w : wrecks) {
        if (Dist2D(w.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 3.0f + w.id);
        DrawBeam(w.pos, 8.0f, 0.3f, Color{220, 140, 40, static_cast<unsigned char>(70 * pulse)});
        DrawGroundRing(w.pos, 3.0f, 3.4f, Color{255, 170, 60, static_cast<unsigned char>(120 * pulse)}, 40);
    }
    // Personen: gelbe Lichtsäule und Bodenring (gelb wie auf der Minikarte)
    for (const auto& pe : persons) {
        if (pe.owner != me.id || Dist2D(pe.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 4.0f + static_cast<float>(pe.id));
        DrawBeam(pe.pos, 10.0f, 0.3f, Color{230, 200, 40, static_cast<unsigned char>(80 * pulse)});
        DrawGroundRing(pe.pos, 2.4f, 2.8f, Color{255, 225, 60, static_cast<unsigned char>(140 * pulse)}, 40);
    }

    // Kristalle glimmen: weicher grüner Schein auf dem Boden und kleine
    // Lichtfunken (drei gekreuzte, dünne Quader), die an wechselnden Stellen
    // der Kristalle kurz aufblitzen. Jeder der vier Funken hat seinen eigenen
    // Takt (1,6..2,4 s) und leuchtet davon die ersten 18 %; die Stelle ergibt
    // sich aus Lagerstätte, Funke und Takt (Hash01). Größe und Form folgen
    // dem Kristall (wie oben beim Zeichnen: einblenden, beim Abbau kleiner);
    // die Funken sitzen zur Mitte hin höher, wie die Spitzen einer Kristallgruppe.
    for (const auto& d : deposits) {
        if (!d.revealed || Dist2D(d.pos, focus) > cullR) continue;
        const float pulse = 0.5f + 0.5f * sinf(time * 3.0f + d.id);
        const float fill = 0.55f + 0.45f * static_cast<float>(d.amount) / std::max(1, d.initial);
        const float sc = d.scale * std::clamp(d.revealT, 0.0f, 1.0f) * fill;
        const bool yellow = d.sample == Sample::Yellow;
        DrawGlowDisc({d.pos.x, 0.1f, d.pos.z}, 1.5f * sc, yellow ? Color{255, 220, 80, static_cast<unsigned char>(45 + 40 * pulse)}
                                                                 : Color{60, 255, 170, static_cast<unsigned char>(45 + 40 * pulse)});
        const BoundingBox b = ModelBounds(d.model);
        const float rad = std::max(b.max.x, b.max.z) * sc * 0.8f, height = b.max.y * sc;
        for (int g = 0; g < 4; ++g) {
            const float period = 1.6f + 0.27f * g;
            const float cyc = (time + d.id * 0.37f + g * 0.61f) / period;
            const float n = floorf(cyc), f = cyc - n;
            if (f > 0.18f) continue;
            const float k = sinf(PI * f / 0.18f);
            const float seed = n * 7.13f + d.id * 3.31f + g * 1.73f;
            const float ang = Hash01(seed) * 2.0f * PI, out = Hash01(seed + 0.5f);
            const Vector3 p{d.pos.x + cosf(ang) * rad * out, height * (1.05f - 0.6f * out), d.pos.z + sinf(ang) * rad * out};
            const float len = 0.75f * k, w = 0.07f;
            const Color c = yellow ? Color{255, 245, 200, static_cast<unsigned char>(255 * k)}
                                   : Color{210, 255, 235, static_cast<unsigned char>(255 * k)};
            DrawCube(p, len, w, w, c);
            DrawCube(p, w, len, w, c);
            DrawCube(p, w, w, len, c);
        }
    }

    // Tore: das "Wurmloch" (Kartenanimation). Der Ring der Kit-Modelle
    // gate_simple/gate_complex hat seine Mitte 0,585 über dem Boden und innen
    // etwa 0,3 Radius (Kit-Maße, aus dem Modell abgelesen); er steht in der
    // x-y-Ebene des Modells, die Fläche zeigt also nach normal (local +z).
    // Ablauf (u = Sekunden seit dem Öffnen): Ring leuchtet auf, ab 0,25 s
    // wächst die Fläche, 0,45..1,5 s schießt ein heller Kegel nach vorn
    // heraus, dann schimmert die Fläche mit nach außen laufenden Wellen,
    // ab 5 s fällt sie zur Mitte zusammen. Dazu ein blauer Schein am Boden.
    for (const auto& d : decos) {
        if ((d.model != gateA && d.model != gateB) || Dist2D(d.pos, focus) > cullR + d.scale) continue;
        const float u = GateTime(d, time);
        const float glow = GateGlow(u);
        if (glow <= 0.0f) continue;
        const float s = d.scale;
        const float open = std::clamp((u - 0.25f) / 0.35f, 0.0f, 1.0f) * std::clamp((5.8f - u) / 0.8f, 0.0f, 1.0f);
        const Vector3 c{d.pos.x, d.pos.y + 0.585f * s, d.pos.z};
        const Vector3 right{cosf(d.yaw), 0.0f, -sinf(d.yaw)};
        const Vector3 normal{sinf(d.yaw), 0.0f, cosf(d.yaw)};
        DrawGlowDisc({d.pos.x, 0.1f, d.pos.z}, 1.1f * s, Color{70, 140, 255, static_cast<unsigned char>(90 * glow)}, 32);
        const float radius = 0.3f * s * open;
        if (radius > 0.01f) {
            // Fläche aus 4 Ringen x 28 Abschnitten; Helligkeit je Eckpunkt
            const int rings = 4, segs = 28;
            auto vertex = [&](int j, int i) {
                const float rr = radius * j / rings, a = 2.0f * PI * i / segs;
                const float wave = 0.5f + 0.5f * sinf(static_cast<float>(j) * 2.3f - u * 8.0f);
                const float bright = std::clamp((0.35f + 0.45f * wave + 0.25f * (1.0f - static_cast<float>(j) / rings)) * glow, 0.0f, 1.0f);
                rlColor4ub(90, 170, 255, static_cast<unsigned char>(210 * bright));
                rlVertex3f(c.x + right.x * cosf(a) * rr, c.y + sinf(a) * rr, c.z + right.z * cosf(a) * rr);
            };
            rlBegin(RL_TRIANGLES);
            for (int j = 0; j < rings; ++j) {
                for (int i = 0; i < segs; ++i) {
                    vertex(j, i); vertex(j + 1, i); vertex(j + 1, i + 1);
                    vertex(j, i); vertex(j + 1, i + 1); vertex(j, i + 1);
                }
            }
            rlEnd();
        }
        if (u > 0.45f && u < 1.5f) {
            const float k = sinf(PI * (u - 0.45f) / 1.05f);
            const Vector3 tip = Vector3Add(c, Vector3Scale(normal, 0.8f * s * k));
            DrawCylinderEx(c, tip, 0.27f * s * open, 0.04f * s, 16, Color{150, 210, 255, static_cast<unsigned char>(170 * k)});
            DrawCylinderEx(c, Vector3Lerp(c, tip, 0.8f), 0.12f * s, 0.02f * s, 12, Color{230, 245, 255, static_cast<unsigned char>(200 * k)});
        }
    }

    // Raketenflammen: zwei Kegel unter dem Fußpunkt entgegen der Längsachse
    // der Rakete (up, um tilt nach "out" gekippt), flackernd. Länge mit dem
    // Schub; schrumpft mit der Rakete in großer Höhe.
    for (size_t i = 0; i < pads.size(); ++i) {
        const RocketPad& pad = pads[i];
        const RocketPose rp = RocketAt(pad, time);
        if (!rp.visible || rp.thrust <= 0.0f || Dist2D(rp.pos, focus) > cullR + 10.0f) continue;
        const Vector3 up{pad.out.x * sinf(rp.tilt), cosf(rp.tilt), pad.out.z * sinf(rp.tilt)};
        const float len = (2.2f + 1.6f * rp.thrust + 0.35f * sinf(time * 37.0f + i)) * rp.thrust * rp.size;
        const Vector3 base = Vector3Add(rp.pos, Vector3Scale(up, 0.2f * rp.size));
        const Vector3 tip = Vector3Subtract(rp.pos, Vector3Scale(up, len));
        DrawCylinderEx(base, tip, 0.75f * rp.size, 0.05f, 10, Color{255, 150, 50, 200});
        DrawCylinderEx(base, Vector3Lerp(base, tip, 0.6f), 0.38f * rp.size, 0.02f, 8, Color{255, 240, 190, 230});
    }

    // Abbaustrahlen
    // Vom Schiffsboden (0,3 m unter der Schiffsmitte) zum Ziel in 0,8 m Höhe.
    // Zwei Kegel übereinander: ein schmaler heller Kern und ein breiter
    // schwacher Schein; additiv addieren sie sich zu einem Leuchten. Die
    // Breite flackert schnell (20 rad/s, gut 3 Mal pro Sekunde) und wird zum
    // Ziel hin breiter.
    auto beam = [&](Vector3 from, Vector3 to) {
        float w = 0.12f + 0.05f * sinf(time * 20.0f);
        DrawCylinderEx({from.x, from.y - 0.3f, from.z}, {to.x, 0.8f, to.z}, w, w * 1.8f, 8, Color{60, 255, 170, 150});
        DrawCylinderEx({from.x, from.y - 0.3f, from.z}, {to.x, 0.8f, to.z}, w * 2.5f, w * 4.0f, 8, Color{30, 140, 90, 60});
    };
    // Eigener Strahl: Das Ziel steht im eigenen Spielerzustand (mineKind 1 =
    // Lagerstätte, 2 = Wrack, mineId = dessen id) und wird hier gesucht.
    // Im Mehrspieler hat das vorhergesagte eigene Schiff mining = false
    // (NetClient), damit der eigene Strahl nicht unten ein zweites Mal
    // gezeichnet wird.
    if (me.mining && !player.crashing && !player.hidden) {
        Vector3 t{};
        bool ok = false;
        for (const auto& d : deposits) if (me.mineKind == 1 && d.id == me.mineId) { t = d.pos; ok = true; }
        for (const auto& w : wrecks) if (me.mineKind == 2 && w.id == me.mineId) { t = w.pos; ok = true; }
        if (ok) beam(player.pos, t);
    }
    // Abbaustrahlen der anderen Schiffe (Händler und, im Mehrspieler, andere Spieler)
    // Ziel aus s.mineTarget: im Einzelspieler von der KI (Ai.cpp) gesetzt,
    // im Mehrspieler aus dem Snapshot (nur x/z, die Höhe setzt beam selbst).
    for (const auto& s : ships) {
        if (&s != &player && s.mining && !s.hidden && Dist2D(s.pos, focus) < cullR) beam(s.pos, s.mineTarget);
    }

    // Triebwerke
    // Zwei Flammenkegel am Heck jedes Schiffs (Lage siehe EnginePos).
    // Länge: 0.25 m im Leerlauf, bis zu 0.9 m mehr bei vollem Schub
    // (thrustVisual 0..1), dazu schnelles Flackern (40 rad/s, ca. 6 Mal pro
    // Sekunde) mit der Schiffs-id als Phasenversatz. Piraten rot, sonst orange.
    for (const auto& s : ships) {
        if (s.dead || s.hidden || Dist2D(s.pos, focus) > cullR) continue;
        Vector3 fwd = ForwardFromYaw(s.yaw);
        float len = 0.25f + s.thrustVisual * 0.9f + 0.08f * sinf(time * 40.0f + s.id);
        for (int k = -1; k <= 1; k += 2) {
            Vector3 base = EnginePos(s, k);
            Vector3 tip = Vector3Add(base, Vector3Scale(fwd, -len));
            Color c = s.faction == Faction::Pirate ? Color{255, 90, 70, 200} : Color{255, 170, 60, 200};
            // DrawCylinderEx(Start, Ende, Radius am Start, Radius am Ende, ...):
            // breit am Triebwerk, spitz am Ende.
            DrawCylinderEx(base, tip, 0.18f, 0.02f, 6, c);
        }
    }

    // Triebwerksschweif (Grafikoption, siehe TrailPuff oben)
    // Neue Wolken auf einem festen Zeitraster (alle kTrailStep Sekunden
    // Weltzeit) für die Zeit seit dem letzten Bild: So entstehen unabhängig
    // von der Bildrate gleich viele, und in der Pause (Weltzeit steht) bleibt
    // der Schweif stehen. Eine Wolke, die vor "ago" Sekunden entstand, kommt
    // an die Stelle, an der das Triebwerk damals war (pos - vel * ago).
    // Ohne Schub (Leerlauf, langsam) entsteht nichts. Zufall für die Drift aus
    // Hash01 (Schiff, Zeitpunkt, Triebwerk), damit nichts gespeichert werden muss.
    // Eine neue Welt (oder ein Zeitsprung) beginnt ohne alte Wolken.
    const float trailLife = kTrailLife[std::clamp(g_clientConfig.trail, 0, 3)];
    if (g_trail.world != this || time < g_trail.last || time - g_trail.last > 1.0f) {
        g_trail.puffs.clear();
        g_trail.world = this;
        g_trail.last = time;
    }
    if (trailLife > 0.0f && cullR < 200.0f) {
        const long first = static_cast<long>(floorf(g_trail.last / kTrailStep)) + 1;
        const long newest = static_cast<long>(floorf(time / kTrailStep));
        for (const auto& s : ships) {
            if (s.dead || s.hidden || s.crashing || Dist2D(s.pos, focus) > cullR) continue;
            const float speed = sqrtf(s.vel.x * s.vel.x + s.vel.z * s.vel.z);
            const float power = std::max(s.thrustVisual, std::min(1.0f, speed / 12.0f));
            if (power < 0.08f) continue;
            const Vector3 fwd = ForwardFromYaw(s.yaw);
            const Vector3 side{fwd.z, 0, -fwd.x};
            // höchstens 8 Zeitpunkte je Bild (bei sehr niedriger Bildrate)
            for (long t = std::max(first, newest - 7); t <= newest; ++t) {
                const float ago = time - t * kTrailStep;
                for (int k = -1; k <= 1; k += 2) {
                    const float seed = static_cast<float>(t % 100000) * 0.731f + s.id * 5.17f + k * 1.9f;
                    TrailPuff p;
                    p.pos = Vector3Subtract(Vector3Add(EnginePos(s, k), Vector3Scale(fwd, -(0.3f + 0.5f * s.thrustVisual))),
                                            Vector3Scale(s.vel, ago));
                    p.vel = Vector3Add(Vector3Scale(fwd, -(1.2f + 1.0f * Hash01(seed))), Vector3Scale(side, (Hash01(seed + 0.3f) - 0.5f) * 0.8f));
                    p.vel.y = (Hash01(seed + 0.6f) - 0.3f) * 0.6f;
                    p.vel = Vector3Add(p.vel, Vector3Scale(s.vel, 0.15f));
                    p.born = t * kTrailStep;
                    p.power = power;
                    p.pirate = s.faction == Faction::Pirate;
                    g_trail.puffs.push_back(p);
                }
            }
        }
    }
    g_trail.last = time;
    g_trail.puffs.erase(std::remove_if(g_trail.puffs.begin(), g_trail.puffs.end(),
                                       [&](const TrailPuff& p) { return time - p.born >= trailLife; }),
                        g_trail.puffs.end());
    // Zeichnen: Würfel wie die Partikel. Neu heiß gelblich-weiß, dann in der
    // Farbe der Flamme (Piraten rot, sonst orange); dabei kleiner und
    // durchsichtiger. Die Drift wird zum Ende hin langsamer (Faktor 1 - age/2life).
    for (const TrailPuff& p : g_trail.puffs) {
        const float age = time - p.born;
        const float f = 1.0f - age / trailLife;   // 1 = neu, 0 = gleich weg
        const Vector3 pos = Vector3Add(p.pos, Vector3Scale(p.vel, age * (1.0f - 0.5f * age / trailLife)));
        if (Dist2D(pos, focus) > cullR) continue;
        const float size = (0.07f + 0.17f * f) * (0.6f + 0.4f * p.power);
        const Color hot{255, 240, 200, 255};
        const Color flame = p.pirate ? Color{255, 90, 70, 255} : Color{255, 160, 70, 255};
        const float hk = std::clamp((f - 0.55f) / 0.45f, 0.0f, 1.0f);
        const Color c{static_cast<unsigned char>(flame.r + (hot.r - flame.r) * hk), static_cast<unsigned char>(flame.g + (hot.g - flame.g) * hk),
                      static_cast<unsigned char>(flame.b + (hot.b - flame.b) * hk),
                      static_cast<unsigned char>(210.0f * f * f * (0.4f + 0.6f * p.power))};
        DrawCube(pos, size, size, size, c);
    }

    // Geschosse
    // Leuchtspur: 1,6 m lang entgegen der Flugrichtung hinter dem Geschoss,
    // vorn dicker. Darüber ein breiterer Schein mit 35 % Alpha.
    // Farben: Geschütztürme (ownerId -1) hellblau, Spieler gold, Piraten rot,
    // andere (Händler, die sich wehren) blassgelb.
    // Geschosse der Figuren (ground) sind dünner und höchstens 0,9 m lang;
    // die Spur wächst erst aus der Waffe heraus (sonst reichte sie in der
    // Ego-Ansicht bis hinter die Kamera).
    // Im Mehrspieler kommen die Geschosse aus dem Snapshot (die nächsten 128)
    // und werden vom Client zwischen den Snapshots weiterbewegt.
    for (const auto& b : bullets) {
        if (Dist2D(b.pos, focus) > cullR) continue;
        Color c;
        if (b.ownerId == -1 && !b.ground) c = Color{110, 220, 255, 255};
        else if (b.faction == Faction::Player) c = Color{255, 210, 90, 255};
        else if (b.faction == Faction::Pirate) c = Color{255, 70, 60, 255};
        else c = Color{255, 240, 150, 255};
        Vector3 dir = Vector3Normalize(b.vel);
        if (b.ground) {
            const float len = std::min(0.9f, (cfg::TROOPER_BULLET_LIFE - b.life) * cfg::TROOPER_BULLET_SPEED);
            const Vector3 tail = Vector3Subtract(b.pos, Vector3Scale(dir, len));
            DrawCylinderEx(tail, b.pos, 0.012f, 0.035f, 6, c);
            DrawCylinderEx(tail, b.pos, 0.04f, 0.08f, 6, Fade(c, 0.35f));
            continue;
        }
        Vector3 tail = Vector3Subtract(b.pos, Vector3Scale(dir, 1.6f));
        DrawCylinderEx(tail, b.pos, 0.06f, 0.14f, 6, c);
        DrawCylinderEx(tail, b.pos, 0.18f, 0.3f, 6, Fade(c, 0.35f));
    }

    // Partikel
    // Würfel sind die billigste 3D-Form und passen zum kantigen Stil des Kits.
    // f = verbleibende Lebenszeit (1 = neu, 0 = gleich weg).
    // Partikel werden nicht nach Entfernung aussortiert: Es gibt sie nur kurz
    // und nur dort, wo gerade etwas passiert. Einzelspieler: World erzeugt sie
    // selbst. Mehrspieler: Der Client erzeugt sie u. a. aus den
    // Effekt-Ereignissen des Servers (World::Explosion / World::SpawnSparks).
    // Leuchtende Partikel (Funken, Feuer, Blitz) additiv: schrumpfen auf 40 %
    // und verblassen.
    for (const auto& p : particles) {
        if (!p.glow) continue;
        float f = p.life / p.maxLife;
        float s = p.size * (0.4f + 0.6f * f);
        DrawCube(p.pos, s, s, s, Fade(p.color, f));
    }

    // Explosionsblitze (Treffer-Aufblitzen, Stärke aus den Optionen): ein
    // heller Lichtball, der sich in 0,45 s ausdehnt und verblasst, mit
    // weißem Kern, dazu ein warmer Lichtschein am Boden. Das Punktlicht auf
    // den Modellen kommt aus dem Beleuchtungs-Shader (siehe oben, SetFlashes).
    if (flashK > 0.0f) {
        for (const Flash& f : flashes) {
            if (Dist2D(f.pos, focus) > cullR + 10.0f) continue;
            const float k = f.life / f.maxLife, bright = k * k * flashK;
            const float r = (1.4f + 2.4f * (1.0f - k)) * f.size;
            DrawSphereEx(f.pos, r, 8, 12, Color{255, 200, 130, static_cast<unsigned char>(150 * bright)});
            DrawSphereEx(f.pos, r * 0.45f, 6, 10, Color{255, 250, 235, static_cast<unsigned char>(255 * bright)});
            DrawGlowDisc({f.pos.x, 0.1f, f.pos.z}, 9.0f * f.size, Color{255, 170, 90, static_cast<unsigned char>(170 * bright)}, 32);
        }
    }
    // Zurück zum normalen Mischen (Alpha-Blending): Rauch und Trümmer sollen
    // auch abdunkeln können, was additiv nicht geht.
    EndBlendMode();
    // Trümmer (gravity > 0, fallen herunter) behalten ihre Größe; Rauch dehnt
    // sich beim Verblassen auf 160 % aus. Das erste Drittel der Lebenszeit
    // voll deckend (f * 1.5 >= 1), danach ausblenden.
    for (const auto& p : particles) {
        if (p.glow) continue;
        float f = p.life / p.maxLife;
        float s = p.size * (p.gravity > 0 ? 1.0f : (1.6f - f * 0.6f));
        DrawCube(p.pos, s, s, s, Fade(p.color, std::min(1.0f, f * 1.5f)));
    }

    // Rauch der Raketen: Wolken, die alle 0,1 s am Startplatz entstehen,
    // solange das Triebwerk läuft und die Rakete tiefer als 14 m ist (beim
    // Start und kurz vor der Landung). Statt sie als Partikel zu speichern,
    // wird für jeden Zeitpunkt der letzten 3 s nachgerechnet, ob dort eine
    // Wolke entstand (RocketAt ist eine reine Funktion der Zeit). Die Wolke
    // treibt nach außen (Richtung aus Hash01), steigt etwas, wächst und verblasst.
    for (size_t i = 0; i < pads.size(); ++i) {
        const RocketPad& pad = pads[i];
        if (Dist2D(pad.pos, focus) > cullR + 10.0f) continue;
        const long newest = static_cast<long>(floorf(time / 0.1f));
        for (long e = newest; e > newest - 30; --e) {
            const float born = e * 0.1f, age = time - born;
            if (age < 0.0f || age > 3.0f) continue;
            const RocketPose rp = RocketAt(pad, born);
            if (!rp.visible || rp.thrust < 0.2f || rp.pos.y > 14.0f) continue;
            const float ang = Hash01(static_cast<float>(e) * 0.73f + i * 5.1f) * 2.0f * PI;
            const float dist = 1.6f + 7.0f * (1.0f - expf(-age * 1.2f));
            const Vector3 c{pad.pos.x + cosf(ang) * dist, 0.5f + age * 0.7f, pad.pos.z + sinf(ang) * dist};
            const float size = 0.9f + age * 1.1f;
            DrawCube(c, size, size, size, Fade(Color{170, 168, 172, 255}, 0.45f * (1.0f - age / 3.0f)));
        }
    }

    // Sandkörner im Sandsturm: kurze Striche, die mit dem Wind (gleiche
    // Richtung wie die Schwaden im Shader, Effects.hpp) durch einen Würfel von
    // 2 x 36 m um den Blickpunkt fliegen. Wer hinten hinausfliegt, kommt vorn
    // wieder herein (Modulo); am Rand des Würfels blenden sie aus, damit das
    // nicht auffällt. Startpunkt und Tempo jedes Korns aus Hash01, die Lage
    // folgt aus der Zeit – es wird nichts gespeichert. Anzahl und Deckkraft
    // wachsen mit der Stärke des Sandsturms.
    if (sand > 0.0f) {
        const int count = static_cast<int>(320.0f * sand);
        const float box = 36.0f;
        const Vector3 wind = Vector3Normalize({0.9f, 0.0f, 0.55f});
        const Color dust = fx::SandColor();
        const Color grain{static_cast<unsigned char>(std::min(255, dust.r + 50)), static_cast<unsigned char>(std::min(255, dust.g + 45)),
                          static_cast<unsigned char>(std::min(255, dust.b + 40)), 255};
        auto wrap = [box](float v, float center) {
            const float span = 2.0f * box;
            float m = fmodf(v - (center - box), span);
            if (m < 0.0f) m += span;
            return center - box + m;
        };
        rlBegin(RL_LINES);
        for (int i = 0; i < count; ++i) {
            const float fi = static_cast<float>(i);
            const float speed = 14.0f + 10.0f * Hash01(fi * 3.33f + 0.77f);
            const float x = wrap(Hash01(fi * 1.37f + 0.11f) * 2.0f * box + wind.x * speed * time, focus.x);
            const float z = wrap(Hash01(fi * 2.71f + 0.53f) * 2.0f * box + wind.z * speed * time, focus.z);
            const float y = 0.3f + 7.0f * Hash01(fi * 0.93f + 0.29f) + 0.4f * sinf(time * 2.0f + fi);
            const float edge = std::clamp(std::min(box - fabsf(x - focus.x), box - fabsf(z - focus.z)) / 6.0f, 0.0f, 1.0f);
            const float len = 0.5f + 0.7f * Hash01(fi * 4.19f + 0.37f);
            const unsigned char a = static_cast<unsigned char>(140.0f * sand * edge);
            rlColor4ub(grain.r, grain.g, grain.b, a);
            rlVertex3f(x, y, z);
            rlColor4ub(grain.r, grain.g, grain.b, 0);
            rlVertex3f(x - wind.x * len, y, z - wind.z * len);
        }
        rlEnd();
    }

    // Alles Gesammelte noch mit den Effekt-Einstellungen zeichnen, dann den
    // Normalzustand wiederherstellen (für das nächste Frame und alles, was
    // nach World::Draw gezeichnet wird).
    fx::EndEffects();
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();

    // Zu Fuß: die eigene Waffe vor der Kamera (nach allem anderen, ohne Effekte)
    if (me.role == PlayerRole::Trooper)
        if (const Trooper* t = FindTrooper(me.trooperId); t && !t->down) DrawTrooperWeapon(*t, time);
}
