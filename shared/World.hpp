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
// Abhängigkeiten: nur raylib.h/raymath.h (Datentypen und Mathe aus Headern),
// Config.hpp (Balancing), GameSettings.hpp, SoundEvents.hpp, Models.hpp.
// Keine raylib-Funktionen, kein Fenster, keine Grafik. Draw() ist zwar hier
// deklariert, aber in src/Render.cpp definiert und nur im Client vorhanden.
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

enum class Faction { Player, Pirate, Neutral };
enum class ShipKind { Player = 0, Pirate = 1, Elite = 2, Miner = 3 };
enum class AiState { Wander = 0, Attack = 1, GoMine = 2, Mining = 3, GoSell = 4, Docked = 5, Flee = 6 };
enum class StationType { Trade = 0, Shipyard = 1 };

// Eingaben für ein Schiff (vom Spieler, vom Autopiloten oder über das Netzwerk)
struct ShipInput {
    float thrust = 0.0f;   // -1..1
    float turn = 0.0f;     // -1 (rechts) .. 1 (links)
    bool fire = false;
    bool scanToggle = false;
    bool interact = false;
};

struct Ship {
    int id = 0;
    ShipKind kind = ShipKind::Player;
    Faction faction = Faction::Player;
    int model = -1;               // Modell-ID (Models.hpp)
    Vector3 pos{};
    Vector3 vel{};
    float yaw = 0.0f;
    float bank = 0.0f;
    float hp = 100.0f, maxHp = 100.0f;
    float maxSpeed = 16.0f;
    float turnRate = 2.8f;
    float damage = 10.0f;
    float fireInterval = 0.5f;
    float fireCd = 0.0f;
    int cargo = 0;
    int cargoMax = 0;
    float hitFlash = 0.0f;
    bool crashing = false;
    float crashT = 0.0f;
    float crashSpin = 0.0f;
    bool dead = false;            // wird am Ende des Frames entfernt
    bool hostileToPlayer = false; // Händler/Miner nach einem Angriff
    float thrustVisual = 0.0f;
    int playerId = -1;            // >= 0: Schiff eines menschlichen Spielers (PlayerState::id)
    bool docked = false;          // Spieler ist angedockt: nicht angreifbar, bewegt sich nicht

    // KI
    AiState ai = AiState::Wander;
    Vector3 waypoint{};
    int targetShip = -1;     // Schiffs-ID
    int targetDeposit = -1;  // Lagerstätten-ID
    int station = -1;        // Index der Station, an der ein Händler angedockt hat
    float aiTimer = 0.0f;
    float thinkT = 0.0f;
    float breakOff = 0.0f;
    float aggroT = 0.0f;     // nach Treffer: verfolgt Angreifer länger
    bool mining = false;
    Vector3 mineTarget{};
    bool hidden = false;     // angedockt (nicht sichtbar)
};

struct Deposit {
    int id = 0;
    Vector3 pos{};
    float yaw = 0.0f;
    float scale = 1.0f;
    int model = -1;
    int amount = 0;
    int initial = 0;
    bool revealed = false;          // Einzelspieler/Client: für den eigenen Spieler entdeckt
    float revealT = 0.0f;           // Einblend-Animation (0..1)
    std::vector<int> revealedFor;   // Server: IDs der Spieler, die sie entdeckt haben
};

struct Wreck {
    int id = 0;
    Vector3 pos{};
    float yaw = 0.0f;
    float tilt = 0.0f;
    int model = -1;
    int amount = 0;
    int initial = 0;
    float age = 0.0f;
};

struct Bullet {
    Vector3 pos{};
    Vector3 vel{};
    float life = 0.0f;
    float damage = 0.0f;
    Faction faction = Faction::Player;
    int ownerId = -1;      // Schiffs-ID des Schützen, -1 = Geschützturm
    int ownerPlayer = -1;  // Spieler-ID, falls ein Spieler geschossen hat (für Abschüsse)
};

struct Particle {
    Vector3 pos{};
    Vector3 vel{};
    float life = 0.0f;
    float maxLife = 1.0f;
    float size = 0.3f;
    Color color = WHITE;
    float gravity = 0.0f;
    bool glow = false;
};

struct Deco {
    int model = -1;
    Vector3 pos{};
    float yaw = 0.0f;
    float scale = 1.0f;
    float yScale = 0.0f; // 0 = gleich scale (für flache, große Krater)
};

struct StationPart {
    int model = -1;
    Vector3 offset{};
    float yaw = 0.0f;
    float scale = 1.0f;
    int stage = 0;
};

struct Turret {
    Vector3 offset{};
    float yaw = 0.0f;
    float cd = 0.0f;
    int stage = 0;
    bool isDouble = true;
};

struct Station {
    std::string name;
    StationType type = StationType::Trade;
    Vector3 pos{};
    std::vector<StationPart> parts;
    std::vector<Turret> turrets;
    std::vector<Vector3> obstacles; // x,z = Position (lokal), y = Radius (für hohe Teile)
};

struct Notice {
    std::string text;
    float time = 0.0f;
    Color color = WHITE;
    int count = 1;
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

enum class Outcome { None, Dead, Won };

// ---------------------------------------------------------------------------
// Alles, was zu EINEM menschlichen Spieler gehört (Konto, Aufrüstungen,
// Scanner, Abbau, Andocken). Im Einzelspieler gibt es genau einen davon.
// Das Schiff selbst steht in World::ships (verbunden über shipId).
// ---------------------------------------------------------------------------
struct PlayerState {
    int id = 0;                  // Spieler-ID (Einzelspieler: 0)
    int shipId = -1;             // Ship::id des eigenen Schiffs, -1 = gerade keins (wartet auf neues)
    std::string name;
    ShipInput input;             // Eingaben für den nächsten Update-Schritt

    long long balance = 0;       // Kontostand (Ziel: cfg::GOAL_POINTS)
    long long delivered = 0;     // selbst abgelieferte Ressourcen (Statistik)
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0};

    float scanEnergy = cfg::SCANNER_MAX_ENERGY;
    bool scanOn = false;
    float scanPulse = 0.0f;

    bool mining = false;
    float mineT = 0.0f;
    int mineKind = 0;            // 1 = Lagerstätte, 2 = Wrack
    int mineId = -1;

    int dockedAt = -1;           // Index in World::stations, -1 = nicht angedockt
    int lastSale = 0;

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
    void NewGame(unsigned int seed, bool simulation);              // Einzelspieler
    void NewServerWorld(unsigned int seed, const GameSettings& s); // Server (noch ohne Spieler)
    void BuildMirror(unsigned int seed, const GameSettings& s);    // Mehrspieler-Client: nur Kulisse
    void Update(float dt);                                         // Eingaben aus players[i].input
    void Update(float dt, const ShipInput& input) { Me().input = input; Update(dt); }
    void UpdateAmbient(float dt);   // während angedockt (Einzelspieler): nur Effekte
    void UpdateCosmetics(float dt); // Mehrspieler-Client: Rauch, Abbau-Partikel aus dem Zustand erzeugen

    // --- Spieler -----------------------------------------------------------
    // Einzelspieler und Client: der eigene Spieler ist players[0], sein Schiff ships[0].
    PlayerState& Me() { return players[0]; }
    const PlayerState& Me() const { return players[0]; }
    Ship& Player() { return ships[0]; }
    const Ship& Player() const { return ships[0]; }

    PlayerState* FindPlayer(int playerId);
    const PlayerState* FindPlayer(int playerId) const;
    Ship* PlayerShip(const PlayerState& ps);
    const Ship* PlayerShip(const PlayerState& ps) const;
    PlayerState* OwnerOfShip(int shipId);   // Spieler, dem ein Schiff gehört (oder nullptr)

    // Server: Spieler hinzufügen (mit Schiff an einer Handelsstation) und entfernen
    PlayerState& AddPlayer(int playerId, const std::string& name);
    void RemovePlayer(int playerId);
    void SpawnPlayerShip(PlayerState& ps);
    void ResetProgress(PlayerState& ps);    // nach Absturz: Fortschritt von vorne

    Ship* FindShip(int id);
    const Ship* FindShip(int id) const;
    Deposit* FindDeposit(int id);
    Wreck* FindWreck(int id);

    // --- Aktionen an den Stationen -----------------------------------------
    void Dock(PlayerState& ps, int station);   // station = Index in stations
    void Undock(PlayerState& ps);
    int RepairCost(const PlayerState& ps) const;
    bool Repair(PlayerState& ps);
    int UpgradeCost(const PlayerState& ps, int type) const;  // -1 wenn maximal
    bool BuyUpgrade(PlayerState& ps, int type);
    void ApplyUpgrades(PlayerState& ps, bool refill);

    // Kurzformen für den Einzelspieler (wirken auf den eigenen Spieler)
    void Dock(int station) { Dock(Me(), station); }
    void Undock() { Undock(Me()); }
    int RepairCost() const { return RepairCost(Me()); }
    bool Repair() { return Repair(Me()); }
    int UpgradeCost(int type) const { return UpgradeCost(Me(), type); }
    bool BuyUpgrade(int type) { return BuyUpgrade(Me(), type); }

    // --- Abfragen (HUD, KI, Server) ----------------------------------------
    int StationStage() const;
    int NextStageThreshold() const;
    bool NearStation(const Ship& s, int station, float range) const;
    bool NearStation(int station, float range) const { return NearStation(Player(), station, range); }
    int NearestStation(Vector3 p, StationType type) const;   // Index oder -1
    int CountStations(StationType type) const;
    // 0 nichts, 1 Lagerstätte, 2 Wrack, 3 Handelsstation, 4 Werft (id = Lagerstätte/Wrack/Stationsindex)
    int InteractTarget(const PlayerState& ps, int* kind, int* id) const;
    int InteractTarget(int* kind, int* id) const { return InteractTarget(Me(), kind, id); }
    bool PlayerAlive(const PlayerState& ps) const;
    bool PlayerAlive() const { return !Player().crashing && !Player().dead; }
    bool RevealedFor(const Deposit& d, const PlayerState& ps) const;
    bool Targetable(const Ship& s) const;   // darf von Gegnern angegriffen werden

    void Notify(PlayerState& ps, const std::string& text, Color c = WHITE);
    void Notify(const std::string& text, Color c = WHITE) { Notify(Me(), text, c); }
    void SoundAt(Sfx id, Vector3 pos, float volume = 1.0f, float delay = 0.0f, int forPlayer = -1,
                 int sourceShip = -1);

    // --- Rendering (nur Client, src/Render.cpp) --------------------------------
    void Draw(const Camera3D& cam) const;

    // --- Daten -------------------------------------------------------------
    unsigned int seed = 1;
    bool simulation = false;   // Einzelspieler-Autopilot (keine Meldungen)
    bool isServer = false;     // Mehrspieler-Regeln, keine Partikel (Server)
    bool isMirror = false;     // Mehrspieler-Client: Welt wird vom Server gefüllt
    GameSettings settings;
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
    int lastStage = 0;
    float stageAnim = 0.0f;

    float shake = 0.0f;
    float time = 0.0f;
    int nextId = 1;
    float pirateSpawnT = 8.0f;
    float minerSpawnT = 20.0f;
    float depositSpawnT = 0.0f;
    int depositRevision = 0;   // zählt hoch, wenn sich Lagerstätten ändern (Server: neu verschicken)

    Outcome outcome = Outcome::None;   // nur Einzelspieler
    float outcomeT = 0.0f;
    bool wantAutosave = false;

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
    } mdl{};

    // --- interne Helfer ------------------------------------------------------
    float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
    int RandInt(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

    void ResolveModels();
    void GenerateTerrain();          // WorldGen.cpp
    void BuildStations();            // WorldGen.cpp
    void SpawnDeposit(bool initial); // WorldGen.cpp
    int DepositTarget() const;       // gewünschte Anzahl Lagerstätten
    Vector3 RandomFreePos(float minDistStations, float minDistPlayer);
    Ship MakeShip(ShipKind kind, Vector3 pos);
    void SpawnPirate();
    void SpawnMiner(bool atStation);

    void UpdatePlayer(PlayerState& ps, float dt);
    void UpdateShipPhysics(Ship& s, float dt, const ShipInput& in);
    void UpdateAi(Ship& s, float dt);     // Ai.cpp
    void UpdateTurrets(float dt);
    void UpdateBullets(float dt);
    void UpdateParticles(float dt);
    void FireBullet(Ship& s);
    void DamageShip(Ship& s, float dmg, int attackerId, Faction attacker, int attackerPlayerId = -1);
    void PlayerShipLost(Ship& s);    // Server: Spielerschiff ist aufgeschlagen
    void GoalReached(PlayerState& ps);
    void Explosion(Vector3 pos, float size);
    void SpawnSparks(Vector3 pos, Color c, int n);
    void StartMining(PlayerState& ps, int kind, int id);
    void FinishMining(PlayerState& ps);
};

// Autopilot für den Simulationsmodus (Ai.cpp), steuert den eigenen Spieler (players[0])
ShipInput AutopilotInput(World& w, float dt);
void AutopilotDockActions(World& w);
void ResetAutopilot();
