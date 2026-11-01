#pragma once
// ---------------------------------------------------------------------------
// MapFiles.hpp – eigene Karten als Dateien im Ordner "karten" neben der
// ausführbaren Datei (Umsetzung: MapFiles.cpp, Dateiformat: shared/GameMap.hpp).
//
// Mitgelieferte Karten (die vier Einsatzkarten) liegen in assets/karten und
// werden nur gelesen. Eine eigene Karte mit demselben Dateinamen hat
// Vorrang: Wer eine mitgelieferte Karte im Editor ändert und speichert,
// bekommt eine eigene Kopie in "karten".
//
// Wer benutzt sie?
//   - src/MapEditor.cpp: Kartenliste, "Karte laden", "Speichern"
//   - src/main.cpp: Kartenauswahl bei "Neues Spiel"
//   - src/Save.cpp: Der Spielstand merkt sich den Dateinamen der Karte und
//     lädt sie beim Fortsetzen wieder.
//
// Dateiname: Aus dem Kartennamen wird ein "Stamm" (MapFileStem, GameMap.hpp),
// dazu die Endung ".karte". Der echte Name steht in der Datei.
//
// Abhängigkeiten: shared/GameMap.hpp, Assets.hpp (Ordner assets), raylib
// (GetApplicationDirectory, Ordner anlegen und auflisten).
// ---------------------------------------------------------------------------
#include "GameMap.hpp"
#include <string>
#include <vector>

struct MapFileInfo {
    std::string stem;   // Dateiname ohne Ordner und Endung
    std::string name;   // Name aus der Datei (zum Anzeigen)
    MapKind kind = MapKind::Moon;
    MapTheme theme = MapTheme::Village;   // nur Einsatzkarten (Command Center)
    int size = 0;
    MapSky sky = MapSky::Day;
};

std::string MapDir();                                   // <Ordner der .exe>/karten/
std::vector<MapFileInfo> ListMaps();                    // alle lesbaren Karten (eigene und mitgelieferte), nach Name sortiert
bool MapFileExists(const std::string& stem);            // eigene oder mitgelieferte
bool LoadMapFile(const std::string& stem, GameMap& out);   // eigene Karte, sonst die mitgelieferte
// Speichert unter MapFileStem(m.name). false, wenn die Datei nicht
// geschrieben werden konnte.
bool SaveMapFile(const GameMap& m);
