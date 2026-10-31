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
// Kontostand beim Start (neues Spiel, neuer Mehrspieler-Spieler, nach dem
// letzten Leben). Zum Testen der neuen Inhalte 10.000; im normalen Spiel 0.
constexpr long long START_BALANCE = 10000;

// Welt
constexpr float WORLD_HALF = 300.0f;        // Spielfeld geht von -WORLD_HALF bis +WORLD_HALF (x und z), also 600 x 600
// Flughöhe über der Mondoberfläche (y). Alle Schiffe fliegen in dieser Höhe,
// die Spiellogik ist also im Grunde zweidimensional (x/z). Stationen sind keine
// Kollisionsobjekte, Schiffe fliegen hindurch.
constexpr float SHIP_ALTITUDE = 3.2f;

// Kartenanimation (shared/MapAnim.cpp, nur Kulisse ohne Einfluss aufs Spiel)
constexpr int MAP_ROVERS = 14;            // zusätzliche fahrende Mondfahrzeuge in der Zufallskarte
constexpr int MAP_GATES = 5;              // Tore in der Zufallskarte; jedes leuchtet einmal pro Minute auf
constexpr float TRACK_TIME_MAX = 300.0f;  // längste einstellbare Dauer der Fahrspuren in Sekunden (Optionen, Seite "Grafik")

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
enum Upgrade { UP_SPEED = 0, UP_ARMOR = 1, UP_WEAPON = 2, UP_CARGO = 3, UP_GUNS = 4, UP_COUNT = 5 };
// Höchste Stufe je Upgrade (die Kanonen höchstens MAX_GUNS - 1 Stufen, alle anderen UPGRADE_LEVELS).
// Die Stufe dient trotzdem als Index in Tabellen mit UPGRADE_LEVELS + 1 Einträgen.
// Wie viele Kanonen ein Schiff wirklich bekommt, legt sein Schiffstyp fest
// (ShipType::guns, ShipUpgradeMax in GameSettings.hpp).
constexpr int MAX_GUNS = 5;
constexpr int UPGRADE_MAX[UP_COUNT] = {UPGRADE_LEVELS, UPGRADE_LEVELS, UPGRADE_LEVELS, UPGRADE_LEVELS, MAX_GUNS - 1};

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
// Anzahl gleichzeitiger Schüsse (Ship::guns) je Kanonen-Stufe, höchstens
// ShipType::guns des Schiffs. Ausrichtung (World::FireBullet): 2 parallel
// nach vorn; 3 = Mitte und je GUN_ANGLE_INNER Grad links/rechts; 4 = zwei
// parallel und je GUN_ANGLE_INNER Grad; 5 = Mitte, je GUN_ANGLE_INNER und je
// GUN_ANGLE_OUTER Grad.
constexpr int   GUN_COUNT[UPGRADE_LEVELS + 1]     = {1, 2, 3, 4, 5, 5};
constexpr float GUN_ANGLE_INNER = 3.0f;
constexpr float GUN_ANGLE_OUTER = 5.0f;
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
    {4000, 9000, 15000, 22000, 0},     // Kanonen   (höchstens 4 Stufen, zusammen 50.000)
};

// Schiffstypen (Raumschiffsverkäufer)
// Typ 0 ist das Startschiff. Die anderen kauft man beim Raumschiffsverkäufer.
// Die Prozentwerte beziehen sich auf die Aufrüstungstabellen oben (Stufe 0
// des Startschiffs = 100 %): Tempo = SPEED_VALUES[Stufe] * speed %, Schaden =
// WEAPON_DAMAGE[Stufe] * weapon %, Scanner = SCANNER_RADIUS * scanner %.
// Schild und Lager sind die Werte auf Stufe 0; Aufrüstungen wachsen im selben
// Verhältnis wie beim Startschiff (ARMOR_VALUES bzw. CARGO_VALUES).
// guns = so viele Kanonen lassen sich höchstens kaufen (1..MAX_GUNS).
// Jedes Schiff hat seine eigenen Aufrüstungen (sie lassen sich nicht übertragen).
// Das hier sind die Standardwerte (Einzelspieler und neuer Server). Im
// Mehrspieler stellt der Betreiber sie auf der Verwaltungsseite ein und kann
// weitere Schiffe hinzufügen (GameSettings::ships, ShipType in GameSettings.hpp).
// Die Nummern sind Teil von Netzwerkprotokoll, Kontodatei und Spielstand:
// Reihenfolge nicht ändern.
enum ShipTypeId { SHIP_START = 0, SHIP_CARGO = 1, SHIP_HUNTER = 2, SHIP_SPEEDSTER = 3, SHIP_TYPE_COUNT = 4 };
struct ShipTypeDef {
    const char* name;
    const char* model;   // Modellname aus assets/models
    int shield;          // Schild auf Stufe 0
    int scanner;         // Prozent von SCANNER_RADIUS
    int speed;           // Prozent von SPEED_VALUES
    int weapon;          // Prozent von WEAPON_DAMAGE
    int cargo;           // Lager auf Stufe 0
    int cost;            // Kaufpreis in Punkten
    int guns;            // höchstens so viele Kanonen
};
// Für Hunter und Speedster ist kein Antrieb vorgegeben: Hunter wie das
// Startschiff, Speedster etwas schneller.
constexpr ShipTypeDef SHIP_TYPES[SHIP_TYPE_COUNT] = {
    {"Startschiff", "craft_speederA", 100, 50, 100, 100, 300, 5000, 3},
    {"The Cargo", "craft_cargoA", 120, 150, 80, 120, 1200, 20000, 3},
    {"Hunter", "craft_miner", 80, 30, 100, 160, 300, 15000, 3},
    {"Speedster", "craft_speederD", 60, 60, 130, 80, 150, 10000, 3},
};
// In Zahlung geben: so viel Prozent von Kaufpreis und bezahlten
// Aufrüstungen des alten Schiffs werden gutgeschrieben.
constexpr int TRADE_IN_PERCENT = 50;

// Leben: So viele Abstürze verträgt ein Spieler. Einzelspieler fest, im
// Mehrspieler der Standard (GameSettings::lives, einstellbar bis limits::MAX_LIVES).
constexpr int START_LIVES = 3;

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

// Mini-Missionen (Handelsstation, Tab "Missionen", World.cpp Abschnitt "Missionen")
// So viele Missionen gibt es gleichzeitig auf der Karte (Angebote und
// laufende zusammen). Einzelspieler fest, im Mehrspieler der Standard
// (GameSettings::missions, einstellbar bis limits::MAX_MISSIONS).
constexpr int MISSION_SLOTS = 3;
constexpr float MISSION_RESPAWN = 20.0f;   // Sekunden nach einer erfüllten Mission bis zum neuen Angebot
constexpr int MISSION_GOAL = 3;            // Personen, Piraten bzw. Frachter je Mission
constexpr float CUE_TIME = 10.0f;          // so lange steht ein Funkspruch (Clip, Untertitel) unten rechts; länger als jeder Clip
// Konvoi: Tempo und Schild der Frachter, Umweg über offenes Gelände (die
// Stationen liegen nah beieinander und im Schutz der Geschütztürme) und
// Startzeit der beiden Piratenwellen (Sekunden seit dem Abflug, je 2 Piraten).
constexpr float CONVOY_SPEED = 8.5f;
constexpr float CONVOY_HP = 150.0f;
constexpr float CONVOY_DETOUR = 100.0f;
constexpr float CONVOY_WAVES[2] = {8.0f, 22.0f};
// Missionsarten. Index = MissionType (World.hpp); die Zahlenwerte gehen über
// das Netzwerk (SnapMe): Reihenfolge nicht ändern. Funkspruch je Phase
// (0 Start, 1 Ende, 2 Fehlschlag): Clip-Datei aus assets/clips (MPEG-1 mit
// MP2-Ton, abgespielt von src/ClipPlayer.cpp) und Untertitel.
// Leerer Untertitel = kein Funkspruch in dieser Phase.
struct MissionDef {
    const char* name;
    const char* text;      // Kurzbeschreibung im Stationsfenster
    int reward;            // Punkte, sofort aufs Konto
    const char* clip[3];
    const char* subtitle[3];
};
constexpr int MISSION_TYPE_COUNT = 3;
constexpr MissionDef MISSIONS[MISSION_TYPE_COUNT] = {
    {"Personen einsammeln", "Drei verirrte Personen abholen [F] und zu einer Handelsstation bringen.", 2000,
     {"clip_collectingpersonal_start.mpg", "clip_collectingpersonal_end.mpg", ""},
     {"Im Sektor haben sich Personen verlaufen. Hole sie die Personen ab und bringen sie uns zur Station.",
      "Danke das sie die Personen eingesammelt und gebracht haben.", ""}},
    {"Piraten abschießen", "Drei unerlaubte Piraten im Sektor abschießen.", 2500,
     {"clip_shotthem_start.mpg", "clip_shotthem_end.mpg", ""},
     {"Im Sektor befinden sich unerlaubte Piraten. Sie können sich etwas dazu verdienen, in dem sie sie abschießen.",
      "Danke, dass sie unsere Feinde erledigt haben. Sie können mit der Arbeit fortsetzen.", ""}},
    {"Konvoi verteidigen", "Drei Frachter auf dem Weg zu einer anderen Station vor Piraten schützen.", 3500,
     {"clip_convoi_start.mpg", "clip_convoi_end.mpg", "clip_convoi_fail.mpg"},
     {"Wir müssen Ware an die nächsten Station bringen. Bitte verteidigen sie den Konvoi.",
      "Die Ware ist sicher angekommen. Setzen sie nun ihre Aufgabe fort.",
      "Sie haben versagt. Setzen sie ihre vorige Arbeit fort."}},
};
// Funksprüche außerhalb der Missionen (PlayerState::cue ab CUE_NEWSTART):
// beim Beginn eines neuen Spiels und beim Fortsetzen (Einzelspieler: Knopf
// "Fortsetzen", Mehrspieler: Anmeldung mit vorhandenem Fortschritt).
constexpr int CUE_NEWSTART = MISSION_TYPE_COUNT * 3;
constexpr int CUE_RETURN = CUE_NEWSTART + 1;
constexpr int CUE_COUNT = CUE_RETURN + 1;   // gültige Werte für PlayerState::cue: 0 .. CUE_COUNT - 1
struct CueDef {
    const char* name;      // Überschrift im Funkspruch-Kasten
    const char* clip;      // Datei in assets/clips, "" = keine
    const char* subtitle;
};
constexpr CueDef GENERAL_CUES[CUE_COUNT - CUE_NEWSTART] = {
    {"Willkommen", "clip_newstart.mpg",
     "Willkommen Commander, Sammeln und liefern sie uns die Kristalle. Wenn sie genug geliefert haben, können sie den Mond verlassen."},
    {"Willkommen zurück", "clip_return.mpg",
     "Schön das sie wieder da sind. Beginnen sie mit der Arbeit und bringen sie weiter die Kristalle."},
};
// Überschrift, Clip und Untertitel des Funkspruchs cue (0 .. CUE_COUNT - 1).
constexpr CueDef CueInfo(int cue) {
    return cue >= CUE_NEWSTART ? GENERAL_CUES[cue - CUE_NEWSTART]
                               : CueDef{MISSIONS[cue / 3].name, MISSIONS[cue / 3].clip[cue % 3], MISSIONS[cue / 3].subtitle[cue % 3]};
}

// Mehrspieler (nur Server)
constexpr float MP_RESPAWN_TIME = 4.0f;      // Sekunden bis zum neuen Raumschiff nach einem Absturz
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

// Trupp zu Fuß (FPS-Modus, shared/Trooper.cpp). Längen in Metern, Tempo in m/s.
// Die Figur ist ein Drittel so groß wie ein Mensch in Weltgröße (sonst wirken
// Gebäude und Stargates neben ihr winzig).
constexpr float MOON_GRAVITY = 1.62f;         // Schwerkraft des Mondes (m/s²): Sprünge tragen weit
constexpr float TROOPER_WALK = 2.4f;          // Gehen
constexpr float TROOPER_SPRINT = 4.2f;        // Sprinten (nur vorwärts)
constexpr float TROOPER_ACCEL = 30.0f;        // so schnell folgt das Tempo am Boden der Eingabe (m/s²)
constexpr float TROOPER_AIR_CONTROL = 0.25f;  // Anteil davon in der Luft
constexpr float TROOPER_JUMP = 1.27f;         // Absprung nach oben: knapp 0,5 m hoch (fast Figurengröße), gut 1,5 s in der Luft
constexpr float TROOPER_RADIUS = 0.13f;       // Kollision mit Felsen und Stationsteilen
constexpr float TROOPER_HEIGHT = 0.6f;        // Teile, die höher anfangen, gehen über den Kopf hinweg
constexpr float TROOPER_EYE = 0.55f;          // Augenhöhe (Ego-Kamera)
constexpr float TROOPER_STEP = 0.2f;          // so hohe Kanten steigt die Figur ohne Sprung hinauf (Bodenplatten der Stationen: 0,18 m)
constexpr float TROOPER_PITCH_MAX = 1.45f;    // Blick nach oben/unten (Bogenmaß, gut 80°)
constexpr float TROOPER_HP = 100.0f;          // Schild
constexpr int TROOPER_MAG = 30;               // Schuss pro Magazin
constexpr int TROOPER_AMMO = 120;             // Ersatzmunition beim Aussteigen
constexpr float TROOPER_RELOAD = 1.8f;        // Sekunden zum Nachladen
constexpr float TROOPER_FIRE_INTERVAL = 0.15f;
constexpr float TROOPER_DAMAGE = 20.0f;
constexpr float TROOPER_BULLET_SPEED = 70.0f;
constexpr float TROOPER_BULLET_LIFE = 1.2f;
constexpr float TROOPER_REGEN_DELAY = 5.0f;   // so lange ohne Treffer, dann lädt sich der Schild wieder auf ...
constexpr float TROOPER_REGEN = 10.0f;        // ... mit so viel pro Sekunde
constexpr float TROOPER_RESPAWN = 5.0f;       // Spieler gefallen: nach so vielen Sekunden wieder am Ausstiegspunkt
// KI-Figuren (Ai.cpp): Kameraden des SG-Teams und Bodenpiraten. Bisher nur
// im Einzelspieler; beide erscheinen beim Aussteigen und verschwinden beim
// Einsteigen.
constexpr int SP_MATES = 3;                   // NPC-Kameraden (bis zum Command Center fest)
constexpr float MATE_DAMAGE = 12.0f;
constexpr float MATE_FIRE_INTERVAL = 0.35f;
constexpr float MATE_SPREAD = 0.035f;         // Streuung der Schüsse (Bogenmaß)
constexpr float MATE_RESPAWN = 20.0f;         // gefallene Kameraden kommen nach so vielen Sekunden beim Spieler wieder
constexpr int GROUND_PIRATE_GROUPS = 3;       // Gruppen gleichzeitig auf der Karte (später Server-Einstellung je Team)
constexpr int GROUND_PIRATE_MIN = 2;          // Figuren je Gruppe ...
constexpr int GROUND_PIRATE_MAX = 4;          // ... bis
constexpr float GROUND_PIRATE_HP = 60.0f;
constexpr float GROUND_PIRATE_DAMAGE = 6.0f;
constexpr float GROUND_PIRATE_FIRE_INTERVAL = 0.45f;
constexpr float GROUND_PIRATE_SPREAD = 0.05f;
constexpr float GROUND_PIRATE_SPAWN_DIST = 60.0f;   // neue Gruppen mindestens so weit vom Team entfernt
constexpr float GROUND_PIRATE_RESPAWN = 40.0f;      // ist eine Gruppe ganz ausgeschaltet, kommt danach eine neue
constexpr float GROUND_PIRATE_RETREAT = 0.35f;      // unter diesem Anteil des Schilds ziehen sie sich zurück
constexpr float TROOPER_SIGHT = 30.0f;        // so weit sehen KI-Figuren ihre Gegner
// Gelbe Kristalle (FPS-Modus): Erst nimmt ein Trupp zu Fuß eine Probe, dann
// sind sie wieder grün und abbaubar. Bisher nur im Einzelspieler (nicht in
// der Simulation und nicht im Mehrspieler).
constexpr float YELLOW_SHARE = 0.4f;          // Anteil gelber Lagerstätten (0 = keine)
constexpr float SAMPLE_TIME = 3.0f;           // Sekunden E halten für eine Probe
constexpr float SAMPLE_RANGE = 3.0f;          // so nah (m, waagerecht) muss die Figur an der Lagerstättenmitte stehen
constexpr int SAMPLE_GOAL = 5;                // Proben je Team (Anzeige; das Ziel zählt erst mit dem Stargate)

} // namespace cfg
