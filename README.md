# Mondschürfer

Ein kleines 3D-Weltraumspiel in C++ (raylib), gebaut ausschließlich mit den Modellen
aus dem **Kenney Space Kit** und vertont mit den Kenney-Paketen **Sci-Fi Sounds** und
**Interface Sounds**; die Musik stammt von **dekker187**. Du fliegst ein Raumschiff über die Mondoberfläche, suchst
mit dem Scanner nach Kristallen, baust sie ab, verkaufst sie an der Handelsstation und
rüstest dein Schiff in der Werft auf. Piraten machen dir dabei das Leben schwer.

**Ziel:** 100.000 Ressourcenpunkte auf dem Konto.

Neben dem Einzelspieler gibt es einen **Mehrspieler**: Mehrere Piloten schürfen gleichzeitig auf
einem gemeinsamen Mond, den ein eigener Server rechnet (z. B. als Docker-Container auf einem
Raspberry Pi oder Heimserver). Mit Konten, Bestenliste, Zuschauermodus und einer kleinen
Verwaltungs-Webseite für den Betreiber. Siehe [Mehrspieler](#mehrspieler).

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

### Was gebaut wird: CMake-Optionen

| Option | Standard | Baut |
|---|---|---|
| `MONDSCHUERFER_CLIENT` | `ON` | das Spiel `Mondschuerfer` (Einzelspieler **und** Mehrspieler-Client), braucht raylib |
| `MONDSCHUERFER_SERVER` | `OFF` | den Mehrspieler-Server `mondschuerfer-server` und das Messwerkzeug `mondschuerfer-loadtest` (ohne Grafik, ohne raylib-Bibliothek) |

Beide lassen sich kombinieren, z. B. Spiel und Server in einem Build-Ordner:
`cmake -S . -B build -DMONDSCHUERFER_SERVER=ON`. In CLion trägt man die Option unter
*Settings → Build, Execution, Deployment → CMake → CMake options* ein.

Die Netzwerkbibliothek **ENet** und der Webserver **cpp-httplib** liegen im Projekt unter
`third_party/` und werden mitgebaut; dafür ist kein Download nötig. Heruntergeladen wird nur
raylib für das Spiel (falls es nicht installiert ist).

### Mehrspieler-Server ohne Docker bauen

Der Server braucht nur einen C++17-Compiler und CMake (≥ 3.16), kein raylib und kein Internet:

```bash
cmake -S . -B build/srv -DMONDSCHUERFER_CLIENT=OFF -DMONDSCHUERFER_SERVER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/srv --target mondschuerfer-server mondschuerfer-loadtest -j

# Starten. Die Webseite ist so nur auf diesem Rechner erreichbar. Ohne ADMIN_USER/ADMIN_PASSWORD
# (Passwort mind. 10 Zeichen) startet nur das Spiel, nicht die Webseite.
ADMIN_USER=mondverwalter ADMIN_PASSWORD='hier-ein-langes-passwort' \
    ./build/srv/server/mondschuerfer-server --data ./data --web-bind 127.0.0.1
```

`mondschuerfer-server --help` zeigt alle Optionen. Betrieb mit Docker, Einstellungen, Ports,
Sicherheit und Datensicherung: [server/README.md](server/README.md).

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

**Im Mehrspieler** gelten dieselben Tasten. Unterschiede:

| Taste | Aktion im Mehrspieler |
|---|---|
| ESC / P | Pause mit *Weiterspielen*, *Optionen* oder *Abmelden & zum Hauptmenü* (der Server speichert dann deinen Fortschritt). Die Welt läuft während der Pause (und in den Optionen) weiter, dein Schiff bekommt nur keine Steuerbefehle. |
| ESC / F im Stationsfenster | Abdocken |
| Tab | im Anmeldefenster zwischen Name und Passwort wechseln |
| Zuschauer: Mausrad / Ziehen / R / ESC | Zoom / Karte verschieben / ganze Karte / Zuschauen beenden |

---

## Spielregeln

**Startmenü:** Neues Spiel · Fortsetzen · Mehrspieler · Simulation (spielt selbst) · Optionen · Beenden

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

Alle Balancing-Werte (Preise, Upgrades, Gegner, Ausbaustufen ...) stehen in `shared/Config.hpp`
(sie gelten für Einzelspieler und Server gleichermaßen).

---

## Mehrspieler

Im Mehrspieler fliegen mehrere Piloten über denselben Mond. Die Welt rechnet ein **Server**; das
Spiel schickt ihm nur deine Tasten und zeigt, was er zurückmeldet (dein eigenes Schiff sagt es
dabei selbst voraus, damit sich die Steuerung direkt anfühlt).

### Mitspielen

1. **Hauptmenü → Mehrspieler.**
2. **Serveradresse** eingeben: `localhost`, eine LAN-Adresse wie `192.168.1.20` oder ein Name wie
   `mein-server.de`, optional mit Port (`:7777` ist Standard). → **Verbinden**.
   Das Spiel merkt sich Adresse und Spielernamen in `mondschuerfer_client.txt` neben der
   ausführbaren Datei (das Passwort nie; siehe [Musik und Optionen](#musik-und-optionen)).
3. In der **Lobby** stehen Servername, Spieler online und die Einstellungen des Servers.
   Dann wählen:
   - **Zuschauen** – ohne Konto die ganze Karte von oben ansehen, mit Spielerliste und
     Bestenliste (bis zu 16 Zuschauer).
   - **Mitspielen** – Name und Passwort eingeben:
     - **Neues Konto anlegen** beim ersten Mal. Name: 3–16 Zeichen, nur Buchstaben (A–Z),
       Ziffern, `_` und `-`; jeden Namen gibt es pro Server nur einmal (Groß/Klein egal).
       Passwort: mindestens 6 Zeichen.
     - **Anmelden** bei jedem weiteren Mal – du spielst mit deinem letzten Stand weiter.
4. **Abmelden:** ESC → *Abmelden & zum Hauptmenü*. Der Server speichert deinen Fortschritt.

Das Passwort verlässt deinen Rechner nicht: Das Spiel berechnet daraus einen Schlüssel, und nur
damit meldet es sich an. Die Spielverbindung selbst ist aber **nicht verschlüsselt** – nimm für das
Spiel ein eigenes Passwort, das du sonst nirgends benutzt.

### Regeln im Mehrspieler (kurz)

- Grundregeln wie im Einzelspieler; jeder hat **eigenen** Kontostand, eigene Aufrüstungen und muss
  Lagerstätten **selbst** entdecken. Abbauen kann sie aber jeder – wer zuerst kommt …
- **Absturz:** Nach 4 Sekunden gibt es ein neues Schiff an einer Handelsstation, aber der
  Fortschritt (Kontostand, Aufrüstungen, Runde, Schiffszeit) beginnt **von vorn**.
- **Schiffszeit:** wie lange du schon mit dem aktuellen Schiff fliegst.
- **Ziel erreicht** (100.000 Punkte): Eintrag in die **Bestenliste** (die schnellsten Runden),
  alle Spieler sehen es, und eine **neue Runde** beginnt mit Kontostand 0 – die Aufrüstungen
  behältst du.
- Was alle zusammen abliefern, lässt die Handelsstationen wachsen.
- Die **Pause** hält die Welt nicht an.
- Die Anzahl der Piraten, Stationen, die Mondfarbe und die maximale Spielerzahl stellt der
  Betreiber des Servers ein.

### Einen Server betreiben

Am einfachsten mit Docker: `cp .env.example .env`, darin `ADMIN_PASSWORD` setzen, dann
`docker compose up -d --build`. Die Spieler verbinden sich über **UDP-Port 7777**, die
Verwaltungs-Webseite (Auslastung, Spieler, Bestenliste, Einstellungen) liegt auf
<http://localhost:8080>. Alles Weitere – Einstellungen, Ports und Router, Hardware-Empfehlung,
Sicherheit, Datensicherung, Updates, Fehlersuche, Betrieb ohne Docker –
steht in **[server/README.md](server/README.md)**.

### Wie funktioniert das?

Wer aus dem Code lernen möchte, wie der Mehrspieler aufgebaut ist (geteilte Spiellogik,
Netzwerkprotokoll, Vorhersage und Interpolation, Anmeldung, Threads im Server …), findet eine
ausführliche Erklärung mit Leseempfehlung und Übungen in
**[docs/Mehrspieler.md](docs/Mehrspieler.md)**.

---

## Ton

Schüsse, Treffer, Explosionen, Scanner, Abbau, Andocken, Stationen und Menüs haben eigene
Soundeffekte. Triebwerk, Schub, Scanner und Abbau laufen als Dauergeräusch, bei kritischem
Schild ertönt ein Warnton. Geräusche anderer Schiffe werden mit der Entfernung leiser und
kommen von links oder rechts. Die Hintergrund-Simulation im Hauptmenü bleibt stumm (dort läuft
nur die Menümusik, siehe [Musik und Optionen](#musik-und-optionen)).

Welche Datei zu welchem Ereignis gehört, steht in der Tabelle `kSfx` (und `kLoops`) in
`src/Audio.cpp`. Dort lassen sich Dateien, Lautstärke und Tonhöhen-Variation anpassen.
Einträge mit dem Kommentar *Notlösung* sind mit dem nächstbesten Ton belegt, weil die beiden
Pakete nichts wirklich Passendes enthalten. Ohne Audiogerät läuft das Spiel stumm weiter.

---

## Musik und Optionen

Zwei Musikstücke von **dekker187** (erstellt mit Suno) liegen in `assets/music/`:

| Stück | Datei | Läuft … |
|---|---|---|
| *Main Menu Theme* | `main_menu_theme.mp3` | im Hauptmenü, in den Optionen aus dem Hauptmenü und in den Mehrspieler-Menüs (Serveradresse, Lobby, Anmeldung) |
| *Lunar Silence 01* | `lunar_silence_01.mp3` | im Spiel: Einzelspiel samt Pause und Endbildschirmen, Simulation, Mehrspieler-Spiel und Zuschauen |

Beim Wechsel zwischen Menü und Spiel blendet die Musik weich über (das alte Stück wird in 1 s
leiser, das neue in 1,5 s lauter). Ein Stück, das gerade nicht läuft, wird angehalten und setzt
später an derselben Stelle fort. Beide Stücke laufen in einer Endlosschleife.

**Optionen** (Hauptmenü → *Optionen*, oder in der Pause → *Optionen*):

- **Musik: An / Aus** – Umschalter (Mausklick, oder Zeile wählen und Enter).
- **Lautstärke** – Schieberegler in 5-%-Schritten (mit der Maus ziehen oder ← / → drücken).
  Die Änderung ist sofort zu hören.
- **Steuerung** – Übersicht aller Tasten für Tastatur/Maus, Xbox- und PlayStation-Controller.
- Bedienung mit der Tastatur: ↑ / ↓ Zeile wählen, ← / → Lautstärke, Enter umschalten, ESC zurück.

Die Taste **M** schaltet weiterhin jederzeit den *ganzen* Ton stumm (Effekte und Musik); die
Einstellungen in den Optionen bleiben dabei erhalten.

Die Einstellungen stehen zusammen mit Serveradresse und Spielername in
`mondschuerfer_client.txt` neben der ausführbaren Datei und gelten beim nächsten Start wieder:

```
server localhost:7777
name Anna
music 1
music_volume 0.50
```

`music` ist `1` (an) oder `0` (aus), `music_volume` eine Zahl von `0` bis `1` (Punkt oder Komma).
Fehlt die Datei oder eine Zeile, gelten die Standardwerte (Musik an, 50 %).

Im Code: `src/Options.*` (Bildschirm), `src/Audio.*` (Laden, Abspielen, Überblenden),
`src/MusicMix.hpp` (die Überblend-Rechnung, ohne raylib testbar), `src/ClientConfig.*` (Datei).

---

## Projektstruktur

```
Mondschuerfer/
├── CMakeLists.txt        CMake-Projekt (lädt raylib 5.5 automatisch; Optionen CLIENT/SERVER)
├── docker-compose.yml    Server als Docker-Container betreiben (ausführlich kommentiert)
├── .env.example          Vorlage für .env: Admin-Zugang und Startwerte des Servers
├── .dockerignore         was beim Bauen des Images an Docker geschickt wird
├── assets/
│   ├── models/*.glb      Alle 153 Modelle des Kenney Space Kit (unverändert)
│   ├── sounds/scifi/     Kenney Sci-Fi Sounds (73 Dateien, unverändert)
│   ├── sounds/interface/ Kenney Interface Sounds (100 Dateien, unverändert)
│   ├── music/            Musik von dekker187: main_menu_theme.mp3, lunar_silence_01.mp3
│   └── Kenney_*_License.txt
├── shared/               Gemeinsamer Code für Spiel UND Server (ohne raylib-Funktionen)
│   ├── Config.hpp        Balancing (auch die Mehrspieler-Werte MP_*)
│   ├── World.hpp/.cpp    Spiellogik: Flug, Scanner, Abbau, Kampf, Stationen, Mehrspieler-Regeln
│   ├── WorldGen.cpp      Mondoberfläche, Stationsaufbau (alle Ausbaustufen), Lagerstätten
│   ├── Ai.cpp            Verhalten von Piraten und Händlern, Autopilot für die Simulation
│   ├── GameSettings.*    Spielparameter des Servers (Schwierigkeit, Stationen, Mondfarbe ...)
│   ├── Protocol.*        Netzwerkprotokoll: Nachrichten, Kodieren/Dekodieren, Quantisierung
│   ├── Crypto.*          SHA-256, HMAC, PBKDF2 für die Anmeldung
│   ├── Models.*          Modellnamen <-> Nummern (ModelBounds.inc: Abmessungen der Modelle)
│   ├── SoundEvents.hpp   Geräusch-Ereignisse der Spiellogik
│   └── Util.*            kleine Hilfen (Text, Winkel, Abstände)
├── src/                  Das Spiel (raylib)
│   ├── main.cpp          Fenster, Menüs, Spielzustände, Kamera
│   ├── Render.cpp        3D-Darstellung und Effekte
│   ├── Hud.cpp/.hpp      HUD, Radar, Stations- und Endbildschirme
│   ├── Ui.cpp/.hpp       UI-Bausteine (Knöpfe, Schieberegler, Umschalter, Textfelder)
│   ├── Options.cpp/.hpp  Bildschirm "Optionen": Musik an/aus, Lautstärke, Steuerungsübersicht
│   ├── Assets.cpp/.hpp   Modelle laden, Beleuchtungs-Shader
│   ├── Audio.cpp/.hpp    Soundeffekte, Dauergeräusche, Musik, Zuordnung Ereignis -> Datei
│   ├── MusicMix.hpp      Überblenden der Musik (reine Rechnung, ohne raylib)
│   ├── Save.cpp/.hpp     Spielstand
│   ├── NetClient.*       Mehrspieler: Verbindung, Anmeldung, Vorhersage, Interpolation
│   ├── Multiplayer.*     Mehrspieler: Menüs (Adresse, Lobby, Anmeldung), Spielen, Zuschauen
│   └── ClientConfig.*    Einstellungsdatei: Serveradresse, Spielername, Musik
├── server/               Der Mehrspieler-Server (ohne Grafik), Anleitung: server/README.md
│   ├── src/              main, GameServer, Accounts, SharedState, WebServer, LoadTest
│   ├── web/              Verwaltungs-Webseite (index.html, app.js, style.css)
│   ├── Dockerfile        Container-Image
│   └── Caddyfile         optionaler HTTPS-Vorbau für die Webseite
├── third_party/          Fremde Bibliotheken im Projekt (siehe third_party/README.md)
│   ├── enet/             ENet 1.3.18 (Netzwerk über UDP)
│   ├── httplib/          cpp-httplib 0.22.0 (Webserver der Verwaltungsseite)
│   └── raylib/           nur raylib.h und raymath.h 5.5 (für den Server)
└── docs/
    ├── Mehrspieler.md    Wie der Mehrspieler funktioniert (zum Lernen aus dem Code)
    └── *.png             Bildschirmfotos
```

**Technischer Hinweis:** Die GLB-Dateien des Space Kits enthalten einen Knoten, den der
glTF-Parser von raylib ablehnt. `Assets.cpp` korrigiert das beim Laden im Speicher, die
Dateien selbst bleiben unverändert. Die orangefarbenen Gesteinsfarben des Kits werden beim
Laden in Mondgrau umgefärbt.

### Kommandozeilen-Optionen (nur für Tests)

`--mode sim|game|dock0|dock1|pause|continue|options`, `--seed N`, `--steps N` (Simulation beschleunigen),
`--report` (Fortschritt auf der Konsole), `--screenshot datei.png --shot-at Sekunden`,
`--quit-after Sekunden`. Im normalen Spiel werden sie nicht gebraucht.

Weitere Testoptionen: `--size B H` (Fenstergröße, mindestens 960 × 540), `--norender` (nichts zeichnen,
schnelle Läufe), `--nohud`, `--shot-every Sekunden` (Bildserie), `--cam PX PY PZ TX TY TZ` (feste Kamera),
`--zoom Z`, `--pos X Z`, `--scan`, `--thrust-test`, `--pirates N`, `--hp H`, `--balance N`, `--deliver N`
(Gesamtablieferung = Ausbaustufe der Stationen). Die Bedeutung jeder Option steht bei `TestOptions` in
`src/main.cpp`.

`--mode options` öffnet direkt die Optionen (für Bildschirmfotos). `--music-test` prüft die Musik: Es
spielt einen festen Ablauf durch (Menü, neues Spiel, Musik aus und wieder an, Lautstärke 20 %, zurück
ins Menü) und schreibt alle 0,25 s eine Zeile `MUSICTEST …` (Blenden, Lautstärken, läuft das Stück,
Position im Stück) nach stderr. Dafür braucht es ein Audiogerät. Achtung: Es speichert die
Musik-Einstellungen wie ein Spieler in den Optionen.

Für Entwickler: `--dump-model-bounds > shared/ModelBounds.inc` misst alle Modelle in `assets/models`
neu und gibt die Tabelle aus, aus der der Server die Größe der Stationen kennt (nötig, wenn Modelle
hinzukommen oder sich ändern).

**Mehrspieler-Testoptionen** (starten direkt im Mehrspieler-Bildschirm, z. B. für automatische Tests
mit mehreren Spielen auf einem Rechner):

| Option | Wirkung |
|---|---|
| `--mp-connect ADRESSE` | sofort mit dem Server verbinden (`host` oder `host:port`) |
| `--mp-name NAME` | Spielername für die automatische Anmeldung |
| `--mp-pass PASSWORT` | Passwort dazu (nur für Tests – steht sichtbar in der Prozessliste!) |
| `--mp-register` | Konto anlegen; ist der Name schon vergeben, stattdessen anmelden |
| `--mp-spectate` | direkt zuschauen |
| `--mp-autopilot` | der Autopilot fliegt das eigene Schiff (wie die Simulation) |

Beispiel: Server lokal starten, dann zwei Testpiloten und einen Zuschauer:

```bash
./Mondschuerfer --mp-connect localhost --mp-name Testpilot1 --mp-pass geheim123 --mp-register --mp-autopilot
./Mondschuerfer --mp-connect localhost --mp-name Testpilot2 --mp-pass geheim123 --mp-register --mp-autopilot --size 960 540
./Mondschuerfer --mp-connect localhost --mp-spectate --report
```

Mit `--report` schreibt das Spiel alle 10 Sekunden eine Zustandszeile nach stderr (im Mehrspieler:
Phase, Spieler, Kontostand …).

---

## Lizenzen und Mitwirkende

- Grafik: **Kenney Space Kit** von Kenney (www.kenney.nl), CC0 1.0 (siehe `assets/Kenney_SpaceKit_License.txt`)
- Ton: **Kenney Sci-Fi Sounds** und **Kenney Interface Sounds** von Kenney (www.kenney.nl), CC0 1.0
  (siehe `assets/Kenney_SciFiSounds_License.txt` und `assets/Kenney_InterfaceSounds_License.txt`)
- Musik: **dekker187** (erstellt mit Suno), bereitgestellt vom Projektinhaber – *Main Menu Theme*
  und *Lunar Silence 01* (`assets/music/`)
- Bibliothek: **raylib** von Ramon Santamaria, zlib-Lizenz
- Im Projekt mitgeliefert unter `third_party/` (Einzelheiten in `third_party/README.md`):
  - **ENet** 1.3.18 von Lee Salzman, MIT-Lizenz (`third_party/enet/LICENSE`) – Netzwerk für Spiel und Server
  - **cpp-httplib** 0.22.0 von yhirose, MIT-Lizenz (`third_party/httplib/LICENSE`) – Webserver der Verwaltungsseite
  - **raylib**-Header `raylib.h` und `raymath.h` (5.5), zlib-Lizenz (`third_party/raylib/LICENSE`) – Datentypen
    und Vektor-Mathe für den Server
