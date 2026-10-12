#pragma once
// ---------------------------------------------------------------------------
// Save.hpp – Spielstand des Einzelspielers speichern und laden
// (Umsetzung: Save.cpp).
//
// Wofür ist die Datei da?
//   Im Einzelspieler kann man ein Spiel unterbrechen und später im Hauptmenü
//   mit "Fortsetzen" weiterspielen. Es gibt genau EINEN Spielstand: eine
//   Textdatei neben der ausführbaren Datei (SavePath). Gespeichert wird nur,
//   was sich nicht aus dem Seed neu erzeugen lässt (Kontostand,
//   Aufrüstungen, Positionen, Lagerstätten, Wracks, fremde Schiffe).
//
// Wer benutzt sie?
//   Nur src/main.cpp (Klasse App):
//     - SaveExists(): beim Start und bei der Rückkehr ins Hauptmenü, um den
//       Knopf "Fortsetzen" freizuschalten.
//     - SaveGame(): automatisch nach jedem Andocken (World::wantAutosave) und
//       Abdocken, in der Pause mit "Speichern & zum Hauptmenü" und beim
//       Schließen des Fensters (App::OnExit).
//     - LoadGame(): Knopf "Fortsetzen" (App::Continue).
//     - DeleteSave(): nach Sieg oder Absturz – nach dem Ende gibt es kein
//       Weiterspielen.
//   Der Mehrspieler benutzt diese Datei NICHT: Dort speichert der Server den
//   Fortschritt im Spielerkonto (server/src/Accounts.cpp).
//
// Abhängigkeiten:
//   shared/World.hpp (der Zustand, der gespeichert wird), raylib
//   (GetApplicationDirectory für den Ordner der ausführbaren Datei).
// ---------------------------------------------------------------------------
#include "World.hpp"
#include <string>

// Vollständiger Pfad der Spielstand-Datei:
// <Ordner der ausführbaren Datei>/mondschuerfer_spielstand.txt
std::string SavePath();
// true, wenn die Datei existiert und mit der Kennung des Spiels beginnt
// (die Versionsnummer prüft erst LoadGame).
bool SaveExists();
// Schreibt den Spielstand. Tut nichts (false), wenn die Welt eine Simulation
// ist oder die Partie schon zu Ende ist (Sieg/Absturz). false auch, wenn die
// Datei nicht geschrieben werden konnte.
bool SaveGame(const World& w);
// Baut die Welt aus dem Spielstand neu auf (ersetzt den ganzen Inhalt von w).
// false, wenn die Datei fehlt oder Kennung/Version nicht passen; w ist dann
// unverändert.
bool LoadGame(World& w);
// Löscht die Datei (fehlt sie, passiert nichts).
void DeleteSave();
