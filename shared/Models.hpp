#pragma once
// ---------------------------------------------------------------------------
// Models.hpp – Modell-Verzeichnis: verbindet Modellnamen (Dateinamen aus dem
// Kenney Space Kit, z. B. "hangar_largeA") mit einer Nummer (Modell-ID).
//
// Warum gibt es das? Die Spiellogik (World) merkt sich für jedes Schiff, jede
// Lagerstätte und jedes Stationsteil, welches Modell es hat. Früher war das
// direkt die Nummer des geladenen 3D-Modells aus der Grafik (Assets). Der
// Server lädt aber keine 3D-Modelle, er hat ja keine Grafikkarte. Deshalb
// vergibt dieses Verzeichnis die Nummern unabhängig von der Grafik:
//   - Spiellogik und Server arbeiten nur mit diesen IDs.
//   - Der Client (src/Assets.cpp) lädt das passende 3D-Modell erst, wenn es
//     zum ersten Mal gezeichnet wird, und benutzt dafür ModelName(id)
//     (Datei "<Asset-Ordner>/models/<Name>.glb").
//
// Wer benutzt es?
//   - shared/WorldGen.cpp (World::ResolveModels, Stationsteile, Deko),
//   - shared/Protocol.cpp (net::DefaultModelIds: dieselben Namen wie
//     ResolveModels, damit Netzwerk-"Varianten" in IDs übersetzt werden),
//   - src/Assets.cpp (Laden und Zeichnen), src/Render.cpp (Geschütztürme).
//
// Für Kollisionen mit Stationsteilen braucht die Spiellogik die Größe der
// Modelle. Die stehen als Tabelle in ModelBounds.inc (aus den GLB-Dateien
// erzeugt), damit auch der Server sie kennt, ohne Modelle zu laden. Das ist
// auch für den Mehrspieler-Client wichtig: Er sagt die Bewegung des eigenen
// Schiffs voraus (World::UpdateShipPhysics) und muss dabei GENAU dieselben
// Hindernisse kennen wie der Server – sonst springt das Schiff an Stationen
// hin und her. Beide lesen dieselbe Tabelle, also passt es immer.
//
// Die IDs werden in der Reihenfolge der ersten Anfrage vergeben. Sie werden
// nie über das Netzwerk geschickt (dafür gibt es eigene "Varianten"-Nummern,
// siehe net::ShipVariant/DepositVariant in Protocol.hpp). Die ID desselben
// Modells kann sich also zwischen Client und Server unterscheiden (der Client
// fragt z. B. zusätzlich nach "turret_double"), ohne dass das schadet.
//
// Abhängigkeiten: nur raylib.h (Datentyp BoundingBox), ModelBounds.inc.
// Umsetzung: Models.cpp.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>

// Liefert die ID zu einem Modellnamen. Unbekannte Namen werden neu eingetragen.
// Ein Name ohne Eintrag in ModelBounds.inc bekommt trotzdem eine ID (nur eine
// leere Box) – mit einer Warnung auf stderr.
int ModelId(const std::string& name);

// Name zu einer ID ("" bei ungültiger ID).
// Die Referenz zeigt in das Verzeichnis selbst. Sie bleibt nur gültig, bis
// das nächste Mal ein NEUER Name eingetragen wird (dann kann der interne
// std::vector umziehen). Darum sofort benutzen oder kopieren (Assets.cpp
// baut daraus sofort den Dateipfad).
const std::string& ModelName(int id);

// Anzahl bisher vergebener IDs (gültige IDs sind 0 .. ModelCount() - 1).
int ModelCount();

// Abmessungen aus der eingebauten Tabelle. Unbekannte Modelle: leere Box.
// Maße im Kit-Raster, ohne Skalierung, Ursprung = Bezugspunkt des Modells
// (Mitte, auf dem Boden), also passend zu den Positionen in der Spiellogik.
BoundingBox ModelBounds(int id);
