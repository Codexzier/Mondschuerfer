# Mondschürfer

Ein kleines 3D-Weltraumspiel in C++ (raylib), gebaut ausschließlich mit den Modellen
aus dem **Kenney Space Kit** und vertont mit den Kenney-Paketen **Sci-Fi Sounds** und
**Interface Sounds**. Du fliegst ein Raumschiff über die Mondoberfläche, suchst
mit dem Scanner nach Kristallen, baust sie ab, verkaufst sie an der Handelsstation und
rüstest dein Schiff in der Werft auf. Piraten machen dir dabei das Leben schwer.

**Ziel:** 100.000 Ressourcenpunkte auf dem Konto.

![Spielszene](docs/spielszene.png)
![Voll ausgebaute Handelsstation](docs/station_ausgebaut.png)

---

## Bauen mit CLion

Das Projekt ist ein normales CMake-Projekt. CLion erkennt es automatisch.
raylib 5.5 wird beim ersten Laden des Projekts automatisch von GitHub geladen und mitgebaut
(einmalig Internet nötig, danach nicht mehr).

### Windows

1. Ordner `Mondschuerfer` in CLion öffnen (*File → Open*).
2. Toolchain: die mitgelieferte **MinGW**-Toolchain von CLion reicht (Standard).
   Visual Studio (MSVC) funktioniert ebenfalls.
3. Warten, bis CMake fertig ist, dann die Konfiguration **Mondschuerfer** starten (▶).

Die `.exe` ist statisch gelinkt und läuft auch außerhalb von CLion, solange der Ordner
`assets` daneben liegt (wird beim Bauen automatisch kopiert). Bei MinGW-Toolchains, die
keinen vollständig statischen Link erlauben (z. B. die mit CLion mitgelieferte GCC 15),
werden zusätzlich `libwinpthread-1.dll` und `libgcc_s_seh-1.dll` neben die `.exe` kopiert.
Diese gehören dann beim Weitergeben dazu.

### Linux (Pop!_OS, Ubuntu, Debian)

1. Entwicklerpakete installieren (für raylib/GLFW):
   ```bash
   sudo apt install build-essential cmake git libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
   ```
2. Ordner `Mondschuerfer` in CLion öffnen.
3. Warten, bis CMake fertig ist, dann **Mondschuerfer** starten (▶).

### Ohne CLion (Kommandozeile)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/Mondschuerfer        # Windows: build\Mondschuerfer.exe
```

---

## Steuerung

| Taste | Aktion |
|---|---|
| W / S oder ↑ / ↓ | Schub vorwärts / bremsen, rückwärts |
| A / D oder ← / → | Drehen |
| Leertaste | Schießen (2 Schuss pro Sekunde) |
| E | Scanner an / aus |
| F | Abbauen (an Lagerstätte oder Wrack) / Andocken (an Station) |
| R | Schild reparieren (an der Handelsstation) |
| 1 – 4 | Upgrade kaufen (in der Werft) |
| Mausrad | Zoom |
| ESC / P | Pause |
| H | Steuerung einblenden |
| M | Ton an / aus |
| F11 | Vollbild |

Ein Gamepad wird ebenfalls unterstützt (linker Stick, A = Feuer, X = Scanner, Y = Abbauen/Andocken).

---

## Spielregeln

**Startmenü:** Neues Spiel · Fortsetzen · Simulation (spielt selbst) · Beenden

- **Scanner (E):** Kristall-Lagerstätten sind unsichtbar, bis der Scanner sie erfasst.
  Der Scanner hat 10 Energieeinheiten und verbraucht 1 pro Sekunde, er läuft also höchstens
  10 Sekunden und schaltet sich dann ab. Ausgeschaltet lädt er 1 Einheit pro Sekunde nach.
  Entdeckte Lagerstätten bleiben markiert (grüner Lichtstrahl, grüne Punkte auf dem Radar).
- **Abbau (F):** In der Nähe einer entdeckten Lagerstätte F drücken. Nach **5 Sekunden** ist das
  Lager gefüllt, sofern die Lagerstätte genug hergibt. Weiter draußen liegen ergiebigere Vorkommen.
- **Handelsstation:** Andocken (F) verkauft die komplette Ladung, 1 Ressource = 1 Punkt.
  Dort kann auch der Schild repariert werden (2 Punkte pro Schildpunkt).
  Je mehr Ressourcen insgesamt abgeliefert wurden, desto größer wird die Station:
  8 Ausbaustufen mit Korridoren, Hangars, Kuppeln, Antennen, Geschütztürmen und Raketen.
- **Werft:** Upgrades für Antrieb (Tempo), Panzerung (maximaler Schild), Bewaffnung
  (Schaden und Feuerrate) und Lager (Kapazität), jeweils 5 Stufen. Bezahlt wird mit
  Ressourcenpunkten, das senkt also den Kontostand.
- **Kampf:** Leertaste feuert 2 Schuss pro Sekunde mit je 10 Schaden. Dein Schild hat zu Beginn
  100 Punkte: nach 10 Treffern stürzt das Schiff ab, und das Spiel ist zu Ende.
  - *Piraten* (rötlich) greifen an, sobald du in ihre Nähe kommst.
  - *Händler/Miner* bauen selbst Kristalle ab und wehren sich nur, wenn du sie angreifst.
  - Abgeschossene Schiffe stürzen ab und hinterlassen ein **Wrack**, das du wie eine
    Lagerstätte abbauen kannst (orangefarbener Lichtstrahl).
  - Die Geschütztürme der Stationen schützen dich vor Piraten.
- **Fortsetzen:** Das Spiel speichert automatisch beim Andocken und beim Schließen des Fensters,
  außerdem über *Pause → Speichern & zum Hauptmenü*. Der Spielstand liegt als
  `mondschuerfer_spielstand.txt` neben der ausführbaren Datei. Nach Sieg oder Absturz wird er gelöscht.
- **Simulation:** Ein Autopilot spielt selbst: scannen, abbauen, verkaufen, kämpfen, aufrüsten.
  Eine beliebige Taste führt zurück ins Menü.

Alle Balancing-Werte (Preise, Upgrades, Gegner, Ausbaustufen ...) stehen in `src/Config.hpp`.

---

## Ton

Schüsse, Treffer, Explosionen, Scanner, Abbau, Andocken, Stationen und Menüs haben eigene
Soundeffekte. Triebwerk, Schub, Scanner und Abbau laufen als Dauergeräusch, bei kritischem
Schild ertönt ein Warnton. Geräusche anderer Schiffe werden mit der Entfernung leiser und
kommen von links oder rechts. Die Hintergrund-Simulation im Hauptmenü bleibt stumm.

Welche Datei zu welchem Ereignis gehört, steht in der Tabelle `kSfx` (und `kLoops`) in
`src/Audio.cpp`. Dort lassen sich Dateien, Lautstärke und Tonhöhen-Variation anpassen.
Einträge mit dem Kommentar *Notlösung* sind mit dem nächstbesten Ton belegt, weil die beiden
Pakete nichts wirklich Passendes enthalten. Ohne Audiogerät läuft das Spiel stumm weiter.

---

## Projektstruktur

```
Mondschuerfer/
├── CMakeLists.txt        CMake-Projekt (lädt raylib 5.5 automatisch)
├── assets/
│   ├── models/*.glb      Alle 153 Modelle des Kenney Space Kit (unverändert)
│   ├── sounds/scifi/     Kenney Sci-Fi Sounds (73 Dateien, unverändert)
│   ├── sounds/interface/ Kenney Interface Sounds (100 Dateien, unverändert)
│   └── Kenney_*_License.txt
└── src/
    ├── main.cpp          Fenster, Menüs, Spielzustände, Kamera
    ├── Config.hpp        Balancing
    ├── World.hpp/.cpp    Spiellogik: Flug, Scanner, Abbau, Kampf, Stationen
    ├── WorldGen.cpp      Mondoberfläche, Stationsaufbau (alle Ausbaustufen), Lagerstätten
    ├── Ai.cpp            Verhalten von Piraten und Händlern, Autopilot für die Simulation
    ├── Render.cpp        3D-Darstellung und Effekte
    ├── Hud.cpp/.hpp      HUD, Radar, Stations- und Endbildschirme
    ├── Ui.cpp/.hpp       UI-Bausteine
    ├── Assets.cpp/.hpp   Modelle laden, Beleuchtungs-Shader
    ├── Audio.cpp/.hpp    Soundeffekte, Dauergeräusche, Zuordnung Ereignis -> Datei
    └── Save.cpp/.hpp     Spielstand
```

**Technischer Hinweis:** Die GLB-Dateien des Space Kits enthalten einen Knoten, den der
glTF-Parser von raylib ablehnt. `Assets.cpp` korrigiert das beim Laden im Speicher, die
Dateien selbst bleiben unverändert. Die orangefarbenen Gesteinsfarben des Kits werden beim
Laden in Mondgrau umgefärbt.

### Kommandozeilen-Optionen (nur für Tests)

`--mode sim|game|dock0|dock1|pause|continue`, `--seed N`, `--steps N` (Simulation beschleunigen),
`--report` (Fortschritt auf der Konsole), `--screenshot datei.png --shot-at Sekunden`,
`--quit-after Sekunden`. Im normalen Spiel werden sie nicht gebraucht.

---

## Lizenzen

- Grafik: **Kenney Space Kit** von Kenney (www.kenney.nl), CC0 1.0 (siehe `assets/Kenney_SpaceKit_License.txt`)
- Ton: **Kenney Sci-Fi Sounds** und **Kenney Interface Sounds** von Kenney (www.kenney.nl), CC0 1.0
  (siehe `assets/Kenney_SciFiSounds_License.txt` und `assets/Kenney_InterfaceSounds_License.txt`)
- Bibliothek: **raylib** von Ramon Santamaria, zlib-Lizenz
