#pragma once
// ---------------------------------------------------------------------------
// Effects.hpp – Grafikeffekte der 3D-Welt, die man unter Optionen → Grafik
// einstellt (Umsetzung: Effects.cpp).
//
// Wofür ist die Datei da?
//   - Schatten (Schattenkarte, g_clientConfig.shadows): Schiffe und andere
//     bewegte Objekte (Rover, Züge, Raketen, Personen) werfen echte Schatten
//     auf Boden, Stationen und Felsen. Dazu wird die Szene vor dem
//     eigentlichen Bild noch einmal aus Sicht der Sonne gezeichnet, aber nur
//     die Tiefe (Entfernung zur Sonne) jedes Bildpunkts gespeichert – die
//     "Schattenkarte". Beim normalen Zeichnen prüft der Beleuchtungs-Shader
//     (Assets.cpp) für jeden Bildpunkt, ob zwischen ihm und der Sonne etwas
//     Näheres in der Karte steht; dann liegt er im Schatten.
//   - Sandsturm (g_clientConfig.sandstorm): Nebel in Sandfarbe, der sich um
//     das eigene Schiff schließt. Nur ein kleiner Kreis um das Schiff bleibt
//     klar, weiter weg verschwinden Schiffe, Felsen und Leuchteffekte im
//     Staub. Der Nebel wogt in Schwaden (Rauschen, vom Wind verschoben), dazu
//     fliegen Sandkörner durchs Bild (Render.cpp).
//   - Bildblitz: eine Explosion nahe am eigenen Schiff hellt das ganze Bild
//     der 3D-Welt kurz auf (Teil von "Treffer-Aufblitzen", Render.cpp).
//
// Wer benutzt sie?
//   - src/Render.cpp (World::Draw): Schattendurchgang, Nebel einstellen,
//     Effekt-Shader für die durchsichtigen Effekte, Bildblitz melden.
//   - src/TiltShift.cpp: Hintergrundfarbe im Sandsturm, Bildblitz.
//   - src/Multiplayer.cpp: Namen über Schiffen im Sandsturm ausblenden.
//   - src/main.cpp: Shutdown() vor CloseWindow().
//
// Abhängigkeiten: raylib/rlgl (Framebuffer, Tiefentextur, Shader),
// Assets.hpp (Uniforms des Beleuchtungs-Shaders), ClientConfig.hpp.
// Läuft im Spiel-Thread (dem Thread mit dem OpenGL-Kontext).
// ---------------------------------------------------------------------------
#include "raylib.h"

namespace fx {

// Texturplatz, auf dem die Schattenkarte beim Zeichnen liegt. Weit oben,
// damit er sich nicht mit den Plätzen der Materialien (0..11, raylib) oder
// des Render-Batches (0..3) überschneidet.
constexpr int kShadowSlot = 15;

// GLSL-Text der Nebelrechnung, wird in den Beleuchtungs-Shader (Assets.cpp)
// und den Effekt-Shader (Effects.cpp) eingefügt. FogAmount(p) liefert für
// einen Weltpunkt p, wie stark er im Sandsturm verschwindet (0 = klar,
// 1 = ganz im Staub).
//   fogCenter  Mitte der klaren Zone (Blickpunkt der Kamera, das Schiff)
//   fogParams  x = Radius der klaren Zone, y = ab hier volle Deckkraft,
//              z = Deckkraft 0..1 (0 = kein Nebel), w = Zeit (Wind)
// Das Rauschen ("value noise"): Auf einem Gitter bekommt jeder Gitterpunkt
// eine Pseudozufallszahl (FogHash, ohne sin, damit es auch bei großen
// Koordinaten genau bleibt), dazwischen wird weich übergeblendet. Zwei
// Lagen in verschiedener Größe ergeben Schwaden; der Wind schiebt sie mit
// der Zeit schräg über die Karte.
inline constexpr const char* kFogGlsl = R"(
uniform vec3 fogCenter;
uniform vec4 fogParams;
float FogHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float FogNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(FogHash(i), FogHash(i + vec2(1.0, 0.0)), u.x),
               mix(FogHash(i + vec2(0.0, 1.0)), FogHash(i + vec2(1.0, 1.0)), u.x), u.y);
}
float FogAmount(vec3 p) {
    if (fogParams.z <= 0.0) return 0.0;
    vec2 q = p.xz * 0.045 + vec2(0.9, 0.55) * fogParams.w * 0.35;
    float n = FogNoise(q) * 0.65 + FogNoise(q * 2.7 + vec2(3.1, 7.7)) * 0.35;
    float dist = length(p.xz - fogCenter.xz) + (n - 0.5) * fogParams.y * 0.4;
    float f = smoothstep(fogParams.x, fogParams.y, dist);
    // auch nah am Schiff ein leichter Schleier, der mit den Schwaden wogt
    f = max(f, 0.18 * n);
    return clamp(f, 0.0, 1.0) * fogParams.z;
}
)";

// --- Schatten ----------------------------------------------------------------
// Am Anfang von World::Draw (zwischen BeginMode3D und dem ersten Modell):
// Ist die Option an, wird ab hier in die Schattenkarte gezeichnet, mit der
// Sonne als Kamera. Mittelpunkt und Größe des abgedeckten Gebiets ergeben
// sich aus cam (der Bildausschnitt). Liefert false, wenn keine Schatten
// gezeichnet werden (Option aus, Grafikkarte kann es nicht, Übersicht).
// Dann gilt im Shader "kein Schatten" und EndShadowPass() entfällt.
bool BeginShadowPass(const Camera3D& cam, float drawRange);
// Nach den Schattenwerfern: zurück zum vorherigen Bild und zur Kamera, die
// Schattenkarte an den Beleuchtungs-Shader geben.
void EndShadowPass();

// --- Sandsturm -----------------------------------------------------------------
// Einmal pro World::Draw: stellt den Nebel für dieses Bild ein (amount 0 =
// aus, sonst 0..1 aus den Optionen), center = Mitte der klaren Zone.
void SetFog(Vector3 center, float amount, float time);
// Wie sichtbar ein Punkt im Sandsturm dieses Bildes ist (1 = klar, 0 = im
// Staub verschwunden; ohne die Schwaden). Ohne Sandsturm immer 1.
float Visibility(Vector3 p);
// Farbe des Staubs (passend zur Mondfarbe, Assets::GetMoonColor).
Color SandColor();
// Durchsichtige Effekte (Render-Batch: Ringe, Strahlen, Partikel) zwischen
// BeginEffects und EndEffects verblassen im Sandsturm wie die Modelle. Ohne
// Sandsturm tun beide nichts.
void BeginEffects();
void EndEffects();

// --- Bildblitz -------------------------------------------------------------------
// Render.cpp meldet je Bild, wie stark eine nahe Explosion das Bild aufhellt
// (0..1); TiltShift.cpp liest den Wert beim Zeichnen des Bildes.
void SetScreenFlash(float v);
float ScreenFlash();

// Schattenkarte und Shader freigeben (vor CloseWindow).
void Shutdown();

} // namespace fx
