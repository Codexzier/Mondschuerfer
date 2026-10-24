#pragma once
// ---------------------------------------------------------------------------
// Controls.hpp – Flugsteuerung des eigenen Schiffs mit Tastatur, Maus und
// Controller, dazu die Tastennamen für Hilfetexte (Umsetzung: Controls.cpp).
//
// Wofür ist die Datei da?
//   Früher gab es zwei fast gleiche Funktionen, die Tastatur und Gamepad in
//   Steuerbefehle übersetzten (eine in main.cpp, eine in Multiplayer.cpp).
//   Jetzt gibt es eine Klasse dafür: PilotControls. Sie liest alle drei
//   Geräte und liefert, was das Schiff tun soll (FlightInput). Die drei
//   Geräte gehen jederzeit, auch gemischt:
//     - Tastatur: W/S Schub/Bremsen, A/D (oder Pfeiltasten) drehen,
//       Leertaste schießen, E Scanner, F Andocken/Abbauen – genau wie bisher.
//     - Maus: Linksklick auf den Boden = dorthin fliegen und anhalten;
//       Linke Maustaste halten = das Schiff folgt dem Zeiger. Rechte
//       Maustaste löscht das Ziel. Schießen, Scanner, Andocken bleiben auf
//       der Tastatur.
//     - Controller: linker Stick = Flugrichtung (so, wie der Stick IM BILD
//       zeigt) – oder mit der Tank-Steuerung aus den Optionen: vor/zurück
//       = Schub, links/rechts = drehen –, rechter Trigger = Tempo, linker Trigger = bremsen,
//       A schießen, B Scanner, Y Andocken/Abbauen, Menü-Taste Pause,
//       rechter Stick hoch/runter = Zoom. PlayStation an denselben Stellen:
//       Kreuz, Kreis, Dreieck, R2/L2, Options.
//   Sobald Tastatur-Lenkung oder Controller übernehmen, wird ein Mausziel
//   gelöscht ("wer lenkt, hat recht").
//
// Wer benutzt sie?
//   - src/main.cpp: App besitzt ein PilotControls-Objekt für den
//     Einzelspieler (Read im freien Flug, Zielmarkierung, Testoptionen
//     --mouse-test und --pad-test).
//   - src/Multiplayer.cpp: MultiplayerScreen besitzt eins für den Mehrspieler.
//     Dort gehen die Befehle als Eingabepakete an den Server (NetClient) –
//     das Netzwerkprotokoll merkt nichts von Maus oder Controller.
//   - src/Hud.cpp, src/Options.cpp, src/Multiplayer.cpp: Tastennamen für
//     Hilfetexte ("[F] Andocken" bzw. "[Y] Andocken" / "[Dreieck] Andocken").
//
// Abhängigkeiten:
//   shared/World.hpp (Ship, ShipInput, cfg-Werte), src/ControlsMath.hpp (die
//   Rechnungen: Totzonen, Stick -> Richtung, Trigger, Anflug), src/Ui.hpp
//   (Flug-/Menümodus, Controller, UI-Flächen), src/ClientConfig.hpp
//   (Einstellung Tank-Steuerung), raylib (Tastatur, Maus,
//   Controller, Kamerastrahl, Zeichnen der Zielmarkierung).
//
// Ablauf pro Frame (nur im freien Flug, nicht angedockt, nicht in der Pause):
//   1. PausePressed()  – soll die Pause aufgehen? Dann nichts weiter.
//   2. Read()          – Geräte lesen, Steuerbefehle liefern.
//   3. Zeichnen: DrawTarget3D() zwischen BeginMode3D und EndMode3D.
//   Angedockt, nach einem Absturz oder bei einem neuen Spiel: Reset().
// Alles läuft im Spiel-Thread.
// ---------------------------------------------------------------------------
#include "World.hpp"
#include "ControlsMath.hpp"
#include <string>
#include <vector>

// Was die Flugsteuerung in einem Frame liefert.
struct FlightInput {
    // Gehaltene Eingaben: Schub, Drehen, Feuer. (scanToggle und interact
    // bleiben hier false – die stehen getrennt darunter, weil der
    // Mehrspieler sie als Tastendruck-Zähler verschickt.)
    ShipInput held;
    bool scan = false;       // Scanner umschalten (E, Controller B / Kreis) – nur im Frame des Drückens
    bool interact = false;   // Andocken / Abbauen (F, Controller Y / Dreieck) – nur im Frame des Drückens
    float zoom = 0.0f;       // Zoom wie Mausrad-Rasten (rechter Stick); positiv = näher heran
};

class PilotControls {
public:
    // ESC, P oder die Menü-Taste des Controllers (PlayStation: Options) in
    // diesem Frame gedrückt? Vor Read() fragen: Geht die Pause auf, wird in
    // diesem Frame nicht geflogen.
    static bool PausePressed();

    // Geräte lesen und Steuerbefehle für das Schiff berechnen. Nur im freien
    // Flug aufrufen (meldet dabei ui::SetFlightMode). cam = die Kamera, mit
    // der das Bild gezeichnet wurde (für "wo zeigt die Maus hin" und "wo ist
    // im Bild oben"); dt = Sekunden seit dem letzten Frame.
    // Ohne Mausklick und ohne Controller ist das Ergebnis genau das der
    // Tastatur, wie es auch vor der Maus- und Controllersteuerung war.
    FlightInput Read(const Ship& ship, const Camera3D& cam, float dt);

    // Mausziel und gehaltene Taste vergessen (angedockt, Absturz, neues Spiel ...).
    void Reset();

    bool HasTarget() const { return target_.active; }
    Vector3 Target() const { return target_.point; }

    // Zielmarkierung am Boden (pulsierender Ring mit Kreuz und Lichtsäule).
    // Zwischen BeginMode3D und EndMode3D aufrufen; zeichnet nur, wenn es ein
    // Ziel gibt. time = laufende Zeit in Sekunden (für das Pulsieren).
    void DrawTarget3D(float time) const;

    // --- nur für Testoptionen (main.cpp) ---------------------------------------
    // --mouse-test: Ziel setzen wie ein Mausklick an diese Stelle.
    void SetTestTarget(Vector3 t);
    // --pad-test: einen Controller vortäuschen – Stick dauerhaft in Richtung
    // angleDeg (Grad im Bild, 0 = oben, 90 = rechts), rechter Trigger = trigger (0..1).
    void SetTestPad(float angleDeg, float trigger);
    // Wie endete das letzte Mausziel? "angekommen", "aufgegeben" (kam nicht
    // näher), "abgebrochen" (Tastatur, Controller, rechte Maustaste) oder
    // "zurückgesetzt" (Reset); "" = noch keins.
    const char* LastEnd() const { return lastEnd_; }

private:
    controls::FlightTarget target_;      // Mausziel (ControlsMath.hpp)
    bool holding_ = false;               // linke Maustaste gehalten: Ziel folgt dem Zeiger
    unsigned lastReadFrame_ = 0;         // ui::FrameIndex() beim letzten Read
    bool fireLatch_ = false;             // Controller-A war beim Flugbeginn schon gedrückt
    int triggerPad_ = -1;                // zu welchem Controller rt_/lt_ gehören
    controls::TriggerReader rt_, lt_;    // rechter / linker Trigger
    const char* lastEnd_ = "";

    bool testPad_ = false;               // --pad-test
    float testAngle_ = 0.0f;
    float testTrigger_ = 0.0f;
};

// ===========================================================================
// Tastennamen und Hilfetexte – passend zum zuletzt benutzten Gerät
// ===========================================================================
namespace controls {

enum class Device { KeyboardMouse, Xbox, PlayStation };
Device ActiveDevice();   // nach ui::UsingGamepad() und ui::GamepadIsPlayStation()

// Aktionen, deren Taste in Hilfetexten genannt wird.
enum class Act { Fire, Scan, Interact, Pause, Confirm, Back };

// Name der Taste für das aktive Gerät, z. B. Interact: "F", "Y" oder "Dreieck".
// Die eingebaute Schrift von raylib kennt keine Symbole wie ✕○△ – deshalb
// stehen die PlayStation-Knöpfe als Wörter da.
const char* KeyName(Act a);
std::string KeyHint(Act a);   // derselbe Name in eckigen Klammern: "[F]"

// Eine Zeile der Tastenhilfe (HUD unten links, Pausenmenüs): Taste | Wirkung.
struct HelpRow {
    const char* keys;
    const char* what;
};
std::vector<HelpRow> HelpRows();           // für das aktive Gerät
std::vector<HelpRow> HelpRows(Device d);   // für ein bestimmtes Gerät
size_t MaxHelpRows();                      // längste der drei Listen (Tastatur/Maus: 9)

} // namespace controls
