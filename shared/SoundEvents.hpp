#pragma once
// ---------------------------------------------------------------------------
// Geräusch-Ereignisse der Spiellogik.
//
// Die Spiellogik (World) spielt selbst keine Töne ab, sie kennt kein
// Audiogerät. Sie legt nur fest, WAS zu hören sein soll (Sfx) und WO es
// passiert (Position). Wer die Ereignisse abspielt, hängt vom Programm ab:
//   - Einzelspieler: main.cpp gibt sie an das Audio-Modul (src/Audio.cpp).
//   - Server: GameServer schickt sie über das Netzwerk an die Spieler,
//     die in Hörweite sind (oder nur an den betroffenen Spieler).
//   - Mehrspieler-Client: empfängt sie vom Server und spielt sie ab.
//
// Diese Datei braucht nur Vector3 aus raylib.h (reiner Datentyp), damit sie
// auch im Server ohne Grafik und Ton verwendet werden kann.
// ---------------------------------------------------------------------------
#include "raylib.h"

// Alle Soundeffekte des Spiels. Die Zuordnung zu Dateien steht in src/Audio.cpp.
// Die Reihenfolge ist Teil des Netzwerkprotokolls (als Zahl übertragen):
// neue Einträge daher nur VOR "Count" anhängen.
enum class Sfx {
    // Kampf
    PlayerShot, EnemyShot, TurretShot, HitMetal, ShieldHit, ShipExplode, CrashBoom,
    // Scanner
    ScanOn, ScanOff, ScanEmpty, ScanPing, Discover,
    // Abbau
    MineStart, MineDone, MineAbort, CargoFull,
    // Stationen
    Dock, Undock, Sale, Repair, Upgrade, StationGrow, Victory, Denied,
    // Warnung
    LowShield,
    // Menüs
    UiMove, UiConfirm, UiBack, PauseOpen, PauseClose,
    Count
};

// Ein Geräusch aus der Spiellogik.
struct SoundEvent {
    Sfx id = Sfx::UiMove;
    Vector3 pos{};
    float volume = 1.0f;
    float delay = 0.0f;   // Sekunden bis zur Wiedergabe
    int forPlayer = -1;   // -1 = für alle in Hörweite, sonst nur für diesen Spieler (PlayerState::id)
    int sourceShip = -1;  // Schiff, das das Geräusch verursacht hat (-1 = keins)
};
