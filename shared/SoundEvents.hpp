#pragma once
// ---------------------------------------------------------------------------
// SoundEvents.hpp – Geräusch-Ereignisse der Spiellogik.
//
// Die Spiellogik (World) spielt selbst keine Töne ab, sie kennt kein
// Audiogerät. Sie legt nur fest, WAS zu hören sein soll (Sfx) und WO es
// passiert (Position). Wer die Ereignisse abspielt, hängt vom Programm ab:
//   - Einzelspieler: main.cpp gibt sie an das Audio-Modul (src/Audio.cpp).
//   - Server: GameServer schickt sie über das Netzwerk an die Spieler,
//     die in Hörweite sind (oder nur an den betroffenen Spieler).
//   - Mehrspieler-Client: empfängt sie vom Server und spielt sie ab.
//
// Weg eines Geräuschs (Beispiel: ein Pirat schießt):
//   World::FireBullet -> World::SoundAt(Sfx::EnemyShot, Position) hängt ein
//   SoundEvent an World::sounds an.
//   Einzelspieler: main.cpp ruft nach World::Update für jedes Ereignis
//     Audio::PlayAt(ereignis, Kameraziel) auf und leert die Liste. PlayAt
//     macht den Ton leiser, je weiter er vom Zuhörer (Kameraziel) entfernt
//     ist, und verteilt ihn nach links/rechts (Stereo-"pan").
//   Server (server/src/GameServer.cpp): schickt das Ereignis an jeden
//     Spieler im Umkreis von net::AOI_EVENTS (120 Einheiten) bzw. nur an
//     forPlayer, und leert die Liste. Zuschauer bekommen keine Töne.
//   Mehrspieler-Client (src/NetClient.cpp): baut aus der Netzwerknachricht
//     wieder ein SoundEvent; src/Multiplayer.cpp holt sie mit TakeSounds()
//     ab und gibt sie wie im Einzelspieler an Audio::PlayAt.
//
// Die Menütöne (UiMove ... PauseClose) gehören nicht zur Spiellogik; die
// Menüs spielen sie direkt mit Audio::Play ab. Sie stehen trotzdem hier, damit
// alle Töne eine gemeinsame Nummerierung und eine gemeinsame Tabelle in
// src/Audio.cpp haben.
//
// Diese Datei braucht nur Vector3 aus raylib.h (reiner Datentyp), damit sie
// auch im Server ohne Grafik und Ton verwendet werden kann.
// ---------------------------------------------------------------------------
#include "raylib.h"

// Alle Soundeffekte des Spiels. Die Zuordnung zu Dateien steht in src/Audio.cpp.
// Die Reihenfolge ist Teil des Netzwerkprotokolls (als Zahl übertragen):
// neue Einträge daher nur VOR "Count" anhängen.
// Was bei einem neuen Eintrag sonst noch zu tun ist:
//   - in src/Audio.cpp eine Zeile in der Tabelle kSfx ergänzen (ein
//     static_assert dort bricht sonst den Build ab);
//   - net::PROTOCOL_VERSION (Protocol.hpp) erhöhen: Ein älterer Client kennt
//     die neue Nummer nicht und verwirft dann die GANZE Ereignis-Nachricht
//     (Protocol.cpp prüft sfx < Count).
// "Count" ist kein Ton, sondern die Anzahl der Einträge (Trick: der letzte
// Wert einer Aufzählung ohne eigene Zahlen = Anzahl der Werte davor).
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

// Ein Geräusch aus der Spiellogik (erzeugt mit World::SoundAt).
struct SoundEvent {
    Sfx id = Sfx::UiMove;
    Vector3 pos{};        // Ort in der Welt (für Lautstärke nach Entfernung und Links/Rechts)
    float volume = 1.0f;  // 0..1, wird beim Abspielen noch mit der Entfernung abgeschwächt
    // Sekunden bis zur Wiedergabe. Damit folgen Töne kurz aufeinander, ohne
    // dass die Spiellogik warten muss (z. B. "Lager voll" 0,45 s nach dem
    // Abbau-Ende, die Sieges-Fanfare 1,3 s nach dem Verkauf). Im Netz wird
    // der Wert in 1/50-s-Schritten übertragen (höchstens 5,1 s).
    float delay = 0.0f;   // Sekunden bis zur Wiedergabe
    // -1 = für alle in Hörweite, sonst nur für diesen Spieler (PlayerState::id).
    // Beispiele für "nur ein Spieler": Abbau-Töne, Kauf, Verkauf, "Verweigert".
    // Im Einzelspieler spielt es keine Rolle (es gibt nur einen Spieler). Über
    // das Netz wird das Feld nicht geschickt – der Server hat schon gefiltert.
    int forPlayer = -1;
    // Schiff, das das Geräusch verursacht hat (-1 = keins). Wichtig für
    // eigene Schüsse im Mehrspieler: Der Client spielt sie sofort beim
    // Drücken selbst ab (Vorhersage). Der Server schickt einem Spieler deshalb
    // keine PlayerShot-Töne seines eigenen Schiffs, sonst hörte man jeden
    // Schuss doppelt und verspätet.
    int sourceShip = -1;
};
