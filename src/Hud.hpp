#pragma once
// ---------------------------------------------------------------------------
// Hud.hpp – Spieloberfläche über der 3D-Welt (Schnittstelle zu Hud.cpp).
//
// Wofür ist die Datei da?
//   Sie beschreibt alles, was während des Fluges als flache 2D-Grafik über
//   dem 3D-Bild liegt: Statusfenster (Kontostand, Schild, Scanner, Lager,
//   Aufrüstungen), Minikarte ("Radar"), Stationsnamen und Randpfeile,
//   Meldungen, Hinweise ("[F] Andocken ..."), Stationsfenster (Reparieren,
//   Aufrüsten) und die beiden Endbildschirme des Einzelspielers.
//
// Wer benutzt sie?
//   - src/main.cpp: Einzelspieler, Pause und Simulation (Autopilot).
//   - src/Multiplayer.cpp: Mehrspieler. Es zeichnet mit denselben Funktionen
//     die Spiegelwelt aus NetClient (World mit isMirror = true). Weil Hud nur
//     w.Me() (= players[0]) und w.Player() (= ships[0]) liest, merkt es keinen
//     Unterschied zwischen Einzel- und Mehrspieler.
//
// Abhängigkeiten:
//   - shared/World.hpp (nur lesend, "const World&": das HUD ändert nie die
//     Spielwelt; Aktionen gehen als DockRequest an den Aufrufer zurück).
//   - Ui.hpp (Texte, Panels, Balken, Knöpfe), Audio.hpp (Ton "Abgelehnt").
//   - raylib: 2D-Zeichenfunktionen und GetWorldToScreen.
//
// Aufrufzeitpunkt:
//   Jedes Frame im Spiel-Thread, zwischen BeginDrawing() und EndDrawing() und
//   NACH EndMode3D(). Innerhalb von BeginMode3D/EndMode3D wären alle
//   Koordinaten 3D-Weltkoordinaten; danach sind es wieder Bildschirmpixel
//   (0,0 = links oben), und genau darin rechnet das HUD.
// ---------------------------------------------------------------------------
#include "World.hpp"

// Spieloberfläche während des Fluges (Statusfenster, Minikarte, Meldungen,
// Hinweise, Stationsnamen). Die Kamera wird gebraucht, um Stationspositionen
// aus der 3D-Welt in Bildschirmpixel umzurechnen (Namen über den Stationen,
// Randpfeile). simulation = true (Menüpunkt "Simulation", der Autopilot
// spielt): die Tastenhilfe unten links wird nicht gezeigt.
void DrawHud(const World& w, const Camera3D& cam, bool simulation);

// Mehrspieler: rechts oben steht unter der Minikarte noch das Panel "Beste
// Piloten" (Multiplayer.cpp). Damit die Randpfeile zu Stationen außerhalb des
// Bildes nicht darunter verschwinden, meldet der Mehrspieler hier Breite und
// Unterkante dieses Bereichs (in Pixeln). 0, 0 = Standard (nur Minikarte),
// so wie es der Einzelspieler immer benutzt. Multiplayer.cpp setzt den Wert
// direkt vor DrawHud und gleich danach wieder auf 0, 0 zurück (der Wert ist
// eine dateiweite Variable in Hud.cpp und würde sonst "hängen bleiben").
void SetHudRightReserve(float width, float bottom);

// Was der Spieler im Stationsfenster angeklickt hat. Ausgeführt wird es vom
// Aufrufer: im Einzelspieler direkt an der World, im Mehrspieler als Anfrage
// an den Server.
struct DockRequest {
    bool undock = false;
    bool repair = false;
    int buy = -1;          // Upgrade-Typ (cfg::Upgrade) oder -1
};

// Stationsfenster des eigenen Spielers (w.Me()). Handelsstation: Verkaufs-
// ergebnis, Kontostand, Stationsausbau, Reparieren [R]. Werft: vier
// Aufrüstungen [1]-[4]. Beide: Abdocken [F]/[ESC], am Controller Y oder B
// (Knöpfe mit dem Controller-Zeiger und A).
// interactive = false: nur Anzeige, Tasten und Klicks werden ignoriert. Das
// gilt im Simulationsmodus (der Autopilot entscheidet) und in den ersten
// beiden Frames nach dem Andocken (der F-Druck, der angedockt hat, würde
// sonst sofort wieder abdocken; die Aufrufer zählen dafür dockFrames_).
// Muss jedes Frame aufgerufen werden, solange w.Me().dockedAt >= 0 ist.
DockRequest DrawDockPanel(const World& w, bool interactive);

// Endbildschirme (nur Einzelspieler; im Mehrspieler gibt es kein Spielende,
// dort bekommt man nach einem Absturz ein neues Schiff). Liefern true, wenn
// der Spieler "Zum Hauptmenü" gewählt hat. Die Tasten Enter/ESC wirken erst
// nach 1 s (w.outcomeT), damit ein gerade noch gehaltener Tastendruck aus dem
// Spiel den Bildschirm nicht sofort wegklickt.
bool DrawGameOver(const World& w);
bool DrawVictory(const World& w);

// Zeigt die Tastenbelegung als zweispaltige Liste (Taste | Wirkung) ab der
// linken oberen Ecke (x, y), und zwar für das zuletzt benutzte Gerät:
// Tastatur/Maus oder Controller (Xbox- bzw. PlayStation-Namen; Zeilen aus
// controls::HelpRows in Controls.hpp). size = Schriftgröße in Pixeln; die
// Zeilen liegen 1,35 * size auseinander.
// Benutzt im HUD (unten links) und in den Pausenmenüs (main.cpp, Multiplayer.cpp).
void DrawControls(float x, float y, float size);
// Höhe, die DrawControls mit dieser Schriftgröße belegt (Zeilenzahl * 1,35 *
// size). Die Pausenmenüs und das HUD rechnen ihr Layout damit aus, statt
// selbst eine Zeilenzahl anzunehmen – kommt in DrawControls eine Zeile dazu,
// passt sich alles von allein an.
float ControlsHeight(float size);
// Höhe der LÄNGSTEN Tastenhilfe (egal welches Gerät). Für Fenster mit
// Knöpfen (Pausenmenüs): Deren Größe darf sich nicht mit dem Gerät ändern.
// Sonst rutschen die Knöpfe genau in dem Frame, in dem der Spieler mit dem
// anderen Gerät klickt (ui::BeginFrame stellt das Gerät sofort um) – und der
// Klick träfe den Nachbarknopf.
float ControlsMaxHeight(float size);
