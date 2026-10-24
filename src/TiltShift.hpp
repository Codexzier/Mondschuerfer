#pragma once
// ---------------------------------------------------------------------------
// TiltShift.hpp – Kameraeffekte der 3D-Welt: Tilt-Shift, Kantenglättung
// (MSAA 2x bis 16x) und Glätten per Shader (FXAA) (Umsetzung: TiltShift.cpp).
//
// Wofür ist die Datei da?
//   Tilt-Shift lässt die Spielwelt wie ein Miniaturmodell wirken: Ein
//   waagerechter Streifen in der Bildmitte bleibt scharf, nach oben und unten
//   wird das Bild zunehmend unscharf.
//   Die Stärke stellt man in den Optionen ein (0 = aus); sie steht in
//   g_clientConfig.tiltShift und wird von dort jedes Frame gelesen. Ebenso
//   die Kantenglättung (g_clientConfig.msaa: 0, 2, 4, 8, 16 Abtastpunkte) und
//   die FXAA-Stärke (g_clientConfig.fxaa 0..1, 0 = aus). Beides lässt sich
//   gleichzeitig einschalten. Dazu der Stil-Look "Retro-Pixel"
//   (g_clientConfig.pixelLook) sowie Bildblitz und Hintergrund im
//   Sandsturm (Effects.hpp).
//
// Wie funktioniert das?
//   Begin() lenkt das Zeichnen der 3D-Welt in ein eigenes Bild um (mit
//   Kantenglättung: ein MSAA-Bild), End() zeichnet dieses Bild dann mit
//   einem Shader auf den Bildschirm, der abseits der Mitte weichzeichnet und
//   Kanten per FXAA glättet. HUD und Menüs kommen erst danach und bleiben
//   deshalb scharf (sie nutzen weiter die 4-fache Kantenglättung des
//   Fensters, siehe main.cpp).
//   Ist der Shader nicht verfügbar, tun Begin() und End() nichts – es wird
//   dann direkt auf den Bildschirm gezeichnet (mit der MSAA des Fensters).
//
// Wer benutzt sie?
//   src/main.cpp (Einzelspieler und Hauptmenü) und src/Multiplayer.cpp
//   (Spielen, Zuschauen, Menü-Hintergrund), jeweils um BeginMode3D/EndMode3D:
//       tiltshift::Begin(Hintergrundfarbe);
//       BeginMode3D(cam); ... EndMode3D();
//       tiltshift::End();
//   main() ruft Shutdown() vor CloseWindow() auf.
//
// Abhängigkeiten: raylib (RenderTexture, Shader), ClientConfig.hpp.
// Läuft im Spiel-Thread.
// ---------------------------------------------------------------------------
#include "raylib.h"

namespace tiltshift {

// Vor BeginMode3D: Ist der Effekt an, wird ab hier in das eigene Bild
// gezeichnet; clear = Hintergrundfarbe, mit der dieses Bild gelöscht wird.
void Begin(Color clear);

// Nach EndMode3D: zeichnet das Bild mit Weichzeichner auf den Bildschirm
// (nur wenn Begin() umgelenkt hat).
void End();

// true zwischen Begin() und End(), wenn umgelenkt wurde. Render.cpp zeichnet
// den Sandsturm nur dann (also nicht im Karteneditor, der ohne
// Kameraeffekte zeichnet).
bool Active();

// Höchste MSAA-Stufe, die die Grafikkarte kann (0 = keine). Für die Anzeige
// in den Optionen; höhere Einstellungen werden darauf begrenzt.
int MaxSamples();

// Shader und Bilder freigeben (vor CloseWindow).
void Shutdown();

} // namespace tiltshift
