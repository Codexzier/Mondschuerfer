// ---------------------------------------------------------------------------
// Ai.cpp – Verhalten der anderen Raumschiffe und der Autopilot für den
// Simulationsmodus.
//
// Wofür ist die Datei da?
//   1. World::UpdateAi: die "KI" der computergesteuerten Schiffe
//        - Piraten (ShipKind::Pirate und Elite, Faction::Pirate): kreuzen
//          umher, greifen Spieler und Händler an, meiden die Stationen.
//        - Händler/Miner (ShipKind::Miner, Faction::Neutral): fliegen zu
//          Lagerstätten, bauen ab, verkaufen an der Handelsstation; wehren
//          sich gegen Spieler, fliehen vor Piraten.
//   2. World::UpdateTrooperAi: Figuren zu Fuß ohne Spieler (FPS-Modus) –
//      NPC-Kameraden des SG-Teams und Bodenpiraten (Abschnitt unten).
//   3. Autopilot (AutopilotInput, AutopilotDockActions, ResetAutopilot):
//      steuert das EIGENE Schiff (players[0]/ships[0]) wie ein Mensch.
//
// Wer ruft das auf?
//   - World::Update (shared/World.cpp) ruft UpdateAi in jedem Schritt für
//     jedes Schiff auf, das keinem Spieler gehört. Das passiert im
//     Einzelspieler und auf dem Server. Der Mehrspieler-Client ruft es NIE
//     auf: Er bekommt die Positionen der KI-Schiffe vom Server.
//   - Autopilot: src/main.cpp (Hintergrund des Hauptmenüs und Menüpunkt
//     "Simulation") und src/Multiplayer.cpp (Testoption --mp-autopilot, dort
//     auf der Spiegelwelt; Käufe gehen dort als Anfrage an den Server).
//
// Grundidee: Die KI "drückt nur Tasten". Sie berechnet ein ShipInput
// (Schub, Drehen, Feuern) – genau wie die Tastatur eines Spielers – und
// danach läuft dieselbe Physik (World::UpdateShipPhysics). KI-Schiffe können
// also nichts, was ein Spieler nicht auch könnte (kein Teleportieren, keine
// unendliche Wendigkeit).
//
// Aufbau als Zustandsautomat ("state machine"): Jedes Schiff hat einen
// Zustand Ship::ai (AiState in World.hpp) und wechselt bei bestimmten
// Ereignissen in einen anderen:
//   Pirat:  Wander <-> Attack   (dazu: Stationen ausweichen)
//   Händler: GoMine -> Mining -> GoMine ... -> GoSell -> Docked -> GoMine
//            Attack  (wenn ein Spieler auf ihn schießt, World::DamageShip)
//            Flee    (wenn ein Pirat auf ihn schießt, World::DamageShip)
//
// Zufall: Alle Zufallszahlen kommen aus World::Rand/RandInt (dem Zufalls-
// generator der Welt). Gleicher Seed + gleiche Eingaben ergeben darum im
// Einzelspieler genau dasselbe Spiel – auch mit dem Autopiloten, der
// ebenfalls aus diesem Generator würfelt.
//
// Abhängigkeiten: World.hpp (und darüber Config.hpp für Balancing-Werte wie
// cfg::PIRATE_AGGRO, Util.hpp für YawTowards/AngleDiff/Dist2D und raymath.h
// für Vektorrechnung und Clamp). Keine Grafik, kein Ton.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include <algorithm>
#include <cmath>

namespace {

// Steuert ein Schiff auf einen Zielpunkt zu
//   arrive   ab diesem Abstand gilt das Ziel als erreicht (kein Schub mehr)
//   throttle Schub (0..1) unterwegs, z. B. 0,6 für gemütliches Kreuzen
// Lenken: einfacher Proportionalregler – je größer der Winkelfehler, desto
// stärker wird gedreht; ab etwa 0,36 rad (~20°) Abweichung mit vollem
// Ausschlag (diff * 2,8 = 1).
// Schub nur, wenn das Ziel ungefähr voraus liegt (Abweichung < 1,3 rad,
// ~75°). Sonst dreht das Schiff erst auf der Stelle; mit Vollgas in die
// falsche Richtung würde es weite Bögen fliegen.
// Bremsen: Im letzten Stück (näher als 2,5 * arrive) nur 45 % Schub, damit
// das Schiff nicht über das Ziel hinausschießt. Ohne Schub bremst die
// Physik das Schiff von selbst ab (Reibung in UpdateShipPhysics).
ShipInput SteerTo(const Ship& s, Vector3 target, float arrive, float throttle = 1.0f) {
    ShipInput in;
    float desired = YawTowards(s.pos, target);
    float diff = AngleDiff(s.yaw, desired);
    in.turn = Clamp(diff * 2.8f, -1.0f, 1.0f);
    float d = Dist2D(s.pos, target);
    if (d > arrive && fabsf(diff) < 1.3f) {
        in.thrust = throttle;
        if (d < arrive * 2.5f) in.thrust *= 0.45f;
    }
    return in;
}

// Zielt mit Vorhalt auf ein Schiff
// Wer auf die AKTUELLE Position eines fliegenden Ziels schießt, trifft
// daneben: Bis das Geschoss ankommt, ist das Ziel weitergeflogen. Daher:
// Flugzeit des Geschosses schätzen (Abstand / Geschossgeschwindigkeit) und
// dorthin zielen, wo das Ziel nach dieser Zeit sein wird (Position +
// Geschwindigkeit * Zeit). Einfache Näherung: Die eigene Geschwindigkeit
// (die das Geschoss teilweise mitbekommt) und Kurven des Ziels werden nicht
// berücksichtigt – die KI soll ja auch nicht perfekt treffen.
// Benutzt von KI-Schiffen und vom Autopiloten (Geschütztürme rechnen in
// World::UpdateTurrets genauso).
Vector3 LeadPoint(const Ship& shooter, const Ship& target) {
    float d = Dist2D(shooter.pos, target.pos);
    float t = d / cfg::BULLET_SPEED;
    return Vector3Add(target.pos, Vector3Scale(target.vel, t));
}

// Liegt p in der Schutzzone einer Station (Handelsstation ODER Werft)?
// Radius cfg::STATION_SAFE_RADIUS (34) passt zur Reichweite der
// Geschütztürme (cfg::TURRET_RANGE = 36). "extra" vergrößert die Zone, z. B.
// damit Piraten ihre Wegpunkte mit Sicherheitsabstand wählen.
bool InsideSafeZone(const World& w, Vector3 p, float extra = 0.0f) {
    for (const auto& st : w.stations)
        if (Dist2D(p, st.pos) < cfg::STATION_SAFE_RADIUS + extra) return true;
    return false;
}

// Kampfmanöver: anfliegen, schießen, abdrehen
// Für Piraten und angegriffene Händler. Liefert die Steuerung und über
// wantFire, ob jetzt geschossen werden soll (ob die Waffe nachgeladen ist,
// prüft erst UpdateAi).
// Ablauf eines Anflugs:
//   - Weiter als 14 entfernt: Vollgas auf den Vorhaltepunkt.
//   - 8..14: nur 35 % Schub (in Schussposition bleiben),
//     unter 8: leicht rückwärts (-0,2), um nicht zu rammen.
//   - Unter 6,5: "Abdrehen" (breakOff) – für 0,9..1,6 s einen Punkt
//     14 voraus und 14 zur Seite (zufällig links oder rechts) anfliegen,
//     dann neu angreifen. Das sieht aus wie ein echter Luftkampf und
//     verhindert, dass zwei Schiffe aneinanderkleben und sich umkreisen.
// Gedreht wird mit Faktor 3,2 etwas schärfer als bei SteerTo (2,8).
// Geschossen wird nur, wenn die Nase auf weniger als 0,2 rad (~11°) genau
// zeigt und das Ziel näher als 34 ist. Geschosse fliegen zwar ~58 weit
// (cfg::BULLET_SPEED * BULLET_LIFE), auf große Entfernung trifft man aber
// kaum – so verschwendet die KI keine Schüsse.
ShipInput Dogfight(World& w, Ship& s, const Ship& target, float dt, bool& wantFire) {
    ShipInput in;
    wantFire = false;
    float d = Dist2D(s.pos, target.pos);
    if (s.breakOff > 0.0f) {
        s.breakOff -= dt;
        return SteerTo(s, s.waypoint, 2.0f, 1.0f);
    }
    Vector3 lead = LeadPoint(s, target);
    float desired = YawTowards(s.pos, lead);
    float diff = AngleDiff(s.yaw, desired);
    in.turn = Clamp(diff * 3.2f, -1.0f, 1.0f);
    in.thrust = d > 14.0f ? 1.0f : (d > 8.0f ? 0.35f : -0.2f);
    if (d < 6.5f) {
        // zu nah: abdrehen und neu anfliegen
        // side = Vorwärtsrichtung um 90° nach links gedreht (auf der
        // x/z-Ebene: (x, z) -> (z, -x)); sgn wählt links oder rechts.
        s.breakOff = w.Rand(0.9f, 1.6f);
        Vector3 fwd = ForwardFromYaw(s.yaw);
        Vector3 side{fwd.z, 0, -fwd.x};
        float sgn = w.Rand(0, 1) < 0.5f ? -1.0f : 1.0f;
        s.waypoint = Vector3Add(s.pos, Vector3Add(Vector3Scale(fwd, 14.0f), Vector3Scale(side, 14.0f * sgn)));
    }
    wantFire = fabsf(diff) < 0.2f && d < 34.0f;
    return in;
}

} // namespace

// ===========================================================================
// KI der Piraten und Händler
//
// Wird von World::Update für jedes KI-Schiff einmal pro Schritt aufgerufen
// (nicht für zerstörte/abstürzende Schiffe und nicht für versteckte, außer
// angedockte Händler, die irgendwann wieder ablegen müssen).
// Am Ende steht immer: ggf. schießen (FireBullet erzeugt Geschoss und Ton),
// dann die Physik mit dem berechneten ShipInput.
// ===========================================================================
void World::UpdateAi(Ship& s, float dt) {
    ShipInput in;
    bool fire = false;
    // Zeitgeber herunterzählen. hitFlash (Aufblitzen nach einem Treffer,
    // nur für die Grafik) klingt mit 3 pro Sekunde ab, also in ~0,33 s.
    s.fireCd -= dt;
    s.hitFlash = std::max(0.0f, s.hitFlash - dt * 3.0f);
    s.aggroT = std::max(0.0f, s.aggroT - dt);
    s.thinkT -= dt;
    s.aiTimer -= dt;

    if (s.kind == ShipKind::Convoy) {
        UpdateConvoy(s, dt);
        return;
    }
    if (s.faction == Faction::Pirate) {
        // ---------------- Pirat -------------------------------------------
        // Zielwahl nur alle 0,35 s ("Nachdenken"): Das wirkt wie eine
        // menschliche Reaktionszeit und spart Rechenzeit – jede Suche geht
        // über ALLE Schiffe, auf dem Server mit bis zu 40 Piraten und
        // 64 Spielern sind das sonst viele tausend Vergleiche pro Schritt.
        if (s.thinkT <= 0.0f) {
            s.thinkT = 0.35f;
            Ship* best = nullptr;
            float bestScore = 1e9f;
            for (auto& o : ships) {
                if (&o == &s || o.dead || o.crashing || o.hidden || o.faction == Faction::Pirate) continue;
                if (o.kind == ShipKind::Player && !Targetable(o)) continue;   // z. B. angedockt
                float d = Dist2D(o.pos, s.pos);
                // Sichtweite: Spieler ab cfg::PIRATE_AGGRO (30), Händler erst
                // ab 24 (die sind nur "Beifang"). Wurde der Pirat gerade von
                // diesem Schiff getroffen (aggroT > 0, gesetzt in
                // World::DamageShip), verfolgt er es bis 70 weit.
                float range = cfg::PIRATE_AGGRO;
                if (o.kind == ShipKind::Miner) range = 24.0f;
                if (s.aggroT > 0.0f && o.id == s.targetShip) range = 70.0f;
                // Piraten einer Konvoi-Welle jagen "ihre" Frachter (gleiche
                // Mission) bis 90 weit und ziehen sie allen anderen Zielen vor.
                const bool prey = s.mission >= 0 && o.mission == s.mission;
                if (prey) range = 90.0f;
                if (d > range) continue;
                // Wer in der Schutzzone einer Station ist, ist sicher.
                if (InsideSafeZone(*this, o.pos)) continue;
                // Spieler werden bevorzugt: Sie zählen, als wären sie 20 %
                // näher. Im Mehrspieler kommen die Schiffe ALLER Spieler in
                // Frage; der Pirat nimmt den mit der kleinsten Wertung.
                float score = o.kind == ShipKind::Player ? d * 0.8f : d;
                if (prey) score = d * 0.5f;
                if (score < bestScore) { bestScore = score; best = &o; }
            }
            if (best) {
                s.ai = AiState::Attack;
                s.targetShip = best->id;
            } else if (s.ai == AiState::Attack) {
                // Kein Ziel mehr in Reichweite: sofort (aiTimer = 0) einen
                // neuen Wegpunkt zum Umherfliegen suchen.
                s.ai = AiState::Wander;
                s.aiTimer = 0.0f;
            }
        }

        // Angreifen: Das Ziel wird JEDEN Schritt geprüft (nicht nur alle
        // 0,35 s), denn es kann inzwischen abgeschossen, angedockt oder
        // verschwunden sein. Ziele merkt sich die KI nur über die
        // Schiffs-ID; FindShip liefert nullptr, wenn es das Schiff nicht
        // mehr gibt (z. B. Spieler hat den Server verlassen).
        if (s.ai == AiState::Attack) {
            Ship* t = FindShip(s.targetShip);
            if (!t || t->crashing || t->hidden || (t->kind == ShipKind::Player && !Targetable(*t))) {
                s.ai = AiState::Wander;
                s.aiTimer = 0.0f;
            } else {
                in = Dogfight(*this, s, *t, dt, fire);
            }
        }
        // Umherfliegen: einen Wegpunkt ansteuern; einen neuen wählen, wenn
        // er erreicht ist (näher als 5) oder die Zeit (8..16 s) abgelaufen
        // ist. Der neue Punkt liegt höchstens 60 entfernt (so bleibt der
        // Pirat ungefähr in seinem Revier), mindestens 10 vom Kartenrand und
        // möglichst außerhalb der Schutzzonen (+12 Sicherheitsabstand; nach
        // 20 vergeblichen Versuchen wird der letzte genommen).
        // Gekreuzt wird mit 60 % Schub – im Angriff sind Piraten schneller.
        if (s.ai != AiState::Attack) {
            if (s.aiTimer <= 0.0f || Dist2D(s.pos, s.waypoint) < 5.0f) {
                Vector3 p = s.pos;
                for (int i = 0; i < 20; ++i) {
                    p = {s.pos.x + Rand(-60, 60), 0, s.pos.z + Rand(-60, 60)};
                    p.x = Clamp(p.x, -cfg::WORLD_HALF + 10, cfg::WORLD_HALF - 10);
                    p.z = Clamp(p.z, -cfg::WORLD_HALF + 10, cfg::WORLD_HALF - 10);
                    if (!InsideSafeZone(*this, p, 12.0f)) break;
                }
                s.waypoint = p;
                s.aiTimer = Rand(8.0f, 16.0f);
            }
            in = SteerTo(s, s.waypoint, 3.0f, 0.6f);
        }
        // Stationen meiden (Geschütztürme)
        // Hat Vorrang vor allem anderen: Kommt ein Pirat näher als
        // Schutzradius + 4 (= 38) an eine Station, dreht er ab, fliegt 20
        // Einheiten direkt von ihr weg, schießt nicht und gibt seinen Angriff
        // auf (4 s Umherfliegen). Dadurch sind die Stationen für Spieler ein
        // sicherer Hafen, und Piraten fliegen nicht in die Geschütztürme.
        for (const auto& st : stations) {
            float d = Dist2D(s.pos, st.pos);
            if (d < cfg::STATION_SAFE_RADIUS + 4.0f) {
                Vector3 away = Vector3Add(s.pos, Vector3Scale(Vector3Normalize(Vector3Subtract(s.pos, st.pos)), 20.0f));
                in = SteerTo(s, away, 1.0f, 1.0f);
                fire = false;
                s.ai = AiState::Wander;
                s.waypoint = away;
                s.aiTimer = 4.0f;
            }
        }
    } else {
        // ---------------- Händler / Miner ------------------------------------
        // s.mining / s.mineTarget sind nur für die Anzeige (Abbau-Strahl in
        // Render.cpp; im Mehrspieler über den Snapshot an die Clients).
        if (s.ai != AiState::Mining) s.mining = false;
        switch (s.ai) {
            case AiState::Attack: {
                // Gegenwehr gegen einen Spieler, der auf den Händler
                // geschossen hat (World::DamageShip setzt Attack, das Ziel und
                // aggroT = 25 s). Aufgegeben wird, wenn das Ziel weg ist,
                // weiter als 60 entfernt ist oder die 25 s um sind.
                Ship* t = FindShip(s.targetShip);
                bool lost = !t || t->crashing || t->hidden || (t->kind == ShipKind::Player && !Targetable(*t)) ||
                            Dist2D(s.pos, t->pos) > 60.0f || s.aggroT <= 0.0f;
                if (lost) {
                    // Zurück an die Arbeit: ab 80 % Ladung erst verkaufen
                    s.ai = s.cargo >= s.cargoMax * 0.8f ? AiState::GoSell : AiState::GoMine;
                    s.targetDeposit = -1;
                } else {
                    in = Dogfight(*this, s, *t, dt, fire);
                }
                break;
            }
            case AiState::Flee: {
                // Zur nächsten Handelsstation fliehen (dort schützen die Geschütztürme)
                // Die Flucht endet nach aiTimer (8 s, gesetzt von
                // World::DamageShip) oder sobald der Händler tief in der
                // Schutzzone ist (60 % des Radius). Dann verkauft er, was er
                // hat (GoSell -> andocken = sicher versteckt).
                // max(0, ...): Gibt es keine Handelsstation (-1), Station 0
                // nehmen – die ist immer eine Handelsstation.
                const Vector3 home = stations[std::max(0, NearestStation(s.pos, StationType::Trade))].pos;
                in = SteerTo(s, home, 8.0f, 1.0f);
                if (s.aiTimer <= 0.0f || Dist2D(s.pos, home) < cfg::STATION_SAFE_RADIUS * 0.6f)
                    s.ai = AiState::GoSell;
                break;
            }
            case AiState::GoMine: {
                Deposit* dep = FindDeposit(s.targetDeposit);
                if (!dep || dep->amount <= 0) {
                    // Eine der nächsten Lagerstätten wählen, die kein anderer Händler anfliegt
                    // Händler brauchen keinen Scanner: Sie kennen auch noch
                    // unentdeckte Lagerstätten und machen den Spielern so
                    // Konkurrenz. Zufällig eine der drei nächsten, damit nicht
                    // alle Händler dasselbe Muster fliegen.
                    std::vector<std::pair<float, int>> cand;   // (Abstand, Lagerstätten-ID)
                    for (auto& d : deposits) {
                        bool taken = false;
                        for (auto& o : ships)
                            if (&o != &s && o.kind == ShipKind::Miner && o.targetDeposit == d.id) taken = true;
                        if (!taken && d.amount > 0 && d.sample != Sample::Yellow) cand.push_back({Dist2D(d.pos, s.pos), d.id});
                    }
                    std::sort(cand.begin(), cand.end());   // Paare sortieren nach dem ersten Wert (Abstand)
                    if (!cand.empty()) {
                        int pick = RandInt(0, std::min<int>(2, static_cast<int>(cand.size()) - 1));
                        s.targetDeposit = cand[pick].second;
                    }
                    dep = FindDeposit(s.targetDeposit);
                }
                if (dep) {
                    // Hinfliegen (85 % Schub) und ab 5 Einheiten Abstand mit
                    // dem Abbau beginnen. Er dauert so lange wie bei Spielern
                    // (cfg::MINING_TIME).
                    in = SteerTo(s, dep->pos, 3.5f, 0.85f);
                    if (Dist2D(s.pos, dep->pos) < 5.0f) {
                        s.ai = AiState::Mining;
                        s.aiTimer = cfg::MINING_TIME;
                        s.mining = true;
                        s.mineTarget = dep->pos;
                    }
                }
                break;
            }
            case AiState::Mining: {
                // Kein ShipInput: Das Schiff treibt aus und steht still.
                Deposit* dep = FindDeposit(s.targetDeposit);
                if (!dep) { s.ai = AiState::GoMine; s.mining = false; break; }   // inzwischen von anderen geleert
                s.mining = true;
                s.mineTarget = dep->pos;
                if (s.aiTimer <= 0.0f) {
                    // Wie bei Spielern: am Ende auf einmal so viel laden, wie
                    // ins Lager passt (höchstens, was noch da ist). Eine
                    // leere Lagerstätte (amount 0) räumt World::Update weg.
                    int take = std::min(s.cargoMax - s.cargo, dep->amount);
                    dep->amount -= take;
                    s.cargo += take;
                    // Menge geändert: Der Server schickt die Liste neu (wie bei Spielern, World::FinishMining).
                    if (take > 0) ++depositRevision;
                    s.mining = false;
                    s.targetDeposit = -1;
                    s.ai = s.cargo >= s.cargoMax * 0.8f ? AiState::GoSell : AiState::GoMine;
                }
                break;
            }
            case AiState::GoSell: {
                // Zur nächsten Handelsstation; näher als 10 = angedockt.
                // Angedockt ist der Händler versteckt (hidden): nicht
                // sichtbar, nicht angreifbar, im Mehrspieler nicht im
                // Snapshot. Der Verkauf ist nur "Ladung = 0" – Händler zählen
                // NICHT zum Ausbau der Stationen (das schaffen nur Spieler).
                const int home = std::max(0, NearestStation(s.pos, StationType::Trade));
                in = SteerTo(s, stations[home].pos, 6.0f, 0.85f);
                if (Dist2D(s.pos, stations[home].pos) < 10.0f) {
                    s.ai = AiState::Docked;
                    s.station = home;
                    s.hidden = true;
                    s.aiTimer = Rand(4.0f, 9.0f);   // so lange "in der Station"
                    s.cargo = 0;
                    s.vel = {0, 0, 0};
                }
                break;
            }
            case AiState::Docked: {
                // Wieder ablegen: an einem zufälligen Punkt 13 Einheiten um
                // die Stationsmitte auftauchen, mit der Nase von der Station
                // weg (Blickrichtung Mitte -> Schiff), und wieder abbauen.
                if (s.aiTimer <= 0.0f) {
                    s.hidden = false;
                    int home = s.station;
                    if (home < 0 || home >= static_cast<int>(stations.size())) home = std::max(0, NearestStation(s.pos, StationType::Trade));
                    const Vector3 hp = stations[home].pos;
                    float a = Rand(0, 2 * PI);
                    s.pos = {hp.x + cosf(a) * 13.0f, cfg::SHIP_ALTITUDE, hp.z + sinf(a) * 13.0f};
                    s.yaw = YawTowards(hp, s.pos);
                    s.ai = AiState::GoMine;
                    s.targetDeposit = -1;
                }
                // Angedockt (und im Schritt des Auftauchens) keine Physik
                return;
            }
            default:
                // Zustände, die für Händler keine Bedeutung haben (z. B.
                // Wander): einfach mit dem Abbau beginnen.
                s.ai = AiState::GoMine;
                break;
        }
    }

    // Schießen nur, wenn die Waffe nachgeladen ist; fireInterval hängt von
    // der Schiffsart ab (World::MakeShip).
    if (fire && s.fireCd <= 0.0f) {
        FireBullet(s);
        s.fireCd = s.fireInterval;
    }
    UpdateShipPhysics(s, dt, in);
}

// ===========================================================================
// Konvoi (Mission "Konvoi verteidigen")
//
// Die drei Frachter folgen nebeneinander (5 Einheiten Abstand quer zur
// Fahrtrichtung) den Wegpunkten ihrer Mission; World::UpdateMissions schaltet
// den Wegpunkt weiter und hält den Konvoi an, solange der Auftraggeber
// angedockt ist. Angriffe ignorieren sie (keine Waffen, keine Flucht). Am
// letzten Wegpunkt (Zielstation) docken sie an: Sie verschwinden und zählen
// als angekommen.
// ===========================================================================
void World::UpdateConvoy(Ship& s, float dt) {
    ShipInput in;
    Mission* m = FindMission(s.mission);
    if (m && !m->route.empty()) {
        const int last = static_cast<int>(m->route.size()) - 1;
        const int leg = std::clamp(m->leg, 0, last);
        Vector3 target = m->route[leg];
        if (leg < last) {
            // Platz in der Formation aus der ID: -1, 0 oder +1 (die drei
            // Frachter haben aufeinanderfolgende IDs)
            const Vector3 from = leg > 0 ? m->route[leg - 1] : s.pos;
            Vector3 dir = Vector3Subtract(target, from);
            dir.y = 0.0f;
            dir = Vector3Length(dir) > 0.1f ? Vector3Normalize(dir) : Vector3{0, 0, -1};
            const float slot = static_cast<float>(s.id % 3) - 1.0f;
            target = Vector3Add(target, Vector3Scale(Vector3{-dir.z, 0.0f, dir.x}, slot * 5.0f));
        }
        if (!m->hold) in = SteerTo(s, target, 2.0f, 1.0f);
        if (leg == last && Dist2D(s.pos, m->route[last]) < 12.0f) {
            s.dead = true;
            s.hidden = true;
            ++m->progress;
            return;
        }
    }
    UpdateShipPhysics(s, dt, in);
}

// ===========================================================================
// Figuren zu Fuß (FPS-Modus): Kameraden und Bodenpiraten
//
// Auch hier "drückt die KI nur Tasten": Sie füllt Trooper::input, danach
// läuft dieselbe Physik wie für den Spieler (World::UpdateTroopers).
//   Kamerad: Follow – läuft auf seinen Platz in der Formation hinter dem
//            Spieler (MateSlot, nach seiner Laufrichtung) und sprintet bei
//            großem Abstand. Attack –
//            Gegner in Sichtweite: dreht sich zu ihm und schießt, läuft dabei
//            aber weiter mit (seitwärts oder rückwärts).
//   Pirat:   Patrol – streift um seinen gelben Kristall (home) und wartet an
//            jedem Punkt kurz. Attack – Gegner in Sichtweite: hält 8 bis 14 m
//            Abstand, macht Seitwärtsschritte und schießt. Retreat – unter
//            GROUND_PIRATE_RETREAT des Schilds läuft er 6 s vom Gegner weg;
//            danach greift er wieder an oder flieht erneut, bis sich der
//            Schild aufgeladen hat.
//   Zombie:  ein Pirat mit Trooper::zombie (Einsatzkarte mit dem Thema
//            Friedhof). Streift wie ein Pirat, im Angriff läuft er langsam
//            geradewegs auf sein Ziel zu (ZOMBIE_CHASE) und schlägt aus der
//            Nähe zu (ZOMBIE_REACH, alle ZOMBIE_HIT_INTERVAL Sekunden). Er
//            schießt nicht und flieht nie.
// Ziel: alle 0,3 bis 0,5 s der nächste Gegner bis TROOPER_SIGHT (Kameraden:
// MATE_SIGHT_CELLS Rasterfelder der Karte) mit freier Sichtlinie
// (TrooperLineClear). Wer getroffen wird, nimmt den Schützen ins
// Visier (DamageTrooper) und behält ihn bis zur anderthalbfachen Sichtweite.
// Zielen: Die Blickrichtung dreht mit begrenztem Tempo zum Ziel (das wirkt
// wie eine Reaktionszeit); geschossen wird erst, wenn sie fast passt und die
// Sicht frei ist. Die Streuung kommt in FireTrooperBullet dazu.
// Hängt die Figur an einem Hindernis fest, springt sie und weicht eine
// Sekunde seitlich aus.
// ===========================================================================
void World::UpdateTrooperAi(Trooper& t, float dt) {
    TrooperInput& in = t.input;
    const bool pirate = t.kind == TrooperKind::Pirate;
    in.forward = in.strafe = 0.0f;
    in.fire = in.jump = in.sprint = in.reload = in.sample = false;

    // --- Ziel suchen ---------------------------------------------------------------
    t.thinkT -= dt;
    if (t.thinkT <= 0.0f) {
        t.thinkT = Rand(0.3f, 0.5f);
        const float sight = pirate ? cfg::TROOPER_SIGHT
                                   : cfg::MATE_SIGHT_CELLS * (instance ? mapgrid::MISSION_CELL : mapgrid::CELL);
        const Trooper* cur = t.targetId >= 0 ? FindTrooper(t.targetId) : nullptr;
        if (cur && (cur->down || Dist2D(cur->pos, t.pos) > sight * 1.5f)) cur = nullptr;
        int best = -1;
        float bestD = sight;
        for (const Trooper& o : troopers) {
            if (o.down || o.faction == t.faction) continue;
            const float d = Dist2D(o.pos, t.pos);
            if (d < bestD && TrooperLineClear(TrooperEye(t), TrooperEye(o))) {
                best = o.id;
                bestD = d;
            }
        }
        t.targetId = best >= 0 ? best : cur ? cur->id : -1;
        const Trooper* tg = t.targetId >= 0 ? FindTrooper(t.targetId) : nullptr;
        t.seesTarget = tg && TrooperLineClear(TrooperEye(t), TrooperEye(*tg));
    }
    const Trooper* target = t.targetId >= 0 ? FindTrooper(t.targetId) : nullptr;
    if (target && target->down) {
        target = nullptr;
        t.targetId = -1;
        t.seesTarget = false;
    }

    // --- Zustand ------------------------------------------------------------------
    if (pirate) {
        if (t.ai == TrooperAiState::Retreat) {
            t.waitT -= dt;
            if (t.waitT <= 0.0f) t.ai = TrooperAiState::Patrol;
        } else if (target && !t.zombie && t.hp < t.maxHp * cfg::GROUND_PIRATE_RETREAT) {
            t.ai = TrooperAiState::Retreat;
            t.waitT = 6.0f;
        } else if (target) {
            t.ai = TrooperAiState::Attack;
        } else if (t.ai == TrooperAiState::Attack) {
            t.ai = TrooperAiState::Patrol;
            t.goal = t.pos;
            t.waitT = Rand(1.0f, 2.0f);
        }
    } else {
        t.ai = target ? TrooperAiState::Attack : TrooperAiState::Follow;
    }

    // --- Laufrichtung (waagerecht, Länge 0..1) und Blickrichtung --------------------
    Vector3 move{0.0f, 0.0f, 0.0f};
    float faceYaw = in.yaw, facePitch = 0.0f;
    auto toward = [&](Vector3 p) {
        Vector3 d = Vector3Subtract(p, t.pos);
        d.y = 0.0f;
        const float l = Vector3Length(d);
        return l > 1e-3f ? Vector3Scale(d, 1.0f / l) : Vector3{0.0f, 0.0f, 0.0f};
    };
    if (!pirate) {
        if (const Trooper* leader = FindTrooper(t.leaderId); leader && !leader->down) {
            const Vector3 slot = MateSlot(*leader, t.slot);
            const float d = Dist2D(slot, t.pos);
            if (d > 0.5f) {
                move = Vector3Scale(toward(slot), std::min(1.0f, d / 1.5f));
                faceYaw = YawTowards(t.pos, slot);
                in.sprint = d > 6.0f && !target;
            } else {
                faceYaw = leader->yaw;
            }
        }
    } else if (t.ai == TrooperAiState::Patrol) {
        if (Dist2D(t.goal, t.pos) < 0.8f) {
            t.waitT -= dt;
            if (t.waitT <= 0.0f) {
                const float a = Rand(-PI, PI), r = Rand(3.0f, 12.0f);
                t.goal = {t.home.x + cosf(a) * r, 0.0f, t.home.z + sinf(a) * r};
                t.waitT = Rand(1.5f, 4.0f);
            }
        } else {
            move = Vector3Scale(toward(t.goal), 0.3f);
            faceYaw = YawTowards(t.pos, t.goal);
        }
    } else if (t.ai == TrooperAiState::Retreat) {
        const Vector3 away = target ? Vector3Negate(toward(target->pos)) : toward(t.home);
        move = away;
        faceYaw = YawTowards(t.pos, Vector3Add(t.pos, away));
        in.sprint = true;
    } else if (target && t.zombie) {
        const float d = Dist2D(target->pos, t.pos);
        if (d > cfg::ZOMBIE_REACH * 0.8f) move = Vector3Scale(toward(target->pos), cfg::ZOMBIE_CHASE);
        faceYaw = YawTowards(t.pos, target->pos);
        // Schlag: wie ein Treffer durch ein Geschoss des Zombies (DamageTrooper)
        if (d < cfg::ZOMBIE_REACH && t.fireCd <= 0.0f && fabsf(target->pos.y - t.pos.y) < 0.4f) {
            if (Trooper* victim = FindTrooper(t.targetId)) {
                Bullet hit;
                hit.damage = cfg::ZOMBIE_DAMAGE;
                hit.faction = t.faction;
                hit.ownerPlayer = -1;
                hit.ownerTrooper = t.id;
                hit.ground = true;
                DamageTrooper(*victim, hit);
            }
            t.fireCd = cfg::ZOMBIE_HIT_INTERVAL;
        }
    } else if (target) {
        const Vector3 dir = toward(target->pos);
        const float d = Dist2D(target->pos, t.pos);
        if (!t.seesTarget || d > 14.0f) move = dir;
        else if (d < 8.0f) move = Vector3Negate(dir);
        t.strafeT -= dt;
        if (t.strafeT <= 0.0f) {
            t.strafeT = Rand(1.2f, 2.5f);
            t.strafeDir = Rand(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
        }
        if (t.seesTarget) move = Vector3Add(move, Vector3Scale(Vector3{-dir.z, 0.0f, dir.x}, 0.7f * t.strafeDir));
    }
    // Im Kampf zum Ziel schauen (auf die Brust) und schießen, wenn die
    // Blickrichtung fast passt
    float aimYaw = faceYaw, aimPitch = 0.0f;
    const bool fighting = target && t.ai == TrooperAiState::Attack && !t.zombie;   // Zombies schießen nicht
    if (fighting) {
        const Vector3 eye = TrooperEye(t);
        const Vector3 chest{target->pos.x, target->pos.y + cfg::TROOPER_HEIGHT * 0.6f, target->pos.z};
        aimYaw = YawTowards(eye, chest);
        aimPitch = atan2f(chest.y - eye.y, std::max(0.1f, Dist2D(eye, chest)));
        faceYaw = aimYaw;
        facePitch = aimPitch;
    }
    const float turn = (pirate ? 3.5f : 5.0f) * dt;
    in.yaw += std::clamp(AngleDiff(in.yaw, faceYaw), -turn, turn);
    in.pitch += std::clamp(facePitch - in.pitch, -turn, turn);
    if (fighting && t.seesTarget && fabsf(AngleDiff(in.yaw, aimYaw)) < 0.12f && fabsf(aimPitch - in.pitch) < 0.15f) in.fire = true;

    // --- Festgelaufen? Springen und seitlich ausweichen ------------------------------
    const float speed = Vector2Length({t.vel.x, t.vel.z});
    if (Vector3Length(move) > 0.3f && speed < 0.6f && t.onGround) t.stuckT += dt;
    else t.stuckT = 0.0f;
    if (t.stuckT > 0.6f) {
        t.stuckT = 0.0f;
        t.detourT = 1.0f;
        t.strafeDir = -t.strafeDir;
        in.jump = true;
        if (t.ai == TrooperAiState::Patrol) t.goal = t.pos;   // Streifenpunkt unerreichbar: gleich einen neuen wählen
    }
    // Wunschrichtung in vor/zurück und seitlich relativ zur Blickrichtung
    const Vector3 fwd = ForwardFromYaw(in.yaw);
    const Vector3 right{-fwd.z, 0.0f, fwd.x};
    in.forward = Vector3DotProduct(move, fwd);
    in.strafe = Vector3DotProduct(move, right);
    if (t.detourT > 0.0f) {
        t.detourT -= dt;
        in.strafe = std::clamp(in.strafe + t.strafeDir, -1.0f, 1.0f);
    }
}

// ===========================================================================
// Autopilot (Simulationsmodus)
//
// Spielt das Spiel wie ein (recht vernünftiger) Mensch. Er liest die Welt
// und liefert ein ShipInput, so als würden Tasten gedrückt; was daraus wird,
// entscheiden die normalen Spielregeln.
// Benutzt im Hauptmenü (Hintergrundwelt), im Menüpunkt "Simulation" und mit
// --mp-autopilot im Mehrspieler (dort mit den Daten der Spiegelwelt, also
// nur mit dem, was dieser Spieler auch sehen kann).
// ===========================================================================
namespace {
// Gedächtnis des Autopiloten zwischen zwei Aufrufen. Eine einzige globale
// Variable reicht, denn es gibt immer nur ein eigenes Schiff. Beim Start
// einer neuen Welt muss ResetAutopilot() sie zurücksetzen.
struct AutopilotState {
    Vector3 explore{};        // aktuelles Erkundungsziel
    float exploreT = 0.0f;    // Restzeit, danach neues Erkundungsziel
    float interactCd = 0.0f;  // Sperrzeit für die Taste F (siehe unten)
    bool hasExplore = false;
} g_ap;

// Günstigstes Upgrade, das höchstens "budget" kostet (-1 = keins).
// Achtung: In src/Multiplayer.cpp steht eine Kopie dieser Funktion (dort mit
// dem Spieler der Spiegelwelt). Beide gleich halten.
int CheapestUpgrade(const World& w, long long budget) {
    // Reihenfolge bei gleichem Preis: Lager, Waffen, Panzerung, Antrieb
    // Das Lager zuerst, weil es am meisten bringt (mehr Punkte pro Fahrt).
    // Bei gleichem Preis gewinnt der erste der Liste ("<" statt "<=").
    const int order[4] = {cfg::UP_CARGO, cfg::UP_WEAPON, cfg::UP_ARMOR, cfg::UP_SPEED};
    int best = -1;
    int bestCost = 1 << 30;   // "unendlich teuer" als Startwert
    for (int t : order) {
        int c = w.UpgradeCost(t);   // -1 = schon höchste Stufe
        if (c >= 0 && c <= budget && c < bestCost) { best = t; bestCost = c; }
    }
    return best;
}
} // namespace

void ResetAutopilot() { g_ap = AutopilotState{}; }

// Aktionen, sobald der Autopilot angedockt hat (nur Einzelspieler: ruft
// World::Repair/BuyUpgrade direkt auf. Im Mehrspieler würde das nur die
// Spiegelwelt ändern; Multiplayer.cpp schickt stattdessen Anfragen an den
// Server). Abgedockt wird danach von src/main.cpp (nach 1,6 s).
// Handelsstation: reparieren (so weit das Geld reicht; Verkaufen passiert
// beim Andocken automatisch). Werft: so lange das günstigste Upgrade kaufen,
// wie danach noch 300 Punkte übrig bleiben – die Reserve für die nächste
// Reparatur (300 Punkte = 150 Schildpunkte). Höchstens 8 Käufe als
// Absicherung gegen eine Endlosschleife.
void AutopilotDockActions(World& w) {
    PlayerState& me = w.Me();
    if (me.dockedAt < 0) return;
    if (w.stations[me.dockedAt].type == StationType::Trade) {
        w.Repair();
    } else {
        for (int i = 0; i < 8; ++i) {
            int t = CheapestUpgrade(w, me.balance - 300);
            if (t < 0 || !w.BuyUpgrade(t)) break;
            // Reihenfolge wie cfg::Upgrade (UP_SPEED = 0 ... UP_CARGO = 3)
            static const char* names[4] = {"Antrieb", "Panzerung", "Bewaffnung", "Lager"};
            w.Notify(Fmt("Autopilot kauft: %s Stufe %d", names[t], me.upgrades[t]), Color{255, 210, 130, 255});
        }
    }
}

// Entscheidet in jedem Schritt, was zu tun ist. Die Regeln werden der Reihe
// nach geprüft; die erste, die zutrifft, bestimmt die Steuerung (Prioritäten):
//   1. Gegner in der Nähe: bei schwachem Schild fliehen, sonst kämpfen
//   2. Abbau läuft: abwarten
//   3. Schild unter 60 %: reparieren; Lager fast voll: verkaufen
//   4. genug Geld: aufrüsten
//   5. Wrack oder entdeckte Lagerstätte anfliegen und abbauen
//   6. sonst: herumfliegen und scannen
// Die Taste F (interact) schaltet um (Abbau starten/abbrechen, andocken).
// Darum gibt es die Sperrzeit interactCd: Sonst würde der Autopilot in
// jedem Schritt erneut "drücken" und den gerade begonnenen Abbau sofort
// wieder abbrechen.
ShipInput AutopilotInput(World& w, float dt) {
    ShipInput in;
    Ship& p = w.Player();
    PlayerState& me = w.Me();
    if (p.crashing || p.dead || p.hidden || me.dockedAt >= 0) return in;
    g_ap.interactCd -= dt;
    g_ap.exploreT -= dt;

    // Nur Schub und Drehen übernehmen; fire/interact setzt der Autopilot
    // selbst.
    auto go = [&](Vector3 target, float arrive, float throttle = 1.0f) {
        ShipInput s = SteerTo(p, target, arrive, throttle);
        in.thrust = s.thrust;
        in.turn = s.turn;
    };
    // Andocken an der nächsten Station des gewünschten Typs
    // F wird erst bei DOCK_RANGE - 3 (= 12) gedrückt, also deutlich innerhalb
    // der Andock-Reichweite, damit leichtes Weitertreiben nicht schadet.
    auto dock = [&](StationType type) {
        const int station = std::max(0, w.NearestStation(p.pos, type));
        go(w.stations[station].pos, 4.0f);
        if (w.NearStation(station, cfg::DOCK_RANGE - 3.0f) && g_ap.interactCd <= 0.0f) {
            in.interact = true;
            g_ap.interactCd = 1.0f;
        }
    };

    // --- Bedrohung? ----------------------------------------------------------
    // Nächstes feindliches Schiff innerhalb von 27 Einheiten: Piraten und
    // Händler, die von einem Spieler angegriffen wurden (hostileToPlayer).
    Ship* threat = nullptr;
    float threatD = 27.0f;
    for (auto& s : w.ships) {
        if (s.dead || s.crashing || s.hidden || s.kind == ShipKind::Player) continue;
        bool hostile = s.faction == Faction::Pirate || s.hostileToPlayer;
        if (!hostile) continue;
        float d = Dist2D(s.pos, p.pos);
        if (d < threatD) { threatD = d; threat = &s; }
    }
    const float shield = p.hp / p.maxHp;   // Schild in Prozent (0..1)
    if (threat) {
        // Schild unter 35 %: Abbau abbrechen und zur Handelsstation fliehen
        // (angedockt ist man unverwundbar und kann reparieren).
        // Hinweis: Hier und beim Kampf unten wird F ohne Sperrzeit gedrückt.
        // Im Einzelspieler ist das harmlos (me.mining ist im nächsten Schritt
        // schon false). Mit --mp-autopilot kommt me.mining aber erst mit dem
        // nächsten Snapshot vom Server, sodass F mehrmals hintereinander
        // gesendet werden kann.
        if (shield < 0.35f) {
            if (me.mining) { in.interact = true; return in; } // Abbau abbrechen
            dock(StationType::Trade);
            return in;
        }
        if (me.mining && threatD > 16.0f) {
            // weiter abbauen, aber Gegner im Blick behalten
        } else {
            // Kämpfen – wie Dogfight, nur ohne Abdrehen und mit stärkerem
            // Rückwärtsschub, wenn der Gegner zu nah kommt.
            if (me.mining) in.interact = true;
            Vector3 lead = LeadPoint(p, *threat);
            float desired = YawTowards(p.pos, lead);
            float diff = AngleDiff(p.yaw, desired);
            in.turn = Clamp(diff * 3.2f, -1.0f, 1.0f);
            in.thrust = threatD > 15.0f ? 0.9f : (threatD > 8.0f ? 0.2f : -0.5f);
            in.fire = fabsf(diff) < 0.22f && threatD < 30.0f;
            return in;
        }
    }

    if (me.mining) return in; // warten, bis das Lager gefüllt ist

    // --- Reparieren / Verkaufen ------------------------------------------------
    // Reparieren nur, wenn wenigstens ein bisschen Geld da ist (50 Punkte =
    // 25 Schildpunkte). Verkaufen schon bei 95 % Ladung: Der letzte Abbau
    // würde kaum noch etwas bringen.
    if (shield < 0.6f && me.balance >= 50) { dock(StationType::Trade); return in; }
    if (p.cargo >= p.cargoMax * 0.95f) { dock(StationType::Trade); return in; }

    // --- Aufrüsten ---------------------------------------------------------------
    // Nur, solange das Ziel noch nicht in Reichweite ist: Kurz vor 100.000
    // Punkten (weniger als 2.000 entfernt) lieber verkaufen und gewinnen, als
    // das Geld in Upgrades zu stecken.
    if (me.balance < cfg::GOAL_POINTS - 2000 && CheapestUpgrade(w, me.balance - 300) >= 0) {
        dock(StationType::Shipyard);
        return in;
    }

    // --- Nächstes Abbauziel ------------------------------------------------------
    // Wracks brauchen keinen Scanner (sie sind immer sichtbar) und werden
    // bis 90 Einheiten Entfernung berücksichtigt. Lagerstätten nur, wenn sie
    // für diesen Spieler entdeckt sind (d.revealed: im Einzelspieler und in
    // der Spiegelwelt des Clients gilt das für den eigenen Spieler).
    Vector3 target{};
    float best = 1e9f;
    bool found = false;
    for (auto& wr : w.wrecks) {
        float d = Dist2D(wr.pos, p.pos);
        if (wr.amount > 0 && d < 90.0f && d < best) { best = d; target = wr.pos; found = true; }
    }
    for (auto& d : w.deposits) {
        if (!d.revealed || d.amount <= 0 || d.sample == Sample::Yellow) continue;
        float dist = Dist2D(d.pos, p.pos) + 10.0f; // Wracks leicht bevorzugen
        if (dist < best) { best = dist; target = d.pos; found = true; }
    }
    if (found) {
        // Bis auf die halbe Abbau-Reichweite heranfliegen und ab 80 % der
        // Reichweite F drücken (sicher innerhalb von cfg::MINING_RANGE).
        go(target, cfg::MINING_RANGE * 0.5f, 1.0f);
        if (Dist2D(target, p.pos) < cfg::MINING_RANGE * 0.8f && g_ap.interactCd <= 0.0f) {
            in.interact = true;
            g_ap.interactCd = 0.8f;
        }
        return in;
    }

    // --- Erkunden und scannen ------------------------------------------------------
    // Nichts zu tun: einen zufälligen Punkt auf der Karte anfliegen (neuer
    // Punkt nach 25 s oder bei Ankunft) und den Scanner einschalten, sobald
    // er voll geladen ist. So läuft er 10 s, lädt 10 s nach, und so weiter.
    if (!g_ap.hasExplore || g_ap.exploreT <= 0.0f || Dist2D(p.pos, g_ap.explore) < 8.0f) {
        const float lim = cfg::WORLD_HALF - 15.0f;
        g_ap.explore = {w.Rand(-lim, lim), 0, w.Rand(-lim, lim)};
        g_ap.exploreT = 25.0f;
        g_ap.hasExplore = true;
    }
    go(g_ap.explore, 4.0f, 1.0f);
    if (!me.scanOn && me.scanEnergy >= cfg::SCANNER_MAX_ENERGY - 0.01f) in.scanToggle = true;
    return in;
}
