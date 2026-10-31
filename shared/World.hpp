#pragma once
// ---------------------------------------------------------------------------
// World: die komplette Spiellogik von Mondschürfer.
//
// Die Klasse enthält den Zustand der Spielwelt (Schiffe, Lagerstätten,
// Wracks, Geschosse, Stationen ...) und die Regeln, nach denen er sich mit
// jedem Zeitschritt verändert (Update). Sie zeichnet NICHTS und spielt keine
// Töne ab. Dadurch kann derselbe Code an drei Stellen arbeiten:
//
//   1. Einzelspieler (src/main.cpp):
//        NewGame() erzeugt eine Welt mit genau einem Spieler (players[0],
//        Schiff ships[0]). main.cpp liest die Tastatur, ruft Update() auf,
//        zeichnet die Welt (Render.cpp) und das HUD (Hud.cpp).
//
//   2. Server (server/src/GameServer.cpp):
//        NewServerWorld() erzeugt die Welt ohne Spieler. Für jeden Spieler,
//        der sich anmeldet, legt AddPlayer() einen PlayerState und ein Schiff
//        an. isServer = true schaltet die Mehrspieler-Regeln ein (neues Schiff
//        nach Absturz, Zielrunden, feste Gegnerzahl, Lagerstätten pro Spieler
//        aufgedeckt) und erzeugt keine Partikel, sondern nur Effekt-Ereignisse.
//
//   3. Mehrspieler-Client (src/NetClient.cpp):
//        BuildMirror() baut nur die Kulisse (Mondoberfläche, Stationen). Den
//        Rest (Schiffe, Lagerstätten ...) füllt der Client mit den Daten vom
//        Server. isMirror = true. Update() wird hier NICHT aufgerufen; der
//        Client nutzt die Welt nur zum Zeichnen und für die Vorhersage des
//        eigenen Schiffs (UpdateShipPhysics).
//
// Wo steht was?
//   World.hpp      diese Datei: alle Datentypen und die Schnittstelle
//   World.cpp      Spielregeln (Update, Scanner, Abbau, Kampf, Stationen)
//   WorldGen.cpp   Aufbau: Mondoberfläche, Stationen, Lagerstätten
//   MapAnim.cpp    Kartenanimation: Rover, Monorail zwischen Stationen, Raketen (nur Kulisse)
//   Ai.cpp         KI der Piraten/Händler und der Autopilot (Simulation)
//   Trooper.cpp    Trupp zu Fuß (FPS-Modus): Bewegung, Hindernisse, Schüsse
//   src/Render.cpp World::Draw (nur im Client)
//
// Abhängigkeiten: nur raylib.h/raymath.h (Datentypen wie Vector3/Color und
// Vektor-Mathe, die raymath komplett im Header definiert), Config.hpp
// (Balancing), GameSettings.hpp (Server-Einstellungen), SoundEvents.hpp
// (Geräusch-Ereignisse) und Util.hpp (Fmt, Dist2D, Winkel). Die .cpp-Dateien
// brauchen zusätzlich Models.hpp (Modellnamen -> IDs). Keine
// raylib-Funktionen, kein Fenster, keine Grafik. Draw() ist zwar hier
// deklariert, aber in src/Render.cpp definiert und nur im Client vorhanden.
//
// Koordinaten: Der Boden ist die x/z-Ebene, y zeigt nach oben (wie in
// raylib üblich). Alle Schiffe fliegen in derselben Höhe
// (cfg::SHIP_ALTITUDE), Abstände werden deshalb meist mit Dist2D nur auf dem
// Boden gemessen. Winkel ("yaw") sind im Bogenmaß; yaw = 0 heißt: Die Nase
// zeigt nach -z (siehe ForwardFromYaw in Util.hpp).
//
// Verweise zwischen Objekten (Ziel eines Piraten, Schiff eines Spielers,
// Schütze eines Geschosses ...) sind immer IDs, nie Zeiger oder
// Vektor-Indizes. Die Listen (ships, deposits ...) ändern sich ständig:
// push_back kann den Speicher verschieben, erase verschiebt die Indizes. Eine
// ID bleibt dagegen gültig; gesucht wird mit FindShip/FindDeposit/FindWreck.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include "Config.hpp"
#include "GameMap.hpp"
#include "GameSettings.hpp"
#include "SoundEvents.hpp"
#include "Util.hpp"
#include <deque>
#include <random>
#include <string>
#include <vector>

// Wer gehört zu wem? Bestimmt, wen ein Geschoss treffen darf (UpdateBullets)
// und wen die Geschütztürme angreifen (nur Piraten).
enum class Faction { Player, Pirate, Neutral };
// ShipKind wird im Netzwerk (Snapshots) und im Spielstand als Zahl
// übertragen bzw. gespeichert: Zahlenwerte nicht ändern.
// Convoy = Frachter der Mission "Konvoi verteidigen" (Faction::Player: Spieler
// treffen sie nicht, Piraten greifen sie an).
enum class ShipKind { Player = 0, Pirate = 1, Elite = 2, Miner = 3, Convoy = 4 };
// Zustände der KI (Ai.cpp). Wander/Attack: Piraten (und wütende Händler).
// GoMine -> Mining -> GoSell -> Docked -> GoMine: der Kreislauf der Händler.
// Flee: Händler flieht vor einem Piraten.
enum class AiState { Wander = 0, Attack = 1, GoMine = 2, Mining = 3, GoSell = 4, Docked = 5, Flee = 6 };
// Dealer = Raumschiffsverkäufer (andere Schiffe kaufen, World::BuyShip)
enum class StationType { Trade = 0, Shipyard = 1, Dealer = 2 };

// Eingaben für ein Schiff (vom Spieler, vom Autopiloten oder über das Netzwerk).
// Die KI-Schiffe benutzen dieselbe Struktur: UpdateAi berechnet ein ShipInput
// und gibt es an UpdateShipPhysics. So fliegen alle Schiffe nach denselben
// physikalischen Regeln.
struct ShipInput {
    float thrust = 0.0f;   // -1..1 (negativ = bremsen/rückwärts)
    float turn = 0.0f;     // -1 (rechts) .. 1 (links)
    bool fire = false;     // gedrückt halten = Dauerfeuer (begrenzt durch fireInterval)
    // Einmalige Tastendrücke: nur in EINEM Schritt true (Scanner an/aus,
    // F = andocken/abbauen/abbrechen). Der Server bildet sie aus Zählern in
    // den Eingabepaketen, damit kein Druck verloren geht.
    bool scanToggle = false;
    bool interact = false;
};

// Ein Raumschiff: Spieler, Pirat, Elite-Pirat oder neutraler Händler/Schürfer.
struct Ship {
    int id = 0;                   // eindeutig (World::nextId), bleibt für die Lebensdauer gleich
    ShipKind kind = ShipKind::Player;
    Faction faction = Faction::Player;
    int model = -1;               // Modell-ID (Models.hpp)
    Vector3 pos{};
    Vector3 vel{};                // Geschwindigkeit (Einheiten/s); y ist außer beim Absturz 0
    float yaw = 0.0f;             // Flugrichtung (Bogenmaß, 0 = Nase nach -z)
    float bank = 0.0f;            // Schräglage in Kurven (nur Optik, Render.cpp kippt das Modell)
    float hp = 100.0f, maxHp = 100.0f;   // Schild: bei 0 stürzt das Schiff ab
    float maxSpeed = 16.0f;       // Startwerte = Stufe 0 aus Config.hpp; ApplyUpgrades/MakeShip setzen sie
    float turnRate = 2.8f;        // Drehgeschwindigkeit in rad/s bei voll eingeschlagenem Ruder
    float damage = 10.0f;         // Schaden pro Treffer
    float fireInterval = 0.5f;    // Sekunden zwischen zwei Schüssen
    int guns = 1;                 // gleichzeitige Schüsse (Upgrade Kanonen, ShipGuns)
    float fireCd = 0.0f;          // Restzeit bis zum nächsten möglichen Schuss
    int cargo = 0;                // Ladung (Spieler/Händler); bei Piraten: Beute, die im Wrack landet
    int cargoMax = 0;
    float hitFlash = 0.0f;        // 1 beim Treffer, klingt ab (Aufblitzen in Render.cpp/Hud.cpp)
    bool crashing = false;        // Schild auf 0: Schiff trudelt zu Boden (dann Explosion + Wrack)
    float crashT = 0.0f;          // Sekunden seit Absturzbeginn
    float crashSpin = 0.0f;       // Drehgeschwindigkeit beim Trudeln (rad/s, zufällige Richtung)
    // Wird in Update am Ende des Schritts aus ships entfernt. Ausnahme im
    // Einzelspieler/Client: ships[0] (eigenes Schiff) bleibt immer stehen.
    // FindShip findet tote Schiffe nicht mehr.
    bool dead = false;
    bool hostileToPlayer = false; // Händler/Miner nach einem Angriff
    float thrustVisual = 0.0f;    // geglätteter Schub 0..1 für die Triebwerksflamme (nur Optik)
    int playerId = -1;            // >= 0: Schiff eines menschlichen Spielers (PlayerState::id)
    bool docked = false;          // Spieler ist angedockt: nicht angreifbar, bewegt sich nicht
    bool chatting = false;        // Mehrspieler: Spieler tippt im Chat (Sprechblase, nur Optik)

    // KI (Ai.cpp)
    AiState ai = AiState::Wander;
    Vector3 waypoint{};      // aktuelles Flugziel (Wander, Flucht, Abdrehen im Kampf)
    int targetShip = -1;     // Schiffs-ID
    int targetDeposit = -1;  // Lagerstätten-ID
    int station = -1;        // Index der Station, an der ein Händler angedockt hat
    float aiTimer = 0.0f;    // Restzeit des aktuellen Zustands (z. B. Abbau, Aufenthalt in der Station)
    float thinkT = 0.0f;     // Piraten suchen nur alle 0,35 s ein neues Ziel (spart Rechenzeit)
    float breakOff = 0.0f;   // Kampf: Restzeit des Abdrehmanövers, wenn ein Pirat zu nah war
    float aggroT = 0.0f;     // nach Treffer: verfolgt Angreifer länger
    // Abbaustrahl für die ANDEREN: Händler beim Abbau (Ai.cpp) und auf dem
    // Server Spielerschiffe, damit die anderen Spieler den Strahl sehen.
    // Der eigene Strahl wird aus PlayerState::mining gezeichnet.
    bool mining = false;
    Vector3 mineTarget{};
    // Unsichtbar und nicht angreifbar: Händler in der Station, das eigene
    // Schiff im Einzelspieler nach dem Absturz, der Platzhalter im Client
    // (BuildMirror, z. B. bei Zuschauern).
    bool hidden = false;
    // Gehört zu einer Mini-Mission (Mission::id): Missionspirat oder
    // Konvoi-Frachter. -1 = normales Schiff.
    int mission = -1;
};

// Eine Kristall-Lagerstätte, die abgebaut werden kann.
struct Deposit {
    int id = 0;
    Vector3 pos{};
    float yaw = 0.0f;
    float scale = 1.0f;
    int model = -1;
    int amount = 0;                 // noch vorhanden; bei 0 wird sie entfernt
    int initial = 0;                // Anfangsmenge (Render.cpp lässt die Kristalle mit amount/initial schrumpfen)
    bool revealed = false;          // Einzelspieler/Client: für den eigenen Spieler entdeckt
    float revealT = 0.0f;           // Einblend-Animation (0..1)
    // Server: IDs der Spieler, die sie entdeckt haben. Jeder Spieler muss
    // selbst scannen; der Server schickt jedem nur die von ihm entdeckten
    // (Zuschauer bekommen alle).
    std::vector<int> revealedFor;
};

// Wrack eines abgeschossenen Schiffs: kann wie eine Lagerstätte abgebaut
// werden (ohne vorher zu scannen). Verschwindet nach 300 s.
struct Wreck {
    int id = 0;
    Vector3 pos{};
    float yaw = 0.0f;
    float tilt = 0.0f;              // Schieflage am Boden (nur Optik)
    int model = -1;                 // dasselbe Modell wie das Schiff
    int amount = 0;
    int initial = 0;
    float age = 0.0f;               // Sekunden seit dem Aufschlag (die ersten 60 s raucht es)
};

struct Bullet {
    Vector3 pos{};
    Vector3 vel{};
    float life = 0.0f;     // Restlebensdauer in Sekunden (bei <= 0 entfernt)
    float damage = 0.0f;
    Faction faction = Faction::Player;
    int ownerId = -1;      // Schiffs-ID des Schützen, -1 = Geschützturm oder Figur (ground)
    int ownerPlayer = -1;  // Spieler-ID, falls ein Spieler geschossen hat (für Abschüsse)
    // Geschoss einer Figur zu Fuß (Trupp): fliegt mit Höhenrichtung, endet am
    // Boden oder an einem Hindernis (TrooperBlock) und trifft keine Schiffe.
    bool ground = false;
};

// Ein Effekt-Teilchen (Rauch, Funken, Explosion). Gibt es nur im
// Einzelspieler und im Client; der Server erzeugt stattdessen FxEvents.
struct Particle {
    Vector3 pos{};
    Vector3 vel{};
    float life = 0.0f;     // Restzeit; life/maxLife steuert Größe und Durchsichtigkeit beim Ausblenden
    float maxLife = 1.0f;
    float size = 0.3f;
    Color color = WHITE;
    float gravity = 0.0f;  // > 0: fällt nach unten und bleibt am Boden liegen (Trümmer)
    // Leuchtet: wird additiv gezeichnet (die Farbe wird zum Hintergrund
    // addiert, das Bild wird also nur heller, Render.cpp).
    bool glow = false;
};

// Aufblitzen einer Explosion (Grafikeffekt "Treffer-Aufblitzen", Render.cpp:
// Lichtkugel und Punktlicht). Wie die Partikel nur im Einzelspieler und im Client.
struct Flash {
    Vector3 pos{};
    float life = 0.0f;     // Restzeit; life/maxLife = Helligkeit beim Abklingen
    float maxLife = 0.45f;
    float size = 1.0f;     // Größe der Explosion
};

// Kulisse: Krater, Felsen, Rover ... (nur Optik, keine Kollision).
struct Deco {
    int model = -1;
    Vector3 pos{};
    float yaw = 0.0f;
    float scale = 1.0f;
    float yScale = 0.0f; // 0 = gleich scale (für flache, große Krater)
};

// ---------------------------------------------------------------------------
// Kartenanimation (MapAnim.cpp): reine Kulisse ohne Einfluss aufs Spiel. Der
// Server legt keine Rover an; Einzelspieler und Mehrspieler-Client erzeugen
// alles selbst (eigener Zufallsgenerator World::animRng).
// ---------------------------------------------------------------------------
// Ein Punkt der Reifenspur eines Rovers. Zwischen zwei Punkten zeichnet
// Render.cpp je Radseite einen schmalen Streifen.
struct TrackPoint {
    float x = 0.0f, z = 0.0f;
    float sx = 0.0f, sz = 0.0f;   // Querrichtung (Länge 1): dort liegen links und rechts die Räder
    float t = 0.0f;               // Weltzeit, zu der der Punkt entstand (die Spur verblasst danach)
};

// Ein fahrendes Mondfahrzeug (Modell "rover"). Es fährt von Ziel zu Ziel um
// Hindernisse herum (World::UpdateMapAnim).
struct Rover {
    Vector3 pos{};
    float yaw = 0.0f;              // Fahrtrichtung (Nase/Fahrerhaus nach -z wie bei den Schiffen)
    float scale = 3.0f;
    float speed = 0.0f;            // aktuelle Geschwindigkeit (m/s)
    float topSpeed = 3.0f;
    Vector3 target{};              // nächstes Ziel
    float wait = 0.0f;             // Restzeit einer Pause (steht still)
    float legT = 0.0f;             // Sekunden seit dem letzten neuen Ziel (gegen Festfahren)
    std::deque<TrackPoint> track;  // Reifenspur, älteste Punkte vorne
};

// Hindernis für Rover: Kapsel (Strecke a-b mit Radius r) auf dem Boden; bei
// runden Hindernissen ist a = b.
struct RoverBlock {
    Vector2 a{}, b{};
    float r = 1.0f;
};

// ---------------------------------------------------------------------------
// Trupp zu Fuß (FPS-Modus, Trooper.cpp). Im Einzelspieler steigt der Spieler
// zum Ausprobieren über die Pause aus dem Schiff aus (StartTrooper).
// ---------------------------------------------------------------------------
// Rolle eines Spielers: Pilot (Schiff) oder Trupp (Figur zu Fuß).
enum class PlayerRole { Pilot = 0, Trooper = 1 };

// Eingaben für eine Figur zu Fuß, wie ShipInput für ein Schiff. Die
// Blickrichtung kommt fertig mit (Maus/rechter Stick, Controls.cpp).
struct TrooperInput {
    float forward = 0.0f;  // -1..1 (zurück .. vor)
    float strafe = 0.0f;   // -1 (links) .. 1 (rechts)
    float yaw = 0.0f;      // Blickrichtung wie Ship::yaw (0 = nach -z)
    float pitch = 0.0f;    // > 0 nach oben, < 0 nach unten
    bool fire = false;     // gedrückt halten = Dauerfeuer
    bool jump = false;
    bool sprint = false;
    bool reload = false;   // einmaliger Druck
};

// Eine Figur zu Fuß. Später nutzen auch die Bodenpiraten diese Struktur
// (Fraktion Pirat).
struct Trooper {
    int id = 0;                   // eindeutig (World::nextTrooperId)
    int playerId = -1;            // PlayerState::id, -1 = KI
    Faction faction = Faction::Player;
    Vector3 pos{};                // Fußpunkt; y = Höhe über dem Boden
    Vector3 vel{};
    float yaw = 0.0f, pitch = 0.0f;
    bool onGround = true;
    float hp = cfg::TROOPER_HP, maxHp = cfg::TROOPER_HP;
    int mag = cfg::TROOPER_MAG;   // Schuss im Magazin
    int ammo = cfg::TROOPER_AMMO; // Ersatzmunition
    float reloadT = 0.0f;         // > 0: lädt nach (Restzeit)
    float fireCd = 0.0f;
    float hitFlash = 0.0f;        // wie Ship::hitFlash
    float age = 0.0f;             // Sekunden seit dem Aussteigen (Tastenhilfe im HUD)
};

// Hindernis für Figuren: Kapsel a-b mit Radius r auf dem Boden, oben flach
// in Höhe h. Bis TROOPER_STEP über den Füßen steigt die Figur hinauf, auf
// höhere kommt sie nur mit einem Sprung.
struct TrooperBlock {
    Vector2 a{}, b{};
    float r = 1.0f;
    float h = 1.0f;
};

// Monorail-Strecke zwischen zwei Stationen (gerade, von Ende a nach Ende b).
// Auf jeder Strecke pendelt ein Zug (RailTrainPos).
struct StationRail {
    Vector3 a{}, b{};
    int from = 0, to = 0;          // Stationen (Index in World::stations) an den Enden a und b
    int cargo = 0;                 // Mittelwagen: 0 Personen, 1 Fracht, 2 Container
    float phase = 0.0f;            // Versatz im Fahrplan (Sekunden), damit nicht alle Züge gleichzeitig fahren
};

// Raketenstartplatz neben einer Station. Abwechselnd startet und landet eine
// Rakete (RocketAt).
struct RocketPad {
    Vector3 pos{};
    Vector3 out{};                 // waagerecht, Länge 1: von der Station weg; dorthin kippt die Rakete beim Aufstieg
    bool typeB = false;            // Bauteile der Rakete B statt A
    float stand = 25.0f;           // Sekunden auf dem Startplatz (20..30)
    float away = 25.0f;            // Sekunden unterwegs (20..30)
    float phase = 0.0f;            // Versatz im Ablauf (Sekunden)
};

// Lage einer Rakete zu einem Zeitpunkt (RocketAt).
struct RocketPose {
    bool visible = false;          // false = gerade unterwegs (nicht zu sehen)
    Vector3 pos{};                 // Fußpunkt der Rakete
    float tilt = 0.0f;             // Neigung in Richtung RocketPad::out (Bogenmaß)
    float size = 1.0f;             // 0..1: wird ganz oben kleiner, als flöge sie in die Ferne
    float thrust = 0.0f;           // 0..1: Triebwerksflamme
};

// Mitte des Zugs auf einer Strecke zur Weltzeit t: Abstand von StationRail::a
// in Metern; -1 = Strecke zu kurz für einen Zug. Der Zug hält an jedem Ende
// 10 Sekunden und fährt dann zur anderen Station.
float RailTrainPos(const StationRail& r, float t);
RocketPose RocketAt(const RocketPad& p, float t);

// Ein Bauteil einer Station (ein Modell aus dem Kenney-Kit).
struct StationPart {
    int model = -1;
    Vector3 offset{};      // Lage relativ zur Stationsmitte
    float yaw = 0.0f;
    float scale = 1.0f;
    int stage = 0;         // ab dieser Ausbaustufe sichtbar (nur Handelsstationen wachsen)
};

struct Turret {
    Vector3 offset{};      // Lage relativ zur Stationsmitte
    float yaw = 0.0f;      // aktuelle Blickrichtung (dreht sich langsam zum Ziel)
    float cd = 0.0f;       // Nachladezeit
    int stage = 0;         // ab dieser Ausbaustufe aktiv
    bool isDouble = true;  // nur Optik: Doppel- oder Einzelrohr
};

struct Station {
    std::string name;
    StationType type = StationType::Trade;
    Vector3 pos{};
    std::vector<StationPart> parts;
    std::vector<Turret> turrets;
};

// Eine Meldung oben im Bild ("Lagerstätte entdeckt!" ...).
struct Notice {
    std::string text;
    float time = 0.0f;     // Restzeit der Anzeige (4 s)
    Color color = WHITE;
    int count = 1;         // gleiche Meldung mehrmals hintereinander: "(3x)" statt dreier Zeilen
};

// Sichtbarer Effekt (nur Server): wird an die Clients geschickt, die daraus
// selbst Partikel erzeugen. Im Einzelspieler entstehen die Partikel direkt.
struct FxEvent {
    enum class Type { Explosion, Sparks };
    Type type = Type::Explosion;
    Vector3 pos{};
    float size = 1.0f;      // Explosion: Größe
    Color color = WHITE;    // Funken: Farbe
    int count = 0;          // Funken: Anzahl
};

// Ein Spieler hat das Spielziel erreicht (nur Server, für die Bestenliste)
struct GoalEvent {
    int playerId = -1;
    float seconds = 0.0f;   // gebrauchte Zeit für diese Runde
    int round = 1;          // 1 = mit neuem Schiff, >1 = mit behaltenen Aufrüstungen
};

// Ein eigenes Schiff, das gerade nicht geflogen wird (geparkt beim
// Raumschiffsverkäufer gekauft, an der Handelsstation im Tab "Schiff"
// wählbar). Jedes Schiff behält seine eigenen Aufrüstungen und seinen Schild.
struct OwnedShip {
    int type = cfg::SHIP_START;               // Index in World::settings.ships
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0};
    float hp = -1.0f;                         // Schild beim Parken, -1 = voll
};

// ---------------------------------------------------------------------------
// Mini-Missionen (World.cpp, Abschnitt "Missionen"). An der Handelsstation
// (Tab "Missionen") nimmt ein Spieler ein Angebot an; die Mission gehört dann
// ihm allein. Jeder Spieler hat höchstens eine laufende Mission.
// Zahlenwerte: Index in cfg::MISSIONS und Teil des Netzwerkprotokolls.
// ---------------------------------------------------------------------------
enum class MissionType { Persons = 0, Pirates = 1, Convoy = 2 };

struct Mission {
    int id = 0;                   // eindeutig (World::nextMissionId)
    MissionType type = MissionType::Persons;
    int owner = -1;               // Spieler-ID; -1 = Angebot (noch nicht angenommen)
    int progress = 0;             // eingesammelte Personen, abgeschossene Piraten bzw. angekommene Frachter
    int goal = cfg::MISSION_GOAL;
    int alive = 0;                // Konvoi: Frachter noch unterwegs (World::UpdateMissions)
    // Konvoi: Wegpunkte von der Start- zur Zielstation, aktueller Wegpunkt,
    // Zeit seit dem Abflug und schon gestartete Piratenwellen.
    std::vector<Vector3> route;
    int leg = 0;
    float t = 0.0f;
    int waves = 0;
    bool hold = false;            // Konvoi wartet (Auftraggeber angedockt oder ohne Schiff)
};

// Eine verirrte Person (Mission "Personen einsammeln"). Nur der Spieler, dem
// die Mission gehört, sieht und sammelt sie.
struct Person {
    int id = 0;
    int mission = -1;             // Mission::id
    int owner = -1;               // Spieler-ID
    Vector3 pos{};
    float yaw = 0.0f;
    int model = -1;
};

// Markierung auf der Minikarte für die laufende Mission des Spielers.
// kind: 0 = Person, 1 = Ziel (Pirat), 2 = Konvoi-Frachter.
struct MissionMarker {
    int kind = 0;
    Vector3 pos{};
};

// Ausgang einer Einzelspieler-Partie (der Mehrspieler kennt kein Spielende).
enum class Outcome { None, Dead, Won };

// ---------------------------------------------------------------------------
// Alles, was zu EINEM menschlichen Spieler gehört (Konto, Aufrüstungen,
// Scanner, Abbau, Andocken). Im Einzelspieler gibt es genau einen davon.
// Das Schiff selbst steht in World::ships (verbunden über shipId).
// Warum getrennt? Im Mehrspieler überlebt der Spieler sein Schiff: Nach
// einem Absturz gibt es kurz kein Schiff (shipId = -1), dann ein neues.
// ---------------------------------------------------------------------------
struct PlayerState {
    int id = 0;                  // Spieler-ID (Einzelspieler: 0)
    int shipId = -1;             // Ship::id des eigenen Schiffs, -1 = gerade keins (wartet auf neues)
    std::string name;
    ShipInput input;             // Eingaben für den nächsten Update-Schritt

    long long balance = cfg::START_BALANCE;   // Kontostand (Ziel: cfg::GOAL_POINTS)
    long long delivered = 0;     // selbst abgelieferte Ressourcen (Statistik)
    int lives = cfg::START_LIVES; // verbleibende Leben inkl. des aktuellen Schiffs (Mehrspieler: settings.lives)
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0, 0};   // Stufe je cfg::Upgrade, 0..cfg::UPGRADE_MAX (des geflogenen Schiffs)
    int shipType = cfg::SHIP_START;              // Typ des geflogenen Schiffs (Index in World::settings.ships)
    std::vector<OwnedShip> hangar;               // weitere eigene Schiffe (je Typ höchstens eins)

    float scanEnergy = cfg::SCANNER_MAX_ENERGY;
    bool scanOn = false;
    float scanPulse = 0.0f;      // Sekunden seit dem Einschalten; daraus folgt der Radius des Scannerrings

    bool mining = false;
    float mineT = 0.0f;          // Sekunden des laufenden Abbaus (fertig bei cfg::MINING_TIME)
    int mineKind = 0;            // 1 = Lagerstätte, 2 = Wrack
    int mineId = -1;

    int dockedAt = -1;           // Index in World::stations, -1 = nicht angedockt
    int lastSale = 0;            // Erlös beim letzten Andocken (Anzeige im Stationsfenster)

    float playTime = 0.0f;       // Einzelspieler: Spielzeit. Mehrspieler: Zeit mit dem aktuellen Schiff
    float roundTime = 0.0f;      // Mehrspieler: Zeit in der aktuellen Zielrunde
    int round = 1;               // Mehrspieler: Zielrunde mit diesem Schiff (1 = neues Schiff)
    int kills = 0;
    int deaths = 0;
    int goals = 0;               // so oft das Ziel erreicht
    float respawnT = 0.0f;       // Mehrspieler: Sekunden bis zum neuen Schiff
    int revealRevision = 0;      // zählt hoch, wenn dieser Spieler eine Lagerstätte entdeckt

    std::vector<Notice> notices; // Meldungen für diesen Spieler
    std::vector<Notice> outbox;  // Server: neue Meldungen, die noch verschickt werden müssen

    int mission = -1;            // Mission::id der laufenden Mini-Mission, -1 = keine
    std::vector<MissionMarker> markers;   // Ziele dieser Mission für die Minikarte (World::UpdateMissions)
    // Funkspruch unten rechts (Clip und Untertitel): Missionstyp * 3 +
    // Phase (0 Start, 1 Ende, 2 Fehlschlag), cfg::CUE_NEWSTART/CUE_RETURN
    // (neues Spiel, Fortsetzen), -1 = keiner. cueT = Restzeit;
    // sie läuft nur, solange der Spieler nicht angedockt ist.
    int cue = -1;
    float cueT = 0.0f;

    // Trupp zu Fuß (Trooper.cpp): Rolle, eigene Figur (Trooper::id, -1 =
    // keine) und ihre Eingaben für den nächsten Update-Schritt.
    PlayerRole role = PlayerRole::Pilot;
    int trooperId = -1;
    TrooperInput trooperInput;
};

class World {
public:
    // --- Lebenszyklus ----------------------------------------------------
    // Alle drei Start-Funktionen ersetzen die komplette Welt (*this = World{})
    // und ziehen danach die Zufallszahlen in fester Reihenfolge aus dem Seed.
    void NewGame(unsigned int seed, bool simulation);              // Einzelspieler
    void NewServerWorld(unsigned int seed, const GameSettings& s); // Server (noch ohne Spieler)
    void BuildMirror(unsigned int seed, const GameSettings& s);    // Mehrspieler-Client: nur Kulisse
    // Eigene Karte aus dem Karteneditor: ersetzt die Zufalls-Deko im Spielfeld
    // (der Felswall am Rand bleibt), sucht die Monorail-Kreise und lässt die
    // Rover der Karte fahren. Nach NewGame/BuildMirror aufrufen; zieht keine
    // Zufallszahlen aus rng (WorldGen.cpp).
    void ApplyMap(const GameMap& m);
    // Ein Zeitschritt der Spielregeln (Einzelspieler: einmal pro Bild mit dem
    // dt des Bildes; Server: fest 1/30 s). Danach liegen in sounds, fx, goals
    // und PlayerState::outbox neue Ereignisse, die der Aufrufer abarbeitet
    // und leert.
    void Update(float dt);                                         // Eingaben aus players[i].input
    void Update(float dt, const ShipInput& input) { Me().input = input; Update(dt); }   // Einzelspieler
    void UpdateAmbient(float dt);   // während angedockt (Einzelspieler): nur Effekte
    void UpdateCosmetics(float dt); // Mehrspieler-Client: Rauch, Abbau-Partikel aus dem Zustand erzeugen
    // Kartenanimation (MapAnim.cpp): Rover fahren und hinterlassen Spuren.
    // Aufgerufen von Update (nicht auf dem Server), UpdateAmbient und UpdateCosmetics.
    void UpdateMapAnim(float dt);
    // Rover aus der Deko (Modell "rover") neu anlegen, Hindernisse sammeln.
    // Nach jedem Austausch der Deko (GenerateTerrain, ApplyMap, Mehrspieler:
    // Wechsel zurück zur Standardkarte). Auf dem Server: keine Rover.
    void SpawnRovers();

    // --- Spieler -----------------------------------------------------------
    // Einzelspieler und Client: der eigene Spieler ist players[0], sein Schiff ships[0].
    // Auf dem Server gibt es kein "Ich": dort nie Me()/Player() und die
    // Kurzformen weiter unten benutzen, sondern immer mit PlayerState arbeiten.
    PlayerState& Me() { return players[0]; }
    const PlayerState& Me() const { return players[0]; }
    Ship& Player() { return ships[0]; }
    const Ship& Player() const { return ships[0]; }

    PlayerState* FindPlayer(int playerId);
    const PlayerState* FindPlayer(int playerId) const;
    Ship* PlayerShip(const PlayerState& ps);               // nullptr: gerade kein Schiff (oder schon aufgeschlagen, dead)
    const Ship* PlayerShip(const PlayerState& ps) const;
    PlayerState* OwnerOfShip(int shipId);   // Spieler, dem ein Schiff gehört (oder nullptr)

    // Server: Spieler hinzufügen (mit Schiff an einer Handelsstation) und entfernen.
    // Achtung: AddPlayer/RemovePlayer verändern den Vektor players. Zeiger und
    // Referenzen auf PlayerStates sind danach ungültig; neu mit FindPlayer suchen.
    PlayerState& AddPlayer(int playerId, const std::string& name);
    void RemovePlayer(int playerId);
    void SpawnPlayerShip(PlayerState& ps);
    void ResetProgress(PlayerState& ps);    // nach Absturz: Fortschritt von vorne

    // Suchen über die ID (nullptr, wenn es das Objekt nicht mehr gibt)
    Ship* FindShip(int id);
    const Ship* FindShip(int id) const;
    Deposit* FindDeposit(int id);
    Wreck* FindWreck(int id);

    // --- Aktionen an den Stationen -----------------------------------------
    void Dock(PlayerState& ps, int station);   // station = Index in stations; Handelsstation: Ladung wird verkauft
    void Undock(PlayerState& ps);
    int RepairCost(const PlayerState& ps) const;
    bool Repair(PlayerState& ps);              // false = nichts zu reparieren oder kein Geld
    int UpgradeCost(const PlayerState& ps, int type) const;  // -1 wenn maximal
    bool BuyUpgrade(PlayerState& ps, int type);
    // Schiffswerte aus Schiffstyp und Aufrüstungsstufen setzen. refill = Schild ganz auffüllen (neues Schiff).
    void ApplyUpgrades(PlayerState& ps, bool refill);
    // Raumschiffsverkäufer: Preis eines Schiffstyps (mit tradeIn abzüglich
    // TradeInValue, kann dann auch negativ sein = Gutschrift), -1 = nicht
    // kaufbar (schon im Besitz, nicht im Verkauf oder ungültig). BuyShip prüft
    // wie BuyUpgrade nicht, an welcher Station der Spieler angedockt ist.
    // Die Schiffstypen stehen in settings.ships (Mehrspieler: vom Server).
    bool OwnsShipType(const PlayerState& ps, int type) const;
    int TradeInValue(const PlayerState& ps) const;
    int ShipPrice(const PlayerState& ps, int type, bool tradeIn) const;
    bool BuyShip(PlayerState& ps, int type, bool tradeIn);
    // Handelsstation, Tab "Schiff": zu einem geparkten Schiff (hangar) wechseln.
    bool SwitchShip(PlayerState& ps, int type);
    float ScannerRadius(const PlayerState& ps) const;

    // --- Mini-Missionen (Abschnitt "Missionen" in World.cpp) -----------------
    int MissionSlots() const;   // so viele Missionen gleichzeitig (Angebote + laufende)
    Mission* FindMission(int id);
    const Mission* FindMission(int id) const;
    // Handelsstation, Tab "Missionen": ein Angebot dieser Art annehmen.
    // false = schon eine Mission, kein Schiff oder kein solches Angebot.
    // Prüft wie BuyUpgrade nicht, an welcher Station der Spieler angedockt ist.
    bool AcceptMission(PlayerState& ps, int type);
    // Laufende Mission abbrechen (Absturz, Abmelden): Das Angebot kommt zurück.
    void FailMission(PlayerState& ps, bool notify);

    // --- Trupp zu Fuß (Trooper.cpp) ------------------------------------------
    // Einzelspieler (Test, Pausenmenü): Die Figur steigt unter dem Schiff aus,
    // das Schiff wird so lange versteckt. false = geht gerade nicht (angedockt,
    // Absturz, schon zu Fuß). EndTrooper: zurück ins Schiff, das an die
    // Stelle der Figur kommt.
    bool StartTrooper(PlayerState& ps);
    void EndTrooper(PlayerState& ps);
    Trooper* FindTrooper(int id);
    const Trooper* FindTrooper(int id) const;
    // Bewegung einer Figur aus ihren Eingaben, ohne Zufall (wie UpdateShipPhysics).
    void UpdateTrooperPhysics(Trooper& t, float dt, const TrooperInput& in);
    // Steckt der Punkt in einem Hindernis (Treffer eines Bodengeschosses)?
    bool TrooperBlocked(Vector3 p) const;
    // Nächstes Tor (Deko gate_simple/gate_complex) zum Punkt p, false = keins
    bool NearestGate(Vector3 p, Vector3* out) const;

    // Kurzformen für den Einzelspieler (wirken auf den eigenen Spieler)
    void Dock(int station) { Dock(Me(), station); }
    void Undock() { Undock(Me()); }
    int RepairCost() const { return RepairCost(Me()); }
    bool Repair() { return Repair(Me()); }
    int UpgradeCost(int type) const { return UpgradeCost(Me(), type); }
    bool BuyUpgrade(int type) { return BuyUpgrade(Me(), type); }
    bool BuyShip(int type, bool tradeIn) { return BuyShip(Me(), type, tradeIn); }
    bool SwitchShip(int type) { return SwitchShip(Me(), type); }
    bool AcceptMission(int type) { return AcceptMission(Me(), type); }

    // --- Abfragen (HUD, KI, Server) ----------------------------------------
    int StationStage() const;          // Ausbaustufe der Handelsstationen 0..7 (aus totalDelivered)
    int NextStageThreshold() const;    // Punkte für die nächste Stufe, -1 = schon die höchste
    bool NearStation(const Ship& s, int station, float range) const;
    bool NearStation(int station, float range) const { return NearStation(Player(), station, range); }
    int NearestStation(Vector3 p, StationType type) const;   // Index oder -1
    int CountStations(StationType type) const;
    // Was würde Taste F jetzt bewirken? Reihenfolge: Station (andocken) vor
    // Wrack vor entdeckter Lagerstätte (abbauen).
    // 0 nichts, 1 Lagerstätte, 2 Wrack, 3 Handelsstation, 4 Werft, 5 Raumschiffsverkäufer,
    // 6 Person (Mission) (id = Lagerstätte/Wrack/Stationsindex/Person)
    // Personen haben Vorrang vor Wracks und Lagerstätten.
    int InteractTarget(const PlayerState& ps, int* kind, int* id) const;
    int InteractTarget(int* kind, int* id) const { return InteractTarget(Me(), kind, id); }
    bool PlayerAlive(const PlayerState& ps) const;
    bool PlayerAlive() const { return !Player().crashing && !Player().dead; }
    bool RevealedFor(const Deposit& d, const PlayerState& ps) const;   // hat dieser Spieler sie entdeckt?
    bool Targetable(const Ship& s) const;   // darf von Gegnern angegriffen werden

    // Meldung für einen Spieler (auf dem Server zusätzlich in seine outbox)
    void Notify(PlayerState& ps, const std::string& text, Color c = WHITE);
    void Notify(const std::string& text, Color c = WHITE) { Notify(Me(), text, c); }
    // Geräusch-Ereignis in sounds ablegen (siehe SoundEvents.hpp). delay in
    // Sekunden; forPlayer = -1 für alle in Hörweite, sonst nur dieser Spieler.
    void SoundAt(Sfx id, Vector3 pos, float volume = 1.0f, float delay = 0.0f, int forPlayer = -1,
                 int sourceShip = -1);

    // --- Rendering (nur Client, src/Render.cpp) --------------------------------
    // Camera3D ist eine raylib-Struktur (Kameraposition, Blickziel, Oben-Richtung,
    // Blickwinkel). Aufruf zwischen BeginMode3D(cam) und EndMode3D().
    void Draw(const Camera3D& cam) const;

    // --- Daten -------------------------------------------------------------
    unsigned int seed = 1;
    bool simulation = false;   // Einzelspieler-Autopilot (keine Meldungen)
    bool isServer = false;     // Mehrspieler-Regeln, keine Partikel (Server)
    bool isMirror = false;     // Mehrspieler-Client: Welt wird vom Server gefüllt
    GameSettings settings;     // Einzelspieler: Standardwerte; Server/Client: vom Server
    // Zufallsgenerator (Mersenne-Twister). Gleicher Seed -> gleiche Zahlenfolge.
    // Darauf beruht, dass Client und Server dieselbe Mondoberfläche erzeugen,
    // ohne sie zu übertragen.
    std::mt19937 rng;
    // Eigener Zufallsgenerator für die Kartenanimation (MapAnim.cpp). So
    // bleibt die Zahlenfolge von rng unverändert, egal wie viele Rover fahren.
    std::mt19937 animRng;

    std::vector<PlayerState> players;
    std::vector<Ship> ships;
    std::vector<Deposit> deposits;
    std::vector<Wreck> wrecks;
    std::vector<Bullet> bullets;
    std::vector<Particle> particles;
    std::vector<Flash> flashes;            // Explosionsblitze (nur Optik, wie particles)
    std::vector<Deco> decos;
    std::vector<MonorailLoop> monorails;   // geschlossene Monorail-Kreise der eigenen Karte (Render.cpp: Züge)
    // Kartenanimation (MapAnim.cpp). Die Rover stehen zusätzlich als Deko in
    // decos (Startplatz); Render.cpp zeichnet sie nur aus rovers.
    std::vector<Rover> rovers;
    std::vector<RoverBlock> roverBlocks;   // Hindernisse, um die die Rover herumfahren
    std::vector<Trooper> troopers;         // Figuren zu Fuß (Trupp)
    std::vector<TrooperBlock> trooperBlocks;   // Hindernisse der Figuren (BuildTrooperBlocks)
    std::vector<StationRail> rails;        // Monorail zwischen den Stationen
    std::vector<RocketPad> pads;           // Raketenstartplätze, einer je Station
    std::string mapFile;                   // Einzelspieler: Datei der eigenen Karte (leer = Zufallskarte), für den Spielstand
    std::vector<Station> stations;   // erst alle Bauplätze in fester Reihenfolge (siehe WorldGen.cpp)
    std::vector<Mission> missions;   // Angebote und laufende Mini-Missionen
    std::vector<Person> persons;     // verirrte Personen (Mission "Personen einsammeln")

    long long totalDelivered = 0;   // insgesamt abgeliefert (alle Spieler) -> Ausbaustufe der Handelsstationen
    int lastStage = 0;              // zuletzt erreichte Ausbaustufe (erkennt "Station ist gewachsen")
    float stageAnim = 0.0f;         // 1 nach dem Wachsen, fällt auf 0: neue Teile "wachsen" heraus (Render.cpp)

    float shake = 0.0f;   // Bildschirmwackeln bei Treffern am eigenen Schiff, 0..1, klingt ab
    float time = 0.0f;    // Weltzeit in Sekunden (für das leichte Auf und Ab der Schiffe und Animationen)
    int nextId = 1;       // nächste freie ID; Schiffe, Lagerstätten und Wracks teilen sich den Zähler
    // Countdowns bis zum nächsten Nachschub (Sekunden). Die Startwerte geben
    // dem Spieler am Anfang etwas Ruhe, bevor neue Gegner/Händler erscheinen.
    float pirateSpawnT = 8.0f;
    float minerSpawnT = 20.0f;
    float depositSpawnT = 0.0f;
    int depositRevision = 0;   // zählt hoch, wenn sich Lagerstätten ändern (Server: neu verschicken)
    // Eigener Zähler für Missionen und Personen: nextId bleibt so im
    // Einzelspieler unverändert (die Schiffs-IDs steuern das Auf und Ab der
    // Schiffe und damit Treffer, siehe UpdateShipPhysics).
    int nextMissionId = 1;
    int nextTrooperId = 1;        // eigener Zähler für Figuren (aus demselben Grund)
    float missionSpawnT = 0.0f;   // Sekunden bis zum nächsten neuen Missionsangebot

    Outcome outcome = Outcome::None;   // nur Einzelspieler
    float outcomeT = 0.0f;             // Sekunden seit Sieg/Absturz (Endbildschirm)
    bool wantAutosave = false;         // Einzelspieler: nach dem Andocken speichern (main.cpp)

    float drawRange = 115.0f;  // Sichtweite beim Zeichnen (Zuschauer: ganze Karte)

    // Ereignisse für main.cpp / den Server. Werden dort abgearbeitet und geleert.
    std::vector<SoundEvent> sounds;
    std::vector<FxEvent> fx;          // nur Server
    std::vector<GoalEvent> goals;     // nur Server

    // Modell-IDs (beim Start aufgelöst)
    struct Models {
        int player, pirateA, pirateB, elite, miner, cargoA, cargoB;
        int terrain;
        int crystalSmall, crystalA, crystalB;
        int astronautA, astronautB;
    } mdl{};

    // --- interne Helfer ------------------------------------------------------
    // Zufallszahlen im Bereich a..b. JEDER Aufruf rückt die Zahlenfolge weiter.
    // Ein zusätzlicher oder weggelassener Aufruf verändert daher alles, was
    // danach kommt (siehe Kommentar am Anfang von World.cpp).
    float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
    int RandInt(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

    void ResolveModels();            // WorldGen.cpp: Modellnamen -> IDs (mdl)
    void GenerateTerrain();          // WorldGen.cpp
    void BuildStations();            // WorldGen.cpp
    // MapAnim.cpp: Monorail zwischen den Stationen und Raketenstartplätze
    // (Ende von BuildStations), mehr Geröll, Tore und Rover (Ende von GenerateTerrain)
    void BuildStationExtras();
    void AddTerrainLife();
    bool NearRail(Vector3 p, float margin) const;   // näher als margin an einer Strecke oder einem Startplatz?
    void PickRoverTarget(Rover& r);
    bool RoverPathClear(Vector2 a, Vector2 b) const;
    void SpawnDeposit(bool initial); // WorldGen.cpp
    int DepositTarget() const;       // gewünschte Anzahl Lagerstätten
    Vector3 RandomFreePos(float minDistStations, float minDistPlayer);   // WorldGen.cpp
    Ship MakeShip(ShipKind kind, Vector3 pos);
    void SpawnPirate();
    void SpawnMiner(bool atStation);

    void UpdatePlayer(PlayerState& ps, float dt);
    // Bewegung eines Schiffs aus seinen Eingaben. Ohne Zufall und ohne andere
    // Schiffe: Der Client benutzt genau diese Funktion für die Vorhersage
    // des eigenen Schiffs und kommt so zum selben Ergebnis wie der Server.
    void UpdateShipPhysics(Ship& s, float dt, const ShipInput& in);
    void UpdateAi(Ship& s, float dt);     // Ai.cpp
    void UpdateTurrets(float dt);
    void UpdateBullets(float dt);
    void UpdateParticles(float dt);
    void FireBullet(Ship& s);
    void DamageShip(Ship& s, float dmg, int attackerId, Faction attacker, int attackerPlayerId = -1);
    void PlayerShipLost(Ship& s);    // Server: Spielerschiff ist aufgeschlagen
    void GoalReached(PlayerState& ps);
    void Explosion(Vector3 pos, float size);           // Partikel, auf dem Server ein FxEvent
    void SpawnSparks(Vector3 pos, Color c, int n);     // Partikel, auf dem Server ein FxEvent
    void StartMining(PlayerState& ps, int kind, int id);
    void FinishMining(PlayerState& ps);
    // Missionen (World.cpp)
    void UpdateMissions(float dt);
    void UpdateConvoy(Ship& s, float dt);   // Ai.cpp: Frachter folgen der Route ihrer Mission
    void SpawnConvoyWave(Mission& m);
    void CollectPerson(PlayerState& ps, int personId);
    void MissionShipDown(const Ship& s);    // Missionsschiff abgeschossen (DamageShip)
    void EndMission(Mission& m, bool success, bool notify = true);
    void Cue(PlayerState& ps, MissionType t, int phase);
    // Trupp (Trooper.cpp)
    void BuildTrooperBlocks();             // aus Deko und Stationsteilen (StartTrooper)
    void UpdateTroopers(float dt);         // Bewegung, Nachladen, Schießen (Update)
    void FireTrooperBullet(Trooper& t);
};

// Augenpunkt und Blickrichtung (Länge 1) einer Figur (Trooper.cpp)
Vector3 TrooperEye(const Trooper& t);
Vector3 TrooperAim(const Trooper& t);

// Autopilot für den Simulationsmodus (Ai.cpp), steuert den eigenen Spieler (players[0]).
// Wird auch für die Hintergrundwelt im Hauptmenü benutzt (main.cpp).
ShipInput AutopilotInput(World& w, float dt);
void AutopilotDockActions(World& w);   // angedockt: reparieren/aufrüsten
void ResetAutopilot();                 // interner Zustand des Autopiloten zurücksetzen (neue Partie)
