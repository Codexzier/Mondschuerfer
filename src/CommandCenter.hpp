// ---------------------------------------------------------------------------
// CommandCenter.hpp – Bildschirm "Command Center" des SG-Teams (FPS-Modus,
// Einzelspieler).
//
// Wofür ist die Datei da?
//   Wer in der Rollenwahl "SG-Team" nimmt, landet hier (und nach jedem
//   Einsatz wieder). Der Bildschirm ist kein Teil der Mondkarte: Er zeigt eine
//   eigene 3D-Kulisse (Halle aus dem Modular Space Kit, Konsolen aus dem
//   Space Station Kit, das Heimat-Stargate und das Team davor) und darüber
//   die Bedienung: Team SG1 mit 0 bis 3 NPC-Kameraden, Auftrag, Scoreboard
//   der eigenen Einsätze (beste zuerst) mit der Vorschau der gewählten
//   Einsatzkarte und die Knöpfe "Tor anwählen", "Ziel" (Mondkarte oder eine
//   Einsatzkarte aus "karten" bzw. assets/karten), "Als Pilot
//   spielen" und "Speichern & zum Hauptmenü". Die Anwahl dauert GATE_DIAL
//   Sekunden; danach geht das Team durch ein Stargate auf die Mondkarte bzw.
//   in die eigene Welt der Einsatzkarte (World::StartSgMission,
//   World::NewInstance, ausgeführt von main.cpp).
//
// Wer benutzt die Datei?
//   src/main.cpp (Bildschirm Screen::Command). DrawGateWormhole ist in
//   src/Render.cpp definiert und zeichnet das Wurmloch in der Welt und hier.
//
// Abhängigkeiten: shared/World.hpp (PlayerState, Stargates, SgRecord),
// src/Assets.hpp (Modelle, Figuren), src/Ui.hpp (Knöpfe, Texte).
// ---------------------------------------------------------------------------
#pragma once
#include "MapFiles.hpp"
#include "World.hpp"
#include <string>
#include <vector>

// Was der Spieler im Command Center gewählt hat; ausgeführt wird es in
// main.cpp. Die Zahl der Kameraden ändert der Bildschirm selbst
// (PlayerState::sgMates).
enum class CommandAction { None, Mission, Pilot, SaveAndMenu };

class CommandCenterScreen {
public:
    // Beim Betreten: Anwahl zurücksetzen, Stargates der Mondkarte anlegen
    // (gibt es keins, ist "Tor anwählen" für die Mondkarte gesperrt), die
    // Einsatzkarten neu einlesen, das Ergebnis eines gerade beendeten
    // Einsatzes übernehmen (PlayerState::sgEnded wird zurückgesetzt).
    // w ist immer die Mondwelt.
    void Open(World& w);
    // Ein Frame: Uhr der Kulisse und der Anwahl weiterzählen.
    void Update(float dt);
    // Kulisse und Oberfläche zeichnen (zwischen BeginDrawing und EndDrawing).
    // Liefert Mission, sobald die Anwahl fertig ist.
    CommandAction Draw(World& w);
    // Läuft gerade die Anwahl (Knöpfe gesperrt)?
    bool Dialing() const { return dialT_ >= 0.0f; }
    // Gewähltes Ziel: Dateistamm der Einsatzkarte, "" = Mondkarte
    std::string TargetStem() const { return target_ > 0 ? targets_[static_cast<size_t>(target_ - 1)].stem : std::string(); }

private:
    void DrawScene(const World& w);
    float time_ = 0.0f;    // Sekunden seit Open (Animationen)
    float dialT_ = -1.0f;  // Anwahl: Sekunden seit dem Start, -1 = keine
    int sel_ = 0;          // per Tastatur gewählter Knopf
    int frames_ = 0;       // Frames seit Open (der Klick, der hierher geführt hat, zählt nicht)
    bool hasGate_ = false; // hat das gewählte Ziel ein Stargate? (Einsatzkarten: immer, der Editor prüft es)
    int ended_ = 0;        // gerade beendeter Einsatz (PlayerState::sgEnded): Ergebnis oben zeigen
    bool moonGate_ = false;              // steht auf der Mondkarte ein Stargate?
    std::vector<MapFileInfo> targets_;   // Einsatzkarten im Ordner "karten"
    int target_ = 0;                     // Ziel: 0 = Mondkarte, sonst targets_[target_ - 1]
    std::string previewStem_;            // Vorschaubild: Einsatzkarte, die in preview_ steht ("" = keine)
    GameMap preview_;
};

// Wurmloch eines Stargates mit Fußpunkt pos, Drehung yaw und Größe scale
// (Render.cpp). Mit BLEND_ADDITIVE und ohne Schreiben in den Tiefenpuffer
// aufrufen.
void DrawGateWormhole(Vector3 pos, float yaw, float scale, const GateLook& look, float time);
