# Wie der Mehrspieler funktioniert

Dieser Text erklärt den Mehrspieler-Teil von Mondschürfer so, dass man mit ihm in der Hand den
Quelltext lesen und verstehen kann. Er beschreibt, **was** passiert, **warum** es so gebaut ist und
**wo** es im Code steht. Wie man den Server installiert und betreibt, steht nicht hier, sondern in
der Betriebsanleitung [server/README.md](../server/README.md).

Vorausgesetzt wird etwas C++ (Klassen, `std::vector`, Lambdas) und eine grobe Vorstellung davon,
was UDP und TCP sind. Alle anderen Begriffe werden beim ersten Auftreten erklärt; am Ende steht
zusätzlich ein kleines [Glossar](#17-glossar).

**Inhalt**

1. [Überblick: drei Programme, ein Spiel](#1-überblick-drei-programme-ein-spiel)
2. [Warum die Spiellogik geteilt ist](#2-warum-die-spiellogik-geteilt-ist)
3. [Ablauf einer Sitzung](#3-ablauf-einer-sitzung)
4. [Die Tick-Schleife des Servers](#4-die-tick-schleife-des-servers)
5. [Threads im Server und SharedState](#5-threads-im-server-und-sharedstate)
6. [Vorhersage, Abgleich und Interpolation](#6-vorhersage-abgleich-und-interpolation)
7. [Quantisierung: Zahlen klein machen](#7-quantisierung-zahlen-klein-machen)
8. [Anmeldung mit PBKDF2 und Challenge-Response](#8-anmeldung-mit-pbkdf2-und-challenge-response)
9. [Ereignisse: Töne, Effekte, Meldungen](#9-ereignisse-töne-effekte-meldungen)
10. [Speicherung im Datenordner](#10-speicherung-im-datenordner)
11. [Die Verwaltungs-Webseite und ihre Schutzmaßnahmen](#11-die-verwaltungs-webseite-und-ihre-schutzmaßnahmen)
12. [Einstellungen und Welt-Neuaufbau](#12-einstellungen-und-welt-neuaufbau)
13. [Spielregeln im Mehrspieler](#13-spielregeln-im-mehrspieler)
14. [Leistung und Messwerte](#14-leistung-und-messwerte)
15. [Wo fange ich an zu lesen?](#15-wo-fange-ich-an-zu-lesen)
16. [Übungsideen](#16-übungsideen)
17. [Glossar](#17-glossar)

---

## 1. Überblick: drei Programme, ein Spiel

Zum Mehrspieler gehören drei Programme, die alle aus diesem Repository gebaut werden:

```
  Spiel "Mondschuerfer"          Server "mondschuerfer-server"          Browser
  (src/, mit Grafik)             (server/, ohne Grafik)                 (Betreiber)
 +--------------------+          +---------------------------+         +-----------+
 | Menüs, 3D, Ton     |  UDP     | Spielwelt, 30 Schritte/s  |  HTTP   | Verwal-   |
 | NetClient          |<-------->| Konten, Bestenliste       |<------->| tungs-    |
 | Spiegelwelt        |  ENet    | Webseite (eigener Thread) |  8080   | seite     |
 +--------------------+  7777    +---------------------------+         +-----------+
                                       |
                                       v
                                 Datenordner (/data)

 Dazu ein Werkzeug zum Messen: "mondschuerfer-loadtest" (viele Bots über das Netzwerk)
```

Der Server ist **autoritativ**: Nur er rechnet die echte Spielwelt. Das Spiel schickt ihm nur,
welche Tasten gedrückt sind, und bekommt zurück, was in seiner Umgebung passiert. Niemand kann
also auf seinem Rechner den Kontostand „hochschreiben“ – der Server würde es nicht glauben.

### Ordner

| Ordner | Inhalt | Wer benutzt es? |
|---|---|---|
| [shared/](../shared/) | Spiellogik ([World.hpp](../shared/World.hpp), [World.cpp](../shared/World.cpp), [WorldGen.cpp](../shared/WorldGen.cpp), [Ai.cpp](../shared/Ai.cpp)), Balancing ([Config.hpp](../shared/Config.hpp)), Einstellungen ([GameSettings.hpp](../shared/GameSettings.hpp)), Netzwerkprotokoll ([Protocol.hpp](../shared/Protocol.hpp)), Kryptografie ([Crypto.hpp](../shared/Crypto.hpp)), Hilfen ([Util.hpp](../shared/Util.hpp), [Models.hpp](../shared/Models.hpp), [SoundEvents.hpp](../shared/SoundEvents.hpp)) | Spiel **und** Server **und** Lasttest |
| [src/](../src/) | das Spiel: [main.cpp](../src/main.cpp), Zeichnen, HUD, Ton, und für den Mehrspieler [NetClient.hpp](../src/NetClient.hpp) (Netzwerk), [Multiplayer.hpp](../src/Multiplayer.hpp) (Menüs und Anzeige), [ClientConfig.hpp](../src/ClientConfig.hpp) (gemerkte Serveradresse, Musik-Einstellungen) | nur das Spiel |
| [server/](../server/) | [src/main.cpp](../server/src/main.cpp) (Start), [GameServer.hpp](../server/src/GameServer.hpp) (Herzstück), [Accounts.hpp](../server/src/Accounts.hpp) (Speichern), [SharedState.hpp](../server/src/SharedState.hpp) (Datenaustausch zwischen Threads), [WebServer.hpp](../server/src/WebServer.hpp) (Verwaltungsseite), [LoadTest.cpp](../server/src/LoadTest.cpp), [web/](../server/web/) (HTML/JS/CSS), [Dockerfile](../server/Dockerfile) | nur der Server |
| [third_party/](../third_party/) | fremde Bibliotheken im Repository: ENet 1.3.18 (Netzwerk), cpp-httplib 0.22.0 (Webserver), `raylib.h`/`raymath.h` 5.5 (nur Datentypen und Vektor-Mathe für den Server) – siehe [third_party/README.md](../third_party/README.md) | Spiel und Server |
| [docs/](./) | dieser Text und Bildschirmfotos | Menschen |

Im Hauptordner liegen außerdem [CMakeLists.txt](../CMakeLists.txt) (Optionen
`MONDSCHUERFER_CLIENT` und `MONDSCHUERFER_SERVER`), [docker-compose.yml](../docker-compose.yml) und
[.env.example](../.env.example) für den Betrieb im Container.

### Wichtige Zahlen auf einen Blick

Alle stehen in [shared/Protocol.hpp](../shared/Protocol.hpp):

| Konstante | Wert | Bedeutung |
|---|---|---|
| `TICK_RATE` | 30 | Simulationsschritte pro Sekunde (Server und Vorhersage im Spiel) |
| `SNAPSHOT_EVERY` | 2 | Spieler bekommen jeden 2. Schritt einen Snapshot → 15 pro Sekunde |
| `SPECTATOR_SNAPSHOT_EVERY` | 3 | Zuschauer → 10 pro Sekunde |
| `INTERP_DELAY` | 0,12 s | fremde Schiffe werden 120 ms „in der Vergangenheit“ gezeigt |
| `AOI_SHIPS` / `AOI_BULLETS` / `AOI_EVENTS` | 110 / 75 / 120 | Sichtweiten (Welt-Einheiten, etwa Meter; die Karte ist 300 × 300) |
| `MAX_INPUTS_PER_MSG` | 4 | jede Eingabe wird bis zu viermal mitgeschickt |
| `KDF_ITERATIONS` | 20.000 | PBKDF2-Runden für den Anmeldeschlüssel |

---

## 2. Warum die Spiellogik geteilt ist

Die Klasse `World` ([shared/World.hpp](../shared/World.hpp)) enthält **alle** Spielregeln: Flug,
Scanner, Abbau, Kampf, Piraten, Stationen. Sie zeichnet nichts und spielt keine Töne ab. Genau
deshalb kann derselbe Code an drei Stellen arbeiten:

| Einsatz | Start | Wer ruft `Update` auf? | Besonderheit |
|---|---|---|---|
| **Einzelspieler** ([src/main.cpp](../src/main.cpp)) | `NewGame(seed, simulation)` | main.cpp, einmal pro Bild mit dem `dt` des Bildes | genau ein Spieler (`players[0]`, Schiff `ships[0]`); Partikel entstehen direkt |
| **Server** ([server/src/GameServer.cpp](../server/src/GameServer.cpp)) | `NewServerWorld(seed, settings)`, dann `AddPlayer` je Anmeldung | GameServer, fest alle 1/30 s | `isServer = true`: neues Schiff nach Absturz, Zielrunden, feste Gegnerzahl, Lagerstätten pro Spieler aufgedeckt; statt Partikeln entstehen `FxEvent`s |
| **Client-Spiegel** ([src/NetClient.cpp](../src/NetClient.cpp)) | `BuildMirror(seed, settings)` | **niemand** – nur `UpdateCosmetics(dt)` und `UpdateShipPhysics` für das eigene Schiff | `isMirror = true`: Schiffe, Lagerstätten, Wracks schreibt der Client aus den Server-Nachrichten hinein |

Was bringt das?

- **Eine Wahrheit für die Physik.** Der Client sagt sein eigenes Schiff mit derselben Funktion
  `World::UpdateShipPhysics` voraus, die auch der Server benutzt. Gleiche Eingaben → gleiches
  Ergebnis. Nur deshalb funktioniert die Vorhersage (Abschnitt 6) ohne ständiges Ruckeln.
- **Zeichnen ohne Sonderfälle.** `Render.cpp` und `Hud.cpp` zeichnen immer eine `World` mit
  `Me()`/`Player()`. Im Mehrspieler ist das eben die Spiegelwelt – derselbe Zeichencode wie im
  Einzelspieler.
- **Keine übertragene Landschaft.** Mondoberfläche, Krater und Stationen werden nicht über das
  Netz geschickt. Der Server schickt nur den **Seed** (eine Zahl). Der Zufallsgenerator
  `std::mt19937` liefert mit gleichem Seed auf jedem Rechner dieselbe Zahlenfolge, also baut der
  Client dieselbe Kulisse. Die Kehrseite steht als Warnung in `World.hpp`: Jeder zusätzliche oder
  weggelassene Aufruf von `Rand()` beim Aufbau verschiebt alles, was danach kommt – Client und
  Server sähen dann verschiedene Monde.

Zwei Regeln halten die Teilung sauber:

1. **Keine raylib-Funktionen in `shared/`.** Der Server hat kein Fenster und keine Grafikkarte.
   Er bekommt nur die beiden Header `raylib.h` und `raymath.h` aus `third_party/raylib/`, weil die
   Spiellogik Datentypen wie `Vector3` und `Color` und die (komplett im Header definierte)
   Vektor-Mathe braucht. `World::Draw` ist zwar in `World.hpp` deklariert, aber nur im Spiel in
   `src/Render.cpp` definiert.
2. **Verweise sind IDs, keine Zeiger.** Ein Pirat merkt sich sein Ziel als `targetShip` (eine
   Zahl), nicht als `Ship*`. Die Listen ändern sich ständig, Zeiger würden ungültig. Im Mehrspieler
   kommt dazu: IDs lassen sich über das Netz schicken, Zeiger nicht.

Ein Detail, das man beim Lesen kennen sollte: `PlayerState` (Konto, Aufrüstungen, Scanner …) und
`Ship` (das fliegende Objekt) sind getrennt. Im Mehrspieler überlebt der Spieler sein Schiff: nach
einem Absturz ist `shipId = -1`, ein paar Sekunden später gibt es ein neues Schiff.

---

## 3. Ablauf einer Sitzung

Transport ist **ENet**, eine kleine Bibliothek über UDP. Sie baut eine „Verbindung“ auf (UDP kennt
eigentlich keine), erkennt Zeitüberschreitungen und bietet **zuverlässige** Kanäle (Paket kommt
garantiert und in Reihenfolge an, notfalls wiederholt) und **unzuverlässige** (kommt an oder
nicht; ENet verwirft verspätete ältere Pakete). Mondschürfer benutzt zwei Kanäle:

| Kanal | Art | Was läuft darüber | Warum so? |
|---|---|---|---|
| 0 `CHANNEL_RELIABLE` | zuverlässig | Anmeldung, Welcome, Events, Deposits, Players, Highscores, DockAction, Leave | darf nicht verloren gehen (ein verlorenes „Ziel erreicht“ käme nie an) |
| 1 `CHANNEL_UNRELIABLE` | unzuverlässig | Input, Snapshot | nur der neueste Stand zählt; ein Wiederholen würde nur alte Daten nachliefern und neue aufhalten |

Der ganze Ablauf als Sequenzdiagramm. `--->` heißt zuverlässig, `~~~>` unzuverlässig:

```
   Spiel (NetClient)                                              Server (GameServer)
        |                                                                  |
        |== ENet-Verbindung, Verbindungsdaten = PROTOCOL_VERSION =========>|  OnConnect: Version prüfen
        |<----------------------------- ServerInfo ------------------------|  Name, Spieler online,
        |<------------------ ServerInfo (alle 2 s, solange Lobby) ---------|  Einstellungen, realmSalt
        |                                                                  |
        |  (a) ZUSCHAUEN                                                   |
        |------------------------------ Spectate ------------------------->|  Platz frei? (max. 16)
        |<----------------- Welcome(role=Spectator, seed, ...) ------------|
        |                                                                  |
        |  (b) NEUES KONTO                                                 |
        |  key = PBKDF2(Passwort, Salz)        [nur lokal, ca. 20 ms]      |
        |------------------------- Register(name, key) ------------------->|  Name gültig und frei?
        |<----------------- Welcome(role=Player, playerId, ...) -----------|  Konto speichern + anmelden
        |                                                                  |
        |  (c) ANMELDEN                                                    |
        |------------------------- LoginStart(name) ---------------------->|
        |<------------------------ LoginChallenge(nonce) ------------------|  16 Zufallsbytes, immer
        |  proof = HMAC(key, nonce + name)                                 |
        |------------------------- LoginProof(proof) --------------------->|  in konstanter Zeit vergleichen
        |<-------------- Welcome(role=Player, ...) oder Error -------------|
        |                                                                  |
        |<------------------ Deposits, Players, Highscores ----------------|  Startpaket
        |                                                                  |
        |  IM SPIEL                                                        |
        |~~~~~~~~~~~~~ Input (30/s, die letzten bis zu 4 Eingaben) ~~~~~~~>|  in Warteschlange
        |<~~~~~~~~~~~~ Snapshot (15/s, Zuschauer 10/s) ~~~~~~~~~~~~~~~~~~~~|  Umgebung + eigener Zustand
        |<------------------ Events (nur wenn etwas passiert ist) ---------|  Töne, Explosionen, Meldungen
        |<------------------ Deposits (bei Änderung, höchstens 4/s) -------|  selbst entdeckte Lagerstätten
        |<------------------ Players (1/s), Highscores (bei Änderung) -----|
        |------------------- DockAction (nur angedockt) ------------------>|  Abdocken/Reparieren/Kaufen
        |                                                                  |
        |------------------- Leave --------------------------------------->|  speichern, entfernen, trennen
```

Ein paar Einzelheiten, die man im Code wiederfindet:

- **Versionsprüfung beim Verbinden.** ENet erlaubt beim Verbinden eine 32-Bit-Zahl als
  „Verbindungsdaten“. Der Client schickt `PROTOCOL_VERSION`, der Server antwortet bei
  Abweichung mit `Error{VersionMismatch}` und trennt. So bekommt ein veraltetes Spiel eine klare
  Meldung („Version passt nicht“) statt unverständlicher Pakete.
- **Lobby.** Verbunden, aber weder Spieler noch Zuschauer. Der Server schickt alle 2 s eine
  frische `ServerInfo` (Spielerzahl ändert sich) und trennt nach 15 Minuten ohne Anmeldung.
- **Zuschauen beenden.** Das Protokoll kennt kein „zurück in die Lobby“. Das Spiel trennt deshalb
  und verbindet sich sofort neu (`MultiplayerScreen::Update`, Phase `Spectating`).
- **Fehler** (`net::ErrorCode`) haben deutsche Texte in `net::ErrorText`, die das Spiel direkt
  anzeigt: „Name vergeben“, „Server voll“, „Name oder Passwort falsch“ …

### Wie eine Nachricht aussieht

Jede Nachricht beginnt mit einem Byte `MsgType`, danach folgen die Felder in fester Reihenfolge.
Zahlen werden **little endian** geschrieben (niedrigstes Byte zuerst), unabhängig vom Prozessor –
sonst würden ein x86-PC und ein Raspberry Pi mit anderer Bytereihenfolge aneinander vorbeireden.
Das erledigen `net::Writer` und `net::Reader` in [shared/Protocol.cpp](../shared/Protocol.cpp).

Der `Reader` ist bewusst misstrauisch: Vor jedem Lesen prüft er, ob noch genug Bytes da sind.
Fehlt etwas, liest er Nullen und merkt sich `ok() == false`; `Decode` liefert dann `false`, und der
Server verwirft das Paket (`GameServer::BadPacket`, nach 50 kaputten Paketen wird getrennt).
Listenlängen werden begrenzt, `NaN`/unendlich werden zu 0. Ein Server darf **keinem** Paket trauen –
es könnte von jemandem stammen, der absichtlich Unsinn schickt.

---

## 4. Die Tick-Schleife des Servers

Ein **Tick** (Simulationsschritt) rechnet die Welt um genau 1/30 s weiter. Der Server macht 30
davon pro Sekunde, egal wie schnell der Rechner ist. Feste Schritte haben zwei Vorteile: Die
Physik verhält sich immer gleich (bei großen Schritten würden schnelle Geschosse sonst durch
Schiffe „hindurchspringen“), und der Client kann dieselben Schritte nachrechnen.

Gekürzt aus `GameServer::Run` ([server/src/GameServer.cpp](../server/src/GameServer.cpp)):

```cpp
auto next = Clock::now() + tickDuration;           // Termin des nächsten Schritts
while (!stop.load()) {
    // Bis zum Termin auf Netzwerkpakete warten (höchstens 100 ms am Stück).
    // Ankommende Pakete werden sofort verarbeitet (Anmeldung, Eingaben in
    // Warteschlangen legen ...), aber die Welt wird NICHT bewegt.
    ServiceNetwork(msBis(next));
    if (Clock::now() < next) continue;

    Step();                    // genau ein Simulationsschritt
    enet_host_flush(host_);    // alles sofort abschicken

    next += tickDuration;      // nächster Termin aus dem vorigen, nicht aus "jetzt"
    if (Clock::now() - next > 250ms) next = Clock::now() + tickDuration;   // nicht endlos aufholen
}
```

`next += tickDuration` ist der Trick gegen „Drift“: Würde man jedes Mal „jetzt + 33,3 ms“ rechnen,
käme jede kleine Verspätung dazu, und der Server liefe auf Dauer langsamer als 30 Schritte/s. Hängt
der Rechner kurz, holt die Schleife die verpassten Schritte schnell nach – bei mehr als 250 ms
Rückstand (Rechner war angehalten) aber nicht, sonst liefe das Spiel im Zeitraffer.

### Was ein Schritt tut (`GameServer::Step`)

1. **Einstellungen von der Webseite** übernehmen, falls welche warten (Abschnitt 12). Immer am
   Anfang, nie mitten in der Simulation.
2. **Eingaben anwenden** (`ApplyInputs`): Für jeden Spieler genau **eine** Eingabe aus seiner
   Warteschlange nehmen und in `PlayerState::input` legen.
3. **`world.Update(TICK_DT)`** – die eigentlichen Spielregeln.
4. **Ereignisse verteilen** (`DistributeWorldEvents`, `SendEvents`, Abschnitt 9).
5. **Snapshots** schicken (`SendSnapshots`): Spieler in jedem 2. Schritt, Zuschauer in jedem 3.
6. **Lagerstätten** schicken, wenn sich für diesen Empfänger etwas geändert hat
   (`SendDepositUpdates`).
7. Jede Sekunde **Spielerliste** an alle, bei Änderung die **Bestenliste**, alle 2 s `ServerInfo`
   an die Lobby, alle 30 s **speichern**, jede Sekunde **Aufräumen** (Flutbremse zurücksetzen,
   Lobby-Zeitüberschreitung) und den **Status für die Webseite** veröffentlichen.

### Die Eingabe-Warteschlange

Warum nicht einfach immer die neueste Eingabe benutzen? Weil der Client sein Schiff vorhersagt,
indem er **jede Eingabe genau einen Schritt lang** anwendet. Wendet der Server dieselben Eingaben in
derselben Reihenfolge je einen Schritt lang an, landet das Schiff dort, wo der Client es erwartet.
Pakete kommen aber unregelmäßig an – mal zwei auf einmal, mal eine Weile keins. Die Warteschlange
gleicht das aus:

```
 (bisher bis #6 empfangen)
 Paket kommt an:      [#4 #5 #6 #7]   [#5 #6 #7 #8]   (keins)   [#7 #8 #9 #10]
 davon neu:            #7              #8                        #9 #10       (seq <= zuletzt empfangen: ignoriert)
 Warteschlange:       [7]             [8]             [ ]       [9 10]
 Schritt verbraucht:   7               8              (leer)     9  -> im nächsten Schritt 10
                                                         \_ leere Schlange: letzte Tasten weiter
                                                            halten, aber ohne Tastendrücke
```

- Jede Eingabe hat eine fortlaufende Nummer `seq`. Weil jedes Paket die letzten bis zu 4 Eingaben
  enthält, schadet ein verlorenes Paket meist nicht: Die Eingabe steckt auch im nächsten.
- Ist die Schlange länger als 6, verwirft der Server die ältesten (sonst würde sich Verzögerung
  aufstauen).
- **Die Schlange wird geregelt.** Nach einem Netz-Ruckler kommen die zurückgehaltenen Pakete
  gleichzeitig an, und die Schlange ist plötzlich z. B. 5 lang. Weil Client und Server beide genau
  30 Eingaben pro Sekunde erzeugen bzw. verbrauchen, bliebe sie für immer so lang: Jede Eingabe
  wirkte dann 5/30 s ≈ 170 ms später (Schüsse, Andocken, die Position, die andere sehen). Deshalb
  merkt sich `ApplyInputs` die **kürzeste** Länge der letzten Sekunde. War die Schlange die ganze
  Sekunde über länger als 1, wird eine Eingabe übersprungen – höchstens eine pro Sekunde, damit
  es sanft geht. Tastendrücke gehen dabei nicht verloren (Zähler, siehe unten), ein Schuss wird an
  die nächste Eingabe weitergereicht. Der Client merkt nur eine kleine Korrektur beim Abgleich.
- Damit der Client wirklich 30 Eingaben pro Sekunde erzeugt, bekommt der Mehrspieler die
  **ungekürzte** Bildzeit (`App::Frame` in `src/main.cpp`). Der Einzelspieler kürzt sie auf
  höchstens 1/20 s; bei 15 Bildern pro Sekunde liefe die Eingabeuhr sonst nur mit 75 %.
- `ackSeq` (die Nummer der zuletzt angewendeten Eingabe) geht im nächsten Snapshot zurück an den
  Client. Das braucht er für den Abgleich (Abschnitt 6).

### Tastendrücke als Zähler

Gehaltene Tasten (Schub, Drehen, Feuer) sind unproblematisch: Geht ein Paket verloren, kommt der
Zustand im nächsten wieder. Ein **einzelner Druck** (E = Scanner, F = Andocken/Abbauen) wäre aber
weg, wenn genau sein Paket verloren geht. Deshalb überträgt der Client keine „gedrückt“-Bits,
sondern **Zähler** (`scanPresses`, `interactPresses`, je 8 Bit), die bei jedem Druck um 1 steigen.
Der Server vergleicht mit dem zuletzt gesehenen Stand:

```cpp
const uint8_t d = static_cast<uint8_t>(neu - alt);   // rechnet modulo 256
bool gedrueckt = d >= 1 && d < 128;
```

Beispiele: alt 41, neu 42 → d = 1 → gedrückt. Überlauf: alt 255, neu 0 → d = (0 − 255) mod 256 = 1 →
gedrückt. Vertauschte Pakete: alt 42, neu 41 → d = 255 → ≥ 128, also kein Druck (ein
Rückwärtsschritt). Die **erste** Eingabe einer Sitzung legt nur den Ausgangsstand fest.

Eine kleine Schutzregel aus der Praxis: Nach dem Abdocken ignoriert der Server 10 Schritte lang
jede Interaktion – sonst würde dasselbe F, mit dem man abdockt, sofort wieder andocken.

### Was bekommt wer? (Sichtweite)

Ein Snapshot enthält nicht die ganze Welt, sondern nur die Umgebung des Empfängers („Area of
Interest“). Das spart Bandbreite und verrät nebenbei nicht, wo alle anderen gerade sind.

| Inhalt | Spieler | Zuschauer |
|---|---|---|
| eigener Zustand (`SnapMe`) | ja, genau (Floats) | – |
| Schiffe | im Umkreis 110 um das eigene Schiff, ohne das eigene, ohne versteckte; höchstens 80 (die nächsten) | alle sichtbaren, höchstens 512 |
| Geschosse | im Umkreis 75, höchstens 128 | keine |
| Wracks | im Umkreis 110, höchstens 48 | alle |
| Lagerstätten (eigene Nachricht `Deposits`) | nur selbst entdeckte | alle |
| Snapshots pro Sekunde | 15 | 10 |

Ohne Schiff (nach einem Absturz) gilt die letzte bekannte Position als Mittelpunkt.

---

## 5. Threads im Server und SharedState

Ein **Thread** ist ein Programmteil, der gleichzeitig mit anderen läuft. Der Server hat drei Arten:

```
 +------------------------------ Prozess mondschuerfer-server ------------------------------+
 |                                                                                          |
 |  SPIEL-THREAD (main -> GameServer::Run)            WEB-THREAD (cpp-httplib)              |
 |  ENet, World, Konten im Speicher                   + 4 Arbeits-Threads für Anfragen      |
 |        |                     ^                            |                 ^           |
 |        | PublishStatus       | TakeSettingsRequest        | RequestSettings | GetStatus |
 |        | (1/s, Kopie)        | (Anfang jedes Schritts)    | RequestReset    | (Kopie)   |
 |        v                     |                            v                 |           |
 |   +--------------------------------- SharedState (Mutex) ---------------------------+   |
 |   |  ServerStatus status_            GameSettings pendingSettings_, pending_         |   |
 |   +----------------------------------------------------------------------------------+   |
 |        |                                                                                 |
 |        | Accounts::Save...(): Dateiinhalt fertig gebaut (Mikrosekunden)                  |
 |        v                                                                                 |
 |  SCHREIB-THREAD (in Accounts) -- *.tmp schreiben, fsync, umbenennen --> Datenordner      |
 +------------------------------------------------------------------------------------------+
```

**Spiel-Thread.** `main()` selbst. Er besitzt die Welt, alle Verbindungen und die Konten im
Speicher. Nur er fasst sie an – deshalb braucht `GameServer` intern keine einzige Sperre.

**Web-Thread.** `WebServer::Start` startet einen Thread, der auf HTTP-Anfragen wartet; jede
Anfrage bearbeitet einer von vier Arbeits-Threads (Thread-Pool von cpp-httplib). Diese Threads
dürfen die Welt **nie** direkt lesen: Liest der Web-Thread gerade `world.players`, während der
Spiel-Thread einen Spieler entfernt, ist der Vektor im selben Moment halb umgebaut – das Programm
liest Müll oder stürzt ab („Data Race“).

**SharedState** ([server/src/SharedState.hpp](../server/src/SharedState.hpp)) ist deshalb die
einzige Brücke, eine kleine Klasse mit einem `std::mutex`:

- Der Spiel-Thread **veröffentlicht** einmal pro Sekunde eine **Kopie** seines Zustands
  (`PublishStatus`). Die Webseite liest nur diese Kopie (`GetStatus`), wieder als Kopie.
- Die Webseite **legt Einstellungswünsche ab** (`RequestSettings`/`RequestReset`). Der
  Spiel-Thread holt sie zu Beginn des nächsten Schritts ab (`TakeSettingsRequest`).

Warum Kopien statt „Welt sperren und lesen“? Die Sperre ist so nur für die Dauer einer Kopie
belegt (Mikrosekunden). Der Spiel-Thread muss nie warten, bis eine langsame Webanfrage fertig ist,
und kann so seine 33,3 ms pro Schritt sicher einhalten. Und der Zustand auf der Webseite ist immer
in sich stimmig, weil er an einem einzigen Zeitpunkt aufgenommen wurde.

**Schreib-Thread.** Das Schreiben einer Datei mit `fsync` („wirklich auf den Datenträger“) dauert
auf einer SD-Karte (Raspberry Pi) gelegentlich 100 ms und mehr. Im Spiel-Thread würde das Spiel
dabei ruckeln. Darum bauen die `Save*`-Funktionen in [server/src/Accounts.hpp](../server/src/Accounts.hpp)
nur den Text und geben ihn einem eigenen Thread. Liegt für dieselbe Datei schon ein Auftrag in der
Warteschlange, wird er durch den neueren ersetzt – es zählt nur der neueste Stand. Beim Beenden
wartet `Flush()`, bis alles geschrieben ist.

**Beenden.** Docker stoppt einen Container mit dem Signal SIGTERM, Strg+C schickt SIGINT. Ein
Signal-Handler unterbricht das Programm an beliebiger Stelle und darf deshalb fast nichts tun. Er
setzt nur eine `std::atomic<bool>`-Variable (siehe [server/src/main.cpp](../server/src/main.cpp)).
Die Hauptschleife prüft sie bei jedem Durchlauf, speichert alle Spieler, schickt den Clients
`Error{ServerShutdown}` und beendet sich geordnet.

**Im Spiel** gibt es übrigens keinen zweiten Thread: `NetClient::Update` holt die Pakete einmal
pro Bild im Haupt-Thread ab ([src/Multiplayer.hpp](../src/Multiplayer.hpp), Abschnitt „Threads“).

---

## 6. Vorhersage, Abgleich und Interpolation

Das schwierigste Problem jedes Netzwerkspiels ist die **Laufzeit**. Angenommen, ein Paket braucht
50 ms zum Server und 50 ms zurück (Rundlaufzeit, RTT = 100 ms). Würde das Spiel einfach nur zeigen,
was der Server schickt, dann:

```
Taste W gedrückt ──50 ms──> Server rechnet (wartet bis zum nächsten Schritt, bis 33 ms)
                 ──bis zum nächsten Snapshot (bis 67 ms)──> ──50 ms──> Bild bewegt sich
                 = 100 bis 200 ms, bis das Schiff reagiert.  Fühlt sich an wie Fliegen in Sirup.
```

Die Lösung besteht aus drei Techniken, alle in [src/NetClient.cpp](../src/NetClient.cpp).

### 6.1 Vorhersage des eigenen Schiffs (Client-side prediction)

Das Spiel wartet nicht auf den Server, sondern wendet jede Eingabe **sofort selbst** an – mit
derselben Funktion `World::UpdateShipPhysics` und denselben 1/30-s-Schritten wie der Server:

```cpp
accumulator += dt;                              // dt = Zeit seit dem letzten Bild
while (accumulator >= net::TICK_DT) {           // so viele ganze Schritte wie fällig
    accumulator -= net::TICK_DT;
    InputRecord rec = ...;  rec.seq = ++seq;    // Tasten dieses Schritts mit Nummer
    pending.push_back(rec);                     // merken: noch nicht vom Server bestätigt
    world.UpdateShipPhysics(pred, TICK_DT, rec);// eigenes Schiff sofort bewegen
    Send(Input{die letzten bis zu 4 aus pending});
}
```

Der **Akkumulator** sorgt dafür, dass bei 60, 144 oder 30 Bildern pro Sekunde immer genau 30
Schritte pro Sekunde gerechnet werden.

### 6.2 Abgleich (Reconciliation)

Die Vorhersage kann falsch sein: Der Server weiß Dinge, die das Spiel nicht weiß (ein anderes
Schiff war im Weg, ein Paket ging verloren). Deshalb hat das letzte Wort der Server. Jeder Snapshot
enthält den Zustand des eigenen Schiffs **nach** der Eingabe `ackSeq`. `NetClient::Impl::ApplyMe`
macht dann:

1. alle Eingaben in `pending` mit `seq <= ackSeq` wegwerfen (die hat der Server schon verrechnet),
2. das Schiff auf den Serverzustand setzen,
3. alle übrigen Eingaben (noch unterwegs zum Server) **erneut** anwenden.

Ein Zeitstrahl mit 50 ms Laufzeit in jede Richtung:

```
 Zeit    Spiel                                                Server
 ------  ---------------------------------------------------  -----------------------------------
   0 ms  Eingabe #1: Schiff sofort bewegen, Input[#1] senden ~~~~~~~~>
  33 ms  Eingabe #2: bewegen, Input[#1 #2] ~~~~~~~~~~~~~~~~~~~~~~~~~~~>  ~50 ms: Schritt wendet #1 an
  67 ms  Eingabe #3: bewegen, Input[#1 #2 #3]                            ~83 ms: Schritt wendet #2 an,
 100 ms  Eingabe #4: bewegen                                                     Snapshot(ackSeq=2, Lage L2)
 133 ms  Eingabe #5: bewegen                                <~~~~~~~~~~~~~~~~~~~~~~
         Snapshot kommt an (ackSeq = 2):
           pending vorher:  #1 #2 #3 #4 #5
           #1, #2 bestätigt  -> weg
           Schiff := L2      (Stand des Servers nach #2)
           #3, #4, #5 erneut anwenden -> neue Vorhersage
           neue Vorhersage minus alte Vorhersage = Fehler (meist 0)
```

Hat der Server dasselbe gerechnet, ist der Fehler null, und man sieht nichts. Gab es einen
Unterschied, würde das Schiff sichtbar springen. Stattdessen wandert der Unterschied in einen
**Korrekturversatz**, der in jedem Bild kleiner wird:

```cpp
corrOffset *= exp(-10 * dt);    // nach 0,1 s noch ~37 %, nach 0,3 s ~5 %
gezeigt = vorhergesagt + corrOffset;
```

Im Moment der Korrektur bleibt das Bild also exakt gleich, danach gleitet das Schiff in
Sekundenbruchteilen an die richtige Stelle. Ist der Fehler größer als 8 m (lange Netzstörung),
springt das Schiff lieber einmal, als sekundenlang durch die Gegend zu gleiten. Angedockt, beim
Absturz oder ohne Schiff übernimmt das Spiel die Serverwerte direkt – da gibt es nichts
vorherzusagen.

Weil die Physik in 1/30-s-Schritten läuft, das Bild aber öfter erneuert wird, mischt
`UpdateOwnDisplay` zusätzlich zwischen dem Zustand vor und nach dem letzten Schritt
(Anteil = Rest im Akkumulator / `TICK_DT`). Sonst würde das eigene Schiff bei 60 Bildern pro
Sekunde in jedem zweiten Bild stillstehen.

**Eigene Schüsse** sagt das Spiel ebenfalls voraus: Den Schuss-Ton spielt es sofort selbst. Der
Server schickt deshalb das Ereignis `PlayerShot` nicht an den Schützen zurück (sonst hörte er jeden
Schuss doppelt).

### 6.3 Interpolation fremder Schiffe

Fremde Schiffe kann das Spiel nicht vorhersagen – es kennt die Tasten der anderen nicht. Snapshots
kommen nur 15-mal pro Sekunde, unregelmäßig und manchmal gar nicht. Würde man ein fremdes Schiff
einfach auf die neueste Position setzen, spränge es 15-mal pro Sekunde.

Der Trick: fremde Schiffe **absichtlich 120 ms in der Vergangenheit** zeigen. Für diesen Zeitpunkt
liegen fast immer zwei Snapshots vor – einer davor, einer danach –, zwischen denen man gleichmäßig
überblenden kann:

```
 Serverzeit:   1,000     1,067     1,133     1,200     1,267        (Snapshots alle 66,7 ms)
                 S1        S2        S3        S4        S5
                                       |  x  |               ^
                                       |  ^  |               geschätzte Serverzeit "jetzt" = 1,280
                                       |  Darstellungszeit = 1,280 - 0,120 = 1,160
                                       |
               zwischen S3 (1,133) und S4 (1,200):
               Anteil   = (1,160 - 1,133) / (1,200 - 1,133) ≈ 0,4
               Position = S3 + 0,4 · (S4 - S3)
               Drehwinkel über AngleDiff (damit der Übergang von +180° nach -180° keinen Salto ergibt)
```

120 ms sind knapp zwei Snapshot-Abstände: Selbst wenn ein Snapshot verloren geht, ist meist noch
ein späterer da. Fehlen trotzdem Daten, rechnet `UpdateRemoteShips` bis zu 0,25 s mit der letzten
Geschwindigkeit weiter (Extrapolation). Springt ein Schiff zwischen zwei Snapshots um mehr als
15 m, wird nicht interpoliert, sondern direkt versetzt. Neue IDs erscheinen, verschwundene fallen
weg.

**Woher kennt das Spiel die Serverzeit?** Jeder Snapshot trägt die Nummer des Server-Schritts
(`tick`), in dem er entstand. Das Spiel rechnet daraus die Serverzeit in Sekunden
(`TickTime`: Schritte seit dem ersten `Welcome` geteilt durch 30). „Serverzeit minus eigene Uhr
beim Empfang“ ist eine Messung des Versatzes zwischen beiden Uhren (samt Laufzeit). Einzelne
Messungen schwanken, deshalb verschiebt jede neue die Schätzung nur um 5 % (`SyncTime`). Weicht
eine Messung um mehr als 1 s ab (Neuverbindung, langer Hänger), beginnt die Schätzung neu.

Warum ein Schrittzähler und keine Kommazahl? Ein `float` hat nur etwa 7 gültige Stellen. Nach
12 Tagen (rund 1 Million Sekunden) könnte er Zeiten nur noch in Schritten von 1/8 s darstellen –
zwei Snapshots im Abstand von 1/10 s bekämen dann manchmal dieselbe Zeit, und die Schiffe
sprängen. Eine ganze Zahl zählt dagegen immer exakt; erst beim Umrechnen entsteht eine
Kommazahl (`double`, 15–16 Stellen). Eine Zuschauer-Anzeige kann so wochenlang laufen.

**Geschosse** fliegen geradeaus. Das Spiel nimmt sie aus dem neuesten Snapshot und rechnet sie mit
`Geschwindigkeit · (jetzt − Empfangszeit)` weiter. **Wracks** und **Lagerstätten** bewegen sich
nicht und werden einfach übernommen.

Die Kehrseite der Interpolation: Man sieht andere Schiffe etwa 120 ms plus die halbe Laufzeit
„zu spät“. Wer auf ein schnelles fremdes Schiff zielt, muss etwas vorhalten – ein bewusster
Kompromiss zugunsten eines ruhigen Bildes.

---

## 7. Quantisierung: Zahlen klein machen

Ein `float` braucht 4 Byte. Für Positionen auf einer 300 × 300 großen Karte ist das viel Genauigkeit,
die niemand sieht. **Quantisieren** heißt: eine Gleitkommazahl auf eine kleine Ganzzahl abbilden,
indem man eine feste Einheit wählt und rundet. Die Funktionen stehen in
[shared/Protocol.hpp](../shared/Protocol.hpp) (`Q…` hin, `D…` zurück):

| Funktion | Größe | Einheit | Bereich | Genauigkeit |
|---|---|---|---|---|
| `QPos` / `DPos` | `int16_t` (2 Byte) | 1 cm | ±327,67 m | ±0,5 cm |
| `QVel` / `DVel` | `int16_t` | 1 cm/s | ±327,67 m/s | ±0,5 cm/s |
| `QAngle` / `DAngle` | `uint16_t` | 2π/65536 rad | ganzer Kreis | ±0,003° |
| `QHeight` / `DHeight` | `uint8_t` (1 Byte) | 5 cm | 0 … 12,75 m | ±2,5 cm |
| `QUnit` / `DUnit` | `uint8_t` | 1/255 | 0 … 1 (Schild, Schub, Aufblitzen) | ±0,2 % |

Beispiele, mit dem echten Code nachgerechnet:

```
Position x = 123,456 m   ->  QPos:   round(123,456 · 100) = 12346   ->  DPos:   123,46 m    (Fehler 0,4 cm)
Position z = -87,3 m     ->  QPos:   -8730                          ->          -87,30 m
Drehwinkel = 1,0 rad     ->  QAngle: round(1,0 / 2π · 65536) = 10430 ->  DAngle: 0,999964 rad
Drehwinkel = -0,5 rad    ->  QAngle: erst auf 0..2π: 5,7832 -> 60321 ->  DAngle: -0,499982 rad
Höhe y = 2,53 m          ->  QHeight: round(2,53 · 20) = 51          ->  DHeight: 2,55 m
Schild 73 %              ->  QUnit:  round(0,73 · 255) = 186         ->  DUnit:  0,7294
```

Werte außerhalb des Bereichs werden begrenzt (`ClampRound`), nicht „umgeklappt“. Die Karte reicht
von −150 bis +150, passt also bequem in ±327 m.

Was das bringt, für ein Schiff im Snapshot (gemessen mit `net::Encode`):

| Eintrag | quantisiert | mit lauter Floats | Ersparnis |
|---|---|---|---|
| Schiff (`SnapShip`) | 21 Byte (25 beim Abbauen) | 41 Byte | ~49 % |
| Geschoss (`SnapBullet`) | 10 Byte | 21 Byte | ~52 % |
| Wrack (`SnapWreck`) | 19 Byte | 29 Byte | ~34 % |

Ein typischer Snapshot mit eigenem Zustand, 10 Schiffen, 20 Geschossen und 2 Wracks:

```
Kopf 25 + eigener Zustand 105 + 10 · 21 + 20 · 10 + 2 · 19 = 578 Byte      (mit Floats: ~1.018 Byte)
× 15 Snapshots pro Sekunde                               ≈ 8,5 KB/s je Spieler
+ je Paket ~40 Byte für UDP/IPv4/ENet-Kopf               ≈ 0,6 KB/s
```

Das passt gut zu den gemessenen 8,8 KB/s pro Spieler bei 16 Spielern (Abschnitt 14).

**Eine bewusste Ausnahme:** Der eigene Zustand (`SnapMe`, 105 Byte) wird **nicht** quantisiert. Die
Vorhersage setzt das Schiff auf genau diese Werte und rechnet von dort weiter. Ein Rundungsfehler von
0,5 cm würde sich über mehrere nachgerechnete Schritte aufschaukeln und in jedem Snapshot eine kleine
Korrektur auslösen – das Schiff würde zittern. Für den einen eigenen Eintrag lohnt sich die
Genauigkeit.

---

## 8. Anmeldung mit PBKDF2 und Challenge-Response

Ziel der Anmeldung ([shared/Crypto.hpp](../shared/Crypto.hpp)): Das **Passwort verlässt nie den
Rechner des Spielers**, und wer eine Anmeldung mitliest, kann sie später nicht wiederverwenden. Alles
ohne fremde Kryptobibliothek: SHA-256 (FIPS 180-4), HMAC (RFC 2104) und PBKDF2 (RFC 8018) sind in
[shared/Crypto.cpp](../shared/Crypto.cpp) selbst umgesetzt.

### Die Bausteine

- **SHA-256** macht aus beliebigen Daten 32 Byte „Fingerabdruck“. Rückwärts geht es nicht.
- **HMAC-SHA256(Schlüssel, Nachricht)** ist ein Fingerabdruck, den nur berechnen kann, wer den
  Schlüssel kennt.
- **PBKDF2** wendet HMAC sehr oft hintereinander an (hier 20.000-mal). Das macht aus einem Passwort
  einen 32-Byte-Schlüssel und kostet absichtlich Zeit (ca. 20 ms). Für einen Spieler ist das
  unmerklich; wer Millionen Passwörter durchprobieren will, braucht 20.000-mal so lange.
- **Salz:** Zusätzliche Daten, die in PBKDF2 einfließen. Hier
  `"Mondschuerfer|" + Server-Salz (hex) + "|" + name (klein)`. Das Server-Salz sind 16 Zufallsbytes,
  die der Server beim ersten Start erzeugt (`server.txt`) und in jeder `ServerInfo` mitschickt.
  Dadurch ergibt dasselbe Passwort für zwei Spieler oder auf zwei Servern verschiedene Schlüssel, und
  vorberechnete Tabellen („Rainbow Tables“) nützen nichts.

### Ablauf

```
 KONTO ANLEGEN (einmalig)
   Spiel:  key = PBKDF2(Passwort, Salz, 20.000 Runden)         Passwort bleibt hier
   Spiel  ---- Register(name, key) ---------------------------->  Server speichert key (accounts.txt)

 ANMELDEN (jedes Mal)
   Spiel  ---- LoginStart(name) ------------------------------->  Server: nonce = 16 Zufallsbytes
   Spiel  <--- LoginChallenge(nonce) ---------------------------
   Spiel:  key   = PBKDF2(Passwort, Salz, 20.000 Runden)
           proof = HMAC(key, nonce + name (klein))
   Spiel  ---- LoginProof(proof) ------------------------------>  Server: erwartet = HMAC(gespeicherter key,
                                                                          nonce + name)
                                                                  ConstantTimeEqual(erwartet, proof)?
   Spiel  <--- Welcome  oder  Error(WrongLogin) ----------------
```

Feinheiten in `GameServer::HandleLoginStart` und `HandleLoginProof`:

- Der Server schickt **immer** eine Challenge, auch für unbekannte Namen, und rechnet dann gegen
  einen Zufallsschlüssel (`dummyKey_`). Gleicher Rechenweg, gleiche Antwort „Name oder Passwort
  falsch“: Über die Anmeldung lässt sich nicht herausfinden, welche Namen es gibt.
- **Eine Nonce gilt für genau einen Versuch.** Danach ist `challengePending` wieder `false`.
- **`ConstantTimeEqual`** vergleicht immer alle 32 Byte, ohne beim ersten Unterschied abzubrechen.
  Sonst könnte ein Angreifer an der Antwortzeit messen, wie viele Bytes schon stimmen.
- **Begrenzungen:** höchstens 5 Fehlversuche pro Verbindung (dann `TooManyAttempts` und Trennung),
  höchstens 10 pro Konto in 10 Minuten, höchstens 10 neue Konten pro Stunde je öffentlicher
  IP-Adresse, höchstens 10.000 Konten. Heimnetz-Adressen sind von den IP-Grenzen ausgenommen.
- Erst **nach** dem richtigen Beweis prüft der Server, ob der Spieler schon online ist (z. B. weil
  eine alte Verbindung noch nicht abgelaufen ist) und ob der Server voll ist. Wer das Passwort nicht
  kennt, bekommt immer nur „Name oder Passwort falsch“.
- Die Mindestlänge des Passworts (6 Zeichen) prüft nur das Spiel: Der Server sieht das Passwort ja
  nie.

### Was es schützt – und was nicht

| Angriff | Geschützt? | Warum |
|---|---|---|
| Anmeldung mitschneiden und später nachspielen („Replay“) | **ja** | jede Anmeldung bekommt eine neue Nonce, der alte Beweis passt nicht mehr |
| Passwort aus einer mitgeschnittenen Anmeldung lesen | **ja** | übertragen wird nur ein HMAC; das Passwort ließe sich nur durch Raten finden, mit 20.000 PBKDF2-Runden pro Versuch |
| Betreiber des Servers liest Passwörter | **ja** | der Server bekommt nur den abgeleiteten Schlüssel |
| Passwort online durchprobieren | **begrenzt** | 5 Versuche pro Verbindung; je 10 Minuten 10 pro Konto und Adresse, 30 pro Adresse, 100 pro Konto insgesamt (siehe unten) |
| Einen fremden Spieler gezielt aussperren | **ja** | gesperrt wird nur die Kombination aus Konto und Absender-Adresse; die „Notbremse“ (100 pro Konto) gilt nicht für Adressen, von denen sich der Besitzer schon erfolgreich angemeldet hat |
| Herausfinden, ob ein Name existiert | **teilweise** | über die Anmeldung nicht; „Neues Konto anlegen“ meldet aber zwangsläufig „Name vergeben“, und die Spielerliste zeigt, wer online ist |
| Mitlesen genau beim **Anlegen** des Kontos | **nein** | der Schlüssel geht einmal unverschlüsselt übers Netz – wer ihn hat, kann sich anmelden (das Passwort kennt er trotzdem nicht) |
| Diebstahl von `accounts.txt` oder einer Sicherung | **nein** (Passwort bleibt geheim) | der gespeicherte Schlüssel genügt zum Anmelden; das Passwort selbst müsste man erraten |
| Mitlesen oder Verändern des laufenden Spiels | **nein** | die ENet-Verbindung ist nicht verschlüsselt; ein Angreifer mitten im Netz könnte eine Sitzung nach der Anmeldung übernehmen |

**Warum gleich drei Zähler für Fehlversuche?** (`GameServer::LoginBlocked`) Ein einziger
Zähler pro Konto wäre einfach, hätte aber eine böse Nebenwirkung: Jeder könnte mit zehn falschen
Versuchen einen beliebigen Spieler aussperren, denn alle Namen stehen in der Spielerliste. Darum
zählt der Server pro **Konto und Adresse** (sperrt nur den Angreifer), pro **Adresse** über alle
Konten (bremst das Ausprobieren eines Passworts bei vielen Konten) und als Notbremse pro **Konto**
insgesamt (gegen Raten von sehr vielen Adressen aus). Gezählt werden nur Konten, die es gibt; sonst
könnte man die Liste mit erfundenen Namen füllen. Während einer Sperre prüft der Server den Beweis
gar nicht erst – sonst könnte ein Angreifer trotz Sperre weiterraten und an der Antwort erkennen,
ob er richtig lag.

Das Verfahren ist also bewusst einfach gehalten: gut genug für ein Spiel im Heimnetz oder unter
Freunden, kein Ersatz für TLS. Für Spiele über das Internet empfiehlt
[server/README.md](../server/README.md) ein VPN (z. B. Tailscale oder WireGuard) und ein eigenes
Passwort nur für dieses Spiel.

---

## 9. Ereignisse: Töne, Effekte, Meldungen

`World` spielt keine Töne ab und erzeugt auf dem Server keine Partikel. Stattdessen sammelt sie bei
jedem `Update` **Ereignisse** in Listen, die der Aufrufer abarbeitet und leert:

| Liste in `World` | Inhalt | Einzelspieler | Server |
|---|---|---|---|
| `sounds` | `SoundEvent`: Ton, Ort, Lautstärke, Verzögerung, `forPlayer`, `sourceShip` | main.cpp → `Audio::PlayAt` | an Spieler in Hörweite verteilen |
| `fx` | `FxEvent`: Explosion oder Funken | (gibt es nicht, Partikel entstehen direkt) | an Spieler in Sichtweite verteilen |
| `goals` | `GoalEvent`: Spieler hat das Ziel erreicht | – | Bestenliste, an **alle** |
| `PlayerState::outbox` | neue Meldungen („Lagerstätte entdeckt!“) | – | nur an diesen Spieler |

`GameServer::DistributeWorldEvents` entscheidet für jeden Client, was er bekommt, und legt es in
dessen Liste. Am Ende des Schritts schickt `SendEvents` jedem Client **eine** `Events`-Nachricht
(zuverlässig, nur wenn nicht leer). Die Regeln:

| Ereignis (`EventKind`) | Spieler bekommt es, wenn … | Zuschauer |
|---|---|---|
| `Sound` | `forPlayer` er selbst ist, oder `forPlayer == -1` und die Quelle im Umkreis 120 liegt – **außer** `PlayerShot` vom eigenen Schiff (schon vorhergesagt) | nein |
| `Explosion`, `Sparks` | im Umkreis 120 | nur Explosionen |
| `Notice` | Meldung aus seiner `outbox` | nein |
| `Goal` | immer (Name, Zeit, Runde) | ja |
| `StationGrow` | immer, wenn die Handelsstationen eine Ausbaustufe weiter sind | ja |
| `Respawned` | er hat ein neues Schiff bekommen | nein |

Im Spiel baut `NetClient` daraus wieder dieselben Dinge wie im Einzelspieler: `SoundEvent`s für
`Audio::PlayAt` (Lautstärke nach Entfernung, links/rechts), Partikel über `World::Explosion` und
`World::SpawnSparks` in der Spiegelwelt, Meldungen über `World::Notify`. Der Weg eines Tons ist am
Anfang von [shared/SoundEvents.hpp](../shared/SoundEvents.hpp) Schritt für Schritt beschrieben.

Warum Ereignisse zuverlässig, Snapshots aber nicht? Ein Snapshot wird 1/15 s später durch den
nächsten ersetzt. Eine Explosion oder die Meldung „Ziel erreicht“ dagegen gibt es nur einmal – ginge
sie verloren, käme sie nie an.

---

## 10. Speicherung im Datenordner

Der Server soll nach einem Neustart noch wissen, wer welchen Fortschritt hat. Dafür sorgt `Accounts`
([server/src/Accounts.hpp](../server/src/Accounts.hpp)). Der Datenordner ist `./data` bzw. im
Container `/data` (Docker-Volume), einstellbar mit `MONDSCHUERFER_DATA` oder `--data`.

| Datei | Inhalt |
|---|---|
| `accounts.txt` | je Konto: Name (Original-Schreibweise), Schlüssel (hex), angelegt, letzte Anmeldung, gesamte Anmeldezeit, Fortschritt (Kontostand, Aufrüstungen, Ladung, Schild, Schiffszeit, Rundenzeit, Runde, Abschüsse, Abstürze, Ziele) |
| `highscores.txt` | jede erreichte Zielrunde eine Zeile |
| `server.txt` | Server-Salz, Welt-Seed, insgesamt abgelieferte Ressourcen (Ausbaustufe der Stationen) |
| `settings.txt` | nur, wenn auf der Webseite Einstellungen gespeichert wurden |
| `*.defekt-…` | Kopie einer Datei, in der beim Laden kaputte Zeilen übersprungen wurden |

Gestaltungsentscheidungen, die man im Code sieht:

- **Einfacher Text** (`name=wert`) statt einer Datenbank: zur Not mit einem Editor lesbar (bei
  gestopptem Server!), keine zusätzliche Bibliothek.
- **Atomar schreiben:** erst in `datei.tmp`, `fsync`, dann umbenennen. Ein Umbenennen ersetzt die
  Datei in einem Schritt. Fällt mitten im Schreiben der Strom aus, ist entweder die alte oder die
  neue Datei vollständig da – nie eine halbe.
- **Kaputte Zeilen überspringen**, statt den Start zu verweigern; die Originaldatei wird als
  `*.defekt-…` aufgehoben, damit man nachsehen kann.
- **Dateirechte nur für den Besitzer**, weil `accounts.txt` die Anmeldeschlüssel enthält.
- **Wann gespeichert wird:** beim Abmelden, nach Ziel oder Absturz, alle 30 s für alle angemeldeten
  Spieler, beim Beenden (SIGTERM/SIGINT).
- Das **Server-Salz** ist das Wertvollste: Ohne es passt kein gespeicherter Schlüssel mehr, und
  niemand könnte sich anmelden.

Auch das **Spiel** merkt sich etwas: [src/ClientConfig.hpp](../src/ClientConfig.hpp) speichert die
zuletzt benutzte Serveradresse und den Spielernamen in `mondschuerfer_client.txt` neben der
ausführbaren Datei – **nie** das Passwort. In derselben Datei stehen auch die Musik-Einstellungen
aus den Optionen. Deshalb gibt es davon nur ein gemeinsames Objekt (`g_clientConfig`): Der
Mehrspieler schreibt beim Speichern der Serveradresse so immer die aktuellen Musikwerte mit.

---

## 11. Die Verwaltungs-Webseite und ihre Schutzmaßnahmen

Die Webseite besteht aus drei statischen Dateien ([server/web/](../server/web/): `index.html`,
`app.js`, `style.css`) und einer kleinen JSON-Schnittstelle, die
[server/src/WebServer.cpp](../server/src/WebServer.cpp) mit cpp-httplib bereitstellt:

| Anfrage | Wirkung |
|---|---|
| `GET /`, `/app.js`, `/style.css` | die Seite selbst |
| `GET /api/status` | Auslastung, Spieler, Bestenliste, Einstellungen als JSON (die Kopie aus SharedState) |
| `POST /api/settings` | Einstellungen ändern (Formularfelder) |
| `POST /api/settings/reset` | zurück auf die Standardwerte aus `docker-compose.yml` |

Der Browser lädt die Seite einmal und fragt danach alle 2 Sekunden `/api/status` ab. Der Spiel-Thread
wird dabei nie berührt (Abschnitt 5).

Weil diese Seite den Server steuern kann, ist sie mehrfach abgesichert. Jede Maßnahme gegen eine
bestimmte Gefahr:

| Maßnahme | gegen … |
|---|---|
| Ohne `ADMIN_USER`/`ADMIN_PASSWORD` oder mit schwachem Passwort (unter 10 Zeichen, Standardwörter wie `changeme`, `passwort`, `admin`, Zahlenfolgen, gleich dem Benutzernamen) startet die Webseite **nicht** (das Spiel schon) | vergessene Standardpasswörter |
| HTTP Basic Auth; Benutzername und Passwort werden als SHA-256-Werte mit `ConstantTimeEqual` verglichen, immer beide | Raten über Antwortzeiten (nicht einmal, ob der Benutzername stimmte) |
| nach 5 Fehlversuchen ist die IP-Adresse 60 s gesperrt (Antwort 429), jede weitere Sperre doppelt so lang, höchstens 15 min | Passwort durchprobieren |
| POST nur mit Header `X-Requested-With: mondschuerfer` und passendem `Origin`/`Host` | **CSRF**: eine fremde Webseite, die im Browser des angemeldeten Betreibers heimlich ein Formular abschickt |
| Sicherheits-Header: `Content-Security-Policy`, `X-Frame-Options: DENY`, `nosniff`, `Cache-Control: no-store` | eingeschleuste Skripte, Einbetten in fremde Seiten (Clickjacking), Zwischenspeichern von Daten |
| genau sieben feste Pfade, kein allgemeines Ausliefern eines Ordners | Lesen fremder Dateien mit `/../` |
| `docker-compose.yml` gibt Port 8080 nur auf `127.0.0.1` frei | Mitlesen im Netz: Basic Auth schickt das Passwort bei jeder Anfrage nur Base64-kodiert, und der Server spricht kein HTTPS |
| optionaler HTTPS-Vorbau (Caddy, Profil `online`) | Zugriff über das Internet ohne Klartext-Passwort |

Zwei Dinge, die man bei einem Reverse-Proxy kennen sollte: Hinter ihm sehen alle Anfragen so aus,
als kämen sie vom Proxy, und die Sperre gälte für alle gemeinsam – ein Angreifer könnte den Admin
dauerhaft aussperren. Deshalb schickt Caddy die echte Adresse im Header `X-Real-IP` mit, und der
Server übernimmt ihn **nur**, wenn die Anfrage vom eingetragenen Proxy kommt
(`MONDSCHUERFER_TRUSTED_PROXY=caddy`, `WebContext::ClientAddress`). Von allen anderen wird der
Header ignoriert, denn fälschen kann ihn jeder. Außerdem muss der Proxy den `Host`-Header
unverändert weitergeben, sonst lehnt die Origin-Prüfung jede Änderung ab.

---

## 12. Einstellungen und Welt-Neuaufbau

Die Spielparameter (`GameSettings` in [shared/GameSettings.hpp](../shared/GameSettings.hpp)):
Servername, Schwierigkeit bzw. Piratenanzahl, Handelsstationen, Werften, Mondfarbe, max. Spieler.

**Woher kommen die Werte?** Kommandozeile vor Umgebungsvariablen vor eingebauten Standardwerten.
Speichert man auf der Webseite, landen die Werte in `settings.txt`, und diese Datei hat danach
Vorrang vor den Umgebungsvariablen – auch nach einem Neustart. „Zurücksetzen“ löscht sie wieder.
Ungültige Werte begrenzt `Sanitize` auf den erlaubten Bereich.

**Weg einer Änderung:**

```
 Browser --POST /api/settings--> Web-Thread: Felder prüfen
                                     |  SharedState::RequestSettings(neu)
                                     v
 Spiel-Thread, Anfang des nächsten Schritts: TakeSettingsRequest
     -> Sanitize (nie ungeprüft übernehmen) -> settings.txt speichern
     -> NeedsWorldRebuild(alt, neu)?
          nein (Name, Piraten, max. Spieler):  world.settings = neu
                                               weniger Piraten -> die am weitesten von allen
                                               Spielern entfernten verschwinden zuerst
          ja (Stationen, Mondfarbe):           RebuildWorld()
```

**Warum Stationen und Mondfarbe einen Neuaufbau brauchen:** Die Stationen werden beim Aufbau in
fester Reihenfolge aus dem Seed erzeugt, und ihre Indizes stecken in Spielerzuständen
(`dockedAt`). Die Mondfarbe ist Teil der Kulisse, die jeder Client selbst baut. Beides lässt sich
nicht „im Flug“ ändern.

`GameServer::RebuildWorld` macht:

1. Fortschritt aller angemeldeten Spieler sichern (ein laufender Absturz zählt dabei als Absturz –
   er wird nicht „weggezaubert“).
2. Neue Welt mit **neuem Seed**; `totalDelivered` und die Ausbaustufe bleiben erhalten.
3. Alle Spieler mit **derselben ID** und ihrem Fortschritt wieder hinzufügen; sie starten an einer
   Handelsstation. Meldung: „Der Server hat die Karte neu aufgebaut …“
4. Allen Clients erneut `Welcome`, `Deposits` und `Players` schicken.

Das Spiel erkennt das zweite `Welcome`, baut seine Spiegelwelt mit `BuildMirror` neu auf und
verwirft alte Snapshots (Schiffs-IDs der neuen Welt beginnen wieder bei 1). Eingabenummer und
Tastenzähler laufen dagegen weiter, und die Zeitachse (`TickTime`, gezählt ab dem ersten
`Welcome`) springt nicht zurück.

---

## 13. Spielregeln im Mehrspieler

Grundsätzlich gelten dieselben Regeln wie im Einzelspieler. Unterschiede, alle in
[shared/World.cpp](../shared/World.cpp) unter `isServer`:

- **Gemeinsame Welt, eigener Fortschritt.** Jeder Spieler hat seinen eigenen Kontostand, seine
  Aufrüstungen und seinen Scanner. Lagerstätten muss **jeder selbst** entdecken (der Server schickt
  jedem nur seine); abbauen kann sie aber jeder, also auch wegschnappen. Wracks sieht jeder in der
  Nähe ohne Scannen.
- **Mehr Spieler, mehr Kristalle:** Sollwert = 48 + 8 je weiterem Spieler, höchstens 240.
- **Gemeinsame Stationen:** Was alle zusammen abliefern (`totalDelivered`), lässt die
  Handelsstationen wachsen.
- **Absturz = neues Schiff, Fortschritt von vorn.** Nach dem Aufschlag (`PlayerShipLost`) setzt
  `ResetProgress` Kontostand, Aufrüstungen, Runde, Schiffs- und Rundenzeit zurück (die Ladung geht
  mit dem Schiff verloren); nach 4 Sekunden
  (`cfg::MP_RESPAWN_TIME`) gibt es ein neues Schiff an einer Handelsstation. Abschüsse, Abstürze und
  erreichte Ziele zählen weiter (Statistik). Wer sich **während** eines Absturzes abmeldet, stürzt
  trotzdem ab.
- **Schiffszeit und Rundenzeit.** `playTime` ist die Zeit mit dem aktuellen Schiff („Schiffszeit“),
  `roundTime` die Zeit der aktuellen Zielrunde, `round` die Nummer der Runde mit diesem Schiff.
- **Ziel erreicht** (Kontostand ≥ 100.000 beim Verkauf): `GoalReached` erzeugt ein `GoalEvent` mit
  der Rundenzeit, der Server trägt es in die **Bestenliste** ein (die 20 kürzesten Zeiten) und meldet
  es allen. Danach Kontostand 0, `round + 1`, die **Aufrüstungen bleiben**. Eine Runde 2 ist also
  leichter als Runde 1; die Bestenliste zeigt deshalb, ob mit „neuem Schiff“ oder in welcher Runde.
- **Abmelden behält den Fortschritt** (auch den Schildstand – Abmelden heilt nicht) und den
  **Ort**: Beim nächsten Anmelden steht das Schiff dort, wo es war (`PlayerProgress::posX/posZ`),
  nicht an der Handelsstation. Sonst wäre Ab- und Anmelden eine Abkürzung zur Station (Rückflug
  gespart, und die Zeit dazwischen fehlte in der Bestenliste). Nur nach einem Neuaufbau der Karte
  geht es an einer Station los.
- **Abmelden im Gefecht hilft nicht.** Wer sich außerhalb einer Station abmeldet oder die
  Verbindung verliert, dessen Schiff bleibt noch **10 Sekunden** ohne Steuerung im All
  (`LogoutPlayer`, Liste `departing_`). Wird es in der Zeit abgeschossen, beginnt der Fortschritt
  von vorn, genau wie beim Spielen. Meldet man sich innerhalb der 10 Sekunden wieder an (z. B. nach
  einem WLAN-Aussetzer), übernimmt man sein Schiff einfach wieder. Angedockt verschwindet das
  Schiff sofort – dort ist man ohnehin sicher.
- **Angedockt** ist man unverwundbar und steht still. Reparieren nur an Handelsstationen, Aufrüsten
  nur an Werften. Die Aktionen gehen als `DockAction` an den Server, der prüft, ob sie erlaubt sind.
- **Piratenanzahl** ist fest eingestellt (Schwierigkeit), statt mit dem Spielverlauf zu steigen.
- **Pause** hält im Mehrspieler die Welt nicht an – sie nimmt nur die Steuerung weg.
- **Zuschauer** brauchen kein Konto (bis zu 16 zusätzlich zu den Spielern). Sie sehen die ganze Karte
  von oben, alle Lagerstätten, die Spielerliste und die Bestenliste. Mausrad zoomt, Ziehen
  verschiebt, R zeigt die ganze Karte, ESC beendet das Zuschauen.

---

## 14. Leistung und Messwerte

Gemessen auf einem x86-Server im Container:

| Messung | Ergebnis |
|---|---|
| `--benchmark 64` (ohne Netzwerk) | Ø 0,41 ms pro Schritt = 1,2 % des Zeitbudgets von 33,3 ms; ca. 1,4 KB pro Snapshot |
| Upload je Spieler, 16 Spieler | ca. 8,8 KB/s (zusammen ca. 1,2 Mbit/s) |
| Upload je Spieler, 32 Spieler | ca. 13,5 KB/s (ca. 3,5 Mbit/s) |
| Upload je Spieler, 64 Spieler | ca. 20–26 KB/s (ca. 11–14 Mbit/s) |
| echter ENet-Lasttest, 64 Spieler + 4 Zuschauer | Ø 0,9–1,1 ms pro Schritt, CPU 4,5–7,8 % eines Kerns, 6,5 MB Arbeitsspeicher, Upload 1,4–1,9 MB/s |

**Der Engpass ist der Upload, nicht die CPU.** Warum wächst der Upload *pro Spieler* mit der
Spielerzahl? Jeder Snapshot enthält die Schiffe in Sichtweite. Mehr Spieler → mehr Schiffe in der
Nähe → größere Snapshots. Und es gibt mehr Empfänger. Zusammen wächst die Datenmenge also schneller
als die Spielerzahl (im ungünstigsten Fall quadratisch: n Empfänger × n Schiffe) – die Sichtweiten
und die Obergrenzen je Snapshot (80 Schiffe, 128 Geschosse) bremsen das.

Daraus abgeleitete Empfehlung (dieselbe Tabelle wie in [server/README.md](../server/README.md)):

| System | Empfohlene max. Spieler | Bemerkung |
|---|---|---|
| Raspberry Pi 4 (4 GB) | 24 | per LAN-Kabel; über WLAN eher 8–12; Upload ca. 2,5 Mbit/s |
| Raspberry Pi 5 | 48 | Upload ca. 8 Mbit/s |
| AMD Ryzen 5 / Intel Core i5 (ab ca. 2018) oder besser | 64 (Obergrenze) | Upload ca. 12–15 Mbit/s |

Die Raspberry-Pi-Zeilen sind aus der x86-Messung **geschätzt**, nicht auf einem Pi gemessen.

Selbst messen kann man auf zwei Wegen:

- `mondschuerfer-server --benchmark N [--seconds S]` rechnet dieselbe `Step()`-Funktion mit N
  „Bots“, deren Eingaben im Programm entstehen; gesendete Bytes werden nur gezählt. Misst also die
  reine Rechenarbeit und die Datenmenge, ohne Netzwerk.
- `mondschuerfer-loadtest --server host:7777 --bots N` verbindet N Bots **über das Netzwerk** mit
  einem laufenden Server – mit Anmeldung, Eingaben und allem, was zurückkommt. Die Last des Servers
  zeigt dabei seine Webseite.

Die Bots fliegen dicht beieinander und schießen viel; echte Spieler verteilen sich mehr. Die
Messwerte sind daher eher eine Obergrenze.

---

## 15. Wo fange ich an zu lesen?

Eine Reihenfolge, in der jede Datei auf der vorigen aufbaut. Bei den großen `.cpp`-Dateien lohnt es,
zuerst den Kopfkommentar zu lesen und dann gezielt die genannten Funktionen.

1. **[shared/World.hpp](../shared/World.hpp)** – Kopfkommentar (die drei Einsätze), dann
   `PlayerState`, `Ship`, die Ereignislisten und `UpdateShipPhysics`. Danach versteht man, was der
   Server rechnet und der Client spiegelt.
2. **[shared/Config.hpp](../shared/Config.hpp)** – nur die Werte `MP_…` (Respawn-Zeit,
   Lagerstätten je Spieler).
3. **[shared/Protocol.hpp](../shared/Protocol.hpp)** – Kopfkommentar (Ablauf), Konstanten,
   `MsgType`, die Nachrichten-Strukturen. Dann in **[shared/Protocol.cpp](../shared/Protocol.cpp)**
   `Writer`, `Reader` und `Encode`/`Decode` für `SnapshotMsg` als Beispiel.
4. **[server/src/main.cpp](../server/src/main.cpp)** – wie der Server startet: Umgebungsvariablen,
   Kommandozeile, Signale. Kurz und ein guter Einstieg in die Serverseite.
5. **[server/src/GameServer.hpp](../server/src/GameServer.hpp)**, dann
   **[server/src/GameServer.cpp](../server/src/GameServer.cpp)**: `Run`, `Step`, `OnReceive`,
   `HandleInput` und `ApplyInputs`, `DistributeWorldEvents`, `MakeSnapshot`, `HandleLoginStart` und
   `HandleLoginProof`, `RebuildWorld`.
6. **[shared/Crypto.hpp](../shared/Crypto.hpp)** – die Anmeldung in Kommentarform. Wer mag:
   [shared/Crypto.cpp](../shared/Crypto.cpp) mit SHA-256 und PBKDF2 zum Nachvollziehen.
7. **[server/src/LoadTest.cpp](../server/src/LoadTest.cpp)** – ein vollständiger Client **ohne**
   Grafik und ohne Spiegelwelt. Gut, um das Protokoll einmal von der Client-Seite zu sehen, bevor
   Vorhersage und Interpolation dazukommen.
8. **[src/NetClient.hpp](../src/NetClient.hpp)**, dann **[src/NetClient.cpp](../src/NetClient.cpp)**:
   `Update` (Akkumulator), `StepOwnShip`, `ApplyMe` (Abgleich), `UpdateOwnDisplay`,
   `UpdateRemoteShips` (Interpolation), `SyncTime`.
9. **[src/Multiplayer.hpp](../src/Multiplayer.hpp)** – die Phasen der Menüs (Address → Lobby →
   Login → Playing/Spectating), dann in [src/Multiplayer.cpp](../src/Multiplayer.cpp)
   `ReadGameInput` und `HandleNetwork`. Einstieg aus dem Hauptmenü: `EnterMultiplayer` in
   [src/main.cpp](../src/main.cpp). `ReadGameInput` liest Tastatur, Maus und Controller über
   `PilotControls` ([src/Controls.hpp](../src/Controls.hpp)) – dieselbe Steuerung wie im
   Einzelspieler; ins Netz gehen davon nur Schub, Drehen, Feuer und die Tastendrücke.
10. **[server/src/SharedState.hpp](../server/src/SharedState.hpp)** – kurz, zeigt das
    Mutex-Muster. Danach [server/src/WebServer.hpp](../server/src/WebServer.hpp) und
    [server/src/WebServer.cpp](../server/src/WebServer.cpp) (Kopfkommentar „Ablauf einer Anfrage“)
    sowie [server/web/app.js](../server/web/app.js).
11. **[server/src/Accounts.hpp](../server/src/Accounts.hpp)** und
    [server/src/Accounts.cpp](../server/src/Accounts.cpp) – Dateiformat, atomares Schreiben,
    Schreib-Thread.
12. Zum Schluss der Betrieb: [docker-compose.yml](../docker-compose.yml) (ausführlich kommentiert),
    [server/Dockerfile](../server/Dockerfile), [server/README.md](../server/README.md).

---

## 16. Übungsideen

Für alle Übungen gilt: Server ohne Docker bauen (siehe [README.md](../README.md#mehrspieler-server-ohne-docker-bauen)),
Server und zwei Spiele auf demselben Rechner starten (`--mp-connect localhost`), ausprobieren.

### Übung 1: Netzwerkstörungen sichtbar machen

Unter Linux kann man der lokalen Verbindung künstlich Verzögerung und Paketverlust geben:

```bash
sudo tc qdisc add dev lo root netem delay 100ms 20ms loss 5%   # 100 ms ± 20 ms, 5 % Verlust
# ... spielen ...
sudo tc qdisc del dev lo root                                    # WICHTIG: wieder entfernen
```

Beobachte: Das eigene Schiff reagiert trotzdem sofort (Vorhersage), fremde Schiffe gleiten weich.
Dann nacheinander ausprobieren (und jeweils wieder rückgängig machen):

- `INTERP_DELAY` in [shared/Protocol.hpp](../shared/Protocol.hpp) auf `0.0f` setzen → fremde
  Schiffe ruckeln. Warum? (Tipp: Für „jetzt“ gibt es nie einen Snapshot *danach*.)
- In `NetClient::Impl::ApplyMe` die Schleife, die `pending` erneut anwendet, auskommentieren →
  das eigene Schiff wird bei jedem Snapshot zurückgezogen („Gummiband“). Warum genau um die
  Laufzeit?
- `CORRECTION_RATE` in [src/NetClient.cpp](../src/NetClient.cpp) auf 1 oder 50 setzen und den
  Unterschied beschreiben.

### Übung 2: Eine eigene Nachricht im Protokoll ergänzen – „Winken“

Ziel: Taste G lässt alle Spieler in der Nähe die Meldung „Anna winkt dir zu“ sehen. Daran lernt man
den kompletten Weg einer Nachricht.

1. **Protokoll** ([shared/Protocol.hpp](../shared/Protocol.hpp)): `MsgType::Wave = 8` im Block
   „Client → Server“ ergänzen. Die Nachricht hat keinen Inhalt, also genügt `EncodeSimple`.
   `PROTOCOL_VERSION` um eins erhöhen. *Frage: Was passiert sonst, wenn ein altes Spiel mit dem
   neuen Server spricht?*
2. **Server** ([server/src/GameServer.cpp](../server/src/GameServer.cpp)): In `OnReceive` einen
   `case net::MsgType::Wave:` ergänzen, der `HandleWave(c)` aufruft (in `GameServer.hpp`
   deklarieren). Darin:
   - nur für `c.role == Role::Player`;
   - **Flutbremse:** höchstens einmal pro 2 Sekunden (neues Feld `double lastWaveAt` in `Client`,
     verglichen mit `Now()`). Warum ist das nötig, obwohl es schon `MAX_MSGS_PER_SECOND` gibt?
   - für jeden anderen Spieler, dessen Schiff im Umkreis `net::AOI_EVENTS` liegt:
     `world_.Notify(*anderer, c.name + " winkt dir zu", Farbe)`.
   Mehr ist nicht nötig: `Notify` legt die Meldung auf dem Server in die `outbox`, und
   `DistributeWorldEvents` verschickt sie schon als `Notice`.
3. **Spiel:** In `NetClient` eine Methode `SendWave()` ergänzen, die
   `net::EncodeSimple(net::MsgType::Wave)` zuverlässig schickt. In `MultiplayerScreen::ReadGameInput`
   ([src/Multiplayer.cpp](../src/Multiplayer.cpp)) bei `IsKeyPressed(KEY_G)` aufrufen.
4. **Testen** mit zwei Spielen nebeneinander. Den Lasttest neu bauen (er benutzt dasselbe
   Protokoll).

Erweiterung: eine **Kurznachricht mit Text** (`ChatMsg{std::string text}`). Jetzt muss der Server
fremden Text annehmen: Länge begrenzen, nur druckbare Zeichen zulassen, Flutbremse. Überlege, warum
jede dieser Prüfungen auf dem **Server** stehen muss und nicht nur im Spiel.

### Übung 3: Snapshot-Rate und Datenmenge

Ändere `SNAPSHOT_EVERY` auf 1 (30/s) und 3 (10/s) und miss jeweils mit
`mondschuerfer-server --benchmark 32`. Wie ändern sich KB/s pro Spieler? Was muss mit
`INTERP_DELAY` passieren, wenn Snapshots seltener kommen (Faustregel: mindestens zwei Abstände)?

### Übung 4: Quantisierung nachrechnen

Rechne die Beispiele aus Abschnitt 7 von Hand nach. Dann: Wie groß dürfte die Karte höchstens sein,
damit `QPos` noch reicht? Was müsste man ändern, um eine 1000 × 1000 große Karte zu unterstützen,
ohne die Pakete größer zu machen – und welche Genauigkeit hätte man dann?

### Übung 5: Ein Testprogramm für Writer und Reader

Schreibe in einem eigenen Ordner ein kleines Programm, das jede Nachricht kodiert, wieder dekodiert
und die Felder vergleicht (Roundtrip-Test). Zweiter Teil: Füttere `Decode` mit zufälligen Bytes und
abgeschnittenen echten Nachrichten. Es darf nie abstürzen, nur `false` liefern. Mit
`-fsanitize=address,undefined` kompiliert findet man so auch versteckte Fehler (das nennt man
„Fuzzing“).

### Übung 6: Ein neuer Wert auf der Webseite

Zeige die Anzahl der Wracks auf der Verwaltungsseite an. Der Weg führt durch alle Threads: Feld in
`ServerStatus` ([server/src/SharedState.hpp](../server/src/SharedState.hpp)) → in
`GameServer::PublishStatus` füllen (Spiel-Thread) → in `WebServer.cpp` ins JSON von `/api/status`
schreiben (Web-Thread) → in [server/web/app.js](../server/web/app.js) anzeigen. Prüfe dabei: An
welcher Stelle wird kopiert, wo gesperrt?

### Übung 7: Bestenliste nach Runden

Die Bestenliste mischt „mit neuem Schiff“ (Runde 1) und spätere Runden mit behaltenen Aufrüstungen.
Baue im Zuschauer-Bildschirm einen Umschalter, der nur Runde-1-Einträge zeigt. Brauchst du dafür eine
Änderung am Protokoll oder am Server?

---

## 17. Glossar

| Begriff | Bedeutung |
|---|---|
| **autoritativer Server** | nur der Server entscheidet, was wirklich passiert; Clients schicken nur Eingaben |
| **Tick** | ein Simulationsschritt von 1/30 s |
| **Snapshot** | Momentaufnahme der Umgebung eines Spielers, 15-mal pro Sekunde |
| **AOI** (Area of Interest) | Sichtweite: nur was darin liegt, kommt in den Snapshot |
| **Vorhersage** (prediction) | das Spiel bewegt das eigene Schiff sofort selbst, ohne auf den Server zu warten |
| **Abgleich** (reconciliation) | auf den Serverstand zurücksetzen und unbestätigte Eingaben erneut anwenden |
| **Interpolation** | fremde Schiffe zwischen zwei Snapshots weich überblenden |
| **Extrapolation** | mit der letzten Geschwindigkeit weiterrechnen, wenn Daten fehlen |
| **RTT** | Rundlaufzeit: hin zum Server und zurück |
| **Quantisierung** | Gleitkommazahl → kleine Ganzzahl mit fester Einheit |
| **little endian** | niedrigstes Byte einer Zahl zuerst |
| **Seed** | Startwert des Zufallsgenerators; gleicher Seed → gleiche Zahlenfolge |
| **Nonce** | Zufallszahl, die nur einmal benutzt wird |
| **PBKDF2** | absichtlich langsame Ableitung eines Schlüssels aus einem Passwort |
| **HMAC** | Prüfsumme, die nur mit dem Schlüssel berechnet werden kann |
| **Salz** | Zusatzdaten für PBKDF2, damit gleiche Passwörter verschiedene Schlüssel ergeben |
| **Mutex** | Sperre, die verhindert, dass zwei Threads gleichzeitig dieselben Daten benutzen |
| **Data Race** | zwei Threads greifen ohne Sperre gleichzeitig auf dieselben Daten zu (mindestens einer schreibt) |
| **atomar schreiben** | Datei so ersetzen, dass sie nie halb geschrieben sein kann (temporäre Datei + umbenennen) |
| **CSRF** | eine fremde Webseite lässt den Browser des Opfers unbemerkt Anfragen an eine andere Seite schicken |
