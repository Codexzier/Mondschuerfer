// ---------------------------------------------------------------------------
// WorldGen.cpp – Erzeugung der Mondoberfläche, der Stationen und der
// Lagerstätten (Teil der Klasse World, Deklarationen in World.hpp).
//
// Wofür ist die Datei da?
//   Sie baut die "Kulisse" einer neuen Welt:
//     - ResolveModels:  Modell-IDs für Schiffe, Kristalle und Boden holen.
//     - BuildStations:  Handelsstationen und Werften an festen Bauplätzen,
//                       mit ihren Bauteilen, Geschütztürmen und Hindernissen.
//     - GenerateTerrain: Krater, Felsen, Meteoriten, ein paar Fundstücke und
//                       der Felswall am Kartenrand (nur zum Anschauen).
//     - SpawnDeposit:   eine neue Kristall-Lagerstätte an einem freien Ort.
//     - RandomFreePos:  Zufallsort mit Mindestabstand zu Stationen/Spielern.
//
// Wer ruft das auf? (alles in shared/World.cpp)
//   - World::NewGame (Einzelspieler), World::NewServerWorld (Server) und
//     World::BuildMirror (Mehrspieler-Client) rufen immer in derselben
//     Reihenfolge ResolveModels -> BuildStations -> GenerateTerrain auf.
//   - Lagerstätten erzeugen nur Einzelspieler und Server (beim Start und
//     später in World::Update als Nachschub). Der Client bekommt sie fertig
//     vom Server (Deposits-Nachricht, src/NetClient.cpp).
//   - Gezeichnet wird alles in src/Render.cpp (nur im Client).
//
// Warum sehen Client und Server dieselbe Welt?
//   Beide setzen den Zufallsgenerator (World::rng, ein std::mt19937) auf
//   denselben Seed (der Server schickt ihn in der Welcome-Nachricht) und
//   bekommen dieselben GameSettings. BuildStations benutzt gar keine
//   Zufallszahlen; GenerateTerrain zieht sie in fester Reihenfolge. Der
//   Einzelspieler-Spielstand (src/Save.cpp) speichert deshalb nur den Seed
//   und erzeugt Stationen und Mondoberfläche beim Laden einfach neu.
//
//   Kleine Einschränkung: Bei add(..., Rand(...), Rand(...)) legt C++ NICHT
//   fest, in welcher Reihenfolge die Argumente einer Funktion ausgewertet
//   werden. Zwei verschiedene Compiler (z. B. GCC und Clang) können also yaw
//   und Größe eines Felsens vertauscht würfeln. Die Anzahl der gezogenen
//   Zahlen bleibt gleich, alles danach (Lagerstätten, Piraten) stimmt also
//   überein. Betroffen ist nur das Aussehen der Deko, und die hat keinen
//   Einfluss auf das Spiel (keine Kollision, der Server benutzt sie nicht).
//   Innerhalb von geschweiften Klammern {Rand(), 0, Rand()} ist die
//   Reihenfolge dagegen garantiert (von links nach rechts).
//
// Koordinaten: x = Osten, z = Süden (-z = Norden), y = Höhe. Die Karte geht
// von -cfg::WORLD_HALF bis +cfg::WORLD_HALF (600 x 600 Einheiten), die
// Kartenmitte (0, 0) ist die erste Handelsstation.
//
// Abhängigkeiten: World.hpp (und darüber Config.hpp mit den Balancing-Werten,
// Util.hpp, raymath.h), Models.hpp (ModelId, ModelBounds). Keine Grafik.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Models.hpp"
#include <cmath>

namespace {
// Die Stationen werden im Raster des Kenney-Kits gebaut (1 Einheit = ein
// Kit-Feld, eine große Plattform ist 2 x 2). Für das Spiel ist das zu klein
// – die Raumschiffe sind schon etwa 2 Einheiten lang. Darum werden alle
// Positionen und Größen der Stationsteile am Ende mit 1,8 multipliziert.
constexpr float STATION_SCALE = 1.8f;
}

// Modell-IDs, die die Spielregeln direkt als Zahl brauchen (Schiffe je Art,
// Kristalle je Größe, Boden). Die IDs selbst vergibt Models.cpp.
// Dieselben Namen stehen in net::DefaultModelIds (shared/Protocol.cpp): Über
// das Netzwerk geht nur eine kleine "Varianten"-Nummer (0 = player,
// 1 = pirateA ...), und beide Seiten übersetzen sie mit dieser Liste.
void World::ResolveModels() {
    mdl.player = ModelId("craft_speederA");
    mdl.pirateA = ModelId("craft_speederB");
    mdl.pirateB = ModelId("craft_speederC");
    mdl.elite = ModelId("craft_speederD");
    mdl.miner = ModelId("craft_miner");      // die drei Händler-Modelle (ShipKind::Miner)
    mdl.cargoA = ModelId("craft_cargoA");
    mdl.cargoB = ModelId("craft_cargoB");
    for (int t = 0; t < cfg::SHIP_TYPE_COUNT; ++t) mdl.shipTypes[t] = ModelId(cfg::SHIP_TYPES[t].model);
    mdl.terrain = ModelId("terrain");
    mdl.crystalSmall = ModelId("rock_crystals");          // Lagerstätten bis 1000
    mdl.crystalA = ModelId("rock_crystalsLargeA");        // große Lagerstätten (> 1000)
    mdl.crystalB = ModelId("rock_crystalsLargeB");
    mdl.astronautA = ModelId("astronautA");               // verirrte Personen (Mission)
    mdl.astronautB = ModelId("astronautB");
}

// Zufälliger Ort auf dem Boden (y = 0), der mindestens minDistStations von
// jeder Station und (falls > 0) minDistPlayer von jedem Spielerschiff
// entfernt ist. 10 Einheiten Abstand zum Kartenrand.
// Benutzt von: SpawnDeposit, SpawnPirate/SpawnMiner (World.cpp) und
// GenerateTerrain (Fundstücke).
// Es wird höchstens 200-mal gewürfelt. Passt nichts (sehr unwahrscheinlich),
// kommt der letzte Versuch zurück, auch wenn er zu nah ist – lieber eine
// etwas ungünstige Stelle als eine Endlosschleife.
Vector3 World::RandomFreePos(float minDistStations, float minDistPlayer) {
    const float lim = cfg::WORLD_HALF - 10.0f;
    Vector3 p{};
    for (int tries = 0; tries < 200; ++tries) {
        p = {Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        bool ok = true;
        for (const auto& st : stations) {
            if (Vector2Distance({p.x, p.z}, {st.pos.x, st.pos.z}) < minDistStations) ok = false;
        }
        if (ok && minDistPlayer > 0.0f) {
            // Nicht zu nah an einem Spielerschiff (Einzelspieler: ships[0];
            // Server: die Schiffe aller Spieler). So tauchen neue Piraten und
            // Lagerstätten nicht direkt vor jemandem auf.
            for (const auto& s : ships) {
                if (s.kind != ShipKind::Player) continue;
                if (Vector2Distance({p.x, p.z}, {s.pos.x, s.pos.z}) < minDistPlayer) ok = false;
            }
        }
        if (ok) return p;
    }
    return p;
}

// ===========================================================================
// Mondoberfläche (Deko)
//
// Füllt "decos": eine Liste aus (Modell, Position, Drehung, Größe). Die Deko
// ist reine Kulisse: Schiffe fliegen in cfg::SHIP_ALTITUDE darüber hinweg,
// es gibt keine Kollision damit. Render.cpp zeichnet nur die Teile in
// Sichtweite.
// Bei jedem Durchgang wird erst ein Ort gewürfelt. Liegt er zu nah an einer
// Station, wird er übersprungen (continue) – die Stationen bleiben frei,
// damit man sie gut sieht und anfliegen kann. Die Zahlen in den Schleifen
// sind also Versuche, nicht die genaue Anzahl (insgesamt etwa 600 Teile).
// Die Größen sind Faktoren auf die Kit-Modelle, die selbst etwa 1 Einheit
// groß sind: ein Krater mit Größe 5..15 ist also ~4..13 Einheiten breit.
// ===========================================================================
void World::GenerateTerrain() {
    decos.clear();
    // Kleine Helfer (Lambdas). [&] heißt: sie dürfen auf die Variablen der
    // umgebenden Funktion zugreifen (hier auf "this", also decos/stations).
    // yScale = 0.0f: gleichmäßig skaliert (siehe Deco in World.hpp).
    auto add = [&](const char* name, Vector3 pos, float yaw, float scale) {
        decos.push_back({ModelId(name), pos, yaw, scale, 0.0f});
    };
    auto farFromStations = [&](Vector3 p, float d) {
        for (const auto& st : stations)
            if (Vector2Distance({p.x, p.z}, {st.pos.x, st.pos.z}) < d) return false;
        return true;
    };
    const float lim = cfg::WORLD_HALF - 4.0f;

    // Krater (flach und groß, prägen das Bild am meisten)
    for (int i = 0; i < 110; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (!farFromStations(p, 28.0f)) continue;
        add(Rand(0, 1) < 0.5f ? "crater" : "craterLarge", p, Rand(0, 2 * PI), Rand(5.0f, 15.0f));
    }
    // Gestein und Geröll (viele kleine Teile, damit man Bewegung sieht)
    const char* rocks[] = {"rock", "rock_largeA", "rock_largeB", "rocks_smallA", "rocks_smallB", "meteor_half"};
    for (int i = 0; i < 320; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (!farFromStations(p, 22.0f)) continue;
        add(rocks[RandInt(0, 5)], p, Rand(0, 2 * PI), Rand(1.6f, 4.2f));
    }
    // Große Brocken (Meteoriten)
    for (int i = 0; i < 24; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (!farFromStations(p, 30.0f)) continue;
        add(Rand(0, 1) < 0.5f ? "meteor" : "meteor_detailed", p, Rand(0, 2 * PI), Rand(2.2f, 3.6f));
    }
    // Kleinigkeiten zum Entdecken (Knochen, ein Alien mit Mondfahrzeug,
    // verlassene Fahrzeuge mit Fässern) – ohne Spielfunktion, nur als
    // Belohnung fürs Herumfliegen.
    for (int i = 0; i < 5; ++i) {
        Vector3 p{Rand(-lim, lim), 0.0f, Rand(-lim, lim)};
        if (farFromStations(p, 40.0f)) add("bones", p, Rand(0, 2 * PI), 3.0f);
    }
    {
        Vector3 p = RandomFreePos(70.0f, 0.0f);
        add("alien", p, Rand(0, 2 * PI), 1.6f);
        add("rover", Vector3Add(p, {3.0f, 0, 1.0f}), Rand(0, 2 * PI), 3.0f);
    }
    for (int i = 0; i < 3; ++i) {
        Vector3 p = RandomFreePos(35.0f, 0.0f);
        add("rover", p, Rand(0, 2 * PI), 3.0f);
        add("barrels", Vector3Add(p, {2.5f, 0, -1.5f}), Rand(0, 2 * PI), 2.5f);
    }

    // Grenze des Einsatzgebiets: Felswall
    // Die eigentliche Grenze setzt World::UpdateShipPhysics (Schiffe werden
    // bei +-WORLD_HALF angehalten). Der Wall steht 3 Einheiten dahinter und
    // zeigt sie nur an. Alle 7 Einheiten ein großer Fels auf jeder der vier
    // Seiten; mit Größe 6..9 ist ein Fels etwa 5,5..8 breit, die Lücken
    // schließen sich also. Die Verschiebung um +-2 lässt den Wall natürlich
    // statt wie mit dem Lineal gezogen wirken.
    const float edge = cfg::WORLD_HALF + 3.0f;
    for (float t = -edge; t <= edge; t += 7.0f) {
        const char* r = Rand(0, 1) < 0.5f ? "rock_largeA" : "rock_largeB";
        add(r, {t + Rand(-2, 2), 0, -edge + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));   // Nordrand
        add(r, {t + Rand(-2, 2), 0, edge + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));    // Südrand
        add(r, {-edge + Rand(-2, 2), 0, t + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));   // Westrand
        add(r, {edge + Rand(-2, 2), 0, t + Rand(-2, 2)}, Rand(0, 2 * PI), Rand(6.0f, 9.0f));    // Ostrand
    }
}

// ---------------------------------------------------------------------------
// Stationen
//
// Es gibt 9 feste Bauplätze: die Kartenmitte und 8 Plätze auf einem Ring
// (Abstand ~92 zur Mitte). Die erste Handelsstation steht immer in der Mitte,
// die erste Werft immer bei (72, -58) – genau wie im Einzelspieler. Weitere
// Stationen (Mehrspieler-Einstellungen) wechseln sich auf dem Ring ab und
// werden möglichst weit voneinander verteilt.
//
// Die Reihenfolge in "stations" ist die Bauplatz-Reihenfolge. Sie ist im
// Client und im Server gleich, weil beide dieselbe Funktion mit denselben
// Einstellungen aufrufen. Das ist wichtig, denn im Netzwerk werden Stationen
// nur über ihren Index angesprochen (z. B. PlayerState::dockedAt).
//
// Jede Station besteht aus:
//   - parts:     Bauteile (Kit-Modelle) relativ zur Stationsmitte. Jedes Teil
//                hat eine Ausbaustufe "stage"; Render.cpp zeigt es erst, wenn
//                die Station so weit gewachsen ist.
//   - turrets:   Geschütztürme (schießen auf Piraten, World::UpdateTurrets),
//                ebenfalls mit Ausbaustufe.
// Die Ausbaustufe der Handelsstationen ergibt sich aus der Gesamtmenge
// abgelieferter Ressourcen (World::StationStage, Schwellen in
// cfg::STATION_THRESHOLDS). Im Mehrspieler zählen dafür die Lieferungen ALLER
// Spieler, und alle Handelsstationen wachsen gemeinsam. Werften wachsen nicht
// (alle Teile haben Stufe 0).
// ---------------------------------------------------------------------------
namespace {

// Ring-Bauplätze in der Reihenfolge, in der sie belegt werden (möglichst weit verteilt)
// Platz k liegt bei k * 45 Grad. Zuerst 0 und 4 (gegenüber), dann 2 und 6
// (rechtwinklig dazu), danach die Plätze dazwischen. So stehen z. B. zwei
// Werften nie nebeneinander, solange es noch freie Plätze gibt.
constexpr int kRingOrder[8] = {0, 4, 2, 6, 1, 5, 3, 7};

// Position von Ring-Bauplatz k (0..7).
Vector3 RingSlot(int k) {
    // Platz 0 ist (72, -58); die anderen entstehen durch Drehen um je 45 Grad
    // (72, -58) liegt nordöstlich der Mitte, ~92 Einheiten entfernt: weit
    // genug, dass sich die Schutzzonen der Stationen (cfg::STATION_SAFE_RADIUS
    // = 34) nicht berühren.
    // Drehung eines Punkts (x, z) um den Winkel a in der Ebene ("Drehmatrix"):
    //   x' = x * cos(a) - z * sin(a),   z' = x * sin(a) + z * cos(a)
    const float a = k * (PI / 4.0f);
    const float c = cosf(a), s = sinf(a);
    return {72.0f * c - (-58.0f) * s, 0.0f, 72.0f * s + (-58.0f) * c};
}

} // namespace

// Vorwärtsdeklarationen: Die beiden Baupläne stehen weiter unten, werden aber
// schon in BuildStations gebraucht.
namespace {
void BuildTradeStation(Station& t);
void BuildShipyard(Station& y);
void BuildShipDealer(Station& d);
} // namespace

void World::BuildStations() {
    stations.clear();
    // settings ist im Einzelspieler der Standard (1 Handelsstation, 1 Werft),
    // auf Server und Client kommen die Werte aus den Server-Einstellungen
    // (bereits mit Sanitize geprüft: 1..5 bzw. 1..4).
    const int trades = settings.tradeStations;
    const int yards = settings.shipyards;

    // Handelsstation 1 in der Mitte
    Station first;
    first.type = StationType::Trade;
    first.pos = {0, 0, 0};
    BuildTradeStation(first);
    stations.push_back(first);

    // Ring: abwechselnd Werft, Handelsstation, Werft, ... bis alle verteilt sind
    // Ist eine Sorte aufgebraucht, bekommt die andere den Platz. Beispiel
    // 1 Handelsstation + 3 Werften: Mitte = Handel, Ring = Werft, Werft, Werft.
    // 5 + 4 Stationen füllen genau alle 8 Ringplätze (siehe limits in
    // GameSettings.hpp).
    int tradeLeft = trades - 1, yardLeft = yards;
    bool nextIsYard = true;
    for (int slot = 0; slot < 8 && (tradeLeft > 0 || yardLeft > 0); ++slot) {
        bool yard = nextIsYard ? yardLeft > 0 : tradeLeft <= 0;
        Station st;
        st.pos = RingSlot(kRingOrder[slot]);
        if (yard) {
            st.type = StationType::Shipyard;
            BuildShipyard(st);
            --yardLeft;
        } else {
            st.type = StationType::Trade;
            BuildTradeStation(st);
            --tradeLeft;
        }
        stations.push_back(st);
        nextIsYard = !nextIsYard;
    }

    // Raumschiffsverkäufer: immer genau einer, als letzte Station. Er steht
    // südlich der Mitte außerhalb des Rings (Ring-Bauplätze ~92 Einheiten von
    // der Mitte, also mindestens 78 Abstand: die Schutzzonen berühren sich
    // nicht). Das Startschiff blickt beim Start genau in diese Richtung.
    Station dealer;
    dealer.type = StationType::Dealer;
    dealer.pos = {0, 0, 170.0f};
    BuildShipDealer(dealer);
    stations.push_back(dealer);

    // Namen mit Nummer, wenn es mehrere gibt
    // (Nummerierung in Bauplatz-Reihenfolge; "Handelsstation 1" ist immer
    // die in der Mitte.)
    int tn = 0, yn = 0;
    for (auto& st : stations) {
        if (st.type == StationType::Trade) st.name = trades > 1 ? Fmt("Handelsstation %d", ++tn) : "Handelsstation";
        else if (st.type == StationType::Shipyard) st.name = yards > 1 ? Fmt("Werft %d", ++yn) : "Werft";
        else st.name = "Raumschiffsverkäufer";
    }

    // Skalieren: Erst hier wird vom Kit-Raster in Weltgrößen umgerechnet (STATION_SCALE).
    // Stationen sind keine Kollisionsobjekte, Schiffe fliegen durch.
    for (auto& st : stations) {
        for (auto& p : st.parts) {
            p.offset = Vector3Scale(p.offset, STATION_SCALE);
            p.scale *= STATION_SCALE;
        }
        for (auto& tu : st.turrets) tu.offset = Vector3Scale(tu.offset, STATION_SCALE);
    }
}

namespace {
// 90 Grad im Bogenmaß. Gänge (corridor) liegen im Kit in Nord-Süd-Richtung;
// um H gedreht verlaufen sie von West nach Ost.
const float H = PI * 0.5f;

// Fügt ein Bauteil hinzu.
//   name   Kit-Modell (Dateiname ohne .glb)
//   x, z   Lage relativ zur Stationsmitte im Kit-Raster (+x Osten, -z Norden)
//   yaw    Drehung des Teils um die senkrechte Achse (Bogenmaß)
//   stage  ab dieser Ausbaustufe sichtbar (0 = von Anfang an)
//   y      Höhe; 0,1 = steht auf einer Plattform (platform_large ist 0,1 hoch)
//   scale  Größe (1 = Originalgröße im Kit)
void Part(Station& st, const char* name, float x, float z, float yaw, int stage, float y = 0.0f, float scale = 1.0f) {
    st.parts.push_back({ModelId(name), {x, y, z}, yaw, scale, stage});
}

// Geschütztürme werden direkt angelegt:
//   t.turrets.push_back({{x, y, z}, yaw, cd, stage, isDouble})
// yaw/cd (Drehung, Nachladezeit) starten bei 0; isDouble wählt nur das Modell
// (Doppel- oder Einzelrohr), schießen tun beide gleich.

void BuildTradeStation(Station& t) {
    // -------------------------------------------------------------------
    // Handelsstation – wächst in 8 Stufen (0..7)
    // Koordinaten im Raster des Kits (1 Einheit = ein Kit-Feld)
    // Die Stufen gehören zu den Schwellen in cfg::STATION_THRESHOLDS
    // (0, 1.500, 5.000, 12.000, 25.000, 45.000, 70.000, 100.000 abgelieferte
    // Ressourcen). Mit jeder Stufe kommt ein Gebäudeteil dazu, und mit den
    // Stufen 2, 4, 6 und 7 je ein weiterer Geschützturm: Die Station wird
    // mit dem Spielfortschritt sicherer, während auch die Piraten stärker
    // werden (World::MakeShip, PirateTier).
    // -------------------------------------------------------------------
    // Stufe 0: Kuppel auf Plattform, kleine Antenne, ein Geschützturm
    // (vier 2x2-Plattformen um die Mitte ergeben eine 4x4-Fläche)
    Part(t, "platform_large", -1, -1, 0, 0);
    Part(t, "platform_large", 1, -1, 0, 0);
    Part(t, "platform_large", -1, 1, 0, 0);
    Part(t, "platform_large", 1, 1, 0, 0);
    Part(t, "hangar_roundA", 0, 0, 0, 0, 0.1f);
    Part(t, "satelliteDish", 1.5f, 1.5f, -0.6f, 0, 0.1f);
    Part(t, "barrels", -1.5f, 1.5f, 0.3f, 0, 0.1f);
    t.turrets.push_back({{-1.6f, 0.1f, -1.6f}, 0, 0, 0, true});

    // Stufe 1: Ostflügel mit kleinem Hangar
    Part(t, "corridor", 2.5f, 0, H, 1);
    Part(t, "corridor_window", 3.5f, 0, H, 1);
    Part(t, "platform_large", 5.5f, 0, 0, 1);
    Part(t, "hangar_smallA", 5.5f, 0, H, 1, 0.1f);
    Part(t, "machine_generator", 3.5f, 1.4f, 0, 1);
    Part(t, "machine_barrel", 2.6f, -1.4f, 0, 1);

    // Stufe 2: Westflügel mit großer Kuppel, zweiter Geschützturm
    Part(t, "corridor", -2.5f, 0, H, 2);
    Part(t, "corridor_window", -3.5f, 0, H, 2);
    Part(t, "platform_large", -6.2f, -1, 0, 2);
    Part(t, "platform_large", -6.2f, 1, 0, 2);
    Part(t, "hangar_roundB", -6.1f, 0, 0, 2, 0.1f);
    Part(t, "satelliteDish_large", -3.6f, 1.6f, 2.4f, 2);
    t.turrets.push_back({{-4.0f, 0.0f, -1.8f}, 0, 0, 2, true});

    // Stufe 3: Nordflügel mit großem Hangar und Lager
    Part(t, "corridor", 0, -2.5f, 0, 3);
    Part(t, "corridor_window", 0, -3.5f, 0, 3);
    Part(t, "platform_large", 0, -4.5f, 0, 3);
    Part(t, "platform_large", 0, -6.5f, 0, 3);
    Part(t, "hangar_largeA", 0, -5.5f, 0, 3, 0.1f);
    Part(t, "machine_barrelLarge", 1.8f, -3.2f, 0, 3);
    Part(t, "machine_barrelLarge", -1.6f, -3.4f, 0, 3);
    Part(t, "barrels", 1.8f, -4.6f, 0.4f, 3);

    // Stufe 4: Südflügel mit Glaskuppel, Generator, dritter Geschützturm
    Part(t, "corridor", 0, 2.5f, 0, 4);
    Part(t, "corridor_window", 0, 3.5f, 0, 4);
    Part(t, "platform_large", -1, 5.4f, 0, 4);
    Part(t, "platform_large", 1, 5.4f, 0, 4);
    Part(t, "hangar_roundGlass", 0, 5.4f, 0, 4, 0.1f);
    Part(t, "machine_generatorLarge", 2.6f, 3.2f, 0, 4);
    Part(t, "satelliteDish_detailed", -2.4f, 3.0f, 0.8f, 4);
    t.turrets.push_back({{3.0f, 0.0f, -1.8f}, 0, 0, 4, true});

    // Stufe 5: Raketenstartplatz Nord-Ost
    // Die Rakete ist aus vier Teilen gestapelt (y = Höhe des jeweiligen
    // Abschnitts), daneben ein Gerüst aus drei übereinandergesetzten Teilen.
    Part(t, "platform_large", 5.5f, -4.0f, 0, 5);
    Part(t, "rocket_baseA", 5.5f, -4.0f, 0, 5, 0.1f);
    Part(t, "rocket_fuelA", 5.5f, -4.0f, 0, 5, 1.1f);
    Part(t, "rocket_sidesA", 5.5f, -4.0f, 0, 5, 1.6f);
    Part(t, "rocket_topA", 5.5f, -4.0f, 0, 5, 2.6f);
    Part(t, "structure_detailed", 4.0f, -4.0f, 0, 5);
    Part(t, "structure", 4.0f, -4.0f, 0, 5, 1.0f);
    Part(t, "structure", 4.0f, -4.0f, 0, 5, 2.0f);

    // Stufe 6: Werkhallen Nord-West, Schornsteine, vierter Turm
    Part(t, "platform_large", -5.5f, -4.5f, 0, 6);
    Part(t, "hangar_largeB", -5.5f, -4.5f, H, 6, 0.1f);
    Part(t, "chimney_detailed", -7.2f, -3.0f, 0, 6);
    Part(t, "chimney", -7.2f, -6.0f, 0, 6);
    Part(t, "machine_wireless", -3.6f, -4.6f, H, 6);
    t.turrets.push_back({{-4.0f, 0.0f, 3.2f}, 0, 0, 6, false});

    // Stufe 7: zweite Rakete und großer Kuppelkomplex
    Part(t, "platform_large", 5.5f, 4.5f, 0, 7);
    Part(t, "rocket_baseB", 5.5f, 4.5f, 0, 7, 0.1f);
    Part(t, "rocket_fuelB", 5.5f, 4.5f, 0, 7, 1.1f);
    Part(t, "rocket_sidesB", 5.5f, 4.5f, 0, 7, 2.1f);
    Part(t, "rocket_topB", 5.5f, 4.5f, 0, 7, 3.1f);
    Part(t, "platform_large", -5.8f, 4.6f, 0, 7);
    Part(t, "hangar_roundB", -5.8f, 4.6f, 0, 7, 0.1f);
    Part(t, "satelliteDish_large", 3.5f, 6.2f, -0.6f, 7);
    t.turrets.push_back({{3.4f, 0.0f, 3.0f}, 0, 0, 7, false});
}

void BuildShipyard(Station& y) {
    // -------------------------------------------------------------------
    // Werft (Upgrades)
    // Wächst nicht: alle Teile und beide Geschütztürme haben Stufe 0. Alle
    // Teile sind niedrig genug zum Überfliegen (keine Hindernisse).
    // Mitte: Hangar auf 2x2 Plattformen; Osten: Dock mit vier Gerüsten und
    // einem Frachtschiff, das gerade "gewartet" wird (nur Deko, kein echtes
    // Schiff aus World::ships).
    // -------------------------------------------------------------------
    for (int ix = -1; ix <= 1; ix += 2)
        for (int iz = -1; iz <= 1; iz += 2) Part(y, "platform_large", ix * 1.0f, iz * 1.0f, 0, 0);
    Part(y, "platform_large", 3.0f, -1.0f, 0, 0);
    Part(y, "platform_large", 3.0f, 1.0f, 0, 0);
    Part(y, "hangar_largeA", -1.0f, 0.0f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 2.2f, -1.4f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 3.8f, -1.4f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 2.2f, 1.4f, 0, 0, 0.1f);
    Part(y, "structure_detailed", 3.8f, 1.4f, 0, 0, 0.1f);
    Part(y, "craft_cargoB", 3.0f, 0.0f, H, 0, 0.35f, 0.9f);
    Part(y, "gate_complex", 1.2f, 2.3f, 0, 0, 0.0f);
    Part(y, "machine_wireless", -2.6f, 1.6f, H, 0);
    Part(y, "desk_computerScreen", 1.0f, -2.2f, PI, 0);
    Part(y, "satelliteDish_large", -2.6f, -1.8f, 0.6f, 0);
    Part(y, "barrels", 4.6f, 2.4f, 0.0f, 0);
    y.turrets.push_back({{-2.8f, 0.0f, 0.0f}, 0, 0, 0, true});
    y.turrets.push_back({{5.0f, 0.0f, -2.4f}, 0, 0, 0, false});
}

void BuildShipDealer(Station& d) {
    // -------------------------------------------------------------------
    // Raumschiffsverkäufer (andere Schiffe kaufen)
    // Wächst nicht (Stufe 0). Render.cpp färbt alle Teile lime-grün.
    // Mitte: Glashangar auf 2x2 Plattformen; davor eine Reihe Plattformen,
    // auf denen die drei Schiffe zum Kauf ausgestellt sind (nur Deko).
    // -------------------------------------------------------------------
    for (int ix = -1; ix <= 1; ix += 2)
        for (int iz = -1; iz <= 1; iz += 2) Part(d, "platform_large", ix * 1.0f, iz * 1.0f, 0, 0);
    for (int ix = -3; ix <= 3; ix += 3) Part(d, "platform_large", ix * 1.0f, 3.0f, 0, 0);
    Part(d, "hangar_roundGlass", 0.0f, -0.6f, 0, 0, 0.1f);
    Part(d, "craft_cargoA", -3.0f, 3.0f, PI, 0, 0.35f, 0.8f);
    Part(d, "craft_miner", 0.0f, 3.0f, PI, 0, 0.35f, 0.8f);
    Part(d, "craft_speederD", 3.0f, 3.0f, PI, 0, 0.35f, 0.8f);
    Part(d, "machine_wireless", 2.4f, -1.6f, 0, 0);
    Part(d, "desk_computerScreen", -2.2f, 1.2f, PI, 0);
    Part(d, "barrels", -2.6f, -1.8f, 0.0f, 0);
    d.turrets.push_back({{-3.0f, 0.0f, -2.0f}, 0, 0, 0, true});
    d.turrets.push_back({{3.4f, 0.0f, 0.4f}, 0, 0, 0, false});
}

} // namespace

// ===========================================================================
// Lagerstätten (Kristalle)
//
// initial = true: beim Start einer Welt (NewGame/NewServerWorld), false:
// Nachschub während des Spiels (World::Update, wenn weniger Lagerstätten da
// sind als World::DepositTarget() verlangt).
// Neue Lagerstätten sind zunächst unentdeckt (revealed = false bzw. leere
// revealedFor-Liste); erst der Scanner eines Spielers deckt sie auf.
// ===========================================================================
void World::SpawnDeposit(bool initial) {
    Deposit d;
    d.id = nextId++;   // IDs sind für Schiffe, Lagerstätten und Wracks gemeinsam fortlaufend
    // Nicht direkt neben den Stationen
    // 30 Einheiten Abstand zu jeder Station. Nachschub zusätzlich nicht
    // näher als 25 an einem Spielerschiff: Wer stillsteht und wartet, soll
    // keine neuen Kristalle direkt vor die Nase bekommen.
    d.pos = RandomFreePos(30.0f, initial ? 0.0f : 25.0f);
    // Balancing "Risiko gegen Ertrag": Je weiter von der Kartenmitte (der
    // ersten Handelsstation) entfernt, desto mehr Kristalle – aber auch
    // desto weiter der Rückweg und desto mehr Piraten unterwegs.
    // richness: 1,0 in der Mitte, ~2,6 am Kartenrand, ~3,3 in den Ecken.
    // Menge: 450..1100 mal richness, also etwa 600..1500 nahe der Mitte
    // und bis zu ~3600 in den Ecken.
    float dist = Vector2Length({d.pos.x, d.pos.z});
    float richness = 1.0f + dist / 85.0f;            // weiter draußen = ergiebiger
    d.amount = static_cast<int>(Rand(450.0f, 1100.0f) * richness);
    d.amount = (d.amount / 10) * 10;                 // auf Zehner abrunden (glatte Zahlen in der Anzeige)
    d.initial = d.amount;                            // Startmenge: Render.cpp lässt die Kristalle beim Abbau schrumpfen
    d.yaw = Rand(0, 2 * PI);
    // Das Aussehen verrät die Größe: große Kristallgruppen ab 1000, die
    // größten (> 1600) zusätzlich größer gezeichnet. So erkennt man lohnende
    // Lagerstätten schon von Weitem.
    if (d.amount > 1600) {
        d.model = Rand(0, 1) < 0.5f ? mdl.crystalA : mdl.crystalB;
        d.scale = 3.4f;
    } else if (d.amount > 1000) {
        d.model = Rand(0, 1) < 0.5f ? mdl.crystalA : mdl.crystalB;
        d.scale = 2.8f;
    } else {
        d.model = mdl.crystalSmall;
        d.scale = 2.8f;
    }
    deposits.push_back(d);
    // Server: Lagerstätten haben sich geändert -> neue Liste an die Clients
    // schicken (GameServer::SendDepositUpdates vergleicht diesen Zähler).
    ++depositRevision;
}
