#pragma once
// ---------------------------------------------------------------------------
// ClientConfig: kleine Einstellungsdatei des Spiels für den Mehrspieler.
//
// Wofür? Damit man die Serveradresse und den eigenen Spielernamen nicht bei
// jedem Start neu eintippen muss. Das Mehrspieler-Menü (src/Multiplayer.cpp)
// lädt die Datei beim Öffnen, füllt damit die Eingabefelder vor und speichert
// sie wieder, sobald man auf "Verbinden" drückt bzw. sich erfolgreich
// angemeldet hat.
//
// Wo liegt die Datei? Neben der ausführbaren Datei (wie der Spielstand in
// Save.cpp): GetApplicationDirectory() + "mondschuerfer_client.txt".
//
// Format: einfache Textdatei, eine Einstellung pro Zeile, "Schlüssel Wert":
//     server 192.168.1.20:7777
//     name Anna
// Unbekannte Zeilen werden ignoriert (so bleiben ältere Spielversionen mit
// neueren Dateien verträglich).
//
// WICHTIG: Das Passwort wird NIE gespeichert. Es existiert nur im Speicher,
// solange das Anmeldefenster offen ist, und wird auch nicht zum Server
// geschickt (siehe shared/Crypto.hpp: nur ein daraus berechneter Schlüssel).
//
// Abhängigkeiten: raylib (nur GetApplicationDirectory), Standardbibliothek.
// Läuft im Spiel-Thread (das Spiel hat nur einen).
// ---------------------------------------------------------------------------
#include <string>

struct ClientConfig {
    std::string server = "localhost:7777";   // zuletzt benutzte Serveradresse ("Host" oder "Host:Port")
    std::string name;                         // zuletzt erfolgreich angemeldeter Spielername

    // Vollständiger Pfad der Datei (neben der .exe)
    static std::string Path();

    // Liest die Datei. Fehlt sie oder ist sie unlesbar, bleiben die
    // Standardwerte stehen und es wird false geliefert (kein Fehler fürs Spiel).
    bool Load();

    // Schreibt die Datei neu. false, wenn sie nicht geschrieben werden konnte
    // (z. B. Spielordner schreibgeschützt) – das Spiel läuft trotzdem weiter.
    bool Save() const;
};
