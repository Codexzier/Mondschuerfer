#pragma once
// ---------------------------------------------------------------------------
// Util.hpp – kleine Hilfsfunktionen, die Client UND Server brauchen.
//
// Wofür ist die Datei da?
//   - Fmt: Text wie mit printf zusammensetzen ("+%d Ressourcen geladen").
//   - ForwardFromYaw, YawTowards, AngleDiff, Dist2D: Richtungen, Winkel und
//     Abstände auf der Bodenebene. Damit lenken die KI (shared/Ai.cpp), die
//     Geschütztürme und die Schiffsphysik (shared/World.cpp).
//   - ToLowerAscii: Namen ohne Rücksicht auf Groß/Klein vergleichen.
//
// Wer benutzt sie?
//   Praktisch alle Teile: die Spiellogik (World.hpp bindet die Datei ein,
//   dadurch ist sie überall verfügbar, wo die Welt bekannt ist),
//   GameSettings.cpp, der Server (server/src/GameServer.cpp, Accounts.cpp,
//   WebServer.cpp, LoadTest.cpp) und der Client (src/NetClient.cpp, Hud.cpp,
//   Assets.hpp ...). Umsetzung der nicht-inline Funktionen: Util.cpp.
//
// Abhängigkeiten: raylib.h (nur Datentypen wie Vector3/Color und die
// Konstante PI) und raymath.h (Rechenfunktionen wie Vector2Distance).
//
// Warum nicht einfach die raylib-Funktionen (z. B. TextFormat)? Der Server
// läuft ohne Fenster und ohne raylib-Bibliothek. Er bindet nur die Header
// raylib.h (Datentypen wie Vector3, Color) und raymath.h (Rechenfunktionen,
// komplett im Header definiert) ein. Jede raylib-FUNKTION, die in shared/
// aufgerufen würde, ließe sich im Server nicht linken.
//
// Koordinaten im ganzen Spiel: Der Mond ist die x/z-Ebene, y zeigt nach oben
// (Höhe). -Z ist "Norden" (oben auf der Karte), +X ist "Osten". Ein
// Drehwinkel (yaw) von 0 heißt Blick nach Norden; ein größerer Winkel dreht
// das Schiff nach links (von oben gesehen gegen den Uhrzeigersinn). Deshalb
// bedeutet ShipInput::turn = +1 "links drehen".
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include <cmath>
#include <string>

// printf-artige Formatierung in einen std::string (Ersatz für raylibs TextFormat).
// Anders als TextFormat (das in wenige feste, statische Puffer schreibt) ist
// das Ergebnis ein eigener String: beliebig lang, darf aufgehoben werden und
// funktioniert auch, wenn mehrere Threads gleichzeitig formatieren (Server).
// Das __attribute__ sagt GCC/Clang, dass "fmt" ein printf-Formattext ist
// (Parameter 1, Werte ab Parameter 2). Der Compiler warnt dann z. B. bei
// Fmt("%d", 1.5f), statt dass zur Laufzeit Unsinn herauskommt.
std::string Fmt(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Vorwärtsrichtung eines Kit-Raumschiffs (die Nase zeigt im Modell nach -Z).
// yaw = Drehwinkel um die senkrechte Y-Achse im Bogenmaß.
// Herleitung: Der Vektor (0, 0, -1) wird um yaw um die Y-Achse gedreht; das
// ergibt x = -sin(yaw), z = -cos(yaw). Länge immer 1.
inline Vector3 ForwardFromYaw(float yaw) { return {-sinf(yaw), 0.0f, -cosf(yaw)}; }

// Drehwinkel, mit dem ein Schiff bei "from" auf den Punkt "to" schaut.
// Die Umkehrung von ForwardFromYaw: aus -sin(yaw) = dx und -cos(yaw) = dz
// folgt yaw = atan2(-dx, -dz). atan2 liefert -PI..PI und kommt auch mit
// dz = 0 zurecht (anders als atan(dx/dz)).
inline float YawTowards(Vector3 from, Vector3 to) { return atan2f(-(to.x - from.x), -(to.z - from.z)); }

// Kleinster Winkel von a nach b, Ergebnis im Bereich -PI..PI.
// Warum nicht einfach b - a? Winkel "laufen im Kreis": von 170° nach -170°
// sind es nur 20° (nach links), b - a ergäbe aber -340°. Ist das Ergebnis
// positiv, muss nach links gedreht werden (turn > 0), sonst nach rechts.
float AngleDiff(float a, float b);

// Abstand zweier Punkte auf der Bodenebene (Höhe y wird ignoriert).
// Die Spiellogik ist im Grunde zweidimensional: Schiffe fliegen in fester
// Höhe (cfg::SHIP_ALTITUDE), Lagerstätten und Wracks liegen am Boden. Für
// "Wie weit ist das weg?" zählt deshalb nur x/z.
inline float Dist2D(Vector3 a, Vector3 b) { return Vector2Distance({a.x, a.z}, {b.x, b.z}); }

// Kleinbuchstaben (nur ASCII), z. B. für Spielernamen ohne Rücksicht auf Groß/Klein.
// Nur A-Z werden umgewandelt; Umlaute (UTF-8, mehrere Bytes) bleiben, wie
// sie sind: "JÖRG" wird zu "jÖrg", nicht zu "jörg".
// ACHTUNG: Das Ergebnis fließt in den Anmeldeschlüssel der Konten ein
// (src/NetClient.cpp: crypto::DeriveLoginKey mit dem kleingeschriebenen
// Namen; server/src/Accounts.cpp sucht Konten darüber). Wer das Verhalten
// ändert, macht alle gespeicherten Konten unbrauchbar.
std::string ToLowerAscii(const std::string& s);
