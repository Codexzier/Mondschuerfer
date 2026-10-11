# Mondschürfer-Server

Der Mehrspieler-Server für Mondschürfer: Er rechnet die gemeinsame Spielwelt, verwaltet die
Spielerkonten und bringt eine kleine, passwortgeschützte **Verwaltungs-Webseite** mit. Gedacht ist
er für den Betrieb als **Docker-Container** (z. B. auf einem Raspberry Pi oder einem Heimserver),
er lässt sich aber auch ohne Docker bauen und starten.

Wie der Mehrspieler intern funktioniert (Threads, Snapshots, Vorhersage, Anmeldung …), erklärt
[docs/Mehrspieler.md](../docs/Mehrspieler.md). Diese Datei hier ist die **Betriebsanleitung**.

**Inhalt**

1. [Was macht der Server?](#1-was-macht-der-server)
2. [Schnellstart mit Docker](#2-schnellstart-mit-docker)
3. [Einstellungen](#3-einstellungen)
4. [Ports, Firewall und Router](#4-ports-firewall-und-router)
5. [Hardware-Empfehlung](#5-hardware-empfehlung)
6. [Sicherheit](#6-sicherheit)
7. [Datensicherung und Wiederherstellung](#7-datensicherung-und-wiederherstellung)
8. [Updates](#8-updates)
9. [Fehlersuche](#9-fehlersuche)
10. [Ohne Docker bauen und starten](#10-ohne-docker-bauen-und-starten)
11. [Lasttest](#11-lasttest)
12. [Dateien in diesem Ordner](#12-dateien-in-diesem-ordner)

---

## 1. Was macht der Server?

- **Spielwelt:** Er rechnet 30-mal pro Sekunde die gemeinsame Welt (Flug, Scanner, Abbau, Kampf,
  Piraten, Stationen) mit genau derselben Spiellogik wie der Einzelspieler (`shared/`). Er ist
  *autoritativ*: Was wirklich passiert, entscheidet der Server; das Spiel sagt nur das eigene
  Schiff voraus, damit sich die Steuerung direkt anfühlt.
- **Netzwerk:** Die Spieler verbinden sich über **UDP-Port 7777** (Bibliothek ENet). Jeder Spieler
  bekommt 15-mal pro Sekunde eine Momentaufnahme seiner Umgebung, dazu Töne, Explosionen,
  Meldungen, Spielerliste und Bestenliste.
- **Konten:** Spieler legen im Spiel ein Konto (Name + Passwort) an. Kontostand, Aufrüstungen,
  Zielrunde, Abschüsse usw. bleiben zwischen zwei Sitzungen erhalten. Das Passwort selbst bekommt
  der Server nie zu sehen (siehe [Sicherheit](#6-sicherheit)).
- **Zuschauer:** Bis zu 16 Zuschauer können zusätzlich zu den Spielern die ganze Karte von oben
  ansehen, ohne ein Konto zu brauchen.
- **Speichern:** Konten, Bestenliste, Server-Salz, Kartennummer (Seed), Ausbaustufe der
  Stationen und die auf der Webseite geänderten Einstellungen liegen im **Datenordner**
  (`/data` im Container). Gespeichert wird beim Abmelden, nach Ziel oder Absturz, alle 30 Sekunden
  und beim Beenden.
- **Verwaltungsseite (HTTP, Port 8080):** Auslastung (Rechenzeit pro Schritt, CPU, Speicher,
  Netzwerk), angemeldete Spieler mit Fortschritt und Ping, Bestenliste, Spielparameter ändern.
  Geschützt mit Benutzername und Passwort, standardmäßig **nur auf dem Server-Rechner selbst**
  erreichbar.

---

## 2. Schnellstart mit Docker

**Voraussetzung:** Docker mit dem Compose-Plugin. Prüfen mit `docker compose version`.
(Linux: Paket *Docker Engine* bzw. auf dem Raspberry Pi `curl -fsSL https://get.docker.com | sh`;
Windows/macOS: *Docker Desktop*.)

Alle Befehle im **Hauptordner** des Projekts ausführen (dort liegt `docker-compose.yml`):

```bash
# 1. Vorlage kopieren (einmalig)
cp .env.example .env

# 2. In .env ein sicheres ADMIN_PASSWORD eintragen (mind. 10 Zeichen, kein Standardwort)
nano .env                    # oder ein anderer Editor
chmod 600 .env               # Linux: nur für dich lesbar

# 3. Bauen und im Hintergrund starten (das erste Bauen dauert einige Minuten,
#    auf einem Raspberry Pi deutlich länger)
docker compose up -d --build

# 4. Kontrollieren: Startmeldung mit Ports, Datenordner und Einstellungen
docker compose logs server
```

Danach:

- **Webseite:** <http://localhost:8080> im Browser auf dem Server-Rechner öffnen, anmelden mit
  `ADMIN_USER` / `ADMIN_PASSWORD` aus `.env`. (Server ohne Bildschirm? Siehe
  [SSH-Tunnel](#4-ports-firewall-und-router).)
- **Spielen:** Im Spiel *Hauptmenü → Mehrspieler*, als Adresse den Server-Rechner eintragen
  (`localhost`, eine LAN-Adresse wie `192.168.1.20` oder eine Domain, optional mit `:Port`),
  *Verbinden → Mitspielen → Neues Konto anlegen*. Oder *Zuschauen*.

Fehlt das Passwort in `.env`, startet Compose gar nicht erst:

```
required variable ADMIN_PASSWORD is missing a value: Bitte ADMIN_PASSWORD in .env setzen (...)
```

### Wichtige Befehle

| Befehl | Wirkung |
|---|---|
| `docker compose up -d --build` | (neu) bauen und starten; auch nach Änderungen an `.env` oder am Code |
| `docker compose logs -f server` | Ausgaben live ansehen (Strg+C beendet nur die Anzeige) |
| `docker compose ps` | Läuft der Container? Welche Ports? |
| `docker compose stop` / `start` | anhalten / wieder starten (Server speichert vorher alles) |
| `docker compose restart server` | neu starten |
| `docker compose down` | stoppen und Container entfernen – **die Daten im Volume bleiben** |
| `docker compose run --rm server --help` | alle Kommandozeilen-Optionen des Servers |
| `docker stats mondschuerfer-server-1` | CPU- und Speicherverbrauch live |

> **Vorsicht:** `docker compose down -v` löscht zusätzlich das Daten-Volume – also alle Konten,
> die Bestenliste und das Server-Salz. Nur benutzen, wenn du wirklich von vorn anfangen willst.

---

## 3. Einstellungen

Es gibt drei Quellen, mit klarer Rangfolge:

1. **Kommandozeile** (z. B. `--port 7800`) – schlägt alles andere; im Docker-Betrieb selten nötig.
2. **Umgebungsvariablen** – im Docker-Betrieb aus `.env`, eingesetzt in `docker-compose.yml`.
3. **Eingebaute Standardwerte** des Programms.

Für die **Spielparameter** (Servername, Schwierigkeit, Stationen, Mondfarbe, max. Spieler) gibt es
zusätzlich die **Webseite**: Speichert man dort, schreibt der Server die Werte nach
`/data/settings.txt`. Diese Datei hat danach **Vorrang vor `.env`** – auch nach einem Neustart.
Ändert man danach `DIFFICULTY` in `.env`, passiert also nichts. **„Zurücksetzen“** auf der Webseite
löscht `settings.txt`; ab dann gelten wieder die Werte aus `.env`/`docker-compose.yml`. In der
Startmeldung steht, woher die Werte kommen (`Einstellungen : aus settings.txt …` bzw.
`… aus den Umgebungsvariablen`).

### Umgebungsvariablen

| Variable | Standard | Bedeutung |
|---|---|---|
| `ADMIN_USER` | – (Pflicht) | Benutzername der Webseite, ohne `:` |
| `ADMIN_PASSWORD` | – (Pflicht) | Passwort der Webseite: mind. 10 Zeichen, kein Standardwort (`changeme`, `passwort`, `admin`, `mondschuerfer`, Zahlenfolgen, gleich dem Benutzernamen …). Sonst startet nur die Webseite nicht, das Spiel läuft trotzdem. |
| `SERVER_NAME` | `Mondschürfer-Server` | Name in der Lobby des Spiels (max. 40 Zeichen) |
| `DIFFICULTY` | `medium` | `easy`/`leicht` (keine Piraten), `medium`/`mittel` (3), `hard`/`schwer` (20), `custom`/`benutzerdefiniert` |
| `ENEMY_SHIPS` | leer (= 3) | Anzahl Piraten 0–40, **nur** mit `DIFFICULTY=custom` |
| `TRADE_STATIONS` | `1` | Handelsstationen 1–5 (verkaufen, reparieren) |
| `SHIPYARDS` | `1` | Werften 1–4 (aufrüsten) |
| `MOON_COLOR` | `grey` | `grey`/`grau`, `orange`, `blue`/`blau` |
| `MAX_PLAYERS` | `16` | gleichzeitig angemeldete Spieler, 1–64 (plus bis zu 16 Zuschauer) |
| `MAX_CONNECTIONS_PER_IP` | `8` | max. Verbindungen je öffentlicher IP-Adresse, `0` = unbegrenzt; Heimnetz-Adressen sind ausgenommen |
| `DOMAIN` | leer | nur für das Profil `online` (HTTPS mit Caddy), z. B. `mond.example.org` |

Nur für den Betrieb **ohne Docker** interessant (im Container fest eingestellt, in
`docker-compose.yml` unter „Innenleben“):

| Variable | Kommandozeile | Standard ohne Docker | im Container |
|---|---|---|---|
| `MONDSCHUERFER_PORT` | `--port N` | `7777` (UDP) | `7777` |
| `MONDSCHUERFER_WEB_PORT` | `--web-port N` | `8080` (TCP) | `8080` |
| `MONDSCHUERFER_WEB_BIND` | `--web-bind ADRESSE` | `0.0.0.0` (alle Netzwerkkarten) | `0.0.0.0` |
| `MONDSCHUERFER_DATA` | `--data ORDNER` | `./data` | `/data` |
| `MONDSCHUERFER_WEB_ROOT` | `--web-root ORDNER` | `<Programmordner>/web` | `/app/web` |
| – | `--no-web` | Webseite aus | – |
| `MAX_CONNECTIONS_PER_IP` | `--max-per-ip N` | `8` | `8` |
| – | `--benchmark N [--seconds S]` | Leistungstest, siehe [Hardware](#5-hardware-empfehlung) | – |

Ungültige Werte (z. B. `MAX_PLAYERS=500`) werden mit einer Warnung im Log auf den erlaubten
Bereich begrenzt.

### Die Webseite

Die Seite aktualisiert sich alle 2 Sekunden selbst und zeigt:

- **Auslastung:** Rechenzeit pro Simulationsschritt (Durchschnitt/Maximum; das Budget sind
  33,3 ms), Auslastung in Prozent, CPU, Arbeitsspeicher, Netzwerk ein/aus, Laufzeit.
- **Spieler:** Name, Fortschritt zum Ziel (100.000), Kontostand, Runde, Aufrüstungen, Schiffszeit,
  Sitzungsdauer, gesamte Anmeldezeit, Abschüsse, Abstürze, Ziele, Ping.
- **Bestenliste:** die 20 schnellsten Zielrunden.
- **Einstellungen:** Servername, Schwierigkeit, Piratenanzahl, Stationen, Mondfarbe, max. Spieler.
  - Änderungen an **Stationen oder Mondfarbe** bauen die Karte neu auf: Alle Spieler starten an
    einer Handelsstation neu, ihr Fortschritt (Kontostand, Aufrüstungen …) bleibt erhalten.
  - **Weniger Piraten** wirken sofort (die am weitesten entfernten verschwinden zuerst).
  - **Weniger max. Spieler** gilt für neue Anmeldungen; wer schon spielt, bleibt.
  - **Zurücksetzen** stellt die Werte aus `.env`/`docker-compose.yml` wieder her.

---

## 4. Ports, Firewall und Router

| Port | Wofür | Von außen erreichbar? |
|---|---|---|
| **7777/UDP** | das Spiel | ja – dafür ist er da |
| **8080/TCP** | Verwaltungsseite (HTTP) | **nein**, nur `127.0.0.1` (dieser Rechner) |
| 80/TCP, 443/TCP+UDP | HTTPS-Vorbau Caddy | nur mit Profil `online` |

**Im Heimnetz (LAN)** genügt meist schon der Start: Die anderen geben die LAN-Adresse des
Server-Rechners ein (Linux: `hostname -I`, Windows: `ipconfig`).

**Für Spieler aus dem Internet:**

1. **Router:** Portweiterleitung **UDP 7777** auf die LAN-Adresse des Server-Rechners einrichten
   (bei der FRITZ!Box: *Internet → Freigaben → Portfreigaben*). Dem Rechner im Router am besten
   immer dieselbe Adresse geben.
2. **Firewall des Rechners:** z. B. `sudo ufw allow 7777/udp`. (Hinweis: Von Docker freigegebene
   Ports umgehen ufw oft ohnehin, weil Docker eigene iptables-Regeln setzt – genau deshalb ist die
   Webseite in `docker-compose.yml` an `127.0.0.1` gebunden.)
3. **Adresse für die Mitspieler:** die öffentliche IPv4-Adresse des Anschlusses oder besser ein
   DynDNS-Name (z. B. von der FRITZ!Box/MyFRITZ!, DuckDNS), weil sich die IP-Adresse ändern kann.
4. **Testen** am besten von außen, z. B. ein Laptop im Handy-Hotspot.

> **Kein eigenes IPv4?** Viele Anschlüsse (z. B. mit *DS-Lite*, oft bei Kabel- und
> Glasfaseranschlüssen) haben keine eigene öffentliche IPv4-Adresse; dann kommt von außen über
> IPv4 nichts an, und der Server lauscht nur auf IPv4. Auswege: ein VPN wie **Tailscale** oder
> **WireGuard** (siehe [Sicherheit](#6-sicherheit)) oder ein kleiner gemieteter Server (vServer).

**Anderer Spielport nach außen:** in `docker-compose.yml` nur die linke Seite ändern, z. B.
`"7800:7777/udp"`. Die Spieler verbinden sich dann mit `adresse:7800`.

**Webseite von einem anderen Rechner aus ansehen**, ohne etwas zu öffnen – per SSH-Tunnel:

```bash
ssh -L 8080:127.0.0.1:8080 benutzer@server-rechner
# und dann auf dem EIGENEN Rechner: http://localhost:8080
```

Für den Zugriff über das Internet siehe Profil `online` unter [Sicherheit](#6-sicherheit).

---

## 5. Hardware-Empfehlung

Die Simulation selbst ist sparsam: Auf einem aktuellen x86-Kern braucht ein Simulationsschritt mit
64 Spielern im Schnitt nur etwa 0,4 ms von 33,3 ms (gut 1 %). Der Engpass ist fast immer die
**Upload-Bandbreite** der Internetleitung – und die Übersicht auf der 300×300-Karte.

<!-- HARDWARE-EMPFEHLUNG -->
<!-- Einzige Hardware-Tabelle dieser Datei (zum leichten Aktualisieren nach neuen Messungen).
     Grundlage: --benchmark 64 und ein ENet-Lasttest auf einem x86-Server. Die Raspberry-Pi-Werte
     sind daraus geschätzt, NICHT auf einem Pi gemessen. -->
| System | Empfohlene max. Spieler | Bemerkung |
|---|---|---|
| Raspberry Pi 4 (4 GB) | 24 | per LAN-Kabel; über WLAN eher 8–12; Upload ca. 2,5 Mbit/s |
| Raspberry Pi 5 | 48 | Upload ca. 8 Mbit/s |
| AMD Ryzen 5 / Intel Core i5 (ab ca. 2018) oder besser | 64 (Obergrenze) | Upload ca. 12–15 Mbit/s |
<!-- /HARDWARE-EMPFEHLUNG -->

Gemessen auf einem x86-Server (Container):

- `--benchmark 64`: Ø 0,41 ms pro Schritt (1,2 % des Zeitbudgets), ca. 1,4 KB pro Snapshot.
- Upload pro Spieler wächst mit der Spielerzahl (mehr Schiffe in Sichtweite): bei 16 Spielern
  ca. 8,8 KB/s (zusammen ca. 1,2 Mbit/s), bei 32 ca. 13,5 KB/s (ca. 3,5 Mbit/s), bei 64 ca.
  20–26 KB/s (ca. 11–14 Mbit/s).
- Echter Netzwerk-Lasttest mit 64 Spielern + 4 Zuschauern: Ø 0,9–1,1 ms pro Schritt,
  CPU 4,5–7,8 % eines Kerns, 6,5 MB Arbeitsspeicher, Upload 1,4–1,9 MB/s.
- Die Raspberry-Pi-Zeilen sind aus diesen Werten **geschätzt** (nicht auf einem Pi gemessen).
  Am besten selbst messen:

**Selbst messen** (auf dem Rechner, auf dem der Server laufen soll):

```bash
docker compose run --rm server --benchmark 64              # 64 simulierte Spieler, 20 s Spielzeit
docker compose run --rm server --benchmark 32 --seconds 60 # 32 Spieler, 60 s
```

Das läuft neben einem laufenden Server (es öffnet keine Ports) und gibt aus: Rechenzeit pro Schritt
(Durchschnitt, 95 %, Maximum), die **Auslastung** in Prozent des Zeitbudgets, die Datenmenge pro
Spieler und gesamt sowie eine Einschätzung für typische Internetleitungen. Faustregeln:

- **Auslastung** unter ca. 50 % (auch beim 95-%-Wert): genug Reserve für Kampfspitzen.
- **Upload gesamt** höchstens ca. 70 % des Uploads der Leitung (Speedtest, Wert „Upload“).
- Die Bots fliegen dicht beieinander und schießen viel – echte Spieler verteilen sich mehr.
  Die Messwerte sind daher eher eine Obergrenze.

Raspberry Pi: am besten das **64-Bit**-Raspberry-Pi-OS verwenden und den Pi per **Kabel**
anschließen. Bricht das Bauen auf einem Pi mit wenig Speicher ab („Killed“):
`docker compose build --build-arg BUILD_JOBS=1`.

---

## 6. Sicherheit

### Die Verwaltungsseite

- **Nur über HTTPS ins Internet.** Die Seite benutzt *HTTP Basic Auth*: Der Browser schickt
  Benutzername und Passwort bei **jeder** Anfrage mit, nur Base64-kodiert – das ist keine
  Verschlüsselung. Über unverschlüsseltes HTTP kann jeder mitlesen, der den Datenverkehr sieht
  (öffentliches WLAN, Provider …). Der Server selbst spricht kein HTTPS.
- **Standard: nur lokal.** `docker-compose.yml` gibt Port 8080 nur auf `127.0.0.1` frei. Zugriff
  von woanders: [SSH-Tunnel](#4-ports-firewall-und-router), VPN oder das Profil `online`.
  **Port 8080 nie im Router freigeben.**
- **Passwortregeln:** mindestens 10 Zeichen, keine Standardwörter/Platzhalter, nicht gleich dem
  Benutzernamen. Sonst verweigert der Server den Start der Webseite (das Spiel läuft trotzdem) und
  schreibt den Grund ins Log. Empfehlung: 20+ zufällige Zeichen aus einem Passwortmanager.
- **Sperre bei Fehlversuchen:** Nach 5 falschen Anmeldungen ist die IP-Adresse 60 Sekunden
  gesperrt (Antwort 429), jede weitere Sperre dauert doppelt so lange, höchstens 15 Minuten.
  Eine erfolgreiche Anmeldung setzt den Zähler absichtlich nicht zurück.
- **Schutz gegen fremde Webseiten (CSRF):** Änderungen werden nur angenommen, wenn sie von der
  Seite selbst kommen (eigener Header, Origin- und Host-Prüfung). Dazu kommen
  Sicherheits-Header (Content-Security-Policy, kein Einbetten in fremde Seiten, kein Zwischenspeichern).

### Verwaltungsseite über das Internet: Profil `online` (Caddy, HTTPS)

`docker-compose.yml` enthält einen optionalen zweiten Dienst: **Caddy**, ein Webserver, der sich
automatisch ein kostenloses **Let's-Encrypt-Zertifikat** holt und verlängert. Er nimmt HTTPS auf
Port 443 an und reicht die Anfragen im internen Docker-Netz an den Server weiter.

1. Eine Domain oder einen DynDNS-Namen auf die öffentliche IP des Anschlusses zeigen lassen.
2. In `.env`: `DOMAIN=mond.example.org`
3. Im Router **TCP 80 und TCP 443** an den Server-Rechner weiterleiten (Port 80 braucht Let's
   Encrypt zur Bestätigung; danach leitet Caddy dort nur auf HTTPS um).
4. Starten: `docker compose --profile online up -d --build`
5. Aufrufen: `https://mond.example.org` – Anmeldung wie gewohnt.

Konfiguration: [Caddyfile](Caddyfile). Zertifikate liegen im Volume `mondschuerfer-caddy-data`.
Stoppen: `docker compose --profile online down` (ohne `--profile online` werden nur die Dienste
ohne Profil angefasst).

Gut zu wissen:

- Hinter dem Proxy sehen alle Anfragen für den Server so aus, als kämen sie von Caddy (der
  Header `X-Forwarded-For` wird absichtlich nicht ausgewertet, weil man ihn fälschen kann).
  Die Sperre nach Fehlversuchen gilt dann für **alle** gemeinsam: Ein Angreifer kann zwar nicht
  schneller raten, dich aber bis zu 15 Minuten aussperren. Der lokale Zugang (`127.0.0.1:8080`,
  SSH-Tunnel) hat einen eigenen Zähler und funktioniert weiter.
- Einen **anderen Reverse-Proxy** (nginx, Traefik …) kann man genauso benutzen. Wichtig: Er muss
  den **Host-Header unverändert** weitergeben, sonst lehnt die Origin-Prüfung jede Änderung ab
  (nginx: `proxy_set_header Host $http_host;`; Caddy macht das automatisch).
- Wer die Seite gar nicht öffentlich braucht, nimmt besser ein **VPN** (siehe unten) – dann ist
  sie aus dem Internet überhaupt nicht sichtbar.

### Spieler-Passwörter und die Spielverbindung

- Das **Passwort verlässt nie den Rechner des Spielers.** Das Spiel berechnet daraus mit
  **PBKDF2-HMAC-SHA256** (20.000 Runden, „gesalzen“ mit einem Zufallswert dieses Servers und dem
  Spielernamen) einen Schlüssel. Nur dieser Schlüssel wird beim Anlegen des Kontos übertragen
  und gespeichert.
- **Anmelden mit Challenge-Response:** Der Server schickt eine frische Zufallszahl (Nonce), das
  Spiel antwortet mit `HMAC(Schlüssel, Nonce + Name)`. Wer eine Anmeldung mitschneidet, kann sie
  nicht wiederverwenden – die nächste Nonce ist eine andere.
- **Begrenzungen:** höchstens 5 Fehlversuche pro Verbindung (dann Trennung), höchstens 10 pro
  Konto in 10 Minuten, höchstens 10 neue Konten pro Stunde und öffentlicher IP-Adresse,
  höchstens 10.000 Konten insgesamt.
- **Grenzen dieses Verfahrens** (bewusst einfach gehalten, siehe `shared/Crypto.hpp`):
  - Beim **Anlegen** eines Kontos geht der Schlüssel unverschlüsselt übers Netz. Wer genau
    diesen Moment mitliest, kann sich später als dieser Spieler anmelden (das Passwort selbst
    erfährt er nicht).
  - Wer die Datei **`accounts.txt`** (oder eine Sicherung davon) stiehlt, kann sich als jeder
    Spieler anmelden. Daher: Datenordner und Sicherungen schützen.
  - Die **Spielverbindung ist nicht verschlüsselt** (Positionen, Namen, Meldungen).
- **Empfehlung für Spiele über das Internet:** Mitspieler über ein **VPN** verbinden, z. B.
  **Tailscale** (einfach, kostenlos für kleine Gruppen) oder **WireGuard** (selbst betrieben).
  Dann ist der Spielport nicht offen im Internet, alles ist verschlüsselt, und die
  Verwaltungsseite ist über die VPN-Adresse erreichbar. Spieler sollten für das Spiel ein
  **eigenes Passwort** verwenden, das sie sonst nirgends benutzen.

### Der Container

- läuft als **Benutzer 10001** (nicht root), mit **schreibgeschütztem Dateisystem** (nur das
  Volume `/data` ist beschreibbar), **ohne Linux-Sonderrechte** (`cap_drop: ALL`) und mit
  `no-new-privileges`;
- hat Obergrenzen für Prozesse (64) und Arbeitsspeicher (256 MB) und begrenzte Log-Dateien;
- enthält keinen Compiler und keine Zugangsdaten (die kommen erst beim Start aus `.env`).
- Weitere Schutzgrenzen im Server: Pakete max. 64 KB, max. 240 Nachrichten pro Sekunde und
  Verbindung, Trennung nach 50 kaputten Paketen, Trennung nach 15 Minuten ohne Anmeldung.

`ADMIN_PASSWORD` steht als Umgebungsvariable im Container und ist für jeden sichtbar, der Docker
bedienen darf (`docker inspect`). Wer Docker bedienen darf, hat aber ohnehin volle Kontrolle über
den Rechner – gib diese Berechtigung (Gruppe `docker`) also nur vertrauenswürdigen Personen.

---

## 7. Datensicherung und Wiederherstellung

Alles Wichtige liegt im Docker-Volume **`mondschuerfer-data`** (im Container `/data`):

| Datei | Inhalt |
|---|---|
| `accounts.txt` | Spielerkonten: Name, Anmeldeschlüssel, Zeiten, Fortschritt |
| `highscores.txt` | Bestenliste |
| `server.txt` | **Server-Salz** (ohne es passt kein gespeicherter Schlüssel mehr – niemand könnte sich anmelden!), Karten-Seed, insgesamt abgelieferte Ressourcen (Ausbaustufe der Stationen) |
| `settings.txt` | nur vorhanden, wenn auf der Webseite Einstellungen gespeichert wurden |
| `*.defekt-…` | Kopien von Dateien, in denen beim Laden kaputte Zeilen übersprungen wurden |

Die Dateien sind einfacher Text. Ansehen ja – **bearbeiten nur bei gestopptem Server**, sonst
überschreibt er die Änderung beim nächsten Speichern.

**Sichern** (Server kurz anhalten, dann ist die Sicherung garantiert in sich stimmig):

```bash
docker compose stop server
docker run --rm -v mondschuerfer-data:/data:ro -v "$PWD":/backup debian:bookworm-slim \
    tar -C /data -czf /backup/mondschuerfer-backup-$(date +%F).tar.gz .
docker compose start server
chmod 600 mondschuerfer-backup-*.tar.gz   # enthält die Anmeldeschlüssel!
```

**Sichern im laufenden Betrieb** (geht auch: der Server schreibt jede Datei „atomar“, eine Datei
ist also nie halb geschrieben; zwischen den Dateien kann höchstens ein paar Sekunden Abstand sein):

```bash
docker compose exec -T server tar -C /data -czf - . > mondschuerfer-backup-$(date +%F).tar.gz
```

Die Sicherung gehört **auf einen anderen Datenträger** (USB-Stick, NAS, anderer Rechner). Täglich
automatisch, z. B. per `crontab -e` (Pfad anpassen):

```
30 4 * * * cd /home/pi/Mondschuerfer && docker compose exec -T server tar -C /data -czf - . > /mnt/backup/mondschuerfer-$(date +\%F).tar.gz
```

**Wiederherstellen** (ersetzt den kompletten Inhalt des Volumes):

```bash
docker compose stop server
docker run --rm -v mondschuerfer-data:/data -v "$PWD":/backup debian:bookworm-slim \
    sh -c 'find /data -mindepth 1 -delete && tar -C /data -xzf /backup/mondschuerfer-backup-2026-10-11.tar.gz && chown -R 10001:10001 /data && chmod 700 /data'
docker compose start server
```

**Umzug auf einen anderen Rechner:** dort das Projekt holen, `.env` anlegen, einmal
`docker compose up -d --build` (legt das Volume an), dann wie oben wiederherstellen.

**Eigener Ordner statt Volume:** Wer die Daten lieber direkt in einem Ordner sehen möchte, ersetzt
in `docker-compose.yml` `mondschuerfer-data:/data` durch `./data:/data` und gibt den Ordner dem
Container-Benutzer: `mkdir -p data && sudo chown 10001:10001 data && chmod 700 data`
(`data/` steht in `.gitignore`).

---

## 8. Updates

```bash
git pull                              # neuen Code holen
docker compose build --pull           # neu bauen, dabei auch das Debian-Basisimage aktualisieren
docker compose up -d                  # Container mit dem neuen Image neu starten (Daten bleiben)
docker image prune -f                 # alte, nicht mehr benutzte Images löschen
```

Mit HTTPS-Vorbau zusätzlich `docker compose --profile online pull` (neues Caddy-Image) und
`docker compose --profile online up -d`.

- Vor größeren Updates eine [Sicherung](#7-datensicherung-und-wiederherstellung) anlegen.
- Ändert sich das Netzwerkprotokoll, meldet ein altes Spiel „Version passt nicht“ – dann müssen
  alle Mitspieler das Spiel ebenfalls aktualisieren.
- Auch das Betriebssystem des Server-Rechners regelmäßig aktualisieren
  (`sudo apt update && sudo apt upgrade`).

---

## 9. Fehlersuche

Erster Schritt ist immer das Log:

```bash
docker compose logs --tail 100 server   # die letzten 100 Zeilen
docker compose logs -f server           # live mitlesen
docker compose ps                       # läuft er? ("Restarting" = startet immer wieder neu)
```

| Meldung / Problem | Ursache und Lösung |
|---|---|
| `required variable ADMIN_PASSWORD is missing a value` | `.env` fehlt oder `ADMIN_PASSWORD` ist leer → `cp .env.example .env`, Passwort eintragen. |
| `[Web] … ADMIN_PASSWORD ist zu kurz` / `zu leicht zu erraten` | Webseite bleibt aus (Spiel läuft). Besseres Passwort in `.env`, dann `docker compose up -d`. |
| `FEHLER: Datenordner ist nicht beschreibbar: /data` | Bei einem eigenen Ordner statt Volume: `sudo chown -R 10001:10001 ./data`. |
| `UDP-Port 7777 kann nicht geöffnet werden` oder `port is already allocated` | Port belegt, z. B. läuft schon ein Server → anderen Server stoppen oder Port ändern. |
| Webseite: `429 Zu viele falsche Anmeldungen` | Sperre nach Fehlversuchen, Wartezeit steht in der Meldung. |
| Webseite: `Origin passt nicht zum Host` (hinter einem Proxy) | Der Proxy verändert den Host-Header → siehe [Profil online](#verwaltungsseite-über-das-internet-profil-online-caddy-https). |
| Webseite nicht erreichbar | `docker compose ps`; Browser auf demselben Rechner? Sonst SSH-Tunnel. Log: läuft die Verwaltungsseite? |
| Spiel: „Server nicht erreichbar“ | Adresse/Port richtig? Erst im LAN testen. Router-Weiterleitung **UDP** (nicht TCP) 7777, Firewall, kein eigenes IPv4 (DS-Lite)? |
| Spiel: „Version passt nicht“ | Spiel und Server haben verschiedene Protokollversionen → beide aktualisieren. |
| Spiel: „Server voll“ | `MAX_PLAYERS` bzw. Einstellung auf der Webseite erhöhen. |
| Caddy: `FEHLER: DOMAIN ist nicht gesetzt` | `DOMAIN=…` in `.env` eintragen. |
| Caddy bekommt kein Zertifikat | DNS zeigt nicht auf diesen Anschluss oder TCP 80/443 nicht weitergeleitet: `docker compose --profile online logs caddy`. |
| Bauen bricht ab mit „Killed“ (Raspberry Pi) | zu wenig Arbeitsspeicher → `docker compose build --build-arg BUILD_JOBS=1`. |

Die Uhrzeiten im Log sind UTC. Für Ortszeit (nur Linux-Gastgeber) die Zeile
`/etc/localtime:/etc/localtime:ro` in `docker-compose.yml` einkommentieren, oder
`docker compose logs -t` benutzen (Docker-Zeitstempel).

Verbindungen über `localhost` erscheinen im Log mit einer Docker-internen Adresse (`172.x.x.x`) –
das ist normal; Spieler aus dem Internet erscheinen mit ihrer echten Adresse.

---

## 10. Ohne Docker bauen und starten

Der Server braucht nur einen C++17-Compiler und CMake (≥ 3.16); alle Bibliotheken liegen im Projekt
(`third_party/`), es wird nichts heruntergeladen. Unter Linux (Debian/Ubuntu/Raspberry Pi OS):

```bash
sudo apt install build-essential cmake
cmake -S . -B build/srv -DMONDSCHUERFER_CLIENT=OFF -DMONDSCHUERFER_SERVER=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/srv --target mondschuerfer-server mondschuerfer-loadtest -j

# Starten: Webseite nur auf diesem Rechner (ohne Docker gibt es keine Port-Freigabe dazwischen!)
ADMIN_USER=mondverwalter ADMIN_PASSWORD='hier-ein-langes-passwort' \
    ./build/srv/server/mondschuerfer-server --data ./data --web-bind 127.0.0.1
```

- `MONDSCHUERFER_CLIENT=OFF` lässt das Spiel (und damit raylib) weg; `MONDSCHUERFER_SERVER=ON`
  baut Server und Lasttest. Beides zusammen geht auch (Standard ist Client an, Server aus).
- Die Webseite wird beim Bauen nach `build/srv/server/web` kopiert und dort automatisch gefunden.
- `--web-bind 127.0.0.1` ist ohne Docker wichtig: Der Standard `0.0.0.0` würde die Webseite
  (unverschlüsselt) im ganzen Netz anbieten.
- **Windows** (CLion oder Visual Studio): in den CMake-Optionen `-DMONDSCHUERFER_SERVER=ON` setzen,
  Ziel `mondschuerfer-server` bauen. Beenden mit Strg+C (der Server speichert vorher).
- Alle Optionen: `mondschuerfer-server --help`.

---

## 11. Lasttest

`mondschuerfer-loadtest` startet viele simulierte Spieler (Bots), die sich wie echte Spiele **über
das Netzwerk** mit einem laufenden Server verbinden – im Gegensatz zu `--benchmark`, das ohne
Netzwerk nur die Rechenarbeit misst.

```bash
./build/srv/server/mondschuerfer-loadtest --server host:7777 --bots 32 --seconds 60
```

- Vorher `MAX_PLAYERS` (bzw. die Einstellung auf der Webseite) mindestens auf die Zahl der Bots
  setzen, sonst meldet der Server „Server voll“.
- Während des Tests auf der Webseite die Auslastung beobachten.
- Am aussagekräftigsten von einem **anderen** Rechner aus; auf demselben Rechner brauchen die Bots
  selbst Rechenzeit.
- Der Lasttest ist auch im Docker-Image enthalten und lässt sich gegen den laufenden Container
  starten (im internen Docker-Netz heißt der Server einfach `server`):

  ```bash
  docker compose run --rm --no-deps --entrypoint /app/mondschuerfer-loadtest server \
      --server server:7777 --bots 32 --seconds 60
  ```

> Nur auf dem **eigenen** Server benutzen – ein Lasttest gegen fremde Server ist ein Angriff.

---

## 12. Dateien in diesem Ordner

```
server/
├── CMakeLists.txt        Build: mondschuerfer-server und mondschuerfer-loadtest
├── Dockerfile            Container-Image (zweistufig: bauen, dann schlank ausführen)
├── Caddyfile             optionaler HTTPS-Vorbau (Profil "online")
├── README.md             diese Anleitung
├── src/
│   ├── main.cpp          Start: Umgebungsvariablen/Kommandozeile, Signale, Hauptschleife
│   ├── GameServer.*      ENet, Anmeldung, Simulation, Snapshots, Benchmark
│   ├── Accounts.*        Konten, Bestenliste, Einstellungen speichern (Datenordner)
│   ├── SharedState.hpp   Datenaustausch Spiel-Thread <-> Web-Thread
│   ├── WebServer.*       Verwaltungsseite (cpp-httplib), Anmeldung, JSON-Schnittstelle
│   └── LoadTest.cpp      Lasttest mit Netzwerk-Bots
└── web/                  index.html, app.js, style.css der Verwaltungsseite
```

Im Hauptordner gehören dazu: `docker-compose.yml`, `.env.example` (Vorlage für `.env`) und
`.dockerignore` (was beim Bauen an Docker geschickt wird).
