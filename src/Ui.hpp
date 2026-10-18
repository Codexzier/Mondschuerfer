#pragma once
// ---------------------------------------------------------------------------
// Ui.hpp – einfache 2D-Bausteine für Menüs und HUD (Schnittstelle zu Ui.cpp).
//
// Wofür ist die Datei da?
//   Texte (mit Schatten, zentriert, rechtsbündig), Panels, Balken, Knöpfe,
//   senkrechte Knopflisten, Schieberegler, Umschalter, einzeilige Textfelder
//   und zwei Zahlenformate (Tausenderpunkte, Uhrzeit). Dazu die Farbpalette
//   des Spiels, angelehnt an den Stil der Kenney-Grafiken (dunkle Panels,
//   orangefarbene Akzente), und die Zeiger-Abfragen (PointerPressed ...),
//   über die alle Bausteine wissen, ob geklickt wurde.
//   Außerdem der "Menümodus" für Controller: Solange nicht geflogen wird,
//   bewegt der linke Stick den Mauszeiger und Knopf A klickt (siehe
//   "Controller in Menüs" unten). Und: welches Gerät zuletzt benutzt wurde
//   (Tastatur/Maus oder Controller), damit die Hilfetexte passen.
//
// Wer benutzt sie?
//   src/main.cpp (Hauptmenü, Pause), src/Hud.cpp (HUD, Stationsfenster,
//   Endbildschirme), src/Multiplayer.cpp (Mehrspieler-Menüs, Anmeldung),
//   src/Options.cpp (Optionen: Schieberegler, Umschalter) und
//   src/Controls.cpp (Flugsteuerung: meldet den Flug, fragt UI-Flächen ab).
//
// Abhängigkeiten:
//   Nur raylib (Zeichnen, Maus, Tastatur, Controller, eingebaute Schrift),
//   für die Bedientöne Audio.hpp und für den Controller-Namen
//   ControlsMath.hpp (beides in Ui.cpp).
//
// Grundidee "immediate mode" (sofortiger Modus):
//   Es gibt keine Fenster- oder Knopf-Objekte, die irgendwo gespeichert sind.
//   Stattdessen ruft der Aufrufer JEDES FRAME die Funktionen auf; jede
//   zeichnet sofort und wertet gleichzeitig Maus und Tastatur dieses Frames
//   aus (Button liefert z. B. direkt true, wenn er gerade angeklickt wurde).
//   Was ausgewählt ist o. Ä., hält der Aufrufer selbst (z. B. "selected").
//   Das passt gut zu raylib, das ohnehin jedes Bild komplett neu zeichnet.
//
// Voraussetzungen: Aufruf nur im Spiel-Thread, zwischen BeginDrawing() und
// EndDrawing() und außerhalb von BeginMode3D() (also in Pixelkoordinaten,
// 0,0 = links oben). ui::BeginFrame() einmal am Anfang jedes Frames.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include <string>
#include <vector>

// Einfache UI-Bausteine (Texte, Panels, Balken, Knöpfe) im Kenney-Farbstil.
namespace ui {

// Farbpalette. raylib-Farben sind {Rot, Grün, Blau, Alpha}, je 0..255;
// Alpha = Deckkraft (255 = undurchsichtig). Die Panels sind knapp unter 255,
// damit die 3D-Welt dahinter ein wenig durchscheint.
constexpr Color ACCENT{255, 160, 51, 255};      // Orange: Hervorhebungen, Kontostand, Handelsstation
constexpr Color ACCENT_DIM{200, 120, 40, 255};  // gedämpftes Orange: Panel-Rahmen
constexpr Color PANEL{26, 31, 41, 238};         // Hintergrund der Panels
constexpr Color PANEL_LIGHT{44, 51, 64, 235};   // Hintergrund der Knöpfe
constexpr Color TEXT{236, 239, 245, 255};       // normaler Text
constexpr Color MUTED{150, 158, 176, 255};      // Beschriftungen, Nebensächliches
constexpr Color GOOD{110, 240, 170, 255};       // Grün: Erfolg, Kristalle, andere Spieler
constexpr Color BAD{255, 105, 90, 255};         // Rot: Gefahr, Gegner
constexpr Color SHIELD{95, 175, 255, 255};      // Blau: Schildbalken
constexpr Color SCAN{70, 230, 170, 255};        // Türkis: Scannerbalken

// Einmal pro Frame aufrufen, als Erstes (main.cpp, App::Frame). Schiebt die
// Werte "dieses Frame" nach "voriges Frame" (Flug, Textfeld-Fokus,
// UI-Flächen), merkt sich das zuletzt benutzte Gerät und bewegt im
// Menümodus den Zeiger mit dem Controller-Stick.
void BeginFrame();
unsigned FrameIndex();               // zählt die Frames (BeginFrame erhöht um 1)

// ---------------------------------------------------------------------------
// Fliegen oder Menü?
// Ein Controller-Knopf bedeutet im Flug etwas anderes als im Menü: A
// schießt im Flug, im Menü klickt A. Der Stick lenkt im Flug, im Menü
// bewegt er den Zeiger. Deshalb meldet die Flugsteuerung (PilotControls::Read
// in Controls.cpp) in jedem Frame, in dem sie Flugeingaben liest,
// SetFlightMode(). Alles andere – Hauptmenü, Optionen, Pause,
// Stationsfenster, Endbildschirme, Mehrspieler-Menüs, Zuschauen – ist
// "Menümodus".
// MenuMode() wertet das VORIGE Frame aus: BeginFrame läuft am Anfang des
// Frames, bevor die Flugsteuerung sich melden kann. Folge: Im ersten Frame
// nach dem Wechsel gilt noch der alte Modus – deshalb werten die Aufrufer
// Knöpfe ohnehin erst ein, zwei Frames nach einem Bildschirmwechsel aus.
// ---------------------------------------------------------------------------
void SetFlightMode();
bool MenuMode();

// ---------------------------------------------------------------------------
// Controller (raylib nennt sie "Gamepad")
// raylib benennt die Knöpfe nach ihrer LAGE: GAMEPAD_BUTTON_RIGHT_FACE_DOWN
// ist der untere Knopf der rechten Vierergruppe – bei Xbox "A", bei
// PlayStation "Kreuz". Darum gilt jede Belegung automatisch für beide
// Controller-Arten an derselben Stelle.
// ---------------------------------------------------------------------------
int Gamepad();                // erster angeschlossener Controller (0..3), -1 = keiner
bool UsingGamepad();          // war das zuletzt benutzte Gerät ein Controller?
bool GamepadIsPlayStation();  // ... und zwar einer von PlayStation (am Namen erkannt)?
// Nur für Testoptionen (main.cpp, --pad-test/--pad-name): so tun, als sei
// ein Controller mit diesem Namen angeschlossen und zuletzt benutzt worden –
// für Bildschirmfotos der Controller-Hilfetexte ohne echten Controller.
void SetTestGamepad(const char* name);
// Wurde dieser Controller-Knopf in diesem Frame gedrückt? Nur im Menümodus,
// sonst immer false (im Flug wertet die Flugsteuerung die Knöpfe aus).
bool PadPressed(int button);

// ---------------------------------------------------------------------------
// Zeiger-Abfragen: Alle Bausteine hier (Button, Slider, Toggle, TextField)
// fragen Zeiger und Klicks nur über diese Funktionen ab, nie direkt bei
// raylib. Der Zeiger ist die Maus – oder im Menümodus der Controller:
//   - Sobald der linke Stick bewegt wird, übernimmt der Controller den
//     Zeiger (PadPointerActive). Er wird mit SetMousePosition auch als
//     Mauszeiger gesetzt und zusätzlich als oranger Ring gezeichnet
//     (DrawPointer) – auf manchen Systemen (Wayland) darf ein Programm den
//     Mauszeiger nämlich nicht versetzen.
//   - Knopf A wirkt dann wie die linke Maustaste.
//   - Bewegt sich die echte Maus oder beginnt der Flug, gibt der Controller
//     den Zeiger wieder ab ("geparkt"). Bis zur nächsten Stickbewegung
//     bestätigt A dann nur die Auswahl einer Knopfliste (ButtonList) –
//     so klickt A nach dem Öffnen der Pause nicht zufällig einen Knopf, der
//     gerade unter einem alten Zeigerort liegt.
// ---------------------------------------------------------------------------
Vector2 PointerPos();         // wo der Zeiger ist (Pixel)
Vector2 PointerDelta();       // wie weit er sich in diesem Frame bewegt hat
bool PointerMoved();          // PointerDelta ungleich 0
bool PadPointerActive();      // bedient gerade der Controller den Zeiger?
bool PointerPressed();        // in diesem Frame heruntergedrückt (ein "Klick")
bool PointerDown();           // wird gerade gehalten (z. B. Regler ziehen)
bool PointerReleased();       // in diesem Frame losgelassen
// "Zurück" in Menüs und Fenstern: ESC, im Menümodus auch Controller-B
// (PlayStation: Kreis) und die Menü-Taste (PlayStation: Options). Die
// Menü-Taste wirkt also überall wie ESC – auch zum Schließen der Pause.
bool BackPressed();
// Zeichnet den Controller-Zeiger (Ring), wenn er aktiv ist. Als Letztes im
// Frame aufrufen, damit er über allem liegt (main.cpp, App::Draw).
void DrawPointer();

// ---------------------------------------------------------------------------
// UI-Flächen blockieren Flugklicks
// Ein Linksklick ins Bild setzt im Flug ein Flugziel (Controls.cpp). Ein Klick
// auf ein HUD-Fenster (Statusfenster, Minikarte, Hinweise ...) soll das NICHT
// tun. Deshalb merkt sich Panel() jedes gezeichnete Rechteck; Kästen, die
// nicht mit Panel() gezeichnet werden, meldet der Aufrufer mit
// BlockPointer() an. PointerOverPanel fragt die Rechtecke des VORIGEN Frames
// ab (die Flugsteuerung liest ihre Eingaben, bevor dieses Frame gezeichnet ist;
// das Bild, auf das der Spieler geklickt hat, ist ja auch das vorige).
// ---------------------------------------------------------------------------
bool PointerOverPanel(Vector2 p);
void BlockPointer(Rectangle r);
// Skalierungsfaktor für alle Pixelmaße: Die Oberfläche ist für 1500 x 900
// Pixel entworfen. Ergebnis = min(Höhe / 900, Breite / 1500), aber nie
// kleiner als 0,55 (sonst wird die Schrift unlesbar).
float Scale();
float Px(float v);                   // v * Scale()

// Texte mit raylibs eingebauter Pixelschrift. size = Schrifthöhe in Pixeln.
// Width misst die Breite in Pixeln (zum Zentrieren/Rechtsbündig-Setzen).
// shadow = dunkler, leicht versetzter Schatten für Lesbarkeit über der Welt.
float Width(const std::string& t, float size);
void Text(const std::string& t, float x, float y, float size, Color c, bool shadow = true);
void TextCentered(const std::string& t, float cx, float y, float size, Color c, bool shadow = true);
void TextRight(const std::string& t, float rx, float y, float size, Color c, bool shadow = true);

// Panel = gefülltes Rechteck mit Rahmen und orangefarbenen Eck-Akzenten.
// Merkt sich außerdem sein Rechteck (siehe PointerOverPanel oben).
// Bar = Fortschrittsbalken; frac ist der Füllstand 0..1 (wird begrenzt).
void Panel(Rectangle r, Color fill = PANEL, Color border = ACCENT_DIM);
void Bar(Rectangle r, float frac, Color fill, Color back = Color{15, 18, 24, 220});

// Knopf: liefert true beim Anklicken. "focused" = per Tastatur ausgewählt.
// enabled = false: grau, nicht anklickbar (liefert nie true).
// hovered (optional): erfährt, ob die Maus über dem Knopf ist – auch bei
// gesperrtem Knopf, z. B. um dann den Ton "Abgelehnt" zu spielen (Hud.cpp).
// Tastatur (Enter usw.) wertet Button NICHT aus; das macht der Aufrufer.
bool Button(Rectangle r, const std::string& label, float size, bool focused, bool enabled = true,
            bool* hovered = nullptr);

// Schieberegler für einen Wert 0..1 (Optionen: Musiklautstärke).
//   r        Fläche des Reglers in Pixeln. Die Schiene liegt waagerecht in
//            der Mitte, ein runder Griff zeigt den Wert. Die ganze Fläche
//            ist anklickbar: Klick springt dorthin, Halten zieht weiter.
//   value    der Wert; wird beim Ziehen verändert (bleibt immer 0..1)
//   dragging gehört dem Aufrufer und muss über die Frames erhalten bleiben
//            (wie selected bei ButtonList): true, solange der Zeiger den
//            Regler zieht. So folgt der Griff auch dann noch, wenn der
//            Zeiger beim Ziehen über den Rand der Fläche hinausgeht.
//   focused  per Tastatur ausgewählt (Griff und Rahmen orange)
//   step     Raster, auf das beim Ziehen gerundet wird (0,05 = 5-%-Schritte;
//            0 = stufenlos)
// Liefert true, wenn sich value in diesem Frame geändert hat. Wann das
// Ziehen endet, sieht der Aufrufer daran, dass dragging von true auf false
// wechselt (z. B. um erst dann zu speichern). Tastatur (Pfeiltasten) wertet
// Slider NICHT aus – das macht der Aufrufer, genau wie bei Button.
bool Slider(Rectangle r, float& value, bool& dragging, bool focused, float step = 0.05f);

// Umschalter: ein Schalter mit "AN" bzw. "AUS" (Optionen: Musik an/aus).
// on = aktueller Zustand, nur zum Zeichnen. Liefert true beim Anklicken; den
// Zustand ändert der Aufrufer selbst (on = !on), genau wie bei Button.
// size = Schrifthöhe von "AN"/"AUS"; focused = per Tastatur ausgewählt.
bool Toggle(Rectangle r, bool on, float size, bool focused);

// Senkrechte Knopfliste mit Tastatur-, Maus- und Controllersteuerung. Liefert gewählten Index oder -1.
// selected: per Tastatur markierter Eintrag; gehört dem Aufrufer und muss
// über die Frames hinweg erhalten bleiben (z. B. menuSel_ in main.cpp).
// enabled: pro Eintrag, ob wählbar (fehlende Einträge gelten als wählbar).
// cx = Mitte, top = Oberkante des ersten Knopfs, height/gap = Knopfhöhe und
// Abstand, alles in Pixeln. Steuerung: Pfeiltasten oder W/S, Enter/Leertaste
// oder Mausklick; Controller: Steuerkreuz hoch/runter wählt, A bestätigt
// (oder klickt den Knopf unter dem Controller-Zeiger).
int ButtonList(const std::vector<std::string>& items, const std::vector<bool>& enabled, int& selected,
               float cx, float top, float width, float height, float gap, float textSize);

// ---------------------------------------------------------------------------
// Einzeiliges Texteingabefeld (Mehrspieler: Serveradresse, Name, Passwort).
//
// Wie die anderen Bausteine hier ist es "immediate mode": Es gibt kein
// Ereignissystem, sondern der Aufrufer ruft jedes Frame Draw() auf. Draw()
// zeichnet das Feld UND verarbeitet dabei Maus und Tastatur:
//   - Klick ins Feld gibt ihm den Fokus, Klick daneben nimmt ihn weg.
//     Mit Tab wechselt der Aufrufer zwischen mehreren Feldern (CycleFocus).
//   - Getippte Zeichen kommen aus raylibs Zeichen-Warteschlange
//     (GetCharPressed). Das sind Unicode-"Codepoints" (z. B. 'ä' = 228); sie
//     werden als UTF-8 in text gespeichert, so wie alle Texte im Spiel.
//   - Rücktaste löscht ein Zeichen, gedrückt halten wiederholt das Löschen.
//   - Strg+V fügt Text aus der Zwischenablage ein.
// Nur das Feld mit Fokus liest die Tastatur. Solange eines den Fokus hat,
// liefert TextInputActive() true – main.cpp benutzt das, damit z. B. die
// Taste M beim Tippen eines Namens nicht den Ton ausschaltet.
// Die TextField-Objekte selbst leben beim Aufrufer (Multiplayer.cpp), weil
// Inhalt und Fokus über viele Frames erhalten bleiben müssen.
// ---------------------------------------------------------------------------
struct TextField {
    std::string text;                          // Inhalt (UTF-8)
    std::string placeholder;                   // grauer Hinweis, solange das Feld leer ist
    int maxChars = 64;                         // Höchstlänge in Zeichen (nicht in Bytes)
    bool password = false;                     // Inhalt als **** anzeigen
    bool focused = false;                      // bekommt die Tastatureingaben
    bool (*accept)(int codepoint) = nullptr;   // optionaler Filter: welche Zeichen sind erlaubt?

    // Zeichnet das Feld im Rechteck r und verarbeitet die Eingaben.
    // enabled = false: nur Anzeige (z. B. während die Anmeldung läuft).
    // Liefert true, wenn das Feld den Fokus hat und Enter gedrückt wurde.
    bool Draw(Rectangle r, float textSize, bool enabled = true);

    double lastEdit = -10.0;                   // intern: Cursor nach dem Tippen sofort sichtbar
    float repeatT = 0.0f;                      // intern: Wiederholung der Rücktaste
};

// Tab / Umschalt+Tab: Fokus zum nächsten / vorigen Feld der Liste.
// Hat keines den Fokus, bekommt ihn das erste.
void CycleFocus(const std::vector<TextField*>& fields);

// Hatte im letzten Frame ein Textfeld den Fokus? (Dann sind Tasten Text, keine Befehle.)
bool TextInputActive();

// Anzahl Zeichen (Codepoints) eines UTF-8-Texts
int Utf8Length(const std::string& s);

std::string Num(long long v);          // 100.000 (deutsche Tausenderpunkte)
std::string Time(float seconds);       // 12:34, ab einer Stunde 1:02:03

// Wurde in diesem Frame irgendeine Taste, Maustaste oder ein Controller-Knopf
// gedrückt? (z. B. "beliebige Taste: zurück zum Menü" in der Simulation)
bool AnyKeyOrClick();

} // namespace ui
