// Spielstand als einfache Textdatei neben der ausführbaren Datei.
#include "Save.hpp"
#include "Assets.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

const char* kMagic = "MONDSCHUERFER_SAVE";
const int kVersion = 1;

// Modelle werden als Varianten-Nummer gespeichert, nicht als Laufzeit-ID
int ShipVariant(const World& w, int model) {
    const int list[7] = {w.mdl.player, w.mdl.pirateA, w.mdl.pirateB, w.mdl.elite, w.mdl.miner, w.mdl.cargoA, w.mdl.cargoB};
    for (int i = 0; i < 7; ++i) if (list[i] == model) return i;
    return 0;
}
int ShipModel(const World& w, int variant) {
    const int list[7] = {w.mdl.player, w.mdl.pirateA, w.mdl.pirateB, w.mdl.elite, w.mdl.miner, w.mdl.cargoA, w.mdl.cargoB};
    return list[(variant >= 0 && variant < 7) ? variant : 0];
}
int DepositVariant(const World& w, int model) {
    if (model == w.mdl.crystalA) return 1;
    if (model == w.mdl.crystalB) return 2;
    return 0;
}
int DepositModel(const World& w, int v) {
    return v == 1 ? w.mdl.crystalA : (v == 2 ? w.mdl.crystalB : w.mdl.crystalSmall);
}

} // namespace

std::string SavePath() { return std::string(GetApplicationDirectory()) + "mondschuerfer_spielstand.txt"; }

bool SaveExists() {
    std::ifstream f(SavePath());
    std::string magic;
    f >> magic;
    return f.good() && magic == kMagic;
}

void DeleteSave() { std::remove(SavePath().c_str()); }

bool SaveGame(const World& w) {
    if (w.simulation || w.outcome != Outcome::None) return false;
    std::ofstream f(SavePath(), std::ios::trunc);
    if (!f) return false;
    const Ship& p = w.Player();
    const PlayerState& me = w.Me();
    f << kMagic << ' ' << kVersion << '\n';
    f << "seed " << w.seed << '\n';
    f << "balance " << me.balance << '\n';
    f << "delivered " << w.totalDelivered << '\n';
    f << "upgrades " << me.upgrades[0] << ' ' << me.upgrades[1] << ' ' << me.upgrades[2] << ' ' << me.upgrades[3] << '\n';
    f << "stats " << me.playTime << ' ' << me.kills << ' ' << w.nextId << '\n';
    f << "scanner " << me.scanEnergy << '\n';
    f << "player " << p.pos.x << ' ' << p.pos.z << ' ' << p.yaw << ' ' << p.hp << ' ' << p.cargo << '\n';
    for (const auto& d : w.deposits)
        f << "deposit " << d.id << ' ' << d.pos.x << ' ' << d.pos.z << ' ' << d.yaw << ' ' << d.scale << ' '
          << DepositVariant(w, d.model) << ' ' << d.amount << ' ' << d.initial << ' ' << (d.revealed ? 1 : 0) << '\n';
    for (const auto& wr : w.wrecks)
        f << "wreck " << wr.id << ' ' << wr.pos.x << ' ' << wr.pos.z << ' ' << wr.yaw << ' ' << wr.tilt << ' '
          << ShipVariant(w, wr.model) << ' ' << wr.amount << ' ' << wr.initial << ' ' << wr.age << '\n';
    for (size_t i = 1; i < w.ships.size(); ++i) {
        const Ship& s = w.ships[i];
        if (s.dead || s.crashing) continue;
        f << "ship " << static_cast<int>(s.kind) << ' ' << ShipVariant(w, s.model) << ' ' << s.pos.x << ' ' << s.pos.z
          << ' ' << s.yaw << ' ' << s.hp << ' ' << s.maxHp << ' ' << s.maxSpeed << ' ' << s.damage << ' '
          << s.fireInterval << ' ' << s.cargo << ' ' << s.cargoMax << ' ' << (s.hostileToPlayer ? 1 : 0) << ' '
          << (s.hidden ? 1 : 0) << '\n';
    }
    f << "end\n";
    return static_cast<bool>(f);
}

bool LoadGame(World& w) {
    std::ifstream f(SavePath());
    if (!f) return false;
    std::string magic;
    int version = 0;
    f >> magic >> version;
    if (magic != kMagic || version != kVersion) return false;

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
    w.NewGame(seed, false);
    w.deposits.clear();
    w.ships.resize(1);
    w.Me().notices.clear();

    f.clear();
    f.seekg(start);
    Ship& p = w.Player();
    PlayerState& me = w.Me();
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "balance") ls >> me.balance;
        else if (key == "delivered") ls >> w.totalDelivered;
        else if (key == "upgrades") ls >> me.upgrades[0] >> me.upgrades[1] >> me.upgrades[2] >> me.upgrades[3];
        else if (key == "stats") ls >> me.playTime >> me.kills >> w.nextId;
        else if (key == "scanner") ls >> me.scanEnergy;
        else if (key == "player") {
            float hp = 0;
            ls >> p.pos.x >> p.pos.z >> p.yaw >> hp >> p.cargo;
            for (int& u : me.upgrades) u = std::clamp(u, 0, cfg::UPGRADE_LEVELS);
            w.ApplyUpgrades(me, true);
            p.hp = hp;
        } else if (key == "deposit") {
            Deposit d;
            int v = 0, rev = 0;
            ls >> d.id >> d.pos.x >> d.pos.z >> d.yaw >> d.scale >> v >> d.amount >> d.initial >> rev;
            d.model = DepositModel(w, v);
            d.revealed = rev != 0;
            d.revealT = 1.0f;
            w.deposits.push_back(d);
        } else if (key == "wreck") {
            Wreck wr;
            int v = 0;
            ls >> wr.id >> wr.pos.x >> wr.pos.z >> wr.yaw >> wr.tilt >> v >> wr.amount >> wr.initial >> wr.age;
            wr.model = ShipModel(w, v);
            wr.pos.y = 0.15f;
            w.wrecks.push_back(wr);
        } else if (key == "ship") {
            int kind = 0, v = 0, hostile = 0, hidden = 0;
            Ship s;
            ls >> kind >> v >> s.pos.x >> s.pos.z >> s.yaw >> s.hp >> s.maxHp >> s.maxSpeed >> s.damage >>
                s.fireInterval >> s.cargo >> s.cargoMax >> hostile >> hidden;
            if (kind < 1 || kind > 3) continue;
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
            base.ai = base.faction == Faction::Pirate ? AiState::Wander : AiState::GoMine;
            if (hidden && base.kind == ShipKind::Miner) {
                base.hidden = true;
                base.ai = AiState::Docked;
                base.aiTimer = 3.0f;
            }
            w.ships.push_back(base);
        }
    }
    p.pos.y = cfg::SHIP_ALTITUDE;
    p.vel = {0, 0, 0};
    w.lastStage = w.StationStage();
    w.Notify("Spielstand geladen. Weiter geht's!", Color{200, 220, 255, 255});
    return true;
}
