#pragma once
// ---------------------------------------------------------------------------
// MapFiles.hpp – eigene Karten als Dateien im Ordner "karten" neben der
// ausführbaren Datei (Umsetzung: MapFiles.cpp, Dateiformat: shared/GameMap.hpp).
//
// Wer benutzt sie?
//   - src/MapEditor.cpp: Kartenliste, "Karte laden", "Speichern"
//   - src/main.cpp: Kartenauswahl bei "Neues Spiel"
//   - src/Save.cpp: Der Spielstand merkt sich den Dateinamen der Karte und
//     lädt sie beim Fortsetzen wieder.
//
// Dateiname: Aus dem Kartennamen wird ein "Stamm" nur aus Buchstaben a-z/A-Z,
// Ziffern, '-' und '_' (Umlaute umschreiben, alles andere wird '_'), dazu die
// Endung ".karte". Mit reinen ASCII-Namen gibt es auch unter Windows keine
// Probleme mit Sonderzeichen im Pfad; der echte Name steht in der Datei.
//
// Abhängigkeiten: shared/GameMap.hpp, raylib (GetApplicationDirectory,
// Ordner anlegen und auflisten).
// ---------------------------------------------------------------------------
#include "GameMap.hpp"
#include <string>
#include <vector>

struct MapFileInfo {
    std::string stem;   // Dateiname ohne Ordner und Endung
    std::string name;   // Name aus der Datei (zum Anzeigen)
};

std::string MapDir();                                   // <Ordner der .exe>/karten/
std::string MapFileStem(const std::string& name);       // Kartenname -> Dateistamm (nie leer)
std::vector<MapFileInfo> ListMaps();                    // alle lesbaren Karten, nach Name sortiert
bool MapFileExists(const std::string& stem);
bool LoadMapFile(const std::string& stem, GameMap& out);
// Speichert unter MapFileStem(m.name). false, wenn die Datei nicht
// geschrieben werden konnte.
bool SaveMapFile(const GameMap& m);
