#pragma once
#include "raylib.h"
#include "raymath.h"
#include "Config.hpp"
#include <random>
#include <string>
#include <vector>

enum class Faction { Player, Pirate, Neutral };
enum class ShipKind { Player = 0, Pirate = 1, Elite = 2, Miner = 3 };
enum class AiState { Wander = 0, Attack = 1, GoMine = 2, Mining = 3, GoSell = 4, Docked = 5, Flee = 6 };

// Eingaben für ein Schiff (vom Spieler oder vom Autopiloten)
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
    int model = -1;
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

    // KI
    AiState ai = AiState::Wander;
    Vector3 waypoint{};
    int targetShip = -1;     // Schiffs-ID
    int targetDeposit = -1;  // Lagerstätten-ID
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
    bool revealed = false;
    float revealT = 0.0f;
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
    int ownerId = -1;    // -1 = Geschützturm
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

enum class Outcome { None, Dead, Won };

class World {
public:
    // --- Lebenszyklus ----------------------------------------------------
    void NewGame(unsigned int seed, bool simulation);
    void Update(float dt, const ShipInput& input);
    void UpdateAmbient(float dt); // während angedockt: nur Effekte

    // --- Zugriffe --------------------------------------------------------
    Ship& Player() { return ships[0]; }
    const Ship& Player() const { return ships[0]; }
    Ship* FindShip(int id);
    Deposit* FindDeposit(int id);
    Wreck* FindWreck(int id);

    // --- Aktionen an den Stationen -----------------------------------------
    void Dock(int station);       // 0 = Handelsstation, 1 = Werft
    void Undock();
    int RepairCost() const;
    bool Repair();
    int UpgradeCost(int type) const;  // -1 wenn maximal
    bool BuyUpgrade(int type);
    void ApplyUpgrades(bool refill);

    // --- Abfragen für HUD -------------------------------------------------
    int StationStage() const;
    int NextStageThreshold() const;
    bool NearStation(int station, float range) const;
    int InteractTarget(int* kind, int* id) const; // 0 nichts, 1 Lagerstätte, 2 Wrack, 3 Handelsstation, 4 Werft
    bool PlayerAlive() const { return !Player().crashing && !Player().dead; }

    void Notify(const std::string& text, Color c = WHITE);

    // --- Rendering (Render.cpp) ---------------------------------------------
    void Draw(const Camera3D& cam) const;

    // --- Daten -------------------------------------------------------------
    unsigned int seed = 1;
    bool simulation = false;
    std::mt19937 rng;

    std::vector<Ship> ships;
    std::vector<Deposit> deposits;
    std::vector<Wreck> wrecks;
    std::vector<Bullet> bullets;
    std::vector<Particle> particles;
    std::vector<Deco> decos;
    Station stations[2];

    long long balance = 0;
    long long totalDelivered = 0;
    int upgrades[cfg::UP_COUNT] = {0, 0, 0, 0};
    float scanEnergy = cfg::SCANNER_MAX_ENERGY;
    bool scanOn = false;
    float scanPulse = 0.0f;

    bool mining = false;
    float mineT = 0.0f;
    int mineKind = 0;   // 1 = Lagerstätte, 2 = Wrack
    int mineId = -1;

    int dockedAt = -1;
    int lastSale = 0;
    int lastStage = 0;
    float stageAnim = 0.0f;

    float playTime = 0.0f;
    float shake = 0.0f;
    float time = 0.0f;
    int kills = 0;
    int nextId = 1;
    float pirateSpawnT = 8.0f;
    float minerSpawnT = 20.0f;
    float depositSpawnT = 0.0f;

    Outcome outcome = Outcome::None;
    float outcomeT = 0.0f;
    bool wantAutosave = false;

    std::vector<Notice> notices;

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
    Vector3 RandomFreePos(float minDistStations, float minDistPlayer);
    Ship MakeShip(ShipKind kind, Vector3 pos);
    void SpawnPirate();
    void SpawnMiner(bool atStation);

    void UpdatePlayer(float dt, const ShipInput& in);
    void UpdateShipPhysics(Ship& s, float dt, const ShipInput& in);
    void UpdateAi(Ship& s, float dt);     // Ai.cpp
    void UpdateTurrets(float dt);
    void UpdateBullets(float dt);
    void UpdateParticles(float dt);
    void FireBullet(Ship& s);
    void DamageShip(Ship& s, float dmg, int attackerId, Faction attacker);
    void DestroyShip(Ship& s);
    void Explosion(Vector3 pos, float size);
    void SpawnSparks(Vector3 pos, Color c, int n);
    void StartMining(int kind, int id);
    void FinishMining();
};

// Autopilot für den Simulationsmodus (Ai.cpp)
ShipInput AutopilotInput(World& w, float dt);
void AutopilotDockActions(World& w);
void ResetAutopilot();
