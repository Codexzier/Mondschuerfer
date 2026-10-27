#pragma once
// ---------------------------------------------------------------------------
// Verwaltungs-Webseite des Servers (HTTP, geschützt mit Benutzername/Passwort).
//
// Läuft in einem eigenen Thread (cpp-httplib). Liest den Serverzustand nur aus
// SharedState und legt Einstellungsänderungen dort ab (siehe SharedState.hpp).
//
// Seiten und Schnittstellen (alle nur mit gültiger Anmeldung, HTTP Basic Auth):
//   GET  /                     index.html  (aus webRoot)
//   GET  /app.js, /style.css   statische Dateien aus webRoot
//   GET  /api/status           Zustand als JSON (Auslastung, Spieler, Bestenliste, Einstellungen)
//   POST /api/settings         Einstellungen ändern (Formularfelder, siehe WebServer.cpp)
//   POST /api/settings/reset   zurück auf die Standardwerte aus docker-compose.yml
//
// Sicherheit (Details in WebServer.cpp und server/README.md):
//   - Start verweigert, wenn kein oder ein zu schwaches Passwort gesetzt ist.
//   - Nach mehreren falschen Anmeldungen wird die IP-Adresse kurz gesperrt.
//   - POST-Anfragen brauchen den Header "X-Requested-With: mondschuerfer"
//     (Schutz gegen Cross-Site-Request-Forgery).
//   - Sicherheits-Header (CSP, X-Frame-Options, nosniff, no-store).
// ---------------------------------------------------------------------------
#include "SharedState.hpp"
#include <memory>
#include <string>

struct WebConfig {
    std::string bindAddress = "0.0.0.0";
    int port = 8080;
    std::string user;          // ADMIN_USER
    std::string password;      // ADMIN_PASSWORD
    std::string webRoot = "web";
    // Vertrauenswürdiger Reverse-Proxy (Name oder IP, z. B. "caddy"); leer =
    // keiner. Nur von dieser Adresse wird der Header "X-Real-IP" (echte
    // Adresse des Besuchers) übernommen, siehe WebContext::ClientAddress.
    std::string trustedProxy;
    // Ordner des Lastprotokolls (Accounts::LoadLogDir, Tagesdateien last-JJJJ-MM-TT.csv);
    // leer = kein Lastprotokoll. Nur gelesen (Bereich "Verlauf").
    std::string logDir;
};

class WebServer {
public:
    WebServer();
    ~WebServer();   // stoppt den Server, falls er noch läuft
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Startet den Web-Thread. Liefert false (mit Meldung auf stderr), wenn die
    // Konfiguration unsicher ist oder der Port nicht geöffnet werden kann.
    bool Start(const WebConfig& config, SharedState& shared);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
