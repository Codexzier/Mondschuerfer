// ---------------------------------------------------------------------------
// Save.cpp – Spielstand des Einzelspielers als einfache Textdatei neben der
// ausführbaren Datei (Schnittstelle und Aufrufer: Save.hpp).
//
// Warum eine Textdatei? Man kann sie mit jedem Editor ansehen (hilft bei der
// Fehlersuche), sie ist klein, und die Versionsnummer in der ersten Zeile
// erlaubt später ein geändertes Format. Gelesen und geschrieben wird mit den
// C++-Streams (std::ifstream/std::ofstream, operator<< und >>).
//
// Aufbau der Datei (eine Angabe pro Zeile, Werte durch Leerzeichen getrennt):
//   MONDSCHUERFER_SAVE 1                  Kennung und Formatversion
//   seed <Zahl>                           Startwert des Zufallsgenerators
//   balance <Kontostand>
//   delivered <insgesamt abgeliefert>     bestimmt die Ausbaustufe der Stationen
//   upgrades <Tempo> <Panzerung> <Waffe> <Laderaum> <Kanonen>   Stufen 0..5 (Kanonen 0..2; fehlt in alten Spielständen)
//   lives <Leben>                         verbleibende Leben (fehlt in alten Spielständen = 3)
//   stats <Spielzeit s> <Abschüsse> <nächste freie ID>
//   scanner <Energie>
//   player <x> <z> <Drehung> <Schild> <Ladung>
//   deposit ... (eine Zeile je Lagerstätte)
//   wreck ...   (eine Zeile je Wrack)
//   ship ...    (eine Zeile je fremdem Schiff: Pirat, Elite, Händler)
//   end
//
// Was wird NICHT gespeichert und warum?
//   - Mondoberfläche, Stationen, Deko: entstehen aus dem Seed jedes Mal gleich
//     (World::NewGame -> BuildStations/GenerateTerrain). Deshalb liest
//     LoadGame zuerst den Seed und baut damit die Kulisse neu auf.
//   - Geschosse, Partikel, Meldungen, Spawn-Timer, KI-Zwischenstände:
//     kurzlebig, starten nach dem Laden einfach neu.
//   - Höhe (y) und Geschwindigkeit: Schiffe fliegen immer in derselben Höhe
//     (cfg::SHIP_ALTITUDE), nach dem Laden steht das eigene Schiff still.
//   - Andock-Zustand des Spielers: Nach dem Laden ist man abgedockt, steht
//     aber genau dort, wo man gespeichert hat (meist an einer Station, weil
//     nach jedem Andocken gespeichert wird).
//
// Abhängigkeiten: shared/World.hpp (Spielzustand, NewGame, MakeShip,
// ApplyUpgrades), raylib (GetApplicationDirectory). Assets.hpp wird nicht
// wirklich gebraucht, bringt aber raylib.h/raymath.h mit.
// Alles läuft im Haupt-Thread (Aufrufe aus main.cpp).
// ---------------------------------------------------------------------------
#include "Save.hpp"
#include "Assets.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

// Erste Zeile jeder Spielstand-Datei. Die Kennung schützt davor, eine fremde
// Datei mit gleichem Namen als Spielstand zu lesen. kVersion bei jeder
// Formatänderung erhöhen: Alte Spielstände werden dann abgelehnt
// ("Fortsetzen" wird gesperrt) statt falsch gelesen.
const char* kMagic = "MONDSCHUERFER_SAVE";
const int kVersion = 1;

// Modelle werden als Varianten-Nummer gespeichert, nicht als Laufzeit-ID:
// Die Modell-IDs (shared/Models.hpp) werden beim Programmstart in der
// Reihenfolge der ersten Anfrage vergeben und können in einer anderen
// Programmversion anders lauten. Die Variante (Position in dieser festen
// Liste) bleibt dagegen gleich. Gleiche Liste und Reihenfolge wie
// net::ShipVariant in shared/Protocol.cpp (dort für das Netzwerk).
//   0 Spieler, 1/2 Pirat A/B, 3 Elite, 4 Bergbauschiff, 5/6 Frachter A/B
// Unbekannte Modelle werden zu Variante 0.
int ShipVariant(const World& w, int model) {
    const int list[7] = {w.mdl.player, w.mdl.pirateA, w.mdl.pirateB, w.mdl.elite, w.mdl.miner, w.mdl.cargoA, w.mdl.cargoB};
    for (int i = 0; i < 7; ++i) if (list[i] == model) return i;
    return 0;
}
// Umkehrung von ShipVariant; ungültige Nummern aus der Datei ergeben Variante 0.
int ShipModel(const World& w, int variant) {
    const int list[7] = {w.mdl.player, w.mdl.pirateA, w.mdl.pirateB, w.mdl.elite, w.mdl.miner, w.mdl.cargoA, w.mdl.cargoB};
    return list[(variant >= 0 && variant < 7) ? variant : 0];
}
// Lagerstätten: 0 kleine Kristalle, 1/2 große Kristalle A/B
int DepositVariant(const World& w, int model) {
    if (model == w.mdl.crystalA) return 1;
    if (model == w.mdl.crystalB) return 2;
    return 0;
}
int DepositModel(const World& w, int v) {
    return v == 1 ? w.mdl.crystalA : (v == 2 ? w.mdl.crystalB : w.mdl.crystalSmall);
}

} // namespace

// GetApplicationDirectory (raylib) liefert den Ordner der ausführbaren Datei
// mit abschließendem Schrägstrich – unabhängig davon, aus welchem Ordner das
// Spiel gestartet wurde. Ist dieser Ordner schreibgeschützt (z. B.
// installiert unter "Programme"), schlägt SaveGame fehl und es gibt keinen
// Spielstand.
std::string SavePath() { return std::string(GetApplicationDirectory()) + "mondschuerfer_spielstand.txt"; }

// Nur die Kennung prüfen (schnell, wird bei jeder Rückkehr ins Menü
// aufgerufen). Passt die Version nicht, merkt das erst LoadGame; main.cpp
// sperrt dann den Knopf "Fortsetzen" und spielt einen Fehlerton.
bool SaveExists() {
    std::ifstream f(SavePath());
    std::string magic;
    f >> magic;
    return f.good() && magic == kMagic;
}

// std::remove aus <cstdio> löscht eine Datei (nicht zu verwechseln mit dem
// Algorithmus std::remove aus <algorithm>).
void DeleteSave() { std::remove(SavePath().c_str()); }

// Schreibt den ganzen Spielstand in die Datei file (trunc = vorher leeren).
// Die Reihenfolge der Zeilen ist für LoadGame wichtig, siehe dort.
// Zahlen werden mit der Standardgenauigkeit der Streams (6 gültige Stellen)
// geschrieben – für Positionen und Zeiten in einem Spiel genau genug.
static bool WriteSaveFile(const std::string& file, const World& w) {
    std::ofstream f(file, std::ios::trunc);
    if (!f) return false;
    // Einzelspieler: eigenes Schiff ist ships[0], eigener Spieler players[0]
    const Ship& p = w.Player();
    const PlayerState& me = w.Me();
    f << kMagic << ' ' << kVersion << '\n';
    f << "seed " << w.seed << '\n';
    f << "balance " << me.balance << '\n';
    f << "delivered " << w.totalDelivered << '\n';
    f << "upgrades " << me.upgrades[0] << ' ' << me.upgrades[1] << ' ' << me.upgrades[2] << ' ' << me.upgrades[3] << ' ' << me.upgrades[4] << '\n';
    f << "lives " << me.lives << '\n';
    // Schiffstyp (vor "player", denn dort werden die Schiffswerte berechnet)
    // und geparkte Schiffe: hangar Typ Stufe Stufe Stufe Stufe Stufe Schild
    f << "shiptype " << me.shipType << '\n';
    for (const OwnedShip& o : me.hangar)
        f << "hangar " << o.type << ' ' << o.upgrades[0] << ' ' << o.upgrades[1] << ' ' << o.upgrades[2] << ' '
          << o.upgrades[3] << ' ' << o.upgrades[4] << ' ' << o.hp << '\n';
    // nextId: nächste freie ID für Schiffe, Lagerstätten und Wracks. Wird
    // mitgespeichert, damit nach dem Laden neu entstehende Objekte keine ID
    // bekommen, die eine gespeicherte Lagerstätte oder ein Wrack schon hat.
    f << "stats " << me.playTime << ' ' << me.kills << ' ' << w.nextId << '\n';
    f << "scanner " << me.scanEnergy << '\n';
    f << "player " << p.pos.x << ' ' << p.pos.z << ' ' << p.yaw << ' ' << p.hp << ' ' << p.cargo << '\n';
    // deposit: id x z Drehung Größe Variante Restmenge Anfangsmenge entdeckt(0/1)
    for (const auto& d : w.deposits)
        f << "deposit " << d.id << ' ' << d.pos.x << ' ' << d.pos.z << ' ' << d.yaw << ' ' << d.scale << ' '
          << DepositVariant(w, d.model) << ' ' << d.amount << ' ' << d.initial << ' ' << (d.revealed ? 1 : 0) << '\n';
    // wreck: id x z Drehung Neigung Variante Restmenge Anfangsmenge Alter
    for (const auto& wr : w.wrecks)
        f << "wreck " << wr.id << ' ' << wr.pos.x << ' ' << wr.pos.z << ' ' << wr.yaw << ' ' << wr.tilt << ' '
          << ShipVariant(w, wr.model) << ' ' << wr.amount << ' ' << wr.initial << ' ' << wr.age << '\n';
    // ship: Art Variante x z Drehung Schild maxSchild Tempo Schaden Schussabstand
    //       Ladung maxLadung feindlich(0/1) angedockt(0/1)
    // Ab Index 1, denn ships[0] ist das eigene Schiff (Zeile "player").
    // Abstürzende und zerstörte Schiffe werden weggelassen – ihre Wracks
    // entstehen erst beim Aufschlag, das Schiff wäre sonst nach dem Laden
    // wieder heil.
    // Gespeichert werden die tatsächlichen Werte (Schild, Tempo, Schaden ...),
    // nicht nur die Art: Neue Piraten werden stärker, je mehr insgesamt
    // abgeliefert wurde (PirateTier in World::MakeShip). Ein alter Pirat soll
    // nach dem Laden nicht plötzlich die Werte eines neuen bekommen.
    for (size_t i = 1; i < w.ships.size(); ++i) {
        const Ship& s = w.ships[i];
        if (s.dead || s.crashing) continue;
        f << "ship " << static_cast<int>(s.kind) << ' ' << ShipVariant(w, s.model) << ' ' << s.pos.x << ' ' << s.pos.z
          << ' ' << s.yaw << ' ' << s.hp << ' ' << s.maxHp << ' ' << s.maxSpeed << ' ' << s.damage << ' '
          << s.fireInterval << ' ' << s.cargo << ' ' << s.cargoMax << ' ' << (s.hostileToPlayer ? 1 : 0) << ' '
          << (s.hidden ? 1 : 0) << '\n';
    }
    // "end" markiert eine vollständig geschriebene Datei (LoadGame prüft das
    // derzeit nicht, es liest einfach bis zum Dateiende).
    f << "end\n";
    // Ein Stream wird "falsch", wenn ein Schreibvorgang fehlgeschlagen ist
    // (z. B. Datenträger voll). close() übergibt den Rest des Puffers an das
    // Betriebssystem; auch dabei kann noch etwas schiefgehen.
    f.close();
    return static_cast<bool>(f);
}

// Spielstand speichern. Simulation (Autopilot) wird nie gespeichert, ein
// beendetes Spiel auch nicht (main.cpp löscht den Spielstand nach
// Sieg/Absturz). Ob das Schiff gerade abstürzt, prüft der Aufrufer
// (App::OnExit).
// Sicher speichern: erst eine Hilfsdatei ".tmp" vollständig schreiben, dann
// umbenennen. Bricht das Schreiben mittendrin ab (Spiel abgestürzt, Platte
// voll), bleibt der alte Spielstand unversehrt, statt halb überschrieben zu sein.
bool SaveGame(const World& w) {
    if (w.simulation || w.outcome != Outcome::None) return false;
    const std::string path = SavePath();
    const std::string tmp = path + ".tmp";
    if (!WriteSaveFile(tmp, w)) {
        std::remove(tmp.c_str());
        return false;
    }
    // Linux/macOS: rename ersetzt die alte Datei in einem Schritt. Unter
    // Windows schlägt rename fehl, wenn es das Ziel schon gibt: dann die alte
    // Datei löschen und noch einmal versuchen.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    return true;
}

// Laden in zwei Durchgängen durch die Datei:
//   1. Nur die Zeile "seed" suchen und damit World::NewGame aufrufen. Das
//      erzeugt dieselbe Mondoberfläche und dieselben Stationen wie beim
//      ursprünglichen Spielstart und setzt alles andere auf Anfang.
//      Was NewGame zusätzlich erzeugt (neue Lagerstätten, Piraten, Händler,
//      Begrüßungsmeldungen), wird gleich wieder entfernt – nur das eigene
//      Schiff (ships[0]) und der eigene Spieler (players[0]) bleiben.
//   2. Zurück an den Anfang und alle Zeilen der Reihe nach übernehmen.
// Annahmen über die Reihenfolge (so schreibt sie SaveGame):
//   - "upgrades" vor "player": Bei "player" werden mit den Aufrüstungen die
//     Schiffswerte berechnet (ApplyUpgrades).
//   - "stats" (mit nextId) vor "ship": MakeShip vergibt neue IDs ab nextId.
// Unbekannte Schlüsselwörter (auch "end") werden übersprungen.
bool LoadGame(World& w) {
    std::ifstream f(SavePath());
    if (!f) return false;
    std::string magic;
    int version = 0;
    f >> magic >> version;
    if (magic != kMagic || version != kVersion) return false;

    // Rest der ersten Zeile (den Zeilenumbruch) überspringen, damit die
    // Schleifen unten mit der Zeile "seed" beginnen.
    std::string line;
    std::getline(f, line);
    unsigned int seed = 1;
    // Erst den Seed lesen, damit die Mondoberfläche identisch erzeugt wird
    std::streampos start = f.tellg();
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "seed") { ls >> seed; break; }
    }
    // Ab hier wird w verändert. NewGame setzt die Welt komplett zurück
    // (*this = World{}), also sind auch Wracks und Geschosse leer.
    w.NewGame(seed, false);
    w.deposits.clear();
    w.ships.resize(1);
    w.Me().notices.clear();

    // Zweiter Durchgang. clear() ist nötig, weil der erste Durchgang den
    // Stream evtl. bis zum Dateiende gelesen hat (eof-Bit gesetzt); ohne
    // clear würden seekg und getline nichts mehr tun.
    f.clear();
    f.seekg(start);
    Ship& p = w.Player();
    PlayerState& me = w.Me();
    while (std::getline(f, line)) {
        // Jede Zeile einzeln mit einem istringstream zerlegen: Fehlt in einer
        // Zeile ein Wert, betrifft das nur diese Zeile und nicht den Rest.
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "balance") ls >> me.balance;
        else if (key == "delivered") ls >> w.totalDelivered;
        else if (key == "upgrades") { me.upgrades[4] = 0; ls >> me.upgrades[0] >> me.upgrades[1] >> me.upgrades[2] >> me.upgrades[3] >> me.upgrades[4]; }
        else if (key == "lives") { ls >> me.lives; me.lives = std::clamp(me.lives, 1, limits::MAX_LIVES); }
        else if (key == "shiptype") ls >> me.shipType;   // begrenzt ApplyUpgrades
        else if (key == "hangar") {
            // Typ, Stufen, Schild. Alte Spielstände haben 4 Stufen (ohne
            // Kanonen), neue 5: am Anzahl der Werte unterscheiden.
            std::vector<float> v;
            for (float x; ls >> x;) v.push_back(x);
            if (v.size() == 6 || v.size() == 7) {
                OwnedShip o;
                const int n = static_cast<int>(v.size()) - 2;
                o.type = std::clamp(static_cast<int>(v[0]), 0, cfg::SHIP_TYPE_COUNT - 1);
                for (int u = 0; u < n; ++u) o.upgrades[u] = std::clamp(static_cast<int>(v[1 + u]), 0, cfg::UPGRADE_MAX[u]);
                o.hp = v.back();
                if (!w.OwnsShipType(me, o.type)) me.hangar.push_back(o);
            }
        }
        else if (key == "stats") ls >> me.playTime >> me.kills >> w.nextId;
        else if (key == "scanner") ls >> me.scanEnergy;
        else if (key == "player") {
            float hp = 0;
            ls >> p.pos.x >> p.pos.z >> p.yaw >> hp >> p.cargo;
            // Stufen begrenzen: Eine von Hand veränderte Datei darf nicht über
            // das Ende der Wertetabellen in Config.hpp hinaus lesen lassen.
            for (int i = 0; i < cfg::UP_COUNT; ++i) me.upgrades[i] = std::clamp(me.upgrades[i], 0, cfg::UPGRADE_MAX[i]);
            // Tempo, max. Schild, Schaden und Laderaum aus den Stufen
            // berechnen. refill = true füllt den Schild auf das Maximum,
            // deshalb wird der gespeicherte Schild erst danach gesetzt.
            w.ApplyUpgrades(me, true);
            p.hp = hp;
        } else if (key == "deposit") {
            Deposit d;
            int v = 0, rev = 0;
            ls >> d.id >> d.pos.x >> d.pos.z >> d.yaw >> d.scale >> v >> d.amount >> d.initial >> rev;
            d.model = DepositModel(w, v);
            d.revealed = rev != 0;
            // Einblend-Animation schon abgeschlossen: entdeckte Lagerstätten
            // sind nach dem Laden sofort voll sichtbar.
            d.revealT = 1.0f;
            w.deposits.push_back(d);
        } else if (key == "wreck") {
            Wreck wr;
            int v = 0;
            ls >> wr.id >> wr.pos.x >> wr.pos.z >> wr.yaw >> wr.tilt >> v >> wr.amount >> wr.initial >> wr.age;
            wr.model = ShipModel(w, v);
            // gleiche Höhe wie beim Entstehen eines Wracks (World::Update,
            // Aufschlag): liegt knapp über dem Boden
            wr.pos.y = 0.15f;
            w.wrecks.push_back(wr);
        } else if (key == "ship") {
            int kind = 0, v = 0, hostile = 0, hidden = 0;
            Ship s;
            ls >> kind >> v >> s.pos.x >> s.pos.z >> s.yaw >> s.hp >> s.maxHp >> s.maxSpeed >> s.damage >>
                s.fireInterval >> s.cargo >> s.cargoMax >> hostile >> hidden;
            // Nur fremde Schiffe (ShipKind 1 = Pirat, 2 = Elite, 3 = Händler).
            // Ein zweites Spielerschiff (0) oder Unsinn wird ignoriert.
            if (kind < 1 || kind > 3) continue;
            // MakeShip liefert ein vollständiges Schiff dieser Art (Fraktion,
            // Wendigkeit, neue ID ab nextId ...); danach werden die
            // gespeicherten Werte darübergeschrieben. MakeShip würfelt dabei
            // Modell und Werte aus – das ist egal, sie werden ja ersetzt.
            Ship base = w.MakeShip(static_cast<ShipKind>(kind), s.pos);
            base.model = ShipModel(w, v);
            base.yaw = s.yaw;
            base.hp = s.hp;
            base.maxHp = s.maxHp;
            base.maxSpeed = s.maxSpeed;
            base.damage = s.damage;
            base.fireInterval = s.fireInterval;
            base.cargo = s.cargo;
            base.cargoMax = s.cargoMax;
            base.hostileToPlayer = hostile != 0;
            // Der KI-Zustand wird nicht gespeichert. Neu beginnen mit dem
            // Grundverhalten: Piraten streifen umher, Händler fliegen zur
            // nächsten Lagerstätte (shared/Ai.cpp).
            base.ai = base.faction == Faction::Pirate ? AiState::Wander : AiState::GoMine;
            // Ein Händler, der beim Speichern in einer Station angedockt war
            // (unsichtbar), wartet noch 3 s und fliegt dann wieder los. Seine
            // Station ist nicht gespeichert; die KI nimmt dann die nächste
            // Handelsstation (AiState::Docked in Ai.cpp).
            if (hidden && base.kind == ShipKind::Miner) {
                base.hidden = true;
                base.ai = AiState::Docked;
                base.aiTimer = 3.0f;
            }
            w.ships.push_back(base);
        }
    }
    // Das eigene Schiff auf Flughöhe und in Ruhe
    p.pos.y = cfg::SHIP_ALTITUDE;
    p.vel = {0, 0, 0};
    // Aktuelle Ausbaustufe der Stationen als "schon erreicht" merken. NewGame
    // hat lastStage für totalDelivered = 0 berechnet; ohne diese Zeile käme
    // beim nächsten Verkauf die Meldung "Die Station wächst!" samt Fanfare
    // für eine Stufe, die längst erreicht war (World::Dock).
    w.lastStage = w.StationStage();
    w.Notify("Spielstand geladen. Weiter geht's!", Color{200, 220, 255, 255});
    return true;
}
