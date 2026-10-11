#pragma once
// ---------------------------------------------------------------------------
// Modell-Verzeichnis: verbindet Modellnamen (Dateinamen aus dem Kenney Space
// Kit, z. B. "hangar_largeA") mit einer Nummer (Modell-ID).
//
// Warum gibt es das? Die Spiellogik (World) merkt sich für jedes Schiff, jede
// Lagerstätte und jedes Stationsteil, welches Modell es hat. Früher war das
// direkt die Nummer des geladenen 3D-Modells aus der Grafik (Assets). Der
// Server lädt aber keine 3D-Modelle, er hat ja keine Grafikkarte. Deshalb
// vergibt dieses Verzeichnis die Nummern unabhängig von der Grafik:
//   - Spiellogik und Server arbeiten nur mit diesen IDs.
//   - Der Client (src/Assets.cpp) lädt das passende 3D-Modell erst, wenn es
//     zum ersten Mal gezeichnet wird, und benutzt dafür ModelName(id).
//
// Für Kollisionen mit Stationsteilen braucht die Spiellogik die Größe der
// Modelle. Die stehen als Tabelle in ModelBounds.inc (aus den GLB-Dateien
// erzeugt), damit auch der Server sie kennt, ohne Modelle zu laden.
//
// Die IDs werden in der Reihenfolge der ersten Anfrage vergeben. Sie werden
// nie über das Netzwerk geschickt (dafür gibt es eigene "Varianten"-Nummern).
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>

// Liefert die ID zu einem Modellnamen. Unbekannte Namen werden neu eingetragen.
int ModelId(const std::string& name);

// Name zu einer ID ("" bei ungültiger ID).
const std::string& ModelName(int id);

// Anzahl bisher vergebener IDs.
int ModelCount();

// Abmessungen aus der eingebauten Tabelle. Unbekannte Modelle: leere Box.
BoundingBox ModelBounds(int id);
