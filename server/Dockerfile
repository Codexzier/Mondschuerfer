# =============================================================================
# Dockerfile für den Mondschürfer-Mehrspieler-Server
#
# WOFÜR?
#   Baut ein Container-Image, in dem nur der Server läuft (kein Fenster, keine
#   Grafik, kein Ton). Benutzt wird es von docker-compose.yml im Hauptordner:
#       docker compose up -d --build
#   Ohne Compose geht es auch direkt (Kontext = Hauptordner, daher der Punkt):
#       docker build -f server/Dockerfile -t mondschuerfer-server .
#
# WARUM ZWEI STUFEN ("multi-stage build")?
#   Stufe 1 "build" enthält Compiler, CMake und alle Quelltexte – das sind
#   mehrere hundert MB. Daraus entsteht nur eine einzige Programmdatei.
#   Stufe 2 "runtime" startet wieder von einem leeren Debian und bekommt nur
#   diese Programmdatei und die Webseite (HTML/JS/CSS) hineinkopiert.
#   Ergebnis: ein kleines Image (ca. 125 MB, fast nur das Debian-Grundsystem;
#   die Build-Stufe wäre ein Vielfaches davon) ohne Compiler. Was nicht im
#   Image ist, kann ein Angreifer auch nicht benutzen.
#
# WAS WIRD GEBRAUCHT?
#   Nur Dateien aus dem Projekt: CMakeLists.txt, shared/ (Spiellogik, Protokoll),
#   server/ (Server und Webseite) und third_party/ (ENet, cpp-httplib,
#   raylib-Header). Die Spiel-Grafik (assets/) und der Client (src/) werden
#   NICHT gebraucht; .dockerignore im Hauptordner sorgt dafür, dass sie gar
#   nicht erst an Docker geschickt werden. Aus dem Internet werden nur das
#   Debian-Basisimage und die Debian-Pakete g++/cmake/make geladen.
#
# ABLAUF BEIM START DES CONTAINERS
#   Docker startet /app/mondschuerfer-server als Benutzer "mondschuerfer"
#   (UID 10001, kein root). Der Server liest seine Einstellungen aus
#   Umgebungsvariablen (siehe ENV unten und docker-compose.yml), speichert
#   Konten usw. in /data (ein Docker-Volume, überlebt Updates des Images) und
#   beendet sich bei "docker stop" (Signal SIGTERM) geordnet: erst alle
#   Spieler speichern, dann Ende.
#
# ARCHITEKTUR
#   debian:bookworm-slim gibt es für x86-64 (PC/Server) und arm64 (Raspberry
#   Pi 4/5 mit 64-Bit-System). Docker nimmt automatisch die passende Variante;
#   auf dem Raspberry Pi wird also direkt ein Pi-Image gebaut.
# =============================================================================


# -----------------------------------------------------------------------------
# Stufe 1: Bauen
# -----------------------------------------------------------------------------
# Basis: Debian 12 "bookworm" in der schlanken Variante. Dieselbe Basis wie in
# Stufe 2 – so passen die Systembibliotheken (glibc) garantiert zusammen.
# "AS build" gibt der Stufe einen Namen, auf den sich Stufe 2 beziehen kann.
FROM debian:bookworm-slim AS build

# Anzahl paralleler Compiler-Prozesse. Leer = so viele wie CPU-Kerne.
# Auf einem Raspberry Pi mit wenig Arbeitsspeicher (1–2 GB) hilft
#     docker compose build --build-arg BUILD_JOBS=1
# falls der Compiler mangels Speicher abbricht ("Killed").
ARG BUILD_JOBS=

# Compiler (g++ bringt gcc für den C-Teil von ENet mit), CMake und make.
#   --no-install-recommends: nur die wirklich nötigen Pakete, keine "Empfehlungen".
#   rm -rf /var/lib/apt/lists/*: die heruntergeladenen Paketlisten wieder
#   löschen, sie würden das Image nur unnötig vergrößern.
# Alles in EINEM RUN-Befehl, weil jeder RUN eine eigene Schicht (Layer) im
# Image erzeugt; Gelöschtes aus einer späteren Schicht spart sonst keinen Platz.
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make \
 && rm -rf /var/lib/apt/lists/*

# Arbeitsordner für die Quelltexte (wird angelegt, falls er fehlt).
WORKDIR /src

# Quelltexte in das Image kopieren. Reihenfolge mit Absicht: was sich selten
# ändert, zuerst. Docker merkt sich jede Schicht ("Build-Cache"); ändert sich
# z. B. nur server/src/GameServer.cpp, werden die oberen Schichten
# wiederverwendet und nur ab "COPY server/" neu gebaut.
COPY third_party/ third_party/
COPY CMakeLists.txt ./
COPY shared/ shared/
COPY server/ server/

# Konfigurieren und Bauen:
#   MONDSCHUERFER_CLIENT=OFF  das Spiel selbst (braucht raylib + Grafik) weglassen
#   MONDSCHUERFER_SERVER=ON   den Server (und den Lasttest) bauen
#   CMAKE_BUILD_TYPE=Release  mit Optimierung (-O3), ohne Debug-Infos
#   -static-libstdc++/-static-libgcc: die C++-Standardbibliothek wird in die
#       Programmdatei eingebaut. Dann braucht das Programm zur Laufzeit nur
#       noch die C-Bibliothek (glibc), die in jedem Debian-Image steckt – es
#       kann also nichts wegen einer fehlenden/anderen libstdc++ schiefgehen.
#   --target: nur diese beiden Programme bauen (nicht "alles").
#   ${BUILD_JOBS:-$(nproc)}: Shell-Schreibweise für "BUILD_JOBS, oder wenn leer
#       die Anzahl der CPU-Kerne".
#   strip: entfernt Symbolnamen aus den fertigen Programmen (kleiner, gleiche
#       Funktion).
RUN cmake -S . -B /build \
        -DMONDSCHUERFER_CLIENT=OFF \
        -DMONDSCHUERFER_SERVER=ON \
        -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" \
 && cmake --build /build --parallel "${BUILD_JOBS:-$(nproc)}" \
        --target mondschuerfer-server mondschuerfer-loadtest \
 && strip /build/server/mondschuerfer-server /build/server/mondschuerfer-loadtest


# -----------------------------------------------------------------------------
# Stufe 2: Laufzeit (das, was später wirklich läuft)
# -----------------------------------------------------------------------------
# Wieder ein frisches, schlankes Debian – ohne Compiler und Quelltexte.
FROM debian:bookworm-slim AS runtime

# Beschreibung des Images (sichtbar mit "docker image inspect").
LABEL org.opencontainers.image.title="Mondschürfer-Server" \
      org.opencontainers.image.description="Mehrspieler-Server mit Verwaltungs-Webseite für das Spiel Mondschürfer"

# Eigener Benutzer ohne Sonderrechte. Läuft ein Programm im Container als
# root und hat eine Sicherheitslücke, ist der Angreifer root im Container –
# das erleichtert den Ausbruch auf den Gastgeber-Rechner. Deshalb:
#   feste UID/GID 10001: Zahl, die auf dem Gastgeber sicher keinem echten
#       Benutzer gehört. Fest, damit man Ordnerrechte (z. B. für ein
#       eingebundenes ./data) gezielt setzen kann: chown 10001:10001 ./data
#   --no-create-home, Home /nonexistent, Shell nologin: der Benutzer kann sich
#       nicht anmelden und hat kein Heimatverzeichnis.
#   --no-log-init: verhindert eine riesige /var/log/lastlog-Datei bei hohen
#       UIDs (bekannte Eigenheit von useradd in Docker-Images).
# Danach den Datenordner /data anlegen, dem Benutzer schenken und für alle
# anderen sperren (700): accounts.txt enthält die Anmeldeschlüssel der Spieler.
RUN groupadd --system --gid 10001 mondschuerfer \
 && useradd --system --uid 10001 --gid 10001 --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin --no-log-init mondschuerfer \
 && mkdir -p /data \
 && chown 10001:10001 /data \
 && chmod 0700 /data

# Programme und Webseite aus Stufe 1 holen. Sie gehören root und sind für den
# Server-Benutzer nur lesbar/ausführbar: selbst ein übernommener Server kann
# sein eigenes Programm oder die Webseite nicht verändern.
COPY --from=build /build/server/mondschuerfer-server /app/mondschuerfer-server
COPY --from=build /build/server/mondschuerfer-loadtest /app/mondschuerfer-loadtest
COPY --from=build /src/server/web/ /app/web/

# Standardwerte für den Server (siehe server/src/main.cpp und server/README.md).
# docker-compose.yml setzt sie noch einmal ausdrücklich; hier stehen sie, damit
# auch ein einfaches "docker run" richtig funktioniert.
#   MONDSCHUERFER_DATA      Konten, Bestenliste, Einstellungen -> Volume /data
#   MONDSCHUERFER_WEB_ROOT  index.html, app.js, style.css der Verwaltungsseite
#   MONDSCHUERFER_PORT      UDP-Port für das Spiel
#   MONDSCHUERFER_WEB_PORT  TCP-Port der Verwaltungsseite
#   MONDSCHUERFER_WEB_BIND  0.0.0.0 = auf allen Netzwerkkarten des CONTAINERS
#       lauschen. Das ist nötig, weil Docker die Anfragen von außen über eine
#       eigene Netzwerkkarte hereinreicht. Wer die Seite tatsächlich erreichen
#       kann, legt die Port-Freigabe in docker-compose.yml fest (dort
#       standardmäßig nur 127.0.0.1 = dieser Rechner).
# ADMIN_USER/ADMIN_PASSWORD stehen hier mit Absicht NICHT: Zugangsdaten
# gehören nie in ein Image (jeder mit dem Image könnte sie auslesen).
ENV MONDSCHUERFER_DATA=/data \
    MONDSCHUERFER_WEB_ROOT=/app/web \
    MONDSCHUERFER_PORT=7777 \
    MONDSCHUERFER_WEB_PORT=8080 \
    MONDSCHUERFER_WEB_BIND=0.0.0.0

# Ab hier läuft alles als der unprivilegierte Benutzer (Zahl statt Name, damit
# Docker/Kubernetes ohne Nachschlagen erkennt, dass es nicht root ist).
USER 10001:10001

# Arbeitsordner beim Start. Der Server schreibt nur nach MONDSCHUERFER_DATA.
WORKDIR /app

# Datenordner als Volume: Docker legt dafür beim ersten Start ein Volume an
# (in docker-compose.yml ein benanntes Volume) und übernimmt dabei Besitzer
# und Rechte von /data aus dem Image (10001, 700). Der Inhalt bleibt erhalten,
# wenn der Container gelöscht und mit einem neuen Image neu erstellt wird.
VOLUME ["/data"]

# Dokumentation, welche Ports der Container benutzt. EXPOSE öffnet selbst
# nichts – die eigentliche Freigabe passiert mit "ports:" in docker-compose.yml.
EXPOSE 7777/udp 8080/tcp

# "docker stop" schickt dieses Signal (SIGTERM ist auch der Standard; hier
# ausdrücklich, weil der Server genau darauf reagiert): Er speichert alle
# Spieler, verabschiedet die Clients und beendet sich in ~1–2 Sekunden.
# Docker wartet standardmäßig 10 Sekunden, bevor es hart abbricht (SIGKILL).
STOPSIGNAL SIGTERM

# Kein HEALTHCHECK – mit Absicht:
#   Das Image enthält kein curl/wget (wäre ein Zusatzpaket). Prüfbar wäre ohne
#   Zusatzpakete höchstens, ob der Web-Port antwortet. Das sagt aber nichts
#   über das eigentliche Spiel (UDP/ENet) aus, und die Webseite ist absichtlich
#   abgeschaltet, wenn kein sicheres ADMIN_PASSWORD gesetzt ist oder --no-web
#   benutzt wird – ein voll funktionsfähiger Spielserver würde dann als
#   "unhealthy" gemeldet. Stürzt der Server ab, beendet sich der Container und
#   "restart: unless-stopped" (docker-compose.yml) startet ihn neu.

# Das Programm, das beim Start des Containers läuft. Schreibweise als Liste
# ("exec form"): Docker startet das Programm direkt, ohne Shell dazwischen.
# Nur so kommt SIGTERM beim Server an (eine Shell würde es nicht weitergeben).
# Zusätzliche Argumente hängt Docker hinten an, z. B.:
#     docker compose run --rm server --benchmark 64
#     docker compose run --rm server --help
ENTRYPOINT ["/app/mondschuerfer-server"]
