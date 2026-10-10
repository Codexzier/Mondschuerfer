#pragma once
#include "World.hpp"

// Spieloberfläche während des Fluges
void DrawHud(const World& w, const Camera3D& cam, bool simulation);

// Stationsfenster. Liefert true, wenn abgedockt werden soll.
// interactive = false im Simulationsmodus (nur Anzeige).
bool DrawDockPanel(World& w, bool interactive);

// Endbildschirme: liefert true, wenn zurück ins Menü
bool DrawGameOver(const World& w);
bool DrawVictory(const World& w);

// Zeigt die Tastenbelegung in einem Rechteck
void DrawControls(float x, float y, float size);
