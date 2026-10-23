// ---------------------------------------------------------------------------
// World.cpp: die Spielregeln von Mondschürfer (Bewegung, Scanner, Abbau,
// Kampf, Stationen, Mehrspieler-Regeln).
//
// Wer benutzt die Datei? Alle drei Programmteile über die Klasse World
// (Überblick und Datentypen in World.hpp):
//   - Einzelspieler (src/main.cpp): NewGame, Update, Dock/Repair/BuyUpgrade.
//   - Server (server/src/GameServer.cpp): NewServerWorld, AddPlayer/
//     RemovePlayer, 30-mal pro Sekunde Update(net::TICK_DT).
//   - Mehrspieler-Client (src/NetClient.cpp): BuildMirror, UpdateCosmetics,
//     UpdateShipPhysics (Vorhersage), Explosion/SpawnSparks/Notify.
// Der Aufbau der Welt steht in WorldGen.cpp, die KI in Ai.cpp, das Zeichnen
// in src/Render.cpp.
//
// Abhängigkeiten: World.hpp (mit Config.hpp, Util.hpp, SoundEvents.hpp,
// GameSettings.hpp) und die Vektor-Mathe aus raymath.h (Vector3Add, Lerp ...).
// raymath ist komplett im Header definiert, braucht also keine
// raylib-Bibliothek: Deshalb läuft diese Datei auch im Server ohne Grafik.
// (Models.hpp ist eingebunden, wird hier derzeit aber nicht gebraucht; die
// Modell-IDs löst WorldGen.cpp auf.)
//
// Ablauf eines Zeitschritts (Update):
//   Spieler (Eingaben, Scanner, Abbau) -> KI der anderen Schiffe -> abstürzende
//   Schiffe -> Schiffe auseinanderschieben -> Geschütztürme -> Geschosse ->
//   Partikel -> Wracks -> Aufräumen -> neue Lagerstätten/Gegner -> Meldungen.
// Ergebnis neben dem neuen Zustand: Ereignisse in sounds, fx, goals und
// PlayerState::outbox, die der Aufrufer verteilt und danach leert.
//
// Einzelspieler und Server unterscheiden sich nur an wenigen, mit
// "isServer" markierten Stellen: Partikel nur ohne Server (der Server meldet
// FxEvents), Spielende (outcome) nur im Einzelspieler, Neustart mit neuem
// Schiff und Zielrunden nur auf dem Server, Lagerstätten je Spieler
// entdeckt, Gegnerzahl aus den Server-Einstellungen, angedockte Schiffe
// geschützt.
//
// Wichtig für den Einzelspieler: Die Zufallszahlen (Rand/RandInt) werden in
// einer festen Reihenfolge gezogen. Gleicher Seed + gleiche Eingaben + gleiche
// Zeitschritte (dt) ergeben deshalb genau dasselbe Spiel (die Testläufe mit
// --screenshot rechnen deshalb mit festem dt). Beim Umbau für den Mehrspieler
// wurde darauf geachtet, diese Reihenfolge im Einzelspieler nicht zu
// verändern. An manchen Stellen wird deshalb eine Zufallszahl gezogen, die
// eigentlich nicht gebraucht wird, oder bewusst KEINE gezogen (siehe
// MakeShip, SpawnMiner). Einschränkung: Der Wechsel zwischen linker und
// rechter Kanone in FireBullet hängt an einem static-Zähler, der über
// mehrere Partien hinweg weiterläuft.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "Models.hpp"
#include <algorithm>
#include <cmath>

namespace {
// Gefahrenstufe der Piraten 0..5: je 18.000 insgesamt abgelieferte Punkte eine
// Stufe mehr (höchste Stufe ab 90.000). Höhere Stufe = mehr Schild, schneller,
// schießt öfter, mehr Beute im Wrack (MakeShip). So wächst die Gefahr mit dem
// Fortschritt. Im Mehrspieler zählt totalDelivered aller Spieler zusammen.
int PirateTier(long long delivered) { return static_cast<int>(std::min<long long>(5, delivered / 18000)); }
} // namespace

// ===========================================================================
// Neues Spiel
// ===========================================================================
// Einzelspieler: neue Partie. *this = World{} setzt ALLES auf Anfang
// (Listen, Zähler, Ereignisse), danach wird mit dem Seed neu aufgebaut.
// Die Reihenfolge ist wichtig: Die Stationen müssen vor der Mondoberfläche
// stehen (Felsen und Krater halten Abstand zu ihnen), und jeder Schritt
// verbraucht Zufallszahlen (siehe Dateikopf).
void World::NewGame(unsigned int newSeed, bool sim) {
    *this = World{};
    seed = newSeed;
    simulation = sim;
    rng.seed(seed);

    ResolveModels();
    BuildStations();
    GenerateTerrain();

    // Einzelspieler: genau ein Spieler mit ID 0, sein Schiff steht immer an ships[0]
    // (Me()/Player() verlassen sich darauf). Start 24 Einheiten vor der
    // Handelsstation in der Kartenmitte: außerhalb der Andockweite, aber im
    // Schutz ihrer Geschütztürme.
    players.push_back(PlayerState{});
    PlayerState& ps = players[0];
    Ship p = MakeShip(ShipKind::Player, {0.0f, cfg::SHIP_ALTITUDE, 24.0f});
    p.yaw = PI; // Blick nach Süden, weg von der Station
    p.playerId = ps.id;
    ships.push_back(p);
    ps.shipId = p.id;
    ApplyUpgrades(ps, true);

    // Erste Belegung der Karte. Der erste Händler startet an der Station,
    // die übrigen irgendwo draußen.
    for (int i = 0; i < cfg::DEPOSIT_COUNT; ++i) SpawnDeposit(true);
    for (int i = 0; i < cfg::PIRATES_BASE; ++i) SpawnPirate();
    for (int i = 0; i < cfg::MINERS_COUNT; ++i) SpawnMiner(i == 0);

    lastStage = StationStage();
    // Im Simulationsmodus (Autopilot, Menü-Hintergrund) liest niemand die
    // Meldungen; deshalb werden sie dort weggelassen.
    if (!simulation) {
        Notify("Willkommen, Pilot! Finde Kristalle mit dem Scanner [E].", Color{255, 200, 120, 255});
        Notify("Ziel: 100.000 Ressourcenpunkte auf dem Konto.", Color{200, 220, 255, 255});
    }
}

// Server: Welt ohne Spieler. Die Spieler kommen später über AddPlayer dazu.
// totalDelivered (auf dem Server gespeichert) und lastStage setzt
// GameServer::InitWorld direkt danach selbst.
void World::NewServerWorld(unsigned int newSeed, const GameSettings& s) {
    *this = World{};
    seed = newSeed;
    isServer = true;
    settings = s;
    Sanitize(settings);   // Werte in erlaubte Grenzen bringen (GameSettings.hpp)
    rng.seed(seed);

    ResolveModels();
    BuildStations();
    // Die Deko (decos) braucht der Server nicht, er zeichnet nichts. Der
    // Aufruf hält aber den Aufbau gleich wie im Client (BuildMirror) und im
    // Einzelspieler und kostet nur einmal beim Start etwas Zeit.
    GenerateTerrain();

    // Noch ohne Spieler: DepositTarget() ist hier cfg::DEPOSIT_COUNT. Weitere
    // Lagerstätten wachsen in Update nach, wenn Spieler dazukommen.
    for (int i = 0; i < DepositTarget(); ++i) SpawnDeposit(true);
    for (int i = 0; i < settings.enemyShips; ++i) SpawnPirate();
    for (int i = 0; i < cfg::MINERS_COUNT; ++i) SpawnMiner(i == 0);
    lastStage = StationStage();
}

// Mehrspieler-Client: nur die Kulisse. Seed und Einstellungen kommen vom
// Server (Welcome-Nachricht). Weil der Zufallsgenerator mit demselben Seed
// dieselbe Zahlenfolge liefert, entstehen hier dieselben Krater und Felsen
// wie beim Server, ohne dass sie übertragen werden müssen. Die Stationen
// hängen nur von den Einstellungen ab (BuildStations zieht keine
// Zufallszahlen), ihre Reihenfolge ist daher auf beiden Seiten gleich. Das
// ist wichtig, weil Server und Client Stationen über ihren Index ansprechen
// (PlayerState::dockedAt).
void World::BuildMirror(unsigned int newSeed, const GameSettings& s) {
    *this = World{};
    seed = newSeed;
    isMirror = true;
    settings = s;
    Sanitize(settings);
    rng.seed(seed);

    // Gleiche Reihenfolge wie beim Server: dadurch entsteht dieselbe Mondoberfläche
    ResolveModels();
    BuildStations();
    GenerateTerrain();

    // Platzhalter für den eigenen Spieler und sein Schiff (Index 0). Der Client
    // überschreibt beides mit den Daten vom Server. Zuschauer haben kein Schiff:
    // das Platzhalterschiff bleibt dann unsichtbar.
    players.push_back(PlayerState{});
    Ship dummy;
    dummy.id = -1;
    dummy.model = mdl.player;
    dummy.hidden = true;
    dummy.dead = true;
    ships.push_back(dummy);
}

// Neues Schiff mit den Grundwerten seiner Art. Wird noch NICHT in ships
// eingetragen (das macht der Aufrufer). Spielerschiffe bekommen ihre
// eigentlichen Werte danach aus den Aufrüstungen (ApplyUpgrades).
// Balancing der Gegner: Piraten werden mit der Gefahrenstufe (tier 0..5)
// stärker. Ein normaler Pirat hat anfangs 40 Schild (4 Treffer mit der
// Startwaffe), auf Stufe 5 dann 100. Elite-Piraten sind schneller als das
// Startschiff (18,5 gegen 16) und halten 10 Treffer aus. Händler sind
// langsam und schießen nur zurück, wenn man sie angreift.
Ship World::MakeShip(ShipKind kind, Vector3 pos) {
    Ship s;
    s.id = nextId++;
    s.kind = kind;
    s.pos = pos;
    s.pos.y = cfg::SHIP_ALTITUDE;
    // Auch beim Spielerschiff wird ein Zufallswinkel gezogen, obwohl der
    // Aufrufer ihn gleich überschreibt: So bleibt die Folge der
    // Zufallszahlen für alle Schiffsarten gleich (siehe Dateikopf).
    s.yaw = Rand(0, 2 * PI);
    const int tier = PirateTier(totalDelivered);
    switch (kind) {
        case ShipKind::Player:
            s.faction = Faction::Player;
            s.model = mdl.player;
            s.turnRate = cfg::PLAYER_TURN_RATE;
            break;
        case ShipKind::Pirate:
            s.faction = Faction::Pirate;
            s.model = Rand(0, 1) < 0.5f ? mdl.pirateA : mdl.pirateB;
            s.maxHp = s.hp = 40.0f + tier * 12.0f;
            s.maxSpeed = 14.5f + tier * 0.8f;
            s.turnRate = 2.3f;
            s.damage = cfg::PIRATE_DAMAGE;
            s.fireInterval = 0.95f - tier * 0.05f;
            // Beute im Wrack: 150..400 Punkte, je Stufe 80 mehr
            s.cargo = RandInt(15, 40) * 10 + tier * 80;
            s.cargoMax = s.cargo;
            break;
        case ShipKind::Elite:
            s.faction = Faction::Pirate;
            s.model = mdl.elite;
            s.maxHp = s.hp = 100.0f + tier * 18.0f;
            s.maxSpeed = 18.5f + tier * 0.6f;
            s.turnRate = 2.6f;
            s.damage = cfg::ELITE_DAMAGE;
            s.fireInterval = 0.6f;
            // Elite: 500..950 Punkte Beute, je Stufe 120 mehr
            s.cargo = RandInt(50, 95) * 10 + tier * 120;
            s.cargoMax = s.cargo;
            break;
        case ShipKind::Miner: {
            s.faction = Faction::Neutral;
            // Drei Modelle für Abwechslung (Schürfer oder einer von zwei Frachtern)
            int v = RandInt(0, 2);
            s.model = v == 0 ? mdl.miner : (v == 1 ? mdl.cargoA : mdl.cargoB);
            s.maxHp = s.hp = 70.0f;
            s.maxSpeed = 11.5f;
            s.turnRate = 2.0f;
            s.damage = 10.0f;
            s.fireInterval = 1.0f;
            s.cargoMax = RandInt(40, 90) * 10;   // so viel lädt der Händler, bevor er verkaufen fliegt
            s.ai = AiState::GoMine;
            break;
        }
    }
    return s;
}

// Neuer Pirat irgendwo draußen: mindestens 25 Einheiten außerhalb der
// Schutzzonen der Stationen und (sobald es Schiffe gibt) 70 Einheiten weg von
// jedem Spielerschiff, damit niemand direkt neben sich einen Gegner
// "auftauchen" sieht. (RandomFreePos versucht das 200-mal; auf einer sehr
// vollen Karte nimmt es danach die letzte Position.)
// Die Chance auf einen Elite-Piraten wächst mit totalDelivered: 0 % am
// Anfang, höchstens 45 % (ab 31.500 Punkten).
void World::SpawnPirate() {
    Vector3 p = RandomFreePos(cfg::STATION_SAFE_RADIUS + 25.0f, ships.empty() ? 0.0f : 70.0f);
    float eliteChance = std::clamp(static_cast<float>(totalDelivered) / 70000.0f, 0.0f, 0.45f);
    ShipKind k = Rand(0, 1) < eliteChance ? ShipKind::Elite : ShipKind::Pirate;
    Ship s = MakeShip(k, p);
    s.ai = AiState::Wander;
    s.waypoint = s.pos;
    ships.push_back(s);
}

// Neuer neutraler Händler: entweder direkt vor einer Handelsstation (als
// käme er gerade heraus) oder irgendwo draußen. Er beginnt mit der Suche
// nach einer Lagerstätte (AiState::GoMine, Ai.cpp).
void World::SpawnMiner(bool atStation) {
    Vector3 p{};
    if (atStation) {
        // Bei mehreren Handelsstationen eine zufällige wählen (bei einer: ohne Zufallszahl,
        // damit der Einzelspieler dieselbe Zufallsfolge behält)
        int trades = CountStations(StationType::Trade);
        int pick = trades > 1 ? RandInt(0, trades - 1) : 0;
        int idx = 0;
        for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
            if (stations[i].type != StationType::Trade) continue;
            if (pick-- == 0) { idx = i; break; }
        }
        const Vector3 sp = stations[idx].pos;
        p = Vector3{sp.x + Rand(-12, 12), 0, sp.z + 16.0f};
    } else {
        p = RandomFreePos(30.0f, 30.0f);
    }
    Ship s = MakeShip(ShipKind::Miner, p);
    ships.push_back(s);
}

int World::DepositTarget() const {
    if (!isServer) return cfg::DEPOSIT_COUNT;
    // Mehr Spieler brauchen mehr Lagerstätten: 48 + 8 je weiterem Spieler,
    // höchstens 240 (Config.hpp, MP_DEPOSITS_*).
    int extra = std::max(0, static_cast<int>(players.size()) - 1) * cfg::MP_DEPOSITS_PER_PLAYER;
    return std::min(cfg::MP_DEPOSITS_MAX, cfg::DEPOSIT_COUNT + extra);
}

// ===========================================================================
// Spieler (Mehrspieler-Server)
//
// Die Such-Funktionen gehen die Listen einfach der Reihe nach durch. Bei
// höchstens 64 Spielern und rund hundert Schiffen ist das schnell genug
// und viel einfacher als eine zusätzliche Tabelle (ID -> Eintrag), die man
// bei jedem Einfügen/Löschen pflegen müsste.
// ===========================================================================
PlayerState* World::FindPlayer(int playerId) {
    for (auto& ps : players) if (ps.id == playerId) return &ps;
    return nullptr;
}

const PlayerState* World::FindPlayer(int playerId) const {
    for (const auto& ps : players) if (ps.id == playerId) return &ps;
    return nullptr;
}

Ship* World::PlayerShip(const PlayerState& ps) { return ps.shipId >= 0 ? FindShip(ps.shipId) : nullptr; }
const Ship* World::PlayerShip(const PlayerState& ps) const { return ps.shipId >= 0 ? FindShip(ps.shipId) : nullptr; }

PlayerState* World::OwnerOfShip(int shipId) {
    Ship* s = FindShip(shipId);
    if (!s || s->playerId < 0) return nullptr;
    return FindPlayer(s->playerId);
}

// Server: neuer Spieler mit frischem Schiff. Die ID vergibt der GameServer
// (1..30000, passt in die int16-Felder der Snapshots). Gespeicherten
// Fortschritt (Kontostand, Aufrüstungen ...) setzt GameServer::RestoreProgress
// anschließend in den zurückgegebenen PlayerState.
PlayerState& World::AddPlayer(int playerId, const std::string& name) {
    PlayerState ps;
    ps.id = playerId;
    ps.name = name;
    ps.lives = settings.lives;
    players.push_back(ps);
    // Referenz erst NACH push_back holen: push_back kann den Vektor in neuen
    // Speicher umziehen lassen, alte Referenzen wären dann ungültig.
    PlayerState& added = players.back();
    SpawnPlayerShip(added);
    return added;
}

// Server: Spieler abgemeldet. Sein Schiff verschwindet ohne Wrack und ohne
// Explosion (ein Abmelden mitten im Absturz wertet GameServer::LogoutPlayer
// vorher selbst als Absturz). Danach wird der Spieler aus allen
// "entdeckt"-Listen gestrichen, denn Spieler-IDs werden später neu vergeben.
void World::RemovePlayer(int playerId) {
    PlayerState* ps = FindPlayer(playerId);
    if (!ps) return;
    const int shipId = ps->shipId;
    // Schiff sofort entfernen; Geschosse und KI-Ziele verweisen nur über die ID darauf.
    // remove_if + erase ist das übliche C++-Muster zum Löschen aus einem
    // Vektor: remove_if schiebt die zu behaltenden Einträge nach vorn,
    // erase schneidet den Rest ab.
    ships.erase(std::remove_if(ships.begin(), ships.end(), [&](const Ship& s) { return s.id == shipId && shipId >= 0; }),
                ships.end());
    for (auto& d : deposits) {
        auto& v = d.revealedFor;
        v.erase(std::remove(v.begin(), v.end(), playerId), v.end());
    }
    players.erase(std::remove_if(players.begin(), players.end(), [&](const PlayerState& p) { return p.id == playerId; }),
                  players.end());
}

// Server: neues Schiff für einen Spieler (beim Anmelden und nach einem
// Absturz). Es steht auf einem Kreis mit Radius cfg::MP_SPAWN_DISTANCE um
// eine zufällige Handelsstation, also im Schutz der Geschütztürme.
// ApplyUpgrades(ps, true) überträgt die Aufrüstungsstufen auf das Schiff und
// füllt den Schild.
void World::SpawnPlayerShip(PlayerState& ps) {
    // An einer zufälligen Handelsstation, mit Blick von der Station weg
    std::vector<int> trades;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i)
        if (stations[i].type == StationType::Trade) trades.push_back(i);
    const Vector3 c = trades.empty() ? Vector3{0, 0, 0} : stations[trades[RandInt(0, static_cast<int>(trades.size()) - 1)]].pos;
    const float a = Rand(0, 2 * PI);
    Vector3 pos{c.x + cosf(a) * cfg::MP_SPAWN_DISTANCE, cfg::SHIP_ALTITUDE, c.z + sinf(a) * cfg::MP_SPAWN_DISTANCE};
    Ship s = MakeShip(ShipKind::Player, pos);
    s.yaw = YawTowards(c, pos);
    s.playerId = ps.id;
    ships.push_back(s);
    ps.shipId = s.id;
    ps.respawnT = 0.0f;
    ApplyUpgrades(ps, true);
}

// Mehrspieler-Regel: Wer abstürzt, verliert Kontostand, Aufrüstungen und
// Runde; auch die Schiffszeit beginnt von vorn. Die Statistik (delivered,
// kills, deaths, goals) bleibt.
// Aufgerufen von PlayerShipLost und GameServer (Abmelden im Absturz).
void World::ResetProgress(PlayerState& ps) {
    ps.balance = 0;
    for (int& u : ps.upgrades) u = 0;
    ps.shipType = cfg::SHIP_START;   // auch gekaufte Schiffe sind weg
    ps.hangar.clear();
    ps.scanEnergy = cfg::SCANNER_MAX_ENERGY;
    ps.scanOn = false;
    ps.scanPulse = 0.0f;
    ps.mining = false;
    ps.mineT = 0.0f;
    ps.dockedAt = -1;
    ps.lastSale = 0;
    ps.playTime = 0.0f;
    ps.roundTime = 0.0f;
    ps.round = 1;
    ps.lives = settings.lives;
}

bool World::PlayerAlive(const PlayerState& ps) const {
    const Ship* s = PlayerShip(ps);
    return s && !s->crashing && !s->dead;
}

// Einzelspieler und Client kennen nur den eigenen Spieler: dort reicht das
// Feld revealed (der Client bekommt ohnehin nur die Lagerstätten, die er
// entdeckt hat). Der Server führt pro Lagerstätte eine Liste der Spieler.
bool World::RevealedFor(const Deposit& d, const PlayerState& ps) const {
    if (!isServer) return d.revealed;
    return std::find(d.revealedFor.begin(), d.revealedFor.end(), ps.id) != d.revealedFor.end();
}

// Darf ein Gegner dieses Schiff angreifen? Nicht, wenn es schon zerstört ist,
// abstürzt, unsichtbar in einer Station steht oder angedockt ist.
bool World::Targetable(const Ship& s) const {
    return !s.dead && !s.crashing && !s.hidden && !s.docked;
}

// ===========================================================================
// Hauptupdate
//
// Ein Zeitschritt der gesamten Spielwelt. dt = Länge des Schritts in
// Sekunden. Einzelspieler: dt des Bildes (höchstens 1/20 s, main.cpp),
// Server: immer net::TICK_DT = 1/30 s. Die Eingaben stehen vorher in
// players[i].input. Die Reihenfolge der Abschnitte steht im Dateikopf.
//
// Alle Bewegungen sind einfache Euler-Schritte: neue Position = Position +
// Geschwindigkeit * dt. Das ist ungenau bei großen dt, reicht für dieses
// Spiel aber völlig.
// ===========================================================================
void World::Update(float dt) {
    time += dt;
    // Spielzeiten: Einzelspieler zählt bis zum Spielende. Auf dem Server
    // zählt nur die Zeit mit lebendem Schiff (Schiffszeit und Rundenzeit für
    // die Bestenliste), nicht die Wartezeit auf ein neues Schiff.
    for (auto& ps : players) {
        if (!isServer) {
            if (outcome == Outcome::None) ps.playTime += dt;
        } else if (PlayerAlive(ps)) {
            ps.playTime += dt;
            ps.roundTime += dt;
        }
    }
    if (outcome != Outcome::None) outcomeT += dt;

    for (auto& ps : players) UpdatePlayer(ps, dt);

    // KI aller anderen Schiffe. Versteckte Schiffe überspringen, außer
    // Händlern, die in einer Station warten (ihr Aufenthalts-Timer muss
    // weiterlaufen, damit sie wieder herauskommen).
    for (size_t i = 0; i < ships.size(); ++i) {
        Ship& s = ships[i];
        if (s.kind == ShipKind::Player) continue;  // Spielerschiffe werden über ihre Eingaben gesteuert
        if (s.dead || s.crashing || (s.hidden && s.ai != AiState::Docked)) continue;
        UpdateAi(s, dt);
    }

    // Abstürzende Schiffe: Das Schiff wurde beim Schild 0 kurz nach oben
    // geworfen (DamageShip, vel.y = 1,5), fällt jetzt mit 11 Einheiten/s²
    // (kein echtes Mondschwerefeld, sondern so, dass es gut aussieht),
    // trudelt (crashSpin) und wird seitlich gebremst. Von der Flughöhe 3,2 bis
    // zum Aufschlag dauert das knapp 0,9 s.
    for (size_t i = 0; i < ships.size(); ++i) {
        Ship& s = ships[i];
        if (!s.crashing || s.dead) continue;
        s.crashT += dt;
        s.vel.y -= 11.0f * dt;
        s.vel.x *= (1.0f - 0.6f * dt);
        s.vel.z *= (1.0f - 0.6f * dt);
        s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt));
        s.yaw += s.crashSpin * dt;
        s.bank += s.crashSpin * 0.6f * dt;
        // Rauchspur: in etwa jedem dritten Schritt ein Teilchen, jedes vierte
        // davon als leuchtende Flamme. Der Server zieht hier KEINE
        // Zufallszahl (der Client erzeugt den Rauch in UpdateCosmetics selbst).
        if (!isServer && RandInt(0, 2) == 0) {
            Particle sm;
            sm.pos = s.pos;
            sm.vel = {Rand(-1, 1), Rand(1.0f, 2.5f), Rand(-1, 1)};
            sm.life = sm.maxLife = Rand(0.8f, 1.5f);
            sm.size = Rand(0.35f, 0.7f);
            sm.color = RandInt(0, 3) == 0 ? Color{255, 150, 40, 255} : Color{70, 72, 80, 255};
            sm.glow = sm.color.r > 200;
            particles.push_back(sm);
        }
        // Aufschlag (Schiffsmitte 0,45 über dem Boden): große Explosion,
        // dann Wrack bzw. Spielende.
        if (s.pos.y <= 0.45f) {
            Explosion({s.pos.x, 0.8f, s.pos.z}, s.kind == ShipKind::Player ? 2.0f : 1.4f);
            SoundAt(Sfx::CrashBoom, s.pos, s.kind == ShipKind::Player ? 1.0f : 0.8f);
            s.dead = true;
            PlayerState* sp = s.kind == ShipKind::Player && !isServer ? FindPlayer(s.playerId) : nullptr;
            if (sp && sp->lives > 1) {
                // Einzelspieler mit Leben übrig: neues Schiff am Startpunkt an
                // ships[0]; Kontostand und Aufrüstungen bleiben, die Ladung nicht.
                --sp->lives;
                ++sp->deaths;
                sp->mining = false;
                sp->scanOn = false;
                sp->dockedAt = -1;
                Ship n = MakeShip(ShipKind::Player, {0.0f, cfg::SHIP_ALTITUDE, 24.0f});
                n.yaw = PI;
                n.playerId = sp->id;
                s = n;
                sp->shipId = n.id;
                ApplyUpgrades(*sp, true);
                Notify(*sp, Fmt("Leben verloren! Noch %d übrig.", sp->lives), Color{255, 170, 120, 255});
            } else if (s.kind == ShipKind::Player && !isServer) {
                // Einzelspieler: Spiel verloren. Kein Wrack; das Schiff bleibt
                // (unsichtbar) an ships[0], damit Player() gültig bleibt.
                if (sp) sp->lives = 0;
                s.hidden = true;
                outcome = Outcome::Dead;
                outcomeT = 0.0f;
            } else {
                // Gegner, Händler und (auf dem Server) Spielerschiffe hinterlassen
                // ein Wrack mit ihrer Ladung, mindestens 120 Punkte, auf
                // Zehner abgerundet. Abschießen lohnt sich also immer etwas.
                if (s.kind == ShipKind::Player) PlayerShipLost(s);
                Wreck w;
                w.id = nextId++;
                w.pos = {s.pos.x, 0.15f, s.pos.z};
                w.yaw = s.yaw;
                w.tilt = Rand(-0.35f, 0.35f);
                w.model = s.model;
                w.amount = std::max(120, (s.cargo / 10) * 10);
                w.initial = w.amount;
                wrecks.push_back(w);
            }
        }
    }

    // Schiffe voneinander trennen: Kommen sich zwei Schiffe näher als 2,6,
    // werden beide je zur Hälfte auseinandergeschoben (keine echte
    // Kollision, nur "nicht ineinander fliegen"). Jedes Paar einmal prüfen
    // (j beginnt bei i + 1). Bei ~100 Schiffen sind das ~5000 Paare pro
    // Schritt, für den Server kein Problem.
    // Dieser Schritt steckt NICHT in UpdateShipPhysics: Die Vorhersage im
    // Client kennt ihn nicht, die kleine Abweichung gleicht NetClient aus.
    // Angedockte Spielerschiffe (Server) stehen still und werden nicht geschoben.
    for (size_t i = 0; i < ships.size(); ++i) {
        for (size_t j = i + 1; j < ships.size(); ++j) {
            Ship& a = ships[i];
            Ship& b = ships[j];
            if (a.dead || b.dead || a.hidden || b.hidden || a.crashing || b.crashing) continue;
            if (isServer && (a.docked || b.docked)) continue;
            // Rechnung auf dem Boden (x/z als Vector2): Richtung n von a nach b
            Vector2 d{b.pos.x - a.pos.x, b.pos.z - a.pos.z};
            float len = Vector2Length(d);
            const float minD = 2.6f;
            if (len < minD && len > 0.001f) {
                Vector2 n = Vector2Scale(d, 1.0f / len);
                float push = (minD - len) * 0.5f;
                a.pos.x -= n.x * push; a.pos.z -= n.y * push;
                b.pos.x += n.x * push; b.pos.z += n.y * push;
            }
        }
    }

    UpdateTurrets(dt);
    UpdateBullets(dt);
    UpdateParticles(dt);

    // Wracks altern; die ersten 60 s steigt gelegentlich Rauch auf (im
    // Schnitt in jedem 31. Schritt ein Teilchen)
    for (auto& w : wrecks) {
        w.age += dt;
        if (!isServer && RandInt(0, 30) == 0 && w.age < 60.0f) {
            Particle sm;
            sm.pos = Vector3Add(w.pos, {Rand(-0.6f, 0.6f), 0.6f, Rand(-0.6f, 0.6f)});
            sm.vel = {Rand(-0.3f, 0.3f), Rand(0.8f, 1.6f), Rand(-0.3f, 0.3f)};
            sm.life = sm.maxLife = Rand(1.5f, 2.5f);
            sm.size = Rand(0.3f, 0.6f);
            sm.color = {60, 62, 70, 255};
            particles.push_back(sm);
        }
    }

    // Aufräumen: erschöpfte Lagerstätten und Wracks. Wracks verschwinden
    // spätestens nach 300 s (5 min), damit die Karte nicht vollläuft.
    // depositRevision zählt hoch: Der Server schickt die geänderte Liste.
    auto depEnd = std::remove_if(deposits.begin(), deposits.end(), [](const Deposit& d) { return d.amount <= 0; });
    int removedDeposits = static_cast<int>(deposits.end() - depEnd);
    deposits.erase(depEnd, deposits.end());
    if (removedDeposits > 0) ++depositRevision;
    wrecks.erase(std::remove_if(wrecks.begin(), wrecks.end(),
                                [](const Wreck& w) { return w.amount <= 0 || w.age > 300.0f; }),
                 wrecks.end());

    // Neue Lagerstätten (im Mehrspieler schneller, je mehr Spieler da sind).
    // Abstand: cfg::DEPOSIT_RESPAWN (25 s) mal Zufall 0,6..1,2; auf dem
    // Server geteilt durch die halbe Spielerzahl (64 Spieler: im Mittel
    // alle 0,7 s, bis der Sollwert erreicht ist).
    if (static_cast<int>(deposits.size()) < DepositTarget()) {
        depositSpawnT -= dt;
        if (depositSpawnT <= 0.0f) {
            SpawnDeposit(false);
            float interval = cfg::DEPOSIT_RESPAWN;
            if (isServer) interval /= std::max(1.0f, static_cast<float>(players.size()) * 0.5f);
            depositSpawnT = interval * Rand(0.6f, 1.2f);
        }
    }

    // Piraten nachschieben. Einzelspieler: mehr, je weiter das Spiel fortgeschritten ist
    // (4 + 1 je 12.000 abgelieferte Punkte, höchstens 11; Config.hpp).
    // Mehrspieler: feste Anzahl aus den Server-Einstellungen (Schwierigkeitsgrad).
    // Elite-Piraten zählen mit (gleiche Fraktion). Händler werden einzeln
    // nachgeliefert, alle 25–40 s, bis wieder cfg::MINERS_COUNT unterwegs sind.
    int pirates = 0, miners = 0;
    for (const auto& s : ships) {
        if (s.dead) continue;
        if (s.faction == Faction::Pirate) ++pirates;
        if (s.kind == ShipKind::Miner) ++miners;
    }
    int wantPirates = isServer ? settings.enemyShips
                               : std::min<int>(cfg::PIRATES_MAX, cfg::PIRATES_BASE + static_cast<int>(totalDelivered / 12000));
    pirateSpawnT -= dt;
    if (pirates < wantPirates && pirateSpawnT <= 0.0f) {
        SpawnPirate();
        pirateSpawnT = Rand(9.0f, 18.0f);   // nicht alle auf einmal: ein Pirat alle 9–18 s
        // Viele Piraten (Schwer): schneller auffüllen (alle 2,7–5,4 s)
        if (isServer && wantPirates > 8) pirateSpawnT *= 0.3f;
    }
    minerSpawnT -= dt;
    if (miners < cfg::MINERS_COUNT && minerSpawnT <= 0.0f) {
        SpawnMiner(true);
        minerSpawnT = Rand(25.0f, 40.0f);
    }

    // Tote Schiffe entfernen. Einzelspieler: das Spielerschiff bleibt immer an Index 0.
    // Erst hier am Ende, damit während des Schritts kein Schiff aus der Liste
    // verschwindet, auf das gerade noch jemand zugreift.
    const size_t keep = isServer ? 0 : 1;
    if (ships.size() > keep) {
        ships.erase(std::remove_if(ships.begin() + static_cast<long>(keep), ships.end(),
                                   [](const Ship& s) { return s.dead; }),
                    ships.end());
    }

    // Mehrspieler: neues Schiff nach einem Absturz. PlayerShipLost hat
    // respawnT auf cfg::MP_RESPAWN_TIME (4 s) gesetzt; in der Zeit sieht der
    // Spieler noch die Explosion und sein Wrack.
    if (isServer) {
        for (auto& ps : players) {
            if (ps.shipId >= 0 && FindShip(ps.shipId)) continue;
            ps.shipId = -1;
            ps.respawnT -= dt;
            if (ps.respawnT <= 0.0f) {
                SpawnPlayerShip(ps);
                Notify(ps, Fmt("Neues Raumschiff! Verbleibende Leben: %d", ps.lives), Color{200, 220, 255, 255});
            }
        }
    }

    // Meldungen ablaufen lassen; Wachs-Animation der Station (1 -> 0 in
    // etwa 1,7 s); neu entdeckte Lagerstätten in 0,5 s einblenden.
    for (auto& ps : players) {
        for (auto& n : ps.notices) n.time -= dt;
        ps.notices.erase(std::remove_if(ps.notices.begin(), ps.notices.end(), [](const Notice& n) { return n.time <= 0; }),
                         ps.notices.end());
    }
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
    for (auto& d : deposits) if (d.revealed && d.revealT < 1.0f) d.revealT = std::min(1.0f, d.revealT + dt * 2.0f);
}

// Einzelspieler, wenn die Welt "steht": angedockt (Stationsfenster offen),
// nach dem Sieg und beim Autopiloten in der Station (main.cpp). Nur Partikel,
// Meldungen und Animationen laufen weiter, damit das Bild nicht einfriert.
// Keine Zufallszahlen, kein Spielgeschehen.
void World::UpdateAmbient(float dt) {
    if (dt <= 0.0f) return;
    time += dt;
    UpdateParticles(dt);
    for (auto& ps : players) {
        for (auto& n : ps.notices) n.time -= dt;
        ps.notices.erase(std::remove_if(ps.notices.begin(), ps.notices.end(), [](const Notice& n) { return n.time <= 0; }),
                         ps.notices.end());
    }
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
}

void World::UpdateCosmetics(float dt) {
    // Der Mehrspieler-Client simuliert die Welt nicht selbst. Rauch und Funken,
    // die im Einzelspieler nebenbei in Update() entstehen, erzeugt er hier aus
    // dem Zustand, den der Server geschickt hat.
    // Die Zufallszahlen hier betreffen nur die Optik; die Zahlenfolge des
    // Clients muss nach BuildMirror mit nichts mehr übereinstimmen.
    // Aufgerufen einmal pro Bild (NetClient), nachdem die Schiffe aus den
    // Snapshots aktualisiert wurden.
    for (const auto& s : ships) {
        if (!s.crashing || s.dead || s.hidden) continue;
        if (RandInt(0, 2) != 0) continue;
        Particle sm;
        sm.pos = s.pos;
        sm.vel = {Rand(-1, 1), Rand(1.0f, 2.5f), Rand(-1, 1)};
        sm.life = sm.maxLife = Rand(0.8f, 1.5f);
        sm.size = Rand(0.35f, 0.7f);
        sm.color = RandInt(0, 3) == 0 ? Color{255, 150, 40, 255} : Color{70, 72, 80, 255};
        sm.glow = sm.color.r > 200;
        particles.push_back(sm);
    }
    for (const auto& w : wrecks) {
        if (RandInt(0, 30) != 0 || w.age > 60.0f) continue;
        Particle sm;
        sm.pos = Vector3Add(w.pos, {Rand(-0.6f, 0.6f), 0.6f, Rand(-0.6f, 0.6f)});
        sm.vel = {Rand(-0.3f, 0.3f), Rand(0.8f, 1.6f), Rand(-0.3f, 0.3f)};
        sm.life = sm.maxLife = Rand(1.5f, 2.5f);
        sm.size = Rand(0.3f, 0.6f);
        sm.color = {60, 62, 70, 255};
        particles.push_back(sm);
    }
    // Eigener Abbaustrahl: grüne Teilchen fliegen von der Lagerstätte zum Schiff
    const PlayerState& me = Me();
    const Ship& p = Player();
    if (me.mining && !p.dead && !p.hidden && RandInt(0, 2) == 0) {
        Vector3 target{};
        bool ok = false;
        for (const auto& d : deposits) if (me.mineKind == 1 && d.id == me.mineId) { target = d.pos; ok = true; }
        for (const auto& w : wrecks) if (me.mineKind == 2 && w.id == me.mineId) { target = w.pos; ok = true; }
        if (ok) {
            Particle pt;
            Vector3 off{Rand(-0.8f, 0.8f), 0.5f, Rand(-0.8f, 0.8f)};
            pt.pos = Vector3Add(target, off);
            pt.vel = Vector3Scale(Vector3Subtract(p.pos, pt.pos), 1.6f);
            pt.life = pt.maxLife = 0.6f;
            pt.size = Rand(0.15f, 0.3f);
            pt.color = {90, 255, 170, 255};
            pt.glow = true;
            particles.push_back(pt);
        }
    }
    // Wrack-Alter zwischen zwei Snapshots weiterzählen (der Server schickt es
    // mit und überschreibt es damit regelmäßig)
    for (auto& w : wrecks) w.age += dt;
    UpdateParticles(dt);
    for (auto& ps : players) {
        for (auto& n : ps.notices) n.time -= dt;
        ps.notices.erase(std::remove_if(ps.notices.begin(), ps.notices.end(), [](const Notice& n) { return n.time <= 0; }),
                         ps.notices.end());
    }
    if (stageAnim > 0.0f) stageAnim = std::max(0.0f, stageAnim - dt * 0.6f);
    for (auto& d : deposits) if (d.revealed && d.revealT < 1.0f) d.revealT = std::min(1.0f, d.revealT + dt * 2.0f);
}

// ===========================================================================
// Spieler
// ===========================================================================
// Ein Schritt für einen menschlichen Spieler (oder den Autopiloten):
// Bewegung, Scanner, Waffe, Taste F und Abbau-Fortschritt. Die Eingaben
// kommen aus ps.input (Einzelspieler: Tastatur/Autopilot über
// Update(dt, input); Server: aus den Eingabepaketen des Clients).
// Geräusche, die nur dieser Spieler hören soll (Scanner, Abbau ...), bekommen
// forPlayer = ps.id; der Server schickt sie dann nur an ihn.
void World::UpdatePlayer(PlayerState& ps, float dt) {
    if (ps.dockedAt >= 0) return;   // angedockt: Schiff steht still (Einzelspieler ruft Update dann gar nicht auf)
    Ship* sp = PlayerShip(ps);
    if (!sp) return;
    Ship& p = *sp;
    if (p.dead || p.crashing || p.hidden) return;
    const ShipInput& in = ps.input;

    UpdateShipPhysics(p, dt, in);
    // Nachladezeit und Treffer-Aufblitzen (klingt in 1/3 s ab) herunterzählen.
    // Für KI-Schiffe passiert dasselbe in UpdateAi.
    p.fireCd -= dt;
    p.hitFlash = std::max(0.0f, p.hitFlash - dt * 3.0f);

    // --- Scanner ---------------------------------------------------------
    // E schaltet um. Einschalten nur mit mindestens 1 Energie, sonst würde
    // er sofort wieder ausgehen.
    if (in.scanToggle) {
        if (ps.scanOn) {
            ps.scanOn = false;
            SoundAt(Sfx::ScanOff, p.pos, 1.0f, 0.0f, ps.id);
        } else if (ps.scanEnergy >= 1.0f) {
            ps.scanOn = true;
            ps.scanPulse = 0.0f;
            SoundAt(Sfx::ScanOn, p.pos, 1.0f, 0.0f, ps.id);
            SoundAt(Sfx::ScanPing, p.pos, 1.0f, 0.15f, ps.id);
        } else if (!simulation) {
            Notify(ps, "Scanner-Energie zu niedrig", Color{255, 170, 120, 255});
            SoundAt(Sfx::Denied, p.pos, 1.0f, 0.0f, ps.id);
        }
    }
    if (ps.scanOn) {
        ps.scanEnergy -= cfg::SCANNER_DRAIN * dt;
        const float lastPulse = ps.scanPulse;
        ps.scanPulse += dt;
        // Jeder neue Scannerring gibt einen Ping. floor(scanPulse / Periode)
        // zählt die schon begonnenen Ringe; ändert sich die Zahl in diesem
        // Schritt, hat gerade ein neuer Ring begonnen.
        if (floorf(ps.scanPulse / cfg::SCANNER_PULSE_TIME) > floorf(lastPulse / cfg::SCANNER_PULSE_TIME))
            SoundAt(Sfx::ScanPing, p.pos, 1.0f, 0.0f, ps.id);
        // Aktueller Radius des Rings: wächst in jeder Periode (1,25 s)
        // gleichmäßig von 0 auf den Scannerradius des Schiffstyps. fmodf = Rest der Division,
        // also die Zeit seit Beginn des aktuellen Rings. Alles, was der Ring
        // erreicht, ist entdeckt. Render.cpp zeichnet den Ring mit derselben
        // Formel, damit Bild und Regel zusammenpassen.
        float ring = ScannerRadius(ps) * fmodf(ps.scanPulse, cfg::SCANNER_PULSE_TIME) / cfg::SCANNER_PULSE_TIME;
        int found = 0;
        for (auto& d : deposits) {
            if (!RevealedFor(d, ps) && Dist2D(d.pos, p.pos) <= ring) {
                if (isServer) {
                    // Server: nur für diesen Spieler aufdecken. revealRevision
                    // sagt dem GameServer, dass er ihm die Liste neu schicken muss.
                    d.revealedFor.push_back(ps.id);
                    ++ps.revealRevision;
                } else {
                    // Einzelspieler: aufdecken und die Einblend-Animation starten
                    d.revealed = true;
                    d.revealT = 0.0f;
                }
                ++found;
            }
        }
        if (found > 0) SoundAt(Sfx::Discover, p.pos, 1.0f, 0.0f, ps.id);
        if (found > 0 && !simulation)
            Notify(ps, found == 1 ? std::string("Lagerstätte entdeckt!") : Fmt("%d Lagerstätten entdeckt!", found),
                   Color{120, 255, 190, 255});
        if (ps.scanEnergy <= 0.0f) {
            ps.scanEnergy = 0.0f;
            ps.scanOn = false;
            SoundAt(Sfx::ScanEmpty, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Scanner abgeschaltet: Energie leer", Color{255, 170, 120, 255});
        }
    } else {
        ps.scanEnergy = std::min(cfg::SCANNER_MAX_ENERGY, ps.scanEnergy + cfg::SCANNER_REGEN * dt);
    }

    // --- Waffe -------------------------------------------------------------
    // Feuer gedrückt halten = Dauerfeuer im Takt von fireInterval (fireCd
    // wird oben jeden Schritt um dt kleiner).
    if (in.fire && p.fireCd <= 0.0f) {
        FireBullet(p);
        p.fireCd = p.fireInterval;
    }

    // --- Interaktion (F) ----------------------------------------------------
    // Eine Taste für alles: Läuft ein Abbau, bricht F ihn ab. Sonst entscheidet
    // InteractTarget, was in der Nähe ist: Station -> andocken, Wrack oder
    // entdeckte Lagerstätte -> Abbau starten (wenn das Lager nicht voll ist).
    if (in.interact) {
        if (ps.mining) {
            ps.mining = false;
            SoundAt(Sfx::MineAbort, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Abbau abgebrochen", Color{255, 200, 150, 255});
        } else {
            int kind = 0, id = -1;
            InteractTarget(ps, &kind, &id);
            if (kind >= 3) {
                Dock(ps, id);
                return;   // angedockt: Rest dieses Schritts entfällt
            } else if (kind == 1 || kind == 2) {
                if (p.cargo >= p.cargoMax) {
                    SoundAt(Sfx::CargoFull, p.pos, 1.0f, 0.0f, ps.id);
                    if (!simulation) Notify(ps, "Lager voll! Ab zur Handelsstation.", Color{255, 200, 120, 255});
                } else {
                    StartMining(ps, kind, id);
                }
            }
        }
    }

    // --- Abbau-Fortschritt ----------------------------------------------------
    // Der Abbau läuft weiter, solange das Ziel noch existiert, nicht leer ist
    // (ein anderer Spieler oder Händler kann es leeren) und das Schiff in
    // Reichweite bleibt (mit 1,5 Einheiten Toleranz). Nach MINING_TIME
    // Sekunden wird auf einen Schlag geladen (FinishMining).
    // Ship::mining wird jeden Schritt neu bestimmt (nur Server, siehe unten).
    p.mining = false;
    if (ps.mining) {
        Vector3 target{};
        bool valid = false;
        if (ps.mineKind == 1) {
            if (Deposit* d = FindDeposit(ps.mineId)) { target = d->pos; valid = d->amount > 0; }
        } else if (Wreck* w = FindWreck(ps.mineId)) {
            target = w->pos;
            valid = w->amount > 0;
        }
        if (!valid) {
            ps.mining = false;
            SoundAt(Sfx::MineAbort, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Lagerstätte erschöpft", Color{255, 200, 150, 255});
        } else if (Dist2D(target, p.pos) > cfg::MINING_RANGE + 1.5f) {
            ps.mining = false;
            SoundAt(Sfx::MineAbort, p.pos, 1.0f, 0.0f, ps.id);
            if (!simulation) Notify(ps, "Abbau abgebrochen: zu weit entfernt", Color{255, 200, 150, 255});
        } else {
            ps.mineT += dt;
            if (isServer) {
                // Damit andere Spieler den Abbaustrahl sehen
                p.mining = true;
                p.mineTarget = target;
            } else if (RandInt(0, 2) == 0) {
                // Einzelspieler: grüne Teilchen fliegen von der Lagerstätte zum
                // Schiff. Geschwindigkeit = Abstand * 1,6 bei 0,6 s Lebenszeit:
                // Sie kommen fast genau (96 %) am Schiff an und verschwinden dort.
                Particle pt;
                Vector3 off{Rand(-0.8f, 0.8f), 0.5f, Rand(-0.8f, 0.8f)};
                pt.pos = Vector3Add(target, off);
                Vector3 to = Vector3Subtract(p.pos, pt.pos);
                pt.vel = Vector3Scale(to, 1.6f);
                pt.life = pt.maxLife = 0.6f;
                pt.size = Rand(0.15f, 0.3f);
                pt.color = {90, 255, 170, 255};
                pt.glow = true;
                particles.push_back(pt);
            }
            if (ps.mineT >= cfg::MINING_TIME) FinishMining(ps);
        }
    }
}

// Flugmodell für ALLE Schiffe (Spieler, KI, und im Client die Vorhersage
// des eigenen Schiffs). Bewusst einfach und "arcadig", kein echtes
// Raumflug-Verhalten: Das Schiff gleitet nur wenig zur Seite und bremst von
// selbst ab, wenn man keinen Schub gibt.
//
// WICHTIG: Die Funktion benutzt keine Zufallszahlen und keine anderen
// Schiffe, nur das Schiff selbst, die Eingabe, die Stationen und time.
// Nur deshalb kann der Mehrspieler-Client sie mit denselben Eingaben
// nachrechnen und kommt (fast) zum selben Ergebnis wie der Server
// (NetClient.cpp, "Reconciliation"). Wer hier etwas ändert, muss Client und
// Server gemeinsam neu bauen.
void World::UpdateShipPhysics(Ship& s, float dt, const ShipInput& in) {
    // Drehen. Den Winkel im Bereich -PI..PI halten, damit er nicht endlos
    // wächst (große float-Zahlen werden ungenau).
    s.yaw += in.turn * s.turnRate * dt;
    if (s.yaw > PI) s.yaw -= 2 * PI;
    if (s.yaw < -PI) s.yaw += 2 * PI;
    // Schräglage: in Kurven bis 0,5 rad (knapp 30 Grad) zur Seite kippen
    // (nur Optik). Lerp(a, b, t) = a + (b - a) * t; mit t = 6 * dt jeden Schritt
    // nähert sich bank weich dem Zielwert (Rest halbiert sich in ~0,1 s).
    // std::min(1, ...) verhindert ein Überschießen bei großem dt.
    s.bank = Lerp(s.bank, -in.turn * 0.5f, std::min(1.0f, 6.0f * dt));

    // Schub entlang der Nase. Beschleunigung = 1,9 x Höchsttempo: Aus dem
    // Stand ist das Schiff nach knapp 0,6 s auf Höchstgeschwindigkeit.
    // Rückwärts (Bremsen) wirkt nur mit 70 % der Kraft.
    Vector3 fwd = ForwardFromYaw(s.yaw);
    const float accel = s.maxSpeed * 1.9f;
    if (in.thrust > 0.0f) s.vel = Vector3Add(s.vel, Vector3Scale(fwd, accel * in.thrust * dt));
    else if (in.thrust < 0.0f) s.vel = Vector3Add(s.vel, Vector3Scale(fwd, accel * 0.7f * in.thrust * dt));

    // Luftwiderstand gibt es auf dem Mond nicht, als Spielregel aber schon:
    // Jeder Schritt multipliziert das Tempo mit (1 - drag * dt). Ohne Schub
    // (drag 1,4) ist nach 1 s noch etwa ein Viertel übrig, nach 2 s steht
    // das Schiff fast; mit Schub bremst es nur schwach (0,35).
    float drag = (in.thrust == 0.0f) ? 1.4f : 0.35f;
    s.vel = Vector3Scale(s.vel, 1.0f - std::min(1.0f, drag * dt));
    // Seitliches Driften dämpfen: side steht senkrecht zur Nase (fwd um 90
    // Grad gedreht). Das Skalarprodukt vel·side ist der seitliche Anteil der
    // Geschwindigkeit; davon wird in jedem Schritt der Anteil 2,6 * dt
    // abgezogen (nach gut 0,25 s ist die Hälfte weg). So "greift" das Schiff
    // in Kurven ähnlich wie ein Auto und rutscht nur kurz.
    Vector3 side{fwd.z, 0.0f, -fwd.x};
    float lat = Vector3DotProduct(s.vel, side);
    s.vel = Vector3Subtract(s.vel, Vector3Scale(side, lat * std::min(1.0f, 2.6f * dt)));
    s.vel.y = 0.0f;

    // Tempo begrenzen: vorwärts maxSpeed, rückwärts nur die Hälfte
    float fwdSpeed = Vector3DotProduct(s.vel, fwd);
    float speed = Vector3Length(s.vel);
    float limit = fwdSpeed >= 0 ? s.maxSpeed : s.maxSpeed * 0.5f;
    if (speed > limit) s.vel = Vector3Scale(s.vel, limit / speed);

    s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt));
    // Höhe: feste Flughöhe plus leichtes Auf und Ab (±0,12, Periode ~3,1 s).
    // Die ID als Phasenversatz sorgt dafür, dass nicht alle Schiffe im
    // Gleichtakt schaukeln. Reine Optik; der Client korrigiert die Höhe
    // deshalb bei der Vorhersage nicht (seine Uhr time weicht vom Server ab).
    s.pos.y = cfg::SHIP_ALTITUDE + sinf(time * 2.0f + static_cast<float>(s.id)) * 0.12f;
    s.thrustVisual = Lerp(s.thrustVisual, std::max(0.0f, in.thrust), std::min(1.0f, 8.0f * dt));

    // Grenze des Einsatzgebiets: am Rand anhalten, nur die Bewegung nach
    // außen wird gestrichen (am Rand entlangfliegen geht weiter)
    const float lim = cfg::WORLD_HALF;
    if (s.pos.x < -lim) { s.pos.x = -lim; s.vel.x = std::max(0.0f, s.vel.x); }
    if (s.pos.x > lim) { s.pos.x = lim; s.vel.x = std::min(0.0f, s.vel.x); }
    if (s.pos.z < -lim) { s.pos.z = -lim; s.vel.z = std::max(0.0f, s.vel.z); }
    if (s.pos.z > lim) { s.pos.z = lim; s.vel.z = std::min(0.0f, s.vel.z); }
}

// ===========================================================================
// Interaktion, Abbau, Stationen
// ===========================================================================
// Abstände zu Stationen werden auf dem Boden (Dist2D) zur Stationsmitte
// gemessen; Stationen werden über ihren Index in stations angesprochen.
bool World::NearStation(const Ship& s, int station, float range) const {
    if (station < 0 || station >= static_cast<int>(stations.size())) return false;
    return Dist2D(s.pos, stations[station].pos) <= range;
}

int World::NearestStation(Vector3 p, StationType type) const {
    int best = -1;
    float bestD = 1e9f;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
        if (stations[i].type != type) continue;
        float d = Dist2D(p, stations[i].pos);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

int World::CountStations(StationType type) const {
    int n = 0;
    for (const auto& st : stations) if (st.type == type) ++n;
    return n;
}

// Was ist für Taste F gerade in Reichweite? Ergebnis über die Zeiger kind/id
// (zusätzlich als Rückgabewert kind). Vorrang: Station (DOCK_RANGE) vor
// Wrack vor Lagerstätte (je die nächste in MINING_RANGE). Lagerstätten nur,
// wenn dieser Spieler sie schon entdeckt hat; Wracks sieht man immer.
// Das HUD zeigt mit derselben Funktion den passenden Hinweis ("[F] Andocken").
int World::InteractTarget(const PlayerState& ps, int* kind, int* id) const {
    *kind = 0;
    *id = -1;
    const Ship* sp = PlayerShip(ps);
    if (!sp) return 0;
    const Ship& p = *sp;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
        if (NearStation(p, i, cfg::DOCK_RANGE)) {
            *kind = stations[i].type == StationType::Trade ? 3 : (stations[i].type == StationType::Shipyard ? 4 : 5);
            *id = i;
            return *kind;
        }
    }
    float best = cfg::MINING_RANGE;
    for (const auto& w : wrecks) {
        float d = Dist2D(w.pos, p.pos);
        if (d <= best && w.amount > 0) { best = d; *kind = 2; *id = w.id; }
    }
    if (*kind == 2) return 2;
    best = cfg::MINING_RANGE;
    for (const auto& d : deposits) {
        float dist = Dist2D(d.pos, p.pos);
        if (RevealedFor(d, ps) && dist <= best && d.amount > 0) { best = dist; *kind = 1; *id = d.id; }
    }
    return *kind;
}

void World::StartMining(PlayerState& ps, int kind, int id) {
    ps.mining = true;
    ps.mineKind = kind;
    ps.mineId = id;
    ps.mineT = 0.0f;
    if (const Ship* p = PlayerShip(ps)) SoundAt(Sfx::MineStart, p->pos, 1.0f, 0.0f, ps.id);
    if (!simulation) Notify(ps, kind == 2 ? "Wrack wird ausgeschlachtet ..." : "Abbau gestartet ...",
                            Color{150, 255, 200, 255});
}

// Abbau fertig: so viel laden, wie ins Lager passt (höchstens der Rest der
// Lagerstätte bzw. des Wracks). Geänderte Lagerstätten-Mengen erhöhen
// depositRevision, damit der Server die Liste neu verschickt; Wracks stehen
// sowieso in jedem Snapshot.
void World::FinishMining(PlayerState& ps) {
    Ship* sp = PlayerShip(ps);
    ps.mining = false;
    if (!sp) return;
    Ship& p = *sp;
    int free = p.cargoMax - p.cargo;
    int* amount = nullptr;
    if (ps.mineKind == 1) { if (Deposit* d = FindDeposit(ps.mineId)) amount = &d->amount; }
    else if (Wreck* w = FindWreck(ps.mineId)) amount = &w->amount;
    if (!amount) return;
    int take = std::min(free, *amount);
    *amount -= take;
    p.cargo += take;
    if (ps.mineKind == 1) ++depositRevision;
    SoundAt(Sfx::MineDone, p.pos, 1.0f, 0.0f, ps.id);
    if (p.cargo >= p.cargoMax) SoundAt(Sfx::CargoFull, p.pos, 1.0f, 0.45f, ps.id);
    if (!simulation) {
        Notify(ps, Fmt("+%d Ressourcen geladen", take), Color{120, 255, 190, 255});
        if (p.cargo >= p.cargoMax) Notify(ps, "Lager voll! Ab zur Handelsstation.", Color{255, 200, 120, 255});
    }
    SpawnSparks(p.pos, Color{120, 255, 190, 255}, 14);
}

// Höchste Stufe i, deren Schwelle cfg::STATION_THRESHOLDS[i] erreicht ist.
int World::StationStage() const {
    int stage = 0;
    for (int i = 0; i < cfg::STATION_STAGES; ++i)
        if (totalDelivered >= cfg::STATION_THRESHOLDS[i]) stage = i;
    return stage;
}

int World::NextStageThreshold() const {
    int st = StationStage();
    if (st + 1 >= cfg::STATION_STAGES) return -1;
    return cfg::STATION_THRESHOLDS[st + 1];
}

// Andocken (Taste F in DOCK_RANGE). Das Schiff steht danach still und ist
// geschützt. An einer Handelsstation wird sofort die ganze Ladung verkauft:
// 1 Einheit = 1 Punkt auf dem Konto. Danach wird geprüft, ob die Station
// gewachsen ist und ob das Spielziel erreicht ist.
// Einzelspieler: main.cpp öffnet jetzt das Stationsfenster und ruft Update
// nicht mehr auf, bis Undock. Server: Update läuft weiter, UpdatePlayer
// überspringt angedockte Spieler.
// Dock prüft den Abstand nicht selbst; das hat InteractTarget vorher getan.
void World::Dock(PlayerState& ps, int station) {
    Ship* sp = PlayerShip(ps);
    if (!sp || station < 0 || station >= static_cast<int>(stations.size())) return;
    Ship& p = *sp;
    ps.dockedAt = station;
    ps.mining = false;
    ps.scanOn = false;
    p.vel = {0, 0, 0};
    p.docked = true;
    p.mining = false;
    ps.lastSale = 0;
    SoundAt(Sfx::Dock, p.pos, 1.0f, 0.0f, ps.id);
    if (stations[station].type == StationType::Trade) {
        int sale = p.cargo;
        p.cargo = 0;
        ps.balance += sale;
        ps.delivered += sale;
        totalDelivered += sale;
        ps.lastSale = sale;
        // Die Töne sind zeitlich gestaffelt (delay): erst Andocken, 0,35 s
        // später Verkauf, 0,8 s Stationsausbau, 1,3 s Siegesfanfare.
        if (sale > 0) SoundAt(Sfx::Sale, p.pos, 1.0f, 0.35f, ps.id);
        int stage = StationStage();
        if (stage > lastStage) {
            lastStage = stage;
            stageAnim = 1.0f;
            SoundAt(Sfx::StationGrow, p.pos, 1.0f, 0.8f, ps.id);
            if (!simulation) Notify(ps, Fmt("Die Station wächst! Ausbaustufe %d", stage), Color{255, 210, 120, 255});
            // Funken an allen neuen Bauteilen aller Handelsstationen
            for (const auto& st : stations) {
                if (st.type != StationType::Trade) continue;
                for (const auto& part : st.parts)
                    if (part.stage == stage) SpawnSparks(Vector3Add(st.pos, Vector3Add(part.offset, {0, 1.5f, 0})), Color{255, 220, 120, 255}, 12);
            }
        }
        // Spielziel: Im Mehrspieler beginnt danach eine neue Runde, im
        // Einzelspieler ist die Partie gewonnen.
        if (isServer) {
            if (ps.balance >= cfg::GOAL_POINTS) GoalReached(ps);
        } else if (ps.balance >= cfg::GOAL_POINTS && outcome == Outcome::None) {
            outcome = Outcome::Won;
            outcomeT = 0.0f;
            SoundAt(Sfx::Victory, p.pos, 1.0f, 1.3f, ps.id);
        }
    }
    // Einzelspieler: nach jedem Andocken speichern (main.cpp). Der Server
    // speichert selbst und beachtet das Feld nicht.
    wantAutosave = !simulation;
}

void World::Undock(PlayerState& ps) {
    if (ps.dockedAt < 0) return;
    Ship* sp = PlayerShip(ps);
    const int station = ps.dockedAt;
    ps.dockedAt = -1;
    if (!sp) return;
    Ship& p = *sp;
    // Schiff zeigt beim Abdocken von der Station weg und bekommt einen
    // kleinen Schubs (6 Einheiten/s). Damit F nicht sofort wieder andockt,
    // ignoriert der Server danach kurz jede Interaktion (GameServer.cpp).
    p.yaw = YawTowards(stations[station].pos, p.pos);
    p.vel = Vector3Scale(ForwardFromYaw(p.yaw), 6.0f);
    p.docked = false;
    SoundAt(Sfx::Undock, p.pos, 1.0f, 0.0f, ps.id);
}

// Reparaturpreis für den fehlenden Schild (aufgerundet auf ganze Punkte).
// Repair/BuyUpgrade prüfen NICHT, an welcher Station der Spieler angedockt
// ist; das machen die Aufrufer (Stationsfenster: Hud.cpp/main.cpp bzw.
// GameServer::HandleDockAction): Reparatur an Handelsstationen, Aufrüsten an Werften.
int World::RepairCost(const PlayerState& ps) const {
    const Ship* p = PlayerShip(ps);
    if (!p) return 0;
    int missing = static_cast<int>(ceilf(p->maxHp - p->hp));
    return std::max(0, missing) * cfg::REPAIR_COST_PER_POINT;
}

bool World::Repair(PlayerState& ps) {
    Ship* sp = PlayerShip(ps);
    if (!sp) return false;
    Ship& p = *sp;
    int cost = RepairCost(ps);
    if (cost <= 0) return false;
    if (ps.balance >= cost) {
        ps.balance -= cost;
        p.hp = p.maxHp;
        SoundAt(Sfx::Repair, p.pos, 1.0f, 0.0f, ps.id);
        return true;
    }
    // Teilreparatur mit dem vorhandenen Geld
    int points = static_cast<int>(ps.balance / cfg::REPAIR_COST_PER_POINT);
    if (points <= 0) return false;
    ps.balance -= points * cfg::REPAIR_COST_PER_POINT;
    p.hp = std::min(p.maxHp, p.hp + points);
    SoundAt(Sfx::Repair, p.pos, 1.0f, 0.0f, ps.id);
    return true;
}

int World::UpgradeCost(const PlayerState& ps, int type) const {
    if (type < 0 || type >= cfg::UP_COUNT) return -1;
    if (ps.upgrades[type] >= cfg::UPGRADE_MAX[type]) return -1;
    return cfg::UPGRADE_COST[type][ps.upgrades[type]];
}

bool World::BuyUpgrade(PlayerState& ps, int type) {
    int cost = UpgradeCost(ps, type);
    if (cost < 0 || ps.balance < cost) return false;
    ps.balance -= cost;
    ps.upgrades[type]++;
    ApplyUpgrades(ps, false);
    if (const Ship* p = PlayerShip(ps)) SoundAt(Sfx::Upgrade, p->pos, 1.0f, 0.0f, ps.id);
    if (!simulation) {
        // Reihenfolge wie cfg::Upgrade (UP_SPEED, UP_ARMOR, UP_WEAPON, UP_CARGO, UP_GUNS)
        static const char* names[cfg::UP_COUNT] = {"Antrieb", "Panzerung", "Bewaffnung", "Lager", "Kanonen"};
        Notify(ps, Fmt("%s auf Stufe %d verbessert!", names[type], ps.upgrades[type]), Color{110, 240, 170, 255});
    }
    return true;
}

// Überträgt Schiffstyp und Aufrüstungsstufen auf die Werte und das Modell
// des Schiffs (Tabellen und ShipSpeed/ShipArmor ... in Config.hpp). refill = true: neues Schiff, Schild voll. Sonst bleibt ein
// Schaden erhalten; wird die Panzerung verbessert, wächst der aktuelle
// Schild um denselben Betrag wie das Maximum (der Kauf "repariert" also
// nicht, aber er schadet auch nicht).
// Auch GameServer::RestoreProgress ruft das nach dem Laden eines Kontos auf.
void World::ApplyUpgrades(PlayerState& ps, bool refill) {
    Ship* sp = PlayerShip(ps);
    if (!sp) return;
    Ship& p = *sp;
    float oldMax = p.maxHp;
    // Typ begrenzen: er dient als Index (kommt auch aus Spielstand und Konto)
    ps.shipType = std::clamp(ps.shipType, 0, cfg::SHIP_TYPE_COUNT - 1);
    const int t = ps.shipType;
    p.model = mdl.shipTypes[t];
    p.maxSpeed = cfg::ShipSpeed(t, ps.upgrades[cfg::UP_SPEED]);
    p.maxHp = cfg::ShipArmor(t, ps.upgrades[cfg::UP_ARMOR]);
    p.damage = cfg::ShipDamage(t, ps.upgrades[cfg::UP_WEAPON]);
    p.fireInterval = cfg::WEAPON_RATE[ps.upgrades[cfg::UP_WEAPON]];
    p.guns = cfg::GUN_COUNT[ps.upgrades[cfg::UP_GUNS]];
    p.cargoMax = cfg::ShipCargo(t, ps.upgrades[cfg::UP_CARGO]);
    if (refill) p.hp = p.maxHp;
    else if (p.maxHp > oldMax) p.hp += p.maxHp - oldMax;
}

float World::ScannerRadius(const PlayerState& ps) const {
    return cfg::ShipScanner(std::clamp(ps.shipType, 0, cfg::SHIP_TYPE_COUNT - 1));
}

// ===========================================================================
// Raumschiffsverkäufer
// ===========================================================================
bool World::OwnsShipType(const PlayerState& ps, int type) const {
    if (ps.shipType == type) return true;
    for (const auto& o : ps.hangar)
        if (o.type == type) return true;
    return false;
}

// Gutschrift für das geflogene Schiff: TRADE_IN_PERCENT von Kaufpreis und
// allen bezahlten Aufrüstungen (UPGRADE_COST der erreichten Stufen).
int World::TradeInValue(const PlayerState& ps) const {
    long long value = cfg::SHIP_TYPES[std::clamp(ps.shipType, 0, cfg::SHIP_TYPE_COUNT - 1)].cost;
    for (int u = 0; u < cfg::UP_COUNT; ++u)
        for (int l = 0; l < ps.upgrades[u] && l < cfg::UPGRADE_LEVELS; ++l) value += cfg::UPGRADE_COST[u][l];
    return static_cast<int>(value * cfg::TRADE_IN_PERCENT / 100);
}

int World::ShipPrice(const PlayerState& ps, int type, bool tradeIn) const {
    if (type < 0 || type >= cfg::SHIP_TYPE_COUNT || OwnsShipType(ps, type)) return -1;
    return cfg::SHIP_TYPES[type].cost - (tradeIn ? TradeInValue(ps) : 0);
}

// Neues Schiff kaufen. tradeIn = altes Schiff in Zahlung geben (es ist dann
// weg), sonst wird es geparkt (hangar) und kann an der Handelsstation wieder
// gewählt werden. Das neue Schiff beginnt ohne Aufrüstungen und mit vollem
// Schild. Die Ladung wird umgeladen, soweit sie ins neue Lager passt.
bool World::BuyShip(PlayerState& ps, int type, bool tradeIn) {
    Ship* sp = PlayerShip(ps);
    if (!sp) return false;
    const int cost = ShipPrice(ps, type, tradeIn);
    if (type < 0 || type >= cfg::SHIP_TYPE_COUNT || OwnsShipType(ps, type) || ps.balance < cost) return false;
    ps.balance -= cost;
    if (!tradeIn) {
        OwnedShip old;
        old.type = ps.shipType;
        for (int u = 0; u < cfg::UP_COUNT; ++u) old.upgrades[u] = ps.upgrades[u];
        old.hp = sp->hp;
        ps.hangar.push_back(old);
    }
    ps.shipType = type;
    for (int& u : ps.upgrades) u = 0;
    ApplyUpgrades(ps, true);
    sp->cargo = std::min(sp->cargo, sp->cargoMax);
    SoundAt(Sfx::Upgrade, sp->pos, 1.0f, 0.0f, ps.id);
    if (!simulation) Notify(ps, Fmt("Neues Schiff: %s", cfg::SHIP_TYPES[type].name), Color{150, 255, 90, 255});
    return true;
}

// Zum geparkten Schiff dieses Typs wechseln; das bisherige wird geparkt.
bool World::SwitchShip(PlayerState& ps, int type) {
    Ship* sp = PlayerShip(ps);
    if (!sp || type == ps.shipType) return false;
    auto it = std::find_if(ps.hangar.begin(), ps.hangar.end(), [&](const OwnedShip& o) { return o.type == type; });
    if (it == ps.hangar.end()) return false;
    OwnedShip next = *it;
    it->type = ps.shipType;
    for (int u = 0; u < cfg::UP_COUNT; ++u) it->upgrades[u] = ps.upgrades[u];
    it->hp = sp->hp;
    ps.shipType = next.type;
    for (int u = 0; u < cfg::UP_COUNT; ++u) ps.upgrades[u] = std::clamp(next.upgrades[u], 0, cfg::UPGRADE_MAX[u]);
    ApplyUpgrades(ps, true);
    if (next.hp > 0.0f) sp->hp = std::min(next.hp, sp->maxHp);
    sp->cargo = std::min(sp->cargo, sp->cargoMax);
    SoundAt(Sfx::Upgrade, sp->pos, 1.0f, 0.0f, ps.id);
    if (!simulation) Notify(ps, Fmt("Schiff gewechselt: %s", cfg::SHIP_TYPES[next.type].name), Color{150, 255, 90, 255});
    return true;
}

void World::GoalReached(PlayerState& ps) {
    // Mehrspieler: Ziel erreicht -> Eintrag in die Bestenliste, dann beginnt das
    // Ziel von vorn. Das Schiff behält seine Aufrüstungen (nächste Runde).
    goals.push_back({ps.id, ps.roundTime, ps.round});
    ++ps.goals;
    Notify(ps, Fmt("ZIEL ERREICHT! Runde %d geschafft.", ps.round), Color{110, 240, 170, 255});
    Notify(ps, "Neue Runde: Dein Schiff behält seine Aufrüstungen.", Color{200, 220, 255, 255});
    if (const Ship* p = PlayerShip(ps)) SoundAt(Sfx::Victory, p->pos, 1.0f, 1.3f, ps.id);
    ps.balance = 0;
    ps.round++;
    ps.roundTime = 0.0f;
}

void World::PlayerShipLost(Ship& s) {
    // Mehrspieler: Das Schiff ist aufgeschlagen. Der Spieler bekommt nach kurzer
    // Zeit ein neues Schiff und fängt mit seinem Fortschritt von vorne an.
    PlayerState* ps = FindPlayer(s.playerId);
    if (!ps) return;
    ++ps->deaths;
    if (ps->lives > 1) {
        // Noch Leben übrig: Kontostand und Aufrüstungen bleiben.
        --ps->lives;
        ps->mining = false;
        ps->scanOn = false;
        ps->dockedAt = -1;
        Notify(*ps, Fmt("Leben verloren! Noch %d übrig.", ps->lives), Color{255, 170, 120, 255});
    } else {
        ResetProgress(*ps);   // setzt auch die Leben zurück
        Notify(*ps, "Alle Leben verbraucht: Dein Fortschritt beginnt von vorn.", Color{255, 110, 90, 255});
    }
    ps->respawnT = cfg::MP_RESPAWN_TIME;
    ps->shipId = -1;
}

// ===========================================================================
// Kampf
// ===========================================================================
// Ein Schuss nach vorn. Spieler schießen genau geradeaus, KI-Schiffe streuen
// um bis zu ±0,045 rad (±2,6 Grad), damit sie nicht jeden Schuss treffen.
// Das Geschoss startet 1,5 vor der Schiffsmitte, abwechselnd aus der linken
// und rechten Kanone (±0,55 zur Seite), fliegt waagerecht und übernimmt 40 %
// der Schiffsgeschwindigkeit.
void World::FireBullet(Ship& s) {
    Vector3 fwd = ForwardFromYaw(s.yaw);
    float spread = s.kind == ShipKind::Player ? 0.0f : Rand(-0.045f, 0.045f);
    Vector3 side{fwd.z, 0.0f, -fwd.x};
    // Links/rechts im Wechsel. Achtung: static = EIN Zähler für alle Schiffe
    // und alle World-Objekte, der auch über NewGame hinweg weiterläuft. Bei
    // mehreren Schiffen wechseln die Seiten daher nicht streng pro Schiff.
    static int toggle = 0;
    toggle ^= 1;
    // Mehrere Kanonen (Upgrade): alle schießen gleichzeitig. 2 Schüsse aus
    // beiden Kanonen, 3 Schüsse zusätzlich einer in der Mitte, jeweils im
    // Fächer (cfg::GUN_SPREAD). Mit einer Kanone wie bisher abwechselnd.
    const int n = std::max(1, s.guns);
    for (int i = 0; i < n; ++i) {
        const float k = n == 1 ? 0.0f : static_cast<float>(i) - 0.5f * static_cast<float>(n - 1);   // -1..1 um die Mitte
        const float sideOffset = n == 1 ? (toggle ? 0.55f : -0.55f) : k * (n == 2 ? 1.1f : 0.55f);
        Vector3 dir = ForwardFromYaw(s.yaw + spread + k * cfg::GUN_SPREAD);
        Bullet b;
        b.pos = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, 1.5f), Vector3Scale(side, sideOffset)));
        b.pos.y = s.pos.y + 0.25f;
        b.vel = Vector3Add(Vector3Scale(dir, cfg::BULLET_SPEED), Vector3Scale(s.vel, 0.4f));
        b.vel.y = 0.0f;
        b.life = cfg::BULLET_LIFE;
        b.damage = s.damage;
        b.faction = s.faction;
        b.ownerId = s.id;
        b.ownerPlayer = s.playerId;
        bullets.push_back(b);
    }
    // sourceShip = s.id: Der Server schickt den Ton eines Spielerschusses
    // nicht an den Schützen selbst; dessen Client spielt ihn schon beim
    // Tastendruck ab (Vorhersage).
    SoundAt(s.kind == ShipKind::Player ? Sfx::PlayerShot : Sfx::EnemyShot, s.pos, 1.0f, 0.0f, -1, s.id);
}

// Geschütztürme der Stationen: schießen nur auf Piraten (nicht auf
// Händler, auch nicht auf wütende). Handelsstationen bekommen mit jeder
// Ausbaustufe weitere Türme (Turret::stage), Werften haben immer nur ihre
// Grundausstattung.
void World::UpdateTurrets(float dt) {
    for (auto& st : stations) {
        int stage = st.type == StationType::Trade ? StationStage() : 0;
        for (auto& tu : st.turrets) {
            if (tu.stage > stage) continue;
            tu.cd -= dt;
            Vector3 wp = Vector3Add(st.pos, tu.offset);
            Ship* best = nullptr;
            float bestD = cfg::TURRET_RANGE;
            for (auto& s : ships) {
                if (s.dead || s.crashing || s.hidden || s.faction != Faction::Pirate) continue;
                float d = Dist2D(s.pos, wp);
                if (d < bestD) { bestD = d; best = &s; }
            }
            if (!best) continue;
            // Vorhalt: dorthin zielen, wo der Pirat sein wird, wenn das
            // Geschoss ankommt (Flugzeit = Abstand / Geschossgeschwindigkeit).
            // Der Turm dreht sich höchstens 4 rad/s und schießt erst, wenn er
            // bis auf 0,15 rad (~9 Grad) ausgerichtet ist.
            Vector3 lead = Vector3Add(best->pos, Vector3Scale(best->vel, bestD / cfg::BULLET_SPEED));
            float want = YawTowards(wp, lead);
            float diff = AngleDiff(tu.yaw, want);
            tu.yaw += Clamp(diff, -4.0f * dt, 4.0f * dt);
            if (tu.cd <= 0.0f && fabsf(diff) < 0.15f) {
                // Turm: knapp 2 Schuss pro Sekunde, 14 Schaden (ein normaler
                // Pirat der Stufe 0 ist nach 3 Treffern erledigt).
                tu.cd = 0.55f;
                Bullet b;
                // Abschuss aus Rohrhöhe 1,4 schräg nach oben zum Schiff (als
                // einzige Geschosse nicht waagerecht), 1,2 vor dem Turm.
                b.pos = {wp.x, 1.4f, wp.z};
                Vector3 dir = Vector3Normalize(Vector3Subtract({lead.x, best->pos.y, lead.z}, b.pos));
                b.pos = Vector3Add(b.pos, Vector3Scale(dir, 1.2f));
                b.vel = Vector3Scale(dir, cfg::BULLET_SPEED);
                b.life = cfg::TURRET_RANGE / cfg::BULLET_SPEED + 0.2f;   // reicht etwas über die Turmreichweite hinaus
                b.damage = 14.0f;
                b.faction = Faction::Neutral;
                b.ownerId = -1;
                bullets.push_back(b);
                SoundAt(Sfx::TurretShot, wp);
            }
        }
    }
}

// Geschosse bewegen und Treffer prüfen. Geprüft wird nur die Position am
// Ende jedes Schritts. Das reicht, solange ein Geschoss pro Schritt weniger
// weit fliegt als der Trefferdurchmesser (2 * 1,7 = 3,4): Server 55/30 ≈ 1,8,
// Einzelspieler höchstens 55/20 = 2,75 (main.cpp begrenzt dt auf 1/20 s).
// Ein Geschoss trifft höchstens ein Schiff (break) und verschwindet dann.
void World::UpdateBullets(float dt) {
    for (auto& b : bullets) {
        b.pos = Vector3Add(b.pos, Vector3Scale(b.vel, dt));
        b.life -= dt;
        if (b.life <= 0) continue;
        for (auto& s : ships) {
            if (s.dead || s.crashing || s.hidden || s.id == b.ownerId) continue;
            if (isServer && s.docked) continue;   // angedockte Spieler sind geschützt
            // Wer darf wen treffen? Spieler treffen keine Spieler (kein
            // Beschuss untereinander im Mehrspieler), Piraten keine Piraten.
            // Neutrale Geschosse stammen von einem wütenden Händler: Sie
            // treffen alle außer anderen Händlern.
            bool canHit;
            if (b.ownerId == -1) canHit = s.faction == Faction::Pirate;              // Geschützturm
            else if (b.faction == Faction::Player) canHit = s.faction != Faction::Player;
            else if (b.faction == Faction::Pirate) canHit = s.faction != Faction::Pirate;
            else canHit = s.faction != Faction::Neutral;                              // wütender Händler
            if (!canHit) continue;
            if (Vector3Distance(b.pos, s.pos) < cfg::SHIP_HIT_RADIUS) {
                b.life = 0.0f;
                SpawnSparks(b.pos, b.faction == Faction::Player ? Color{255, 200, 90, 255} : Color{255, 90, 80, 255}, 7);
                SoundAt(Sfx::HitMetal, b.pos);
                if (s.kind == ShipKind::Player) SoundAt(Sfx::ShieldHit, s.pos);
                DamageShip(s, b.damage, b.ownerId, b.faction, b.ownerPlayer);
                break;
            }
        }
    }
    bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](const Bullet& b) { return b.life <= 0; }),
                  bullets.end());
}

// Schaden anwenden und darauf reagieren. attackerId = Schiff des Schützen
// (-1 = Geschützturm), attackerPlayerId = Spieler des Schützen (für Meldung
// und Abschusszähler).
// Reaktionen: Spielerschiff -> Bild wackelt (Einzelspieler; im Mehrspieler
// erkennt NetClient den Treffer selbst). Händler, vom Spieler beschossen ->
// wehrt sich 25 s lang und bleibt dauerhaft "feindlich" markiert (rot in
// HUD und Bild, im Mehrspieler für alle Spieler). Händler,
// von Piraten beschossen -> flieht 8 s. Pirat -> greift den Schützen an
// (10 s lang auch über größere Entfernung, Ai.cpp). Turmtreffer lösen keine
// Verfolgung aus.
void World::DamageShip(Ship& s, float dmg, int attackerId, Faction attacker, int attackerPlayerId) {
    if (s.crashing || s.dead) return;
    s.hp -= dmg;
    s.hitFlash = 1.0f;

    PlayerState* attackerPlayer = attackerPlayerId >= 0 ? FindPlayer(attackerPlayerId) : nullptr;

    if (s.kind == ShipKind::Player) {
        if (!isServer) shake = std::min(1.0f, shake + 0.45f);
    } else if (s.faction == Faction::Neutral) {
        if (attacker == Faction::Player && attackerId >= 0) {
            if (!s.hostileToPlayer && !simulation && attackerPlayer)
                Notify(*attackerPlayer, "Ein Händler wehrt sich gegen deinen Angriff!", Color{255, 220, 120, 255});
            s.hostileToPlayer = true;
            s.ai = AiState::Attack;
            s.targetShip = attackerId;
            s.aggroT = 25.0f;
            s.mining = false;
        } else if (attacker == Faction::Pirate) {
            s.ai = AiState::Flee;
            s.aiTimer = 8.0f;
            s.mining = false;
        }
    } else if (s.faction == Faction::Pirate && attackerId >= 0) {
        s.ai = AiState::Attack;
        s.targetShip = attackerId;
        s.aggroT = 10.0f;
    }

    // Schild weg: Absturz beginnen (weiter geht es in Update, "Abstürzende
    // Schiffe"). Kleine Explosion jetzt, große beim Aufschlag. Das Schiff
    // wird leicht angehoben (vel.y = 1,5) und trudelt in zufälliger Richtung.
    if (s.hp <= 0.0f) {
        s.hp = 0.0f;
        s.crashing = true;
        s.crashT = 0.0f;
        s.crashSpin = Rand(0, 1) < 0.5f ? Rand(-5.0f, -3.0f) : Rand(3.0f, 5.0f);
        s.vel.y = 1.5f;
        s.mining = false;
        Explosion(s.pos, 0.7f);
        SoundAt(Sfx::ShipExplode, s.pos);
        if (s.kind == ShipKind::Player) {
            if (PlayerState* ps = FindPlayer(s.playerId)) {
                ps->mining = false;
                ps->scanOn = false;
                Notify(*ps, "Schild zerstört! Dein Schiff stürzt ab!", Color{255, 110, 90, 255});
            }
        } else if (attacker == Faction::Player && attackerId >= 0 && attackerPlayer) {
            ++attackerPlayer->kills;
            if (!simulation) Notify(*attackerPlayer, s.faction == Faction::Pirate ? "Pirat abgeschossen! Wrack kann abgebaut werden."
                                                                                : "Händler abgeschossen. Wrack kann abgebaut werden.",
                                    Color{255, 210, 130, 255});
        }
    }
}

// ===========================================================================
// Effekte
// ===========================================================================
// Explosion aus drei Teilen: Feuerbälle (leuchtend, 26 je Größe), Trümmer
// (grau, fallen mit gravity 14 zu Boden, 12 je Größe) und ein kurzer heller
// Blitz (0,25 s). size skaliert Anzahl, Tempo und Größe.
// Im Mehrspieler-Client wird diese Funktion für die FxEvents des Servers
// aufgerufen (NetClient.cpp) und erzeugt dort die Partikel.
void World::Explosion(Vector3 pos, float size) {
    if (isServer) {
        // Der Server erzeugt keine Partikel, er meldet die Explosion nur an die Clients
        FxEvent e;
        e.type = FxEvent::Type::Explosion;
        e.pos = pos;
        e.size = size;
        fx.push_back(e);
        return;
    }
    for (int i = 0; i < static_cast<int>(26 * size); ++i) {
        Particle p;
        p.pos = pos;
        Vector3 dir = Vector3Normalize({Rand(-1, 1), Rand(-0.3f, 1.0f), Rand(-1, 1)});
        p.vel = Vector3Scale(dir, Rand(3.0f, 10.0f) * size);
        p.life = p.maxLife = Rand(0.4f, 0.9f);
        p.size = Rand(0.35f, 0.8f) * size;
        int c = RandInt(0, 2);
        p.color = c == 0 ? Color{255, 230, 140, 255} : (c == 1 ? Color{255, 150, 50, 255} : Color{255, 90, 40, 255});
        p.glow = true;
        particles.push_back(p);
    }
    for (int i = 0; i < static_cast<int>(12 * size); ++i) {
        Particle p;
        p.pos = pos;
        p.vel = {Rand(-6, 6), Rand(2, 9), Rand(-6, 6)};
        p.life = p.maxLife = Rand(0.8f, 1.6f);
        p.size = Rand(0.15f, 0.35f);
        p.color = RandInt(0, 1) ? Color{200, 205, 215, 255} : Color{70, 76, 86, 255};
        p.gravity = 14.0f;
        particles.push_back(p);
    }
    Particle flash;
    flash.pos = pos;
    flash.life = flash.maxLife = 0.25f;
    flash.size = 3.0f * size;
    flash.color = {255, 240, 200, 255};
    flash.glow = true;
    particles.push_back(flash);
}

// n kurzlebige Funken (0,2–0,45 s) in Farbe c, z. B. bei Treffern, beim
// Abbau und beim Stationsausbau. Server: nur ein FxEvent.
void World::SpawnSparks(Vector3 pos, Color c, int n) {
    if (isServer) {
        FxEvent e;
        e.type = FxEvent::Type::Sparks;
        e.pos = pos;
        e.color = c;
        e.count = n;
        fx.push_back(e);
        return;
    }
    for (int i = 0; i < n; ++i) {
        Particle p;
        p.pos = pos;
        p.vel = {Rand(-5, 5), Rand(-1, 5), Rand(-5, 5)};
        p.life = p.maxLife = Rand(0.2f, 0.45f);
        p.size = Rand(0.1f, 0.22f);
        p.color = c;
        p.glow = true;
        particles.push_back(p);
    }
}

// Partikel bewegen und altern lassen. Trümmer (gravity > 0) bleiben knapp
// über dem Boden liegen und werden dabei stark abgebremst.
void World::UpdateParticles(float dt) {
    for (auto& p : particles) {
        p.vel.y -= p.gravity * dt;
        p.pos = Vector3Add(p.pos, Vector3Scale(p.vel, dt));
        if (p.gravity > 0 && p.pos.y < 0.1f) { p.pos.y = 0.1f; p.vel = Vector3Scale(p.vel, 0.3f); }
        p.life -= dt;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0; }),
                    particles.end());
    // Notbremse für die Bildrate: Bei sehr vielen Partikeln (große Schlacht)
    // die 1000 ältesten auf einmal löschen (vorne in der Liste stehen die ältesten).
    if (particles.size() > 4000) particles.erase(particles.begin(), particles.begin() + 1000);
    shake = std::max(0.0f, shake - dt * 2.5f);   // Wackeln klingt in 0,4 s von 1 auf 0 ab
}

// Meldung anzeigen (4 s). Gleiche Meldung noch sichtbar -> nur Zähler
// erhöhen und Zeit auffrischen ("(3x)" im HUD). Höchstens 5 Meldungen,
// die älteste fällt heraus. Auf dem Server landet jede Meldung zusätzlich in
// ps.outbox; GameServer schickt sie nach dem Schritt an den Client, der sie
// dort wieder mit Notify anzeigt.
void World::Notify(PlayerState& ps, const std::string& text, Color c) {
    if (isServer) ps.outbox.push_back({text, 4.0f, c, 1});   // der Server verschickt die Meldung
    for (auto& n : ps.notices) {
        if (n.text == text) { n.time = 4.0f; n.count++; return; }
    }
    ps.notices.push_back({text, 4.0f, c, 1});
    if (ps.notices.size() > 5) ps.notices.erase(ps.notices.begin());
}

// Nur ein Eintrag in sounds; abgespielt (Einzelspieler, main.cpp/Audio.cpp)
// bzw. verschickt (Server) wird später.
void World::SoundAt(Sfx id, Vector3 pos, float volume, float delay, int forPlayer, int sourceShip) {
    SoundEvent e;
    e.id = id;
    e.pos = pos;
    e.volume = volume;
    e.delay = delay;
    e.forPlayer = forPlayer;
    e.sourceShip = sourceShip;
    sounds.push_back(e);
}

// ===========================================================================
// Suchen
// Über die ID statt über den Index, weil sich Indizes beim Löschen
// verschieben (siehe World.hpp). Tote Schiffe gelten als "nicht gefunden".
// ===========================================================================
Ship* World::FindShip(int id) {
    for (auto& s : ships) if (s.id == id && !s.dead) return &s;
    return nullptr;
}
const Ship* World::FindShip(int id) const {
    for (const auto& s : ships) if (s.id == id && !s.dead) return &s;
    return nullptr;
}
Deposit* World::FindDeposit(int id) {
    for (auto& d : deposits) if (d.id == id) return &d;
    return nullptr;
}
Wreck* World::FindWreck(int id) {
    for (auto& w : wrecks) if (w.id == id) return &w;
    return nullptr;
}
