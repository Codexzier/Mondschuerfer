#pragma once
// ---------------------------------------------------------------------------
// TiltShift.hpp – Kameraeffekt "Tilt-Shift" (Umsetzung: TiltShift.cpp).
//
// Wofür ist die Datei da?
//   Tilt-Shift lässt die Spielwelt wie ein Miniaturmodell wirken: Ein
//   waagerechter Streifen in der Bildmitte bleibt scharf, nach oben und unten
//   wird das Bild zunehmend unscharf.
//   Die Stärke stellt man in den Optionen ein (0 = aus); sie steht in
//   g_clientConfig.tiltShift und wird von dort jedes Frame gelesen.
//
// Wie funktioniert das?
//   Begin() lenkt das Zeichnen der 3D-Welt in ein eigenes Bild
//   (RenderTexture) um, End() zeichnet dieses Bild dann mit einem Shader
//   auf den Bildschirm, der abseits der Mitte weichzeichnet und Kanten per
//   FXAA glättet (das Zwischenbild hat keine MSAA-Kantenglättung). HUD und Menüs
//   kommen erst danach und bleiben deshalb scharf.
//   Ist der Effekt aus (oder der Shader nicht verfügbar), tun Begin() und
//   End() nichts – es wird dann wie bisher direkt auf den Bildschirm gezeichnet.
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

// Shader und Bild freigeben (vor CloseWindow).
void Shutdown();

} // namespace tiltshift
