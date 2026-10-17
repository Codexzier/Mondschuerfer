# Mondschürfer – Arbeitsregeln für Claude

C++17-Spiel mit raylib 5.5 (CMake). Quellcode in `src/`, Modelle in `assets/models/`,
Balancing-Werte in `src/Config.hpp`. Sprache im Projekt: Deutsch (Kommentare, Texte, Commits).

## Grundsatz: sparsam arbeiten
- Nur das umsetzen, was im Auftrag steht. Keine zusätzlichen Features, Refactorings,
  Aufräumarbeiten oder Verbesserungen ohne Rückfrage.
- Nur die Dateien lesen, die für die Änderung nötig sind. Nicht das ganze Projekt durchsuchen,
  wenn die betroffene Stelle bekannt ist.
- Bestehenden Stil übernehmen (Namensgebung, Kommentardichte, Aufbau).
- Bei Unklarheiten kurz nachfragen statt mehrere Varianten auszuprobieren.

## Prüfen nach einer Änderung
- Standard: **nur einmal kompilieren**, um Compilerfehler auszuschließen:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- **Nicht** ohne ausdrücklichen Auftrag:
  - Simulation (`--mode sim`) oder andere Testläufe starten
  - Screenshots erstellen oder ansehen
  - Nutzungs-, Rand- oder Balancingtests durchführen
  - mehrere Build-Varianten oder Plattformen durchprobieren
- Schlägt der Build fehl: den Fehler beheben, erneut kompilieren und dann aufhören.

## Dokumentation
- README und `docs/` nur ändern, wenn der Auftrag es verlangt oder sich die Steuerung oder
  Bedienung sichtbar ändert. Dann nur den betroffenen Abschnitt anpassen.
- Keine neuen Markdown-Dateien anlegen, wenn nicht ausdrücklich gewünscht.

## Git
- Am Ende committen und pushen, mit einer kurzen deutschen Commit-Nachricht.
- Keinen Pull Request erstellen, wenn nicht ausdrücklich gewünscht.

## Antworten
- Kurz zusammenfassen: was geändert wurde, in welchen Dateien und ob der Build erfolgreich war.
- Keine langen Erklärungen, außer auf Nachfrage.
