#pragma once
// ---------------------------------------------------------------------------
// Config.hpp: alle Balancing-Werte an einer Stelle. Hier kann das Spiel
// angepasst werden.
//
// Wofür? Zahlen wie "wie schnell fliegt ein Schiff", "was kostet die nächste
// Aufrüstung" oder "wie viele Piraten gibt es" sollen nicht quer über den
// Code verstreut sein. Wer das Spiel leichter oder schwerer machen will,
// ändert sie hier.
//
// Wer benutzt die Datei?
//   - Spiellogik (shared/World.cpp, WorldGen.cpp, Ai.cpp): die eigentlichen
//     Regeln (Bewegung, Scanner, Abbau, Kampf, Stationen, Gegner).
//   - Client (src/Hud.cpp, Render.cpp, Multiplayer.cpp, NetClient.cpp,
//     Save.cpp): Anzeigen (z. B. "Tempo 16 -> 19" in der Werft), Scannerring,
//     Karte, Vorhersage des eigenen Schiffs im Mehrspieler.
//   - Server (server/src/GameServer.cpp, Accounts.cpp, LoadTest.cpp):
//     Fortschritt speichern/prüfen, Snapshots füllen, Bots steuern.
//
// Abhängigkeiten: keine. Nur constexpr-Konstanten (zur Übersetzungszeit
// bekannt, kein Speicher, keine Linker-Probleme, auch im Server ohne raylib
// verwendbar).
//
// Einheiten: Längen in Welt-Einheiten (etwa Meter; die Karte ist 600 x 600
// groß), Zeiten in Sekunden, Geschwindigkeiten in Einheiten pro Sekunde.
// "Punkte" = Ressourcenpunkte: 1 Einheit Ladung bringt beim Verkauf genau
// 1 Punkt auf das Konto (World::Dock).
//
// WICHTIG für den Mehrspieler: Client und Server rechnen mit DIESEN Werten.
// Der Client sagt z. B. die Bewegung des eigenen Schiffs mit SPEED_VALUES
// voraus (NetClient.cpp). Sind die Werte auf beiden Seiten verschieden,
// springt das Schiff ständig zurück, und das HUD zeigt falsche Zahlen. Die
// Protokollversion (Protocol.hpp) prüft das NICHT: Nach einer Änderung hier
// also Client und Server gemeinsam neu bauen.
//
// Nicht alles steht hier: Werte der Piraten/Händler (Schild, Tempo, Ladung je
// Stufe) stehen in World::MakeShip (World.cpp), die Geschütztürme (Schaden 14,
// 0,55 s Nachladezeit) in World::UpdateTurrets.
// ---------------------------------------------------------------------------

namespace cfg {

// Spielziel: so viele Ressourcenpunkte muss der Kontostand erreichen.
// Achtung: Die Zahl steht zusätzlich als Text in Meldungen ("Ziel: 100.000
// Ressourcenpunkte", World.cpp und GameServer.cpp) und im HUD (Hud.cpp).
// Bei einer Änderung dort mit anpassen.
constexpr int GOAL_POINTS = 100000;

// Welt
constexpr float WORLD_HALF = 300.0f;        // Spielfeld geht von -WORLD_HALF bis +WORLD_HALF (x und z), also 600 x 600
// Flughöhe über der Mondoberfläche (y). Alle Schiffe fliegen in dieser Höhe,
// die Spiellogik ist also im Grunde zweidimensional (x/z). Stationsteile, die
// höher als SHIP_ALTITUDE - 0,6 sind, werden zu Hindernissen (WorldGen.cpp).
constexpr float SHIP_ALTITUDE = 3.2f;

// Scanner (Taste E)
// Voll geladen reicht der Scanner 10 s (MAX_ENERGY / DRAIN). Ausgeschaltet
// lädt er sich in 10 s wieder voll auf. Einschalten geht erst ab 1 Einheit.
constexpr float SCANNER_MAX_ENERGY = 10.0f; // 10 Sekunden Betrieb
constexpr float SCANNER_DRAIN = 1.0f;       // Einheiten pro Sekunde im Betrieb
constexpr float SCANNER_REGEN = 1.0f;       // Einheiten pro Sekunde, wenn aus
// Ein Scannerring wächst in SCANNER_PULSE_TIME Sekunden von 0 auf
// SCANNER_RADIUS und beginnt dann von vorn (mit "Ping"). Lagerstätten, die
// der Ring überstreicht, gelten für diesen Spieler als entdeckt.
// SCANNER_RADIUS ist der Grundwert; jeder Schiffstyp hat einen Faktor darauf
// (SHIP_TYPES unten, das Startschiff 50 %).
constexpr float SCANNER_RADIUS = 34.0f;
constexpr float SCANNER_PULSE_TIME = 1.25f;

// Abbau (Taste F)
// Ein Abbauvorgang dauert MINING_TIME. Am Ende wird auf einen Schlag so viel
// geladen, wie ins Lager passt (höchstens der Rest der Lagerstätte).
constexpr float MINING_TIME = 5.0f;         // Sekunden, bis das Lager gefüllt ist
// Höchstabstand zum Starten. Abgebrochen wird erst ab MINING_RANGE + 1,5,
// damit leichtes Abdriften während des Abbaus nicht sofort stört.
constexpr float MINING_RANGE = 6.5f;

// Andocken: Abstand zur Stationsmitte (auf dem Boden gemessen), ab dem F
// andockt. Stationen haben dabei Vorrang vor Lagerstätten und Wracks.
constexpr float DOCK_RANGE = 15.0f;

// Drehgeschwindigkeit des Spielerschiffs in rad/s (2,9 rad/s ≈ 166 Grad pro
// Sekunde). An einer Stelle festgelegt, weil Server (World::MakeShip) und
// Client-Vorhersage (NetClient) exakt denselben Wert brauchen; sonst würde
// das vorhergesagte Schiff bei jeder Drehung vom Server abweichen.
constexpr float PLAYER_TURN_RATE = 2.9f;

// Waffen
// PLAYER_FIRE_INTERVAL und PLAYER_DAMAGE werden derzeit nirgends benutzt: Die
// echten Werte des Spielerschiffs kommen aus WEAPON_RATE[0] / WEAPON_DAMAGE[0]
// (siehe Aufrüstungen unten, World::ApplyUpgrades). Sie sind gleich groß.
constexpr float PLAYER_FIRE_INTERVAL = 0.5f; // 2 Schuss pro Sekunde
constexpr float PLAYER_DAMAGE = 10.0f;
// Reichweite eines Schusses: 55 * 1,05 ≈ 58 Einheiten (dazu kommen 40 % der
// Schiffsgeschwindigkeit, World::FireBullet). Die KI zielt mit Vorhalt und
// rechnet dafür ebenfalls mit BULLET_SPEED (Ai.cpp).
constexpr float BULLET_SPEED = 55.0f;
constexpr float BULLET_LIFE = 1.05f;        // Sekunden, bis ein Geschoss verschwindet
constexpr float SHIP_HIT_RADIUS = 1.7f;     // Treffer, wenn das Geschoss so nah an der Schiffsmitte ist (3D-Abstand)

// Reparatur an der Handelsstation: Punkte pro Schildpunkt.
// Beispiel: 100er-Schild ganz leer -> 200 Punkte. Reicht das Geld nicht,
// wird so weit repariert, wie es reicht (World::Repair).
constexpr int REPAIR_COST_PER_POINT = 2;

// Upgrades: Index 0 = Startwert, danach je Stufe (1..UPGRADE_LEVELS).
// Die Tabellen haben deshalb UPGRADE_LEVELS + 1 Einträge, und die Stufe eines
// Spielers (PlayerState::upgrades[typ]) dient direkt als Index. Darum wird sie
// überall, wo sie von außen kommt (Spielstand, Konto, Netzwerk), auf
// 0..UPGRADE_LEVELS begrenzt.
constexpr int UPGRADE_LEVELS = 5;
// Die Zahlenwerte der Aufzählung sind Teil von Netzwerkprotokoll
// (DockAction-Parameter), Kontodatei und Spielstand: Reihenfolge nicht ändern.
enum Upgrade { UP_SPEED = 0, UP_ARMOR = 1, UP_WEAPON = 2, UP_CARGO = 3, UP_COUNT = 4 };

// Höchstgeschwindigkeit (Einheiten/s). Die Beschleunigung ist 1,9 x dieser
// Wert (World::UpdateShipPhysics): ein schnelleres Schiff zieht also auch
// kräftiger an. Rückwärts geht es mit halbem Tempo. Zum Vergleich: Die Karte
// ist 600 breit, mit Tempo 16 also knapp 40 s von Rand zu Rand.
constexpr float SPEED_VALUES[UPGRADE_LEVELS + 1]  = {16.0f, 19.0f, 22.0f, 25.0f, 28.5f, 32.0f};
// Schild (Ship::maxHp). Piraten treffen mit 10: Startschild hält 10 Treffer,
// höchste Stufe 42 Treffer.
constexpr float ARMOR_VALUES[UPGRADE_LEVELS + 1]  = {100.0f, 150.0f, 200.0f, 260.0f, 330.0f, 420.0f};
// Schaden pro Treffer und Sekunden zwischen zwei Schüssen. Schaden pro
// Sekunde: Start 10 / 0,5 = 20, höchste Stufe 40 / 0,3 ≈ 133. Ein normaler
// Pirat (anfangs 40 Schild) braucht am Anfang also 4 Treffer.
constexpr float WEAPON_DAMAGE[UPGRADE_LEVELS + 1] = {10.0f, 14.0f, 19.0f, 25.0f, 32.0f, 40.0f};
constexpr float WEAPON_RATE[UPGRADE_LEVELS + 1]   = {0.50f, 0.45f, 0.40f, 0.36f, 0.33f, 0.30f};
// Lagerkapazität (Ship::cargoMax) = so viele Punkte bringt eine volle Ladung.
// Mit Startlager 300 bräuchte man über 330 Fahrten bis zum Ziel, mit dem
// größten Lager (4000) nur noch 25. Das Lager ist deshalb meist die
// lohnendste erste Aufrüstung und zugleich die billigste (UPGRADE_COST).
constexpr int   CARGO_VALUES[UPGRADE_LEVELS + 1]  = {300, 600, 1000, 1600, 2500, 4000};

// Preis der NÄCHSTEN Stufe: UPGRADE_COST[typ][aktuelle Stufe].
// Bezahlt wird vom Kontostand, also von demselben Geld, das für das Spielziel
// zählt. Alles zusammen kostet 108.000 Punkte: Aufrüsten macht schneller
// reich, wirft den Kontostand aber erst einmal zurück.
constexpr int UPGRADE_COST[UP_COUNT][UPGRADE_LEVELS] = {
    {800, 2000, 4000, 7000, 12000},    // Antrieb   (zusammen 25.800)
    {1000, 2500, 5000, 8000, 13000},   // Panzerung (zusammen 29.500)
    {1000, 2500, 5000, 8000, 13000},   // Bewaffnung (zusammen 29.500)
    {600, 1600, 3500, 6500, 11000},    // Lager     (zusammen 23.200)
};

// Schiffstypen (Raumschiffsverkäufer)
// Typ 0 ist das Startschiff. Die anderen kauft man beim Raumschiffsverkäufer.
// Die Werte sind Faktoren auf die Aufrüstungstabellen oben (Stufe 0 des
// Startschiffs = 1,0): Tempo = SPEED_VALUES[Stufe] * speed, Schaden =
// WEAPON_DAMAGE[Stufe] * weapon, Scanner = SCANNER_RADIUS * scanner. Schild
// und Lager sind die Werte auf Stufe 0; Aufrüstungen wachsen im selben
// Verhältnis wie beim Startschiff (ARMOR_VALUES bzw. CARGO_VALUES).
// Jedes Schiff hat seine eigenen Aufrüstungen (sie lassen sich nicht übertragen).
// Die Zahlenwerte sind Teil von Netzwerkprotokoll, Kontodatei und Spielstand:
// Reihenfolge nicht ändern.
enum ShipTypeId { SHIP_START = 0, SHIP_CARGO = 1, SHIP_HUNTER = 2, SHIP_SPEEDSTER = 3, SHIP_TYPE_COUNT = 4 };
struct ShipTypeDef {
    const char* name;
    const char* model;   // Modellname aus assets/models
    float shield;        // Schild auf Stufe 0
    float scanner;       // Faktor auf SCANNER_RADIUS
    float speed;         // Faktor auf SPEED_VALUES
    float weapon;        // Faktor auf WEAPON_DAMAGE
    int cargo;           // Lager auf Stufe 0
    int cost;            // Kaufpreis in Punkten
};
// Für Hunter und Speedster ist kein Antrieb vorgegeben: Hunter wie das
// Startschiff, Speedster etwas schneller.
constexpr ShipTypeDef SHIP_TYPES[SHIP_TYPE_COUNT] = {
    {"Startschiff", "craft_speederA", 100.0f, 0.5f, 1.0f, 1.0f, 300, 5000},
    {"The Cargo", "craft_cargoA", 120.0f, 1.5f, 0.8f, 1.2f, 1200, 20000},
    {"Hunter", "craft_miner", 80.0f, 0.3f, 1.0f, 1.6f, 300, 15000},
    {"Speedster", "craft_speederD", 60.0f, 0.6f, 1.3f, 0.8f, 150, 10000},
};
// In Zahlung geben: so viel Prozent von Kaufpreis und bezahlten
// Aufrüstungen des alten Schiffs werden gutgeschrieben.
constexpr int TRADE_IN_PERCENT = 50;

// Werte eines Schiffstyps auf einer Aufrüstungsstufe (Server, Client und
// Vorhersage rechnen damit, siehe World::ApplyUpgrades)
constexpr float ShipSpeed(int type, int lvl) { return SPEED_VALUES[lvl] * SHIP_TYPES[type].speed; }
constexpr float ShipArmor(int type, int lvl) { return SHIP_TYPES[type].shield * ARMOR_VALUES[lvl] / ARMOR_VALUES[0]; }
constexpr float ShipDamage(int type, int lvl) { return WEAPON_DAMAGE[lvl] * SHIP_TYPES[type].weapon; }
constexpr int ShipCargo(int type, int lvl) { return SHIP_TYPES[type].cargo * CARGO_VALUES[lvl] / CARGO_VALUES[0]; }
constexpr float ShipScanner(int type) { return SCANNER_RADIUS * SHIP_TYPES[type].scanner; }

// Station wächst mit der Gesamtmenge abgelieferter Ressourcen
// (World::totalDelivered). Stufe i ist erreicht, sobald insgesamt
// STATION_THRESHOLDS[i] Punkte abgeliefert wurden. Mit jeder Stufe kommen
// Bauteile und Geschütztürme an den Handelsstationen dazu (WorldGen.cpp).
// Im Einzelspieler zählt nur das eigene Abliefern, die letzte Stufe fällt
// deshalb ungefähr mit dem Spielziel zusammen. Im Mehrspieler zählen alle
// Spieler zusammen (und der Wert bleibt auf dem Server gespeichert).
constexpr int STATION_STAGES = 8; // Stufe 0..7
constexpr int STATION_THRESHOLDS[STATION_STAGES] = {0, 1500, 5000, 12000, 25000, 45000, 70000, 100000};

// Lagerstätten
// DEPOSIT_COUNT: so viele liegen im Einzelspieler auf der Karte (Sollwert;
// abgebaute werden nachgeliefert). Im Mehrspieler siehe MP_DEPOSITS_* unten.
constexpr int DEPOSIT_COUNT = 48;
// Mittlerer Abstand in Sekunden, bis eine neue Lagerstätte erscheint, solange
// weniger als der Sollwert da sind (zufällig 0,6- bis 1,2-fach, also 15–30 s).
// Im Mehrspieler geteilt durch (Spielerzahl / 2), mindestens durch 1.
constexpr float DEPOSIT_RESPAWN = 25.0f;

// Gegner
// Einzelspieler: Es beginnt mit PIRATES_BASE Piraten, je 12.000 abgelieferten
// Punkten kommt einer dazu, höchstens PIRATES_MAX (erreicht bei 84.000).
// Mehrspieler: feste Zahl aus den Server-Einstellungen (GameSettings::enemyShips).
constexpr int PIRATES_BASE = 4;
constexpr int PIRATES_MAX = 11;
constexpr int MINERS_COUNT = 4;             // neutrale Händler/Schürfer (in beiden Modi)
// Bis zu diesem Abstand bemerkt ein Pirat ein Ziel. Wurde er gerade beschossen,
// verfolgt er seinen Angreifer bis 70 Einheiten weit (Ai.cpp).
constexpr float PIRATE_AGGRO = 30.0f;
constexpr float PIRATE_DAMAGE = 10.0f;      // 10 Treffer = 100 Schild
constexpr float ELITE_DAMAGE = 10.0f;      // auch Elite-Piraten: 10 Treffer = 100 Schild
// Schutzzone um JEDE Station: Piraten drehen davor ab und entstehen
// mindestens 25 Einheiten weiter draußen. Die Türme schießen etwas weiter
// (TURRET_RANGE > STATION_SAFE_RADIUS), damit auch Piraten am Rand der Zone
// noch getroffen werden.
constexpr float STATION_SAFE_RADIUS = 34.0f; // Geschütztürme schützen die Stationen
constexpr float TURRET_RANGE = 36.0f;

// Mehrspieler (nur Server)
constexpr float MP_RESPAWN_TIME = 4.0f;       // Sekunden bis zum neuen Raumschiff nach einem Absturz
// Lagerstätten-Sollwert = DEPOSIT_COUNT + (Spieler - 1) * MP_DEPOSITS_PER_PLAYER,
// höchstens MP_DEPOSITS_MAX (erreicht ab 25 Spielern). Die Obergrenze hält
// die Karte übersichtlich und die Lagerstätten-Liste klein, die der Server
// bei jeder Änderung komplett an die Clients schickt.
constexpr int MP_DEPOSITS_PER_PLAYER = 8;     // zusätzliche Lagerstätten je weiterem Spieler
constexpr int MP_DEPOSITS_MAX = 240;          // Obergrenze für Lagerstätten auf der Karte
// Neues Schiff erscheint auf einem Kreis um eine zufällige Handelsstation.
// 24 ist auch der Startabstand im Einzelspieler (World::NewGame): außerhalb
// von DOCK_RANGE, aber noch im Schutz der Geschütztürme.
constexpr float MP_SPAWN_DISTANCE = 24.0f;    // Abstand des neuen Schiffs zur Handelsstation

} // namespace cfg
