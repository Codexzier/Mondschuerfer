#pragma once
// ---------------------------------------------------------------------------
// EventEditor.hpp – Event-Editor im Mehrspieler (Knopf "Event einrichten" in
// der Rollenwahl nach der Anmeldung, nur für Konten mit der Rolle Event Manager).
//
// Wofür ist die Datei da?
//   Ein Event macht Einsatzkarten an einzelnen Tagen im Command Center
//   zusätzlich wählbar oder sperrt sie (shared/MapEvents.hpp). Hier richtet
//   man das im Spiel ein, wie im Bereich "Events" der Verwaltungsseite:
//   links die Events des Servers (laufend, geplant, vorbei), rechts das
//   gewählte Event: Name, Datum oder Zeitraum ("bis" leer = nur ein Tag) und
//   alle Einsatzkarten des Servers, jede "wie sonst", "wählbar" oder
//   "gesperrt" (Klick schaltet weiter). Die Mondkarte steht immer oben und
//   ist immer wählbar.
//
// Ablauf: MultiplayerScreen öffnet ihn mit NetClient::OpenEvents, reicht jede
// neue Liste (net::EventListMsg) an SetList weiter und schickt, was Draw
// liefert, als EventEdit an den Server. Der Server prüft alles noch einmal.
//
// Wer benutzt die Datei? src/Multiplayer.cpp (über der Rollenwahl).
// Abhängigkeiten: shared/Protocol.hpp (EventListMsg), shared/MapEvents.hpp,
// src/Ui.hpp, raylib.
// ---------------------------------------------------------------------------
#include "Protocol.hpp"
#include "Ui.hpp"
#include <map>
#include <string>

// Was im Editor gewählt wurde (MultiplayerScreen schickt Save/Delete als
// EventEdit an den Server bzw. geht bei Back zurück zur Rollenwahl)
struct EventEditorAction {
    enum class Kind { None, Save, Delete, Back };
    Kind kind = Kind::None;
    MapEvent event;   // Save: id 0 = neues Event; Delete: nur id
};

class EventEditorScreen {
public:
    EventEditorScreen();
    // Beim Öffnen: account = eigenes Konto für die Kopfzeile; bis die erste
    // Liste kommt (SetList), wartet der Editor auf den Server
    void Open(const std::string& account);
    // Neue Liste vom Server übernehmen. Ein gerade gespeichertes Event wird
    // ausgewählt (savedId), ein gelöschtes geschlossen; sonst bleibt das
    // Formular, wie es ist (z. B. wenn jemand anderes etwas ändert).
    void SetList(const net::EventListMsg& list);
    // Meldung in der Statuszeile (z. B. Fehler des Servers)
    void SetStatus(const std::string& text, Color c);
    // Ein Frame zeichnen und bedienen (zwischen BeginDrawing und EndDrawing)
    EventEditorAction Draw();

private:
    void NewEvent();                     // leeres Formular: heute, keine Karten
    void Select(const MapEvent& e);      // Formular mit diesem Event füllen
    bool BuildEvent(MapEvent& out);      // Formular prüfen; false = Grund steht im Status

    std::string account_;
    net::EventListMsg list_;
    bool fresh_ = true;                  // noch keine Liste seit Open
    int selected_ = 0;                   // Nummer des gewählten Events, 0 = neues
    ui::TextField nameField_, fromField_, toField_;
    std::map<std::string, int> picks_;   // Karte (Datei) -> 1 wählbar, 2 gesperrt (fehlt = wie sonst)
    int listScroll_ = 0;                 // erste sichtbare Zeile der Eventliste
    int mapScroll_ = 0;                  // erste sichtbare Zeile der Kartenliste
    bool busy_ = false;                  // Speichern/Löschen geschickt, Antwort steht aus
    double busyAt_ = 0.0;
    double confirmDeleteAt_ = -10.0;     // erster Klick auf "Löschen" (zweiter innerhalb von 3 s löscht)
    int frames_ = 0;                     // Frames seit Open (der Klick, der hierher geführt hat, zählt nicht)
    std::string status_;
    Color statusColor_ = ui::MUTED;
};
