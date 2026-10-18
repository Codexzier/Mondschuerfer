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
//   1. Undurchsichtige Modelle: Boden, Dekoration, Stationen, Lagerstätten,
//      Wracks, Schiffe. Sie füllen den Tiefenpuffer.
//   2. Halbtransparente Bodenmarkierungen und Schatten (normales Mischen).
//   3. Leuchteffekte mit additivem Mischen: Scanner, Lichtsäulen,
//      Abbaustrahlen, Triebwerke, Geschosse, leuchtende Partikel.
//   4. Rauch und Trümmer (normales Mischen).
//   Durchsichtiges kommt nach dem Undurchsichtigen, weil es mit dem schon
//   fertigen Bild dahinter gemischt wird.
//
// Sichtweite ("Culling"): Alles, was weiter als drawRange (Standard 115 m,
// Zuschauer: ganze Karte) waagerecht vom Blickpunkt der Kamera (cam.target)
// entfernt ist, wird übersprungen. Jedes Modell kostet mindestens einen
// eigenen Zeichenaufruf; Unsichtbares wegzulassen spart viel Zeit.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Assets.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>

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
// unter dem Schiff. Echte Schatten (Shadow-Mapping: Szene zusätzlich aus Sicht
// der Sonne rendern) wären viel aufwendiger; die Scheibe zeigt trotzdem gut,
// wo ein schwebendes Schiff über dem Boden steht.
void DrawShadow(Vector3 pos, float radius, float alpha) {
    DrawCylinder({pos.x, 0.06f, pos.z}, radius, radius, 0.02f, 18, Fade(BLACK, alpha));
}

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

    // --- Boden --------------------------------------------------------------
    // Das Modell "terrain" ist eine flache Platte von 1 m x 1 m. Zwei Platten:
    //   1. 1400 m groß als Umgebung bis zum Horizont, etwas dunkler und 5 cm
    //      tiefer (sonst Z-Fighting mit der zweiten Platte).
    //   2. das Spielfeld: 2 * WORLD_HALF + 30 = 330 m (Feld ±150 m plus 15 m
    //      Rand), heller, damit man sieht, wo das Spielfeld endet.
    // Der tint wird mit der (umgefärbten) Gesteinsfarbe multipliziert.
    // Beim orangefarbenen Mond (Originalfarbe) den Boden kaum abdunkeln
    g_assets.Draw(mdl.terrain, TRS({0, -0.05f, 0}, 0, 1400.0f), orangeMoon ? Color{150, 140, 136, 255} : Color{120, 120, 128, 255});
    g_assets.Draw(mdl.terrain, TRS({0, 0, 0}, 0, cfg::WORLD_HALF * 2.0f + 30.0f),
                  orangeMoon ? Color{225, 218, 214, 255} : Color{176, 176, 184, 255});

    // --- Dekoration -----------------------------------------------------------
    // Felsen, Krater usw. aus WorldGen.cpp. Große Objekte ragen weit über ihren
    // Mittelpunkt hinaus, deshalb wächst der Culling-Radius mit der Größe.
    for (const auto& d : decos) {
        if (Dist2D(d.pos, focus) > cullR + d.scale * 0.6f) continue;
        if (d.yScale > 0.0f) {
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
            g_assets.Draw(p.model, wp, p.yaw, scale);
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
        // emissive: grünliches Eigenleuchten im Takt von pulse.
        g_assets.Draw(d.model, d.pos, d.yaw, d.scale * appear * fill, WHITE,
                      Vector3{0.05f, 0.18f * pulse, 0.10f * pulse});
    }

    // --- Wracks ------------------------------------------------------------------
    // Schief im Boden liegend (tilt als Nick- und, etwas schwächer, als
    // Rollwinkel) und dunkelgrau wie ausgebrannt.
    for (const auto& w : wrecks) {
        if (Dist2D(w.pos, focus) > cullR) continue;
        g_assets.Draw(w.model, TRS(w.pos, w.yaw, w.tilt, w.tilt * 0.7f, 1.0f), Color{95, 92, 92, 255});
    }

    // --- Schiffe -------------------------------------------------------------------
    // dead = wird gerade entfernt, hidden = angedockt (steht unsichtbar in der
    // Station) bzw. Platzhalter ohne Schiff im Mehrspieler.
    for (const auto& s : ships) {
        if (s.dead || s.hidden) continue;
        if (Dist2D(s.pos, focus) > cullR) continue;
        // Farbton je Schiffsart: Piraten rötlich, Elite-Piraten violett.
        // Spieler (auch fremde im Mehrspieler) und Händler/Miner bleiben unverändert.
        Color tint = WHITE;
        if (s.kind == ShipKind::Pirate) tint = Color{255, 150, 145, 255};
        else if (s.kind == ShipKind::Elite) tint = Color{190, 150, 255, 255};
        // Treffer-Aufblitzen: hitFlash springt beim Treffer auf 1 und klingt
        // in ca. 0,3 s ab (World.cpp); warmweißes Eigenleuchten.
        Vector3 em{s.hitFlash * 0.7f, s.hitFlash * 0.5f, s.hitFlash * 0.4f};
        // Nicken: Beim Absturz zeigt die Nase deutlich nach unten (0,5 rad,
        // ca. 29°); beim Beschleunigen neigt sich das Schiff leicht nach vorn
        // (bis ca. 3,4° bei vollem Schub). bank = Rollwinkel in Kurven (bzw.
        // Trudeln beim Absturz), berechnet in World.cpp.
        float pitch = s.crashing ? -0.5f : -0.06f * s.thrustVisual;
        g_assets.Draw(s.model, TRS(s.pos, s.yaw, pitch, s.bank, 1.0f), tint, em);
    }

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

    // Schatten
    // Alpha 0.16 bis 2,5 m Flughöhe, darüber schwächer, ab 12,5 m nur noch
    // 20 % davon: je höher das Schiff, desto blasser der Schatten.
    for (const auto& s : ships) {
        if (s.dead || s.hidden || Dist2D(s.pos, focus) > cullR) continue;
        float h = std::max(0.0f, s.pos.y);
        DrawShadow(s.pos, 1.1f, 0.16f * std::clamp(1.2f - h * 0.08f, 0.2f, 1.0f));
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
        else
            DrawGroundRing(s.pos, 1.7f, 1.9f, Color{120, 200, 255, 100}, 32);
    }

    // Andockbereich der Stationen
    // Gestrichelter Kreis mit dem Radius DOCK_RANGE (in diesem Abstand kann
    // man andocken). Er erscheint ab 55 m Entfernung und ist ab 30 m voll
    // sichtbar ((55 - d) / 25). Handelsstation orange, Werft blau. Er dreht
    // sich langsam (ein Strichabstand in ca. 6,7 s). Angedockt (hidden):
    // keine Ringe.
    for (const Station& st : stations) {
        if (player.hidden) break;
        float d = Dist2D(player.pos, st.pos);
        if (d < 55.0f) {
            float a = std::clamp((55.0f - d) / 25.0f, 0.0f, 1.0f);
            Color c = st.type == StationType::Trade ? Color{255, 190, 80, static_cast<unsigned char>(120 * a)}
                                                    : Color{120, 210, 255, static_cast<unsigned char>(120 * a)};
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
    // SCANNER_RADIUS (34 m) nach außen und wird dabei blasser (Alpha 200 -> 60).
    // World::Update deckt Lagerstätten mit derselben Formel auf, wenn der Ring
    // sie erreicht – Bild und Spiellogik passen also zusammen. Im Mehrspieler
    // zählt NetClient scanPulse lokal weiter. Der zweite, feste Ring zeigt
    // die Reichweite des Scanners.
    if (me.scanOn && !player.crashing && !player.hidden) {
        float f = fmodf(me.scanPulse, cfg::SCANNER_PULSE_TIME) / cfg::SCANNER_PULSE_TIME;
        float r = cfg::SCANNER_RADIUS * f;
        unsigned char a = static_cast<unsigned char>(200 * (1.0f - f * 0.7f));
        DrawGroundRing(player.pos, std::max(0.0f, r - 0.9f), r, Color{60, 255, 180, a}, 96);
        DrawGroundRing(player.pos, cfg::SCANNER_RADIUS - 0.25f, cfg::SCANNER_RADIUS, Color{60, 220, 160, 90}, 96);
    }

    // Leuchtmarkierungen für entdeckte Lagerstätten und Wracks
    // Lichtsäule (9 m bzw. 8 m hoch) und Bodenring, pulsierend mit der id als
    // Phasenversatz. Von oben weithin sichtbar, damit man Fundstellen schnell
    // wiederfindet. Grün = Kristalle, orange = Wrack (Schrott).
    for (const auto& d : deposits) {
        if (!d.revealed || Dist2D(d.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 2.5f + d.id);
        DrawBeam(d.pos, 9.0f, 0.35f, Color{40, 200, 120, static_cast<unsigned char>(70 * pulse)});
        DrawGroundRing(d.pos, 3.2f, 3.6f, Color{60, 255, 170, static_cast<unsigned char>(120 * pulse)}, 40);
    }
    for (const auto& w : wrecks) {
        if (Dist2D(w.pos, focus) > cullR) continue;
        float pulse = 0.6f + 0.4f * sinf(time * 3.0f + w.id);
        DrawBeam(w.pos, 8.0f, 0.3f, Color{220, 140, 40, static_cast<unsigned char>(70 * pulse)});
        DrawGroundRing(w.pos, 3.0f, 3.4f, Color{255, 170, 60, static_cast<unsigned char>(120 * pulse)}, 40);
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
    // Zwei Flammenkegel am Heck jedes Schiffs. fwd = Flugrichtung aus dem Kurs,
    // side = fwd um 90° in der Waagerechten gedreht (zeigt zur Seite).
    // Lage: back Meter hinter der Schiffsmitte (das Modell der Händler/Miner,
    // ShipKind::Miner, ist länger: 1.4 statt 1.15), 0.45 m links/rechts (k = -1 / +1), 0.35 m hoch.
    // Länge: 0.25 m im Leerlauf, bis zu 0.9 m mehr bei vollem Schub
    // (thrustVisual 0..1), dazu schnelles Flackern (40 rad/s, ca. 6 Mal pro
    // Sekunde) mit der Schiffs-id als Phasenversatz. Piraten rot, sonst orange.
    for (const auto& s : ships) {
        if (s.dead || s.hidden || Dist2D(s.pos, focus) > cullR) continue;
        Vector3 fwd = ForwardFromYaw(s.yaw);
        Vector3 side{fwd.z, 0, -fwd.x};
        float len = 0.25f + s.thrustVisual * 0.9f + 0.08f * sinf(time * 40.0f + s.id);
        float back = s.kind == ShipKind::Miner ? 1.4f : 1.15f;
        for (int k = -1; k <= 1; k += 2) {
            Vector3 base = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, -back), Vector3Scale(side, 0.45f * k)));
            base.y += 0.35f;
            Vector3 tip = Vector3Add(base, Vector3Scale(fwd, -len));
            Color c = s.faction == Faction::Pirate ? Color{255, 90, 70, 200} : Color{255, 170, 60, 200};
            // DrawCylinderEx(Start, Ende, Radius am Start, Radius am Ende, ...):
            // breit am Triebwerk, spitz am Ende.
            DrawCylinderEx(base, tip, 0.18f, 0.02f, 6, c);
        }
    }

    // Geschosse
    // Leuchtspur: 1,6 m lang entgegen der Flugrichtung hinter dem Geschoss,
    // vorn dicker. Darüber ein breiterer Schein mit 35 % Alpha.
    // Farben: Geschütztürme (ownerId -1) hellblau, Spieler gold, Piraten rot,
    // andere (Händler, die sich wehren) blassgelb.
    // Im Mehrspieler kommen die Geschosse aus dem Snapshot (die nächsten 128)
    // und werden vom Client zwischen den Snapshots weiterbewegt.
    for (const auto& b : bullets) {
        if (Dist2D(b.pos, focus) > cullR) continue;
        Color c;
        if (b.ownerId == -1) c = Color{110, 220, 255, 255};
        else if (b.faction == Faction::Player) c = Color{255, 210, 90, 255};
        else if (b.faction == Faction::Pirate) c = Color{255, 70, 60, 255};
        else c = Color{255, 240, 150, 255};
        Vector3 dir = Vector3Normalize(b.vel);
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

    // Alles Gesammelte noch mit den Effekt-Einstellungen zeichnen, dann den
    // Normalzustand wiederherstellen (für das nächste Frame und alles, was
    // nach World::Draw gezeichnet wird).
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}
