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
//   Ai.cpp         KI der Piraten/Händler und der Autopilot (Simulation)
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
#include "GameSettings.hpp"
#include "SoundEvents.hpp"
#include "Util.hpp"
#include <random>
#include <string>
#include <vector>

// Wer gehört zu wem? Bestimmt, wen ein Geschoss treffen darf (UpdateBullets)
// und wen die Geschütztürme angreifen (nur Piraten).
enum class Faction { Player, Pirate, Neutral };
// ShipKind wird im Netzwerk (Snapshots) und im Spielstand als Zahl
// übertragen bzw. gespeichert: Zahlenwerte nicht ändern.
enum class ShipKind { Player = 0, Pirate = 1, Elite = 2, Miner = 3 };
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
    int ownerId = -1;      // Schiffs-ID des Schützen, -1 = Geschützturm
    int ownerPlayer = -1;  // Spieler-ID, falls ein Spieler geschossen hat (für Abschüsse)
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

// Kulisse: Krater, Felsen, Rover ... (nur Optik, keine Kollision).
struct Deco {
    int model = -1;
    Vector3 pos{};
    float yaw = 0.0f;
    float scale = 1.0f;
    float yScale = 0.0f; // 0 = gleich scale (für flache, große Krater)
};

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
    int type = cfg::SHIP_START;               // cfg::ShipTypeId
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0};
    float hp = -1.0f;                         // Schild beim Parken, -1 = voll
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

    long long balance = 0;       // Kontostand (Ziel: cfg::GOAL_POINTS)
    long long delivered = 0;     // selbst abgelieferte Ressourcen (Statistik)
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0};   // Stufe je cfg::Upgrade, 0..cfg::UPGRADE_LEVELS (des geflogenen Schiffs)
    int shipType = cfg::SHIP_START;              // Typ des geflogenen Schiffs (cfg::ShipTypeId)
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
};

class World {
public:
    // --- Lebenszyklus ----------------------------------------------------
    // Alle drei Start-Funktionen ersetzen die komplette Welt (*this = World{})
    // und ziehen danach die Zufallszahlen in fester Reihenfolge aus dem Seed.
    void NewGame(unsigned int seed, bool simulation);              // Einzelspieler
    void NewServerWorld(unsigned int seed, const GameSettings& s); // Server (noch ohne Spieler)
    void BuildMirror(unsigned int seed, const GameSettings& s);    // Mehrspieler-Client: nur Kulisse
    // Ein Zeitschritt der Spielregeln (Einzelspieler: einmal pro Bild mit dem
    // dt des Bildes; Server: fest 1/30 s). Danach liegen in sounds, fx, goals
    // und PlayerState::outbox neue Ereignisse, die der Aufrufer abarbeitet
    // und leert.
    void Update(float dt);                                         // Eingaben aus players[i].input
    void Update(float dt, const ShipInput& input) { Me().input = input; Update(dt); }   // Einzelspieler
    void UpdateAmbient(float dt);   // während angedockt (Einzelspieler): nur Effekte
    void UpdateCosmetics(float dt); // Mehrspieler-Client: Rauch, Abbau-Partikel aus dem Zustand erzeugen

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
    // kaufbar (schon im Besitz oder ungültig). BuyShip prüft wie BuyUpgrade
    // nicht, an welcher Station der Spieler angedockt ist.
    bool OwnsShipType(const PlayerState& ps, int type) const;
    int TradeInValue(const PlayerState& ps) const;
    int ShipPrice(const PlayerState& ps, int type, bool tradeIn) const;
    bool BuyShip(PlayerState& ps, int type, bool tradeIn);
    // Handelsstation, Tab "Schiff": zu einem geparkten Schiff (hangar) wechseln.
    bool SwitchShip(PlayerState& ps, int type);
    float ScannerRadius(const PlayerState& ps) const;

    // Kurzformen für den Einzelspieler (wirken auf den eigenen Spieler)
    void Dock(int station) { Dock(Me(), station); }
    void Undock() { Undock(Me()); }
    int RepairCost() const { return RepairCost(Me()); }
    bool Repair() { return Repair(Me()); }
    int UpgradeCost(int type) const { return UpgradeCost(Me(), type); }
    bool BuyUpgrade(int type) { return BuyUpgrade(Me(), type); }
    bool BuyShip(int type, bool tradeIn) { return BuyShip(Me(), type, tradeIn); }
    bool SwitchShip(int type) { return SwitchShip(Me(), type); }

    // --- Abfragen (HUD, KI, Server) ----------------------------------------
    int StationStage() const;          // Ausbaustufe der Handelsstationen 0..7 (aus totalDelivered)
    int NextStageThreshold() const;    // Punkte für die nächste Stufe, -1 = schon die höchste
    bool NearStation(const Ship& s, int station, float range) const;
    bool NearStation(int station, float range) const { return NearStation(Player(), station, range); }
    int NearestStation(Vector3 p, StationType type) const;   // Index oder -1
    int CountStations(StationType type) const;
    // Was würde Taste F jetzt bewirken? Reihenfolge: Station (andocken) vor
    // Wrack vor entdeckter Lagerstätte (abbauen).
    // 0 nichts, 1 Lagerstätte, 2 Wrack, 3 Handelsstation, 4 Werft, 5 Raumschiffsverkäufer
    // (id = Lagerstätte/Wrack/Stationsindex)
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

    std::vector<PlayerState> players;
    std::vector<Ship> ships;
    std::vector<Deposit> deposits;
    std::vector<Wreck> wrecks;
    std::vector<Bullet> bullets;
    std::vector<Particle> particles;
    std::vector<Deco> decos;
    std::vector<Station> stations;   // erst alle Bauplätze in fester Reihenfolge (siehe WorldGen.cpp)

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
        int shipTypes[cfg::SHIP_TYPE_COUNT];   // Modelle der Spieler-Schiffstypen (cfg::SHIP_TYPES)
        int terrain;
        int crystalSmall, crystalA, crystalB;
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
};

// Autopilot für den Simulationsmodus (Ai.cpp), steuert den eigenen Spieler (players[0]).
// Wird auch für die Hintergrundwelt im Hauptmenü benutzt (main.cpp).
ShipInput AutopilotInput(World& w, float dt);
void AutopilotDockActions(World& w);   // angedockt: reparieren/aufrüsten
void ResetAutopilot();                 // interner Zustand des Autopiloten zurücksetzen (neue Partie)
