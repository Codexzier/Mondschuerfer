# Fremde Bibliotheken

Diese Bibliotheken liegen unverändert im Projekt, damit der Server (z. B. im
Docker-Container) ohne Internetzugang gebaut werden kann.

| Ordner | Projekt | Version | Lizenz | Wofür |
|---|---|---|---|---|
| `enet/` | [ENet](https://github.com/lsalzman/enet) | 1.3.18 | MIT (`enet/LICENSE`) | Netzwerk über UDP (Client und Server) |
| `httplib/` | [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.22.0 | MIT (`httplib/LICENSE`) | Webserver der Verwaltungsseite |
| `raylib/` | [raylib](https://github.com/raysan5/raylib) | 5.5 (nur `raylib.h`, `raymath.h`) | zlib (`raylib/LICENSE`) | Datentypen und Mathe für die Spiellogik im Server |

Das Spiel selbst lädt die vollständige raylib 5.5 beim ersten CMake-Lauf
herunter (siehe `CMakeLists.txt`).
