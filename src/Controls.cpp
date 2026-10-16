// ---------------------------------------------------------------------------
// Controls.cpp – Flugsteuerung mit Tastatur, Maus und Controller sowie die
// Tastennamen für Hilfetexte (Schnittstelle und Überblick: Controls.hpp).
//
// Wer benutzt die Datei? main.cpp (Einzelspieler), Multiplayer.cpp
// (Mehrspieler), Hud.cpp und Options.cpp (Tastennamen).
//
// Abhängigkeiten: ControlsMath.hpp (alle Rechnungen), Ui.hpp (Flug-/
// Menümodus, Controller-Nummer, UI-Flächen, Farbe ACCENT), raylib (Geräte
// abfragen, GetScreenToWorldRay, Zeichnen), rlgl (eigene Dreiecke für die
// Zielmarkierung, wie die Bodenringe in Render.cpp).
//
// Ablauf von PilotControls::Read (ein Frame im freien Flug):
//   1. Tastatur – genau wie früher (damals ReadPlayerInput in main.cpp und
//      ReadHeldInput in Multiplayer.cpp, jetzt nur noch hier).
//   2. Controller – Stick-Richtung, Trigger-Tempo, Knöpfe A/B/Y, Zoom.
//   3. Maus – Ziel setzen (Klick), dem Zeiger folgen (halten), löschen.
//      Hat in 1. oder 2. jemand gelenkt, wird das Mausziel gelöscht.
//   4. Gibt es ein Mausziel, lenken Schub und Drehen dorthin (FlightTarget).
// Alles läuft im Spiel-Thread.
// ---------------------------------------------------------------------------
#include "Controls.hpp"
#include "Ui.hpp"
#include "raylib.h"
#include "rlgl.h"
#include <cmath>

using namespace controls;

namespace {

// Rechter Stick: Zoom-Tempo bei vollem Ausschlag, in Mausrad-Rasten pro
// Sekunde. 9 Rasten * 0,08 (Zoomschritt in main.cpp) = 0,72 pro Sekunde:
// Der ganze Zoombereich (0,55 .. 1,6) ist in knapp 1,5 s durchfahren.
constexpr float kZoomRastersPerSecond = 9.0f;

// Flacher Ring auf dem Boden aus Dreiecken (wie DrawGroundRing in
// Render.cpp; dort steht die ausführliche Erklärung). y = 0.15: knapp über
// den Bodenringen der Schiffe (0.12), damit nichts flackert.
void TargetRing(Vector3 c, float rIn, float rOut, Color col, int segments) {
    rlBegin(RL_TRIANGLES);
    rlColor4ub(col.r, col.g, col.b, col.a);
    for (int i = 0; i < segments; ++i) {
        const float a0 = 2 * PI * i / segments;
        const float a1 = 2 * PI * (i + 1) / segments;
        const float y = 0.15f;
        rlVertex3f(c.x + cosf(a0) * rIn, y, c.z + sinf(a0) * rIn);
        rlVertex3f(c.x + cosf(a0) * rOut, y, c.z + sinf(a0) * rOut);
        rlVertex3f(c.x + cosf(a1) * rOut, y, c.z + sinf(a1) * rOut);
        rlVertex3f(c.x + cosf(a0) * rIn, y, c.z + sinf(a0) * rIn);
        rlVertex3f(c.x + cosf(a1) * rOut, y, c.z + sinf(a1) * rOut);
        rlVertex3f(c.x + cosf(a1) * rIn, y, c.z + sinf(a1) * rIn);
    }
    rlEnd();
}

// Ein Strich auf dem Boden von Radius r0 bis r1 in Richtung angle (rad),
// width breit: ein Viereck aus zwei Dreiecken. side = Richtung um 90°
// gedreht, damit der Strich seitlich Breite bekommt.
void TargetTick(Vector3 c, float angle, float r0, float r1, float width, Color col) {
    const Vector2 d{cosf(angle), sinf(angle)};
    const Vector2 side{-d.y * width * 0.5f, d.x * width * 0.5f};
    const float y = 0.15f;
    const Vector2 a{c.x + d.x * r0, c.z + d.y * r0}, b{c.x + d.x * r1, c.z + d.y * r1};
    rlBegin(RL_TRIANGLES);
    rlColor4ub(col.r, col.g, col.b, col.a);
    rlVertex3f(a.x - side.x, y, a.y - side.y);
    rlVertex3f(b.x - side.x, y, b.y - side.y);
    rlVertex3f(b.x + side.x, y, b.y + side.y);
    rlVertex3f(a.x - side.x, y, a.y - side.y);
    rlVertex3f(b.x + side.x, y, b.y + side.y);
    rlVertex3f(a.x + side.x, y, a.y + side.y);
    rlEnd();
}

} // namespace

// ===========================================================================
// PilotControls
// ===========================================================================
// ESC und P wie früher; Menü-Taste = GAMEPAD_BUTTON_MIDDLE_RIGHT (die rechte
// der mittleren Tasten: Xbox "Menü" mit drei Strichen, PlayStation "Options").
bool PilotControls::PausePressed() {
    const int pad = ui::Gamepad();
    return IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_P) ||
           (pad >= 0 && IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_MIDDLE_RIGHT));
}

void PilotControls::Reset() {
    if (target_.active) lastEnd_ = "zurückgesetzt";
    target_.Clear();
    holding_ = false;
}

void PilotControls::SetTestTarget(Vector3 t) { target_.Set(ClampToMap(t, cfg::WORLD_HALF, 1.0f)); }

void PilotControls::SetTestPad(float angleDeg, float trigger) {
    testPad_ = true;
    testAngle_ = angleDeg;
    testTrigger_ = Clamp(trigger, 0.0f, 1.0f);
}

FlightInput PilotControls::Read(const Ship& ship, const Camera3D& cam, float dt) {
    ui::SetFlightMode();
    // Beginnt der Flug gerade? (Im vorigen Frame wurde nicht gelesen: Pause,
    // Stationsfenster, Menü ...) Wichtig für die "Sperre" von Knopf A unten.
    const unsigned frame = ui::FrameIndex();
    const bool entering = frame != lastReadFrame_ + 1;
    lastReadFrame_ = frame;

    FlightInput out;
    ShipInput& in = out.held;

    // --- 1. Tastatur ------------------------------------------------------------
    // Genau wie früher (Vorzeichen wie in ShipInput: thrust +1 = vorwärts,
    // turn +1 = links). raylib unterscheidet zwei Abfragen:
    //   IsKeyDown    true, solange die Taste gehalten wird  -> Schub, Drehen, Feuer
    //   IsKeyPressed true nur in dem einen Frame, in dem sie heruntergeht
    //                -> Scanner und Interagieren sollen pro Druck genau einmal passieren.
    const bool kUp = IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
    const bool kDown = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
    const bool kLeft = IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT);
    const bool kRight = IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT);
    if (kUp) in.thrust += 1.0f;
    if (kDown) in.thrust -= 1.0f;
    if (kLeft) in.turn += 1.0f;
    if (kRight) in.turn -= 1.0f;
    in.fire = IsKeyDown(KEY_SPACE);
    out.scan = IsKeyPressed(KEY_E);
    out.interact = IsKeyPressed(KEY_F);
    const bool keySteer = kUp || kDown || kLeft || kRight;

    // --- 2. Controller ------------------------------------------------------------
    // Wird ein anderer Controller benutzt als bisher, beginnen die
    // Trigger-Kalibrierungen von vorn (ControlsMath.hpp, TriggerReader).
    const int pad = ui::Gamepad();
    if (pad != triggerPad_) {
        rt_ = TriggerReader{};
        lt_ = TriggerReader{};
        triggerPad_ = pad;
    }
    Vector2 stick{0.0f, 0.0f};
    float rt = 0.0f, lt = 0.0f, zoomStick = 0.0f;
    bool aDown = false, bPressed = false, yPressed = false;
    if (testPad_) {
        stick = StickFromImageAngle(testAngle_);
        rt = testTrigger_;
    } else if (pad >= 0) {
        stick = StickDeadzone({GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X), GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y)});
        rt = rt_.Value(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_TRIGGER));
        lt = lt_.Value(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_TRIGGER));
        zoomStick = StickDeadzone({0.0f, GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y)}).y;
        // Lage der Knöpfe (siehe Ui.hpp): unten = A/Kreuz, rechts = B/Kreis, oben = Y/Dreieck
        aDown = IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        bPressed = IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        yPressed = IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_RIGHT_FACE_UP);
    }
    bool padSteer = false;
    // Linker Stick = Flugrichtung im Bild. Das Schiff dreht sich auf die
    // Sollrichtung wie der Autopilot (Winkelfehler * 2,8). Stick in Ruhe:
    // kein Drehbefehl, die Richtung bleibt.
    if (stick.x != 0.0f || stick.y != 0.0f) {
        const float desired = StickToYaw(stick, CameraGroundAxes(cam.position, cam.target));
        in.turn = Clamp(AngleDiff(ship.yaw, desired) * kSteerGain, -1.0f, 1.0f);
        padSteer = true;
    }
    // Linker Trigger bremst (Rückwärtsschub wie Taste S, stufenlos) und hat
    // Vorrang. Rechter Trigger = Wunschtempo: ganz gedrückt = Vollschub wie
    // Taste W, sonst hält ThrustForSpeed das Tempo "Trigger * Höchsttempo".
    if (lt > 0.0f) {
        in.thrust = -lt;
        padSteer = true;
    } else if (rt > 0.0f) {
        const Vector3 fwd = ForwardFromYaw(ship.yaw);
        const float vFwd = ship.vel.x * fwd.x + ship.vel.z * fwd.z;
        in.thrust = rt >= 0.98f ? 1.0f : ThrustForSpeed(rt * ship.maxSpeed, vFwd, ship.maxSpeed);
        padSteer = true;
    }
    // Knopf A schießt, solange er gehalten wird – aber nicht, wenn er schon
    // beim Flugbeginn gedrückt war: Dann war es der Klick auf "Weiterspielen"
    // oder "Abdocken" (im Menü klickt A). Erst nach dem Loslassen gilt A
    // wieder als Feuerknopf ("Sperre", englisch latch).
    // Dasselbe gilt für die linke Maustaste: Wer beim Öffnen der Pause noch
    // das Schiff "an der Leine" hielt, hat sie dort längst losgelassen – und
    // drückt sie wieder für "Weiterspielen". Ohne dieses Vergessen würde
    // holding_ weiterleben, und das Schiff flöge zum Bodenpunkt unter dem
    // Knopf. Ein Halten beginnt also nach jedem Flugbeginn neu (siehe unten:
    // nur mit IsMouseButtonPressed im Flug).
    if (entering) {
        fireLatch_ = aDown;
        holding_ = false;
    }
    if (!aDown) fireLatch_ = false;
    if (aDown && !fireLatch_) in.fire = true;
    if (bPressed) out.scan = true;
    if (yPressed) out.interact = true;
    // Rechter Stick nach oben (y negativ) = näher heran, wie Mausrad nach vorn.
    out.zoom = -zoomStick * kZoomRastersPerSecond * dt;

    // --- 3. Maus -------------------------------------------------------------------
    const bool canFly = !ship.dead && !ship.crashing && !ship.hidden;
    if (keySteer || padSteer || !canFly) {
        // Jemand lenkt selbst (oder es gibt kein Schiff): Mausziel weg.
        if (target_.active) lastEnd_ = canFly ? "abgebrochen" : "zurückgesetzt";
        target_.Clear();
        holding_ = false;
    } else {
        const Vector2 m = GetMousePosition();
        // Ein Halten beginnt nur mit einem Druck IN DIESEM Frame des Flugs und
        // nicht über einem HUD-Fenster. Ein Druck, der im Menü begann
        // ("Weiterspielen" in der Pause), ist hier schon "gehalten", aber
        // nicht mehr "gedrückt" – er setzt also kein Flugziel.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ui::PointerOverPanel(m)) holding_ = true;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) holding_ = false;
        // Solange gehalten: Ziel = Bodenpunkt unter dem Zeiger (folgen).
        // Nach dem Loslassen bleibt der letzte Punkt das Ziel – ein Klick ist
        // also einfach ein kurzes Halten.
        Vector3 ground;
        if (holding_ && RayToGround(GetScreenToWorldRay(m, cam), &ground))
            target_.Set(ClampToMap(ground, cfg::WORLD_HALF, 1.0f));
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && target_.active) {
            target_.Clear();
            holding_ = false;
            lastEnd_ = "abgebrochen";
        }
    }

    // --- 4. Zum Mausziel lenken -------------------------------------------------------
    if (target_.active) {
        TargetEnd end = TargetEnd::None;
        const SteerCommand c = target_.Step(ship.pos, ship.vel, ship.yaw, ship.maxSpeed, dt, holding_, &end);
        in.thrust = c.thrust;
        in.turn = c.turn;
        if (end == TargetEnd::Arrived) lastEnd_ = "angekommen";
        else if (end == TargetEnd::GaveUp) lastEnd_ = "aufgegeben";
    }
    return out;
}

// Zielmarkierung. Die Zeichen-Einstellungen wie im Effekt-Teil von
// World::Draw (Render.cpp): Render-Batch leeren, Rückseiten zeichnen (flache
// Formen sollen von beiden Seiten sichtbar sein), nichts in den Tiefenpuffer
// schreiben (durchsichtig) – und am Ende alles wieder zurückstellen.
// pulse läuft mit sin(6 * Zeit) zwischen 0 und 1 hin und her (knapp einmal
// pro Sekunde): Der Ring atmet, sein Außenrand wandert zwischen Radius 1,4
// und 1,75. Damit ist er etwas kleiner als der Ring unter dem eigenen Schiff
// (1,7 bis 1,95, Render.cpp): Steht das Schiff am Ziel, liegt die Markierung
// innen in seinem Ring, und nur die vier Striche ragen darüber hinaus.
void PilotControls::DrawTarget3D(float time) const {
    if (!target_.active) return;
    const Vector3 c = target_.point;
    const float pulse = 0.5f + 0.5f * sinf(time * 6.0f);
    const Color accent = ui::ACCENT;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    const float r = 1.4f + 0.35f * pulse;
    TargetRing(c, r - 0.25f, r, Color{accent.r, accent.g, accent.b, 230}, 40);
    // Vier Striche wie ein Fadenkreuz, außen um den Ring (bei 0°, 90°, 180°, 270°)
    for (int i = 0; i < 4; ++i)
        TargetTick(c, i * PI * 0.5f, r + 0.3f, r + 1.1f, 0.22f, Color{accent.r, accent.g, accent.b, 210});
    // Leuchtende Säule (additiv gemischt wie die Lichtsäulen der
    // Lagerstätten): auch aus der Ferne und hinter Felsen gut zu finden.
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCylinder({c.x, 0.2f, c.z}, 0.05f, 0.14f, 3.0f, 8, Color{accent.r, accent.g, accent.b, static_cast<unsigned char>(70 + 50 * pulse)});
    EndBlendMode();
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

// ===========================================================================
// Tastennamen und Hilfetexte
// ===========================================================================
namespace controls {

Device ActiveDevice() {
    if (!ui::UsingGamepad()) return Device::KeyboardMouse;
    return ui::GamepadIsPlayStation() ? Device::PlayStation : Device::Xbox;
}

// Eine Zeile pro Aktion (Reihenfolge wie enum Act), eine Spalte pro Gerät
// (Reihenfolge wie enum Device). Die Belegung muss zu den Abfragen oben
// (Read, PausePressed) und in Ui.cpp (BackPressed, ButtonList) passen.
const char* KeyName(Act a) {
    static const char* const kNames[][3] = {
        // Tastatur/Maus, Xbox,  PlayStation
        {"Leertaste", "A", "Kreuz"},    // Fire
        {"E", "B", "Kreis"},            // Scan
        {"F", "Y", "Dreieck"},          // Interact
        {"ESC", "Menü", "Options"},     // Pause
        {"Enter", "A", "Kreuz"},        // Confirm
        {"ESC", "B", "Kreis"},          // Back
    };
    return kNames[static_cast<int>(a)][static_cast<int>(ActiveDevice())];
}

std::string KeyHint(Act a) { return std::string("[") + KeyName(a) + "]"; }

// Tastenhilfe. Tastatur: 9 Zeilen (mit Maus und Ton), Controller: 7 Zeilen.
// Die Taste M (Ton) gibt es nur auf der Tastatur.
std::vector<HelpRow> HelpRows() { return HelpRows(ActiveDevice()); }

size_t MaxHelpRows() {
    size_t n = 0;
    for (Device d : {Device::KeyboardMouse, Device::Xbox, Device::PlayStation}) n = std::max(n, HelpRows(d).size());
    return n;
}

std::vector<HelpRow> HelpRows(Device d) {
    switch (d) {
        case Device::Xbox:
            return {{"Linker Stick", "Flugrichtung"}, {"RT / LT", "Tempo / Bremsen"}, {"A", "Schießen"},
                    {"B", "Scanner an/aus"},         {"Y", "Abbauen / Andocken"},  {"Rechter Stick", "Zoom"},
                    {"Menü-Taste", "Pause"}};
        case Device::PlayStation:
            return {{"Linker Stick", "Flugrichtung"}, {"R2 / L2", "Tempo / Bremsen"}, {"Kreuz", "Schießen"},
                    {"Kreis", "Scanner an/aus"},      {"Dreieck", "Abbauen / Andocken"}, {"Rechter Stick", "Zoom"},
                    {"Options", "Pause"}};
        case Device::KeyboardMouse:
        default:
            return {{"W / S", "Schub / Bremsen"},  {"A / D", "Drehen"},  {"Linksklick", "Hinfliegen (halten: folgen)"},
                    {"Leertaste", "Schießen"},       {"E", "Scanner an/aus"}, {"F", "Abbauen / Andocken"},
                    {"Mausrad", "Zoom"},             {"ESC", "Pause"},  {"M", "Ton an/aus"}};
    }
}

} // namespace controls
