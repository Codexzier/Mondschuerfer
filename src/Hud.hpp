#pragma once
#include "World.hpp"

// Spieloberfläche während des Fluges
void DrawHud(const World& w, const Camera3D& cam, bool simulation);

// Was der Spieler im Stationsfenster angeklickt hat. Ausgeführt wird es vom
// Aufrufer: im Einzelspieler direkt an der World, im Mehrspieler als Anfrage
// an den Server.
struct DockRequest {
    bool undock = false;
    bool repair = false;
    int buy = -1;          // Upgrade-Typ (cfg::Upgrade) oder -1
};

// Stationsfenster des eigenen Spielers (w.Me()).
// interactive = false im Simulationsmodus (nur Anzeige).
DockRequest DrawDockPanel(const World& w, bool interactive);

// Endbildschirme: liefert true, wenn zurück ins Menü
bool DrawGameOver(const World& w);
bool DrawVictory(const World& w);

// Zeigt die Tastenbelegung in einem Rechteck
void DrawControls(float x, float y, float size);
