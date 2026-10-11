#pragma once
// ---------------------------------------------------------------------------
// Kleine Hilfsfunktionen, die Client UND Server brauchen.
//
// Warum nicht einfach die raylib-Funktionen (z. B. TextFormat)? Der Server
// läuft ohne Fenster und ohne raylib-Bibliothek. Er bindet nur die Header
// raylib.h (Datentypen wie Vector3, Color) und raymath.h (Rechenfunktionen,
// komplett im Header definiert) ein. Jede raylib-FUNKTION, die in shared/
// aufgerufen würde, ließe sich im Server nicht linken.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "raymath.h"
#include <cmath>
#include <string>

// printf-artige Formatierung in einen std::string (Ersatz für raylibs TextFormat).
std::string Fmt(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Vorwärtsrichtung eines Kit-Raumschiffs (die Nase zeigt im Modell nach -Z).
// yaw = Drehwinkel um die senkrechte Y-Achse im Bogenmaß.
inline Vector3 ForwardFromYaw(float yaw) { return {-sinf(yaw), 0.0f, -cosf(yaw)}; }

// Drehwinkel, mit dem ein Schiff bei "from" auf den Punkt "to" schaut.
inline float YawTowards(Vector3 from, Vector3 to) { return atan2f(-(to.x - from.x), -(to.z - from.z)); }

// Kleinster Winkel von a nach b, Ergebnis im Bereich -PI..PI.
float AngleDiff(float a, float b);

// Abstand zweier Punkte auf der Bodenebene (Höhe y wird ignoriert).
inline float Dist2D(Vector3 a, Vector3 b) { return Vector2Distance({a.x, a.z}, {b.x, b.z}); }

// Kleinbuchstaben (nur ASCII), z. B. für Spielernamen ohne Rücksicht auf Groß/Klein.
std::string ToLowerAscii(const std::string& s);
