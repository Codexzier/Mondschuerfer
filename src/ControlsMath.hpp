#pragma once
// ---------------------------------------------------------------------------
// ControlsMath.hpp – die Rechnungen hinter der Maus- und Controller-Steuerung
// (reine Funktionen, ohne raylib-Funktionen).
//
// Wofür ist die Datei da?
//   PilotControls (src/Controls.cpp) liest Tastatur, Maus und Controller und
//   macht daraus die Steuerbefehle des eigenen Schiffs (ShipInput: Schub,
//   Drehen, Feuer ...). Die eigentlichen Rechnungen dafür stehen hier,
//   getrennt vom Abfragen der Geräte:
//     1. Totzone des Sticks                      StickDeadzone
//     2. Stick-Richtung im Bild -> Weltrichtung  CameraGroundAxes, StickToYaw,
//                                                ImageAngleDeg
//     3. Trigger-Werte "kalibrieren"             TriggerReader
//     4. mit dem Trigger ein Tempo halten        ThrustForSpeed
//     5. Mauszeiger -> Punkt auf dem Boden       RayToGround, ClampToMap
//     6. zu einem Punkt fliegen und anhalten     SteerToPoint, FlightTarget
//     7. PlayStation-Controller am Namen erkennen LooksLikePlayStation
//   Jede Funktion bekommt alle Werte als Parameter und liefert ein Ergebnis,
//   ohne Fenster, Geräte oder Zufall. Deshalb lassen sie sich mit einem
//   kleinen Testprogramm prüfen – sogar zusammen mit der echten Flugphysik
//   (World::UpdateShipPhysics), ohne dass Maus oder Controller da sein müssen.
//
// Wer benutzt sie?
//   src/Controls.cpp (Flugsteuerung) und src/Ui.cpp (Name des Controllers).
//
// Abhängigkeiten:
//   raylib.h nur für die Datentypen Vector2, Vector3 und Ray; raymath.h
//   (Vektor-Rechnung, komplett im Header); shared/Util.hpp (ForwardFromYaw,
//   YawTowards, AngleDiff, Dist2D). Alles läuft im Spiel-Thread.
//
// Koordinaten wie überall im Spiel: Der Boden ist die x/z-Ebene, y zeigt nach
// oben. yaw = 0 heißt "Nase nach -z"; ein größerer yaw dreht nach links.
// ShipInput::turn = +1 dreht nach links, -1 nach rechts.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include "Util.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace controls {

// ===========================================================================
// Zahlen, die zur Flugphysik passen müssen
// ===========================================================================
// Die drei Physik-Werte stehen in World::UpdateShipPhysics (shared/World.cpp)
// und werden hier nur nachgerechnet. Ändert sich dort etwas, hier anpassen.
constexpr float kAccelFactor = 1.9f;   // Beschleunigung bei vollem Schub = 1,9 * maxSpeed pro Sekunde
constexpr float kThrustDrag = 0.35f;   // Reibung pro Sekunde, solange Schub gegeben wird
constexpr float kCoastDrag = 1.4f;     // Reibung pro Sekunde ohne Schub (Ausrollen)

// Lenken wie der Autopilot (SteerTo in shared/Ai.cpp): Drehbefehl =
// Winkelfehler * 2,8, begrenzt auf -1..1. Bei 20° Fehler (0,35 rad) also
// knapp 1 = volle Drehung, darunter immer sanfter – so pendelt die Nase
// nicht über die Sollrichtung hinaus.
constexpr float kSteerGain = 2.8f;

// Ab diesem Abstand (in Welteinheiten, gemessen am Boden) gilt ein Mausziel
// als erreicht. 1,5 ist etwas weniger als der Ring unter dem Schiff (1,7).
constexpr float kArriveRadius = 1.5f;
// ... aber nur, wenn das Schiff dabei langsamer ist als 1,2 Einheiten/s
// Rauscht es schneller durch den Ankunftskreis, bleibt das Ziel bestehen:
// Das Schiff bremst, wendet und kommt zurück, statt weit hinter dem Ziel
// auszurollen.
constexpr float kArriveSpeed = 1.2f;
// Rückwärtsschub (Bremsen) wirkt mit 70 % der Kraft (World::UpdateShipPhysics).
constexpr float kBrakeFactor = 0.7f;
// Beim Anflug wird so gebremst, als läge das Ziel 0,8 Einheiten näher
// (siehe SteerToPoint). Das ist die Reserve gegen Überschießen.
constexpr float kStopShort = 0.8f;
// Kommt das Schiff so lange nicht näher an ein (losgelassenes) Mausziel,
// wird es aufgegeben (z. B. Ziel mitten in einer Station, wo das Schiff
// nicht hinkommt). Sekunden.
constexpr float kGiveUpTime = 4.0f;

// Totzonen: Ein Stick in Ruhelage steht nie genau auf 0, und ein
// losgelassener Trigger meldet oft noch ein paar Prozent.
constexpr float kStickDeadzone = 0.25f;
constexpr float kTriggerDeadzone = 0.06f;

// ===========================================================================
// 1. Totzone des Sticks
// ===========================================================================
// raw = Stickausschlag, x und y je -1..1 (raylib: x nach rechts positiv,
// y nach UNTEN positiv – Stick nach oben liefert also y = -1).
// "Kreisförmige" Totzone: Gemessen wird die Länge des Ausschlags, nicht x
// und y einzeln. Sonst wäre die Totzone ein Quadrat, und schräge Richtungen
// würden früher ansprechen als gerade.
// Außerhalb der Totzone wird neu skaliert: Länge dz..1 wird zu 0..1. So
// beginnt die Bewegung weich bei 0 statt mit einem Sprung auf 0,25.
// Die Richtung bleibt dabei erhalten (raw * neueLänge / alteLänge).
inline Vector2 StickDeadzone(Vector2 raw, float dz = kStickDeadzone) {
    const float len = Vector2Length(raw);
    if (len <= dz) return {0.0f, 0.0f};
    const float m = std::min(1.0f, (len - dz) / (1.0f - dz));
    return Vector2Scale(raw, m / len);
}

// ===========================================================================
// 2. Richtungen im Bild <-> Richtungen in der Welt
// ===========================================================================
// Die Kamera dreht sich nicht mit dem Schiff; sie schaut schräg von oben.
// "Im Bild nach oben" ist deshalb die Blickrichtung der Kamera, flach auf den
// Boden gelegt (y weggelassen), und "im Bild nach rechts" steht senkrecht
// dazu. Genau so rechnet auch der Randpfeil im HUD (ArrowDirection in
// src/Hud.cpp).
// Die Vektoren sind (x, z)-Paare auf dem Boden, Länge 1.
struct GroundAxes {
    Vector2 up;      // im Bild nach oben
    Vector2 right;   // im Bild nach rechts
};

// Rechts = oben um 90° im Uhrzeigersinn gedreht (von oben gesehen, mit z im
// Bild nach unten): (x, z) -> (-z, x). Beispiel Spielkamera: Sie steht bei
// +z und schaut nach -z, also oben = (0, -1) und rechts = (1, 0) = +x.
// Schaut die Kamera genau senkrecht nach unten, gibt es keine flache
// Blickrichtung; dann gilt -z als oben (wie auf der Minikarte).
inline GroundAxes CameraGroundAxes(Vector3 camPos, Vector3 camTarget) {
    Vector2 up{camTarget.x - camPos.x, camTarget.z - camPos.z};
    const float len = Vector2Length(up);
    up = len < 1e-4f ? Vector2{0.0f, -1.0f} : Vector2Scale(up, 1.0f / len);
    return {up, {-up.y, up.x}};
}

// Stick -> Sollwinkel (yaw) des Schiffs.
// Weltrichtung = rechts * stick.x + oben * (-stick.y): Stick nach rechts
// zeigt im Bild nach rechts, Stick nach oben (y = -1) im Bild nach oben.
// Aus der Richtung (dx, dz) wird der yaw wie in YawTowards
// (shared/Util.hpp): yaw = atan2(-dx, -dz).
inline float StickToYaw(Vector2 stick, const GroundAxes& ax) {
    const float dx = ax.right.x * stick.x - ax.up.x * stick.y;
    const float dz = ax.right.y * stick.x - ax.up.y * stick.y;
    return atan2f(-dx, -dz);
}

// Umgekehrt: In welche Richtung zeigt ein Schiff mit diesem yaw IM BILD?
// Ergebnis in Grad, 0 = oben, 90 = rechts, 180 = unten, 270 = links.
// Die Vorwärtsrichtung wird in ihre Anteile "nach rechts" und "nach oben"
// zerlegt (Skalarprodukte); atan2(rechts, oben) ist dann der Winkel, im
// Uhrzeigersinn von oben gemessen. Benutzt von der Testoption --pad-test.
inline float ImageAngleDeg(float yaw, const GroundAxes& ax) {
    const Vector3 f = ForwardFromYaw(yaw);
    const float r = f.x * ax.right.x + f.z * ax.right.y;
    const float u = f.x * ax.up.x + f.z * ax.up.y;
    const float deg = atan2f(r, u) * RAD2DEG;
    return deg < 0.0f ? deg + 360.0f : deg;
}

// Stickausschlag für einen Bildwinkel (0 = oben, 90 = rechts), voll
// ausgelenkt. Gegenstück zu ImageAngleDeg; für die Testoption --pad-test.
inline Vector2 StickFromImageAngle(float deg) {
    const float a = deg * DEG2RAD;
    return {sinf(a), -cosf(a)};
}

// ===========================================================================
// 3. Trigger
// ===========================================================================
// raylib meldet einen Trigger (RT/LT, PlayStation R2/L2) als Achse von -1
// (losgelassen) bis +1 (ganz durchgedrückt). Manche Treiber melden aber,
// solange der Trigger noch nie bewegt wurde, 0 statt -1 – das wäre sonst
// "halb gedrückt" und das Schiff führe von allein los.
// Deshalb merkt sich TriggerReader, ob schon einmal ein Wert unter -0,5
// kam ("kalibriert"):
//   - kalibriert:   Wert = (raw + 1) / 2, also -1..1 -> 0..1
//   - noch nicht:   Wert = max(0, raw); die anfängliche 0 bedeutet dann
//                   "losgelassen", und Treiber, die gleich 0..1 liefern,
//                   funktionieren auch.
// Danach eine kleine Totzone (6 %) mit Neuskalierung wie beim Stick.
// Gehört zu genau einem Trigger eines Controllers; wird ein anderer
// Controller benutzt, setzt PilotControls ihn mit = TriggerReader{} zurück.
struct TriggerReader {
    bool calibrated = false;
    float Value(float raw) {
        if (raw < -0.5f) calibrated = true;
        const float v = calibrated ? (raw + 1.0f) * 0.5f : std::max(0.0f, raw);
        if (v <= kTriggerDeadzone) return 0.0f;
        return std::min(1.0f, (v - kTriggerDeadzone) / (1.0f - kTriggerDeadzone));
    }
};

// ===========================================================================
// 4. Tempo halten (rechter Trigger)
// ===========================================================================
// Der Trigger gibt das gewünschte TEMPO vor (halb gedrückt = halbe
// Höchstgeschwindigkeit). Die Physik kennt aber nur Schub (Beschleunigung).
// Diese Funktion rechnet aus, wie viel Schub (0..1) nötig ist, damit das
// Schiff das Tempo "wanted" erreicht und hält:
//   - Halteschub: Mit Schub bremst die Reibung mit 0,35 * Tempo pro Sekunde;
//     der Schub beschleunigt mit 1,9 * maxSpeed * Schub. Beides gleich groß
//     ergibt Schub = 0,35 * wanted / (1,9 * maxSpeed) – damit bleibt das
//     Tempo genau stehen.
//   - Korrektur: Für jedes Achtel von maxSpeed, das noch fehlt, ein ganzer
//     Schub mehr (8 * Fehler / maxSpeed). Ist das Schiff zu schnell, wird
//     der Wert negativ und auf 0 begrenzt: Ohne Schub bremst die stärkere
//     Reibung (1,4 pro Sekunde) das Schiff ab.
// vFwd = Tempo in Flugrichtung (Skalarprodukt von Geschwindigkeit und Nase).
// max(1, ...) schützt vor einer Division durch 0.
inline float ThrustForSpeed(float wanted, float vFwd, float maxSpeed) {
    if (wanted <= 0.0f) return 0.0f;
    const float ms = std::max(1.0f, maxSpeed);
    const float hold = kThrustDrag * wanted / (kAccelFactor * ms);
    return Clamp(hold + 8.0f * (wanted - vFwd) / ms, 0.0f, 1.0f);
}

// ===========================================================================
// 5. Mauszeiger -> Punkt auf dem Boden
// ===========================================================================
// raylib liefert zu einem Bildschirmpunkt einen "Strahl" (GetScreenToWorldRay):
// Er beginnt an der Kamera (position) und läuft durch den Bildpunkt in die
// Szene (direction, Länge 1). Gesucht ist der Punkt, an dem er den Boden
// y = 0 trifft. Punkte auf dem Strahl: position + t * direction. Für y = 0
// gilt position.y + t * direction.y = 0, also t = -position.y / direction.y.
// Zeigt der Strahl nicht nach unten (direction.y >= 0) oder läge der Punkt
// hinter der Kamera (t <= 0), gibt es keinen Schnitt -> false.
// Warum y = 0 und nicht die Flughöhe? Dort liegen der Ring unter dem Schiff,
// Lagerstätten und Stationen; man klickt "auf den Boden", und das Schiff
// fliegt in seiner Höhe über diesen Punkt.
inline bool RayToGround(Ray ray, Vector3* out) {
    if (ray.direction.y > -1e-4f) return false;
    const float t = -ray.position.y / ray.direction.y;
    if (t <= 0.0f) return false;
    *out = {ray.position.x + ray.direction.x * t, 0.0f, ray.position.z + ray.direction.z * t};
    return true;
}

// Ziel ins Spielfeld holen: Am Rand (half = cfg::WORLD_HALF) hält die Physik
// das Schiff an; ein Ziel dahinter wäre nie erreichbar. margin = Abstand
// zum Rand.
inline Vector3 ClampToMap(Vector3 p, float half, float margin) {
    const float lim = half - margin;
    return {Clamp(p.x, -lim, lim), 0.0f, Clamp(p.z, -lim, lim)};
}

// ===========================================================================
// 6. Zu einem Punkt fliegen und dort anhalten (Maussteuerung)
// ===========================================================================
// Ergebnis eines Lenkschritts: Schub und Drehen wie in ShipInput.
// arrived = das Ziel ist erreicht (näher als kArriveRadius und langsamer als
// kArriveSpeed); dann sind Schub und Drehen 0 – das Schiff rollt aus.
struct SteerCommand {
    float thrust = 0.0f;
    float turn = 0.0f;
    bool arrived = false;
};

// Lenken: Sollwinkel zum Ziel, Drehbefehl = Winkelfehler * 2,8 (wie der
// Autopilot, SteerTo in shared/Ai.cpp).
//
// Bremsen, damit das Schiff AM Ziel stehen bleibt. Zwei Wege:
//   - Ausrollen: Ohne Schub nimmt das Tempo pro Sekunde um 1,4 * Tempo ab.
//     Ein Schiff mit Tempo v kommt so noch v / 1,4 Einheiten weit (Summe
//     einer abklingenden Bewegung: v * (1 + (1-1,4dt) + (1-1,4dt)^2 + ...)
//     * dt = v / 1,4). Bei Restweg r darf es also höchstens 1,4 * r schnell sein.
//   - Aktiv bremsen mit Rückwärtsschub: Verzögerung a = 0,7 * 1,9 *
//     maxSpeed. Aus der Bewegungsgleichung (Bremsweg = v² / (2a)) folgt das
//     höchste Tempo, bei dem das Schiff auf dem Restweg r noch anhalten
//     kann: v = Wurzel(2 * a * r). Davon nur 80 % als Sicherheitsreserve.
// Das Wunschtempo ist der größere der beiden Werte, höchstens maxSpeed:
//     wanted = min(maxSpeed, max(1,4 * r, 0,8 * Wurzel(2 * a * r))),  r = d - 0,8
// (0,8 = Reserve, kStopShort). Weit weg ist das die Höchstgeschwindigkeit,
// in der Nähe sinkt es auf 0. ThrustForSpeed regelt darauf. Ist das Schiff
// trotzdem zu schnell – z. B. weil der Spieler ein Ziel kurz VOR das
// fliegende Schiff klickt –, wird mit Rückwärtsschub gebremst. Ebenso, wenn
// das Ziel nicht voraus liegt: Dann soll das Schiff erst langsam werden und
// wenden, statt in einem weiten Bogen vorbeizuschießen.
//
// Schub nur, wenn das Ziel ungefähr voraus liegt: weit weg bis 1,2 rad
// (knapp 70°) Abweichung – das Schiff fliegt dann eine Kurve –, auf den
// letzten 8 Einheiten nur bis 0,5 rad. Sonst könnte das Schiff in der Nähe
// des Ziels im Kreis darum herumfliegen. Liegt das Ziel hinter dem Schiff,
// dreht es also erst (und rollt dabei aus), dann fliegt es los.
inline SteerCommand SteerToPoint(Vector3 pos, Vector3 vel, float yaw, float maxSpeed, Vector3 target) {
    SteerCommand c;
    const float d = Dist2D(pos, target);
    const float speed = sqrtf(vel.x * vel.x + vel.z * vel.z);
    if (d < kArriveRadius && speed < kArriveSpeed) {
        c.arrived = true;
        return c;
    }
    const float diff = AngleDiff(yaw, YawTowards(pos, target));
    c.turn = Clamp(diff * kSteerGain, -1.0f, 1.0f);
    const Vector3 fwd = ForwardFromYaw(yaw);
    const float vFwd = vel.x * fwd.x + vel.z * fwd.z;
    const float ms = std::max(1.0f, maxSpeed);
    const float rest = std::max(0.0f, d - kStopShort);
    const float brakeAccel = kBrakeFactor * kAccelFactor * ms;
    const float wanted = std::min(ms, std::max(kCoastDrag * rest, 0.8f * sqrtf(2.0f * brakeAccel * rest)));
    const float maxAngle = d > 8.0f ? 1.2f : 0.5f;
    const bool ahead = fabsf(diff) < maxAngle;
    if (ahead) c.thrust = ThrustForSpeed(wanted, vFwd, ms);
    // Zu schnell für den Restweg (oder das Ziel liegt nicht voraus): bremsen.
    // Je größer der Überschuss, desto kräftiger; ab einem Viertel des
    // Höchsttempos Überschuss mit voller Kraft. 0,5 Einheiten/s Spielraum,
    // damit kleine Schwankungen nicht ständig zwischen Gas und Bremse wechseln.
    const float limit = ahead ? wanted : 0.0f;
    if (vFwd > limit + 0.5f) c.thrust = -Clamp((vFwd - limit) / (0.25f * ms), 0.0f, 1.0f);
    return c;
}

// Wie ein Lenkschritt mit einem Mausziel endete (für FlightTarget::Step).
enum class TargetEnd { None, Arrived, GaveUp };

// Ein Mausziel samt "Wächter", der unerreichbare Ziele aufgibt.
//   active   gibt es gerade ein Ziel?
//   point    das Ziel am Boden (y = 0)
//   bestDist kleinster Abstand, der seit dem Setzen erreicht wurde
//   sinceBest Sekunden, seit der Abstand zuletzt um mindestens 0,3 kleiner wurde
// Wächter: Kommt das Schiff kGiveUpTime (4 s) lang nicht näher, steckt es
// fest (Ziel in einem Stationsteil oder dahinter) – dann wird das Ziel
// gelöscht, statt ewig gegen die Wand zu schieben. 4 s reichen, damit ein
// Schiff, das erst wenden muss (und sich dabei kurz entfernt), nicht
// fälschlich aufgibt.
// Solange die Maustaste gehalten wird (holding), folgt das Ziel dem Zeiger:
// Dann gilt weder "angekommen" (das Ziel bleibt, das Schiff wartet dort)
// noch der Wächter, und der Wächter beginnt nach dem Loslassen frisch.
struct FlightTarget {
    bool active = false;
    Vector3 point{};
    float bestDist = 0.0f;
    float sinceBest = 0.0f;

    void Set(Vector3 p) {
        active = true;
        point = p;
        bestDist = 1e9f;
        sinceBest = 0.0f;
    }
    void Clear() { active = false; }

    // Ein Schritt (einmal pro Frame, dt in Sekunden). Liefert Schub und
    // Drehen; *end sagt, ob das Ziel dabei erreicht oder aufgegeben wurde
    // (dann ist active danach false und der Befehl 0).
    SteerCommand Step(Vector3 pos, Vector3 vel, float yaw, float maxSpeed, float dt, bool holding, TargetEnd* end) {
        *end = TargetEnd::None;
        if (!active) return {};
        SteerCommand c = SteerToPoint(pos, vel, yaw, maxSpeed, point);
        const float d = Dist2D(pos, point);
        if (holding) {
            bestDist = d;
            sinceBest = 0.0f;
            return c;
        }
        if (c.arrived) {
            Clear();
            *end = TargetEnd::Arrived;
            return c;
        }
        if (d < bestDist - 0.3f) {
            bestDist = d;
            sinceBest = 0.0f;
        } else {
            sinceBest += dt;
            if (sinceBest > kGiveUpTime) {
                Clear();
                *end = TargetEnd::GaveUp;
                return {};
            }
        }
        return c;
    }
};

// ===========================================================================
// 7. PlayStation-Controller erkennen
// ===========================================================================
// raylib kennt nur die LAGE der Knöpfe (RIGHT_FACE_DOWN = unterer Knopf
// rechts), nicht ihre Beschriftung. Für die Hilfetexte ("[Y]" oder
// "[Dreieck]") wird der Name des Controllers (GetGamepadName) angesehen.
// Übliche Namen: "PS4 Controller", "PS5 Controller", "Sony Interactive
// Entertainment DualSense Wireless Controller", "Sony PLAYSTATION(R)3
// Controller" und unter Linux ohne Zuordnung schlicht "Wireless Controller"
// (so meldet sich ein DualShock 4). Achtung: "Xbox Wireless Controller"
// enthält auch "Wireless Controller" – deshalb zählt das nur als GANZER Name.
// Verglichen wird ohne Rücksicht auf Groß/Klein. "PS" zählt nur als eigenes
// Wort (PS, PS3, PS4, PS5), damit z. B. "GPSystem" nicht als PlayStation gilt.
inline bool LooksLikePlayStation(const char* name) {
    if (!name) return false;
    std::string s;
    for (const char* p = name; *p; ++p) s += static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    if (s == "wireless controller") return true;
    for (const char* key : {"playstation", "dualshock", "dualsense", "sony"})
        if (s.find(key) != std::string::npos) return true;
    // Wörter = Folgen aus Buchstaben und Ziffern
    std::string word;
    for (size_t i = 0; i <= s.size(); ++i) {
        const bool alnum = i < s.size() && isalnum(static_cast<unsigned char>(s[i]));
        if (alnum) {
            word += s[i];
            continue;
        }
        if (word == "ps" || word == "ps2" || word == "ps3" || word == "ps4" || word == "ps5") return true;
        word.clear();
    }
    return false;
}

} // namespace controls
