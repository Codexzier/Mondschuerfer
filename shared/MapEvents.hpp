#pragma once
// ---------------------------------------------------------------------------
// MapEvents.hpp – Karten-Events (Einsatzkarten für einzelne Tage) und die
// Kontorollen des Servers.
//
// Was ist ein Event?
//   Ein Zeitraum vom Tag "from" bis zum Tag "to" (beide eingeschlossen; ein
//   einzelner Tag: from == to). Tage im Format JJJJ-MM-TT in der Ortszeit des
//   Servers. Dazu zwei Listen von Einsatzkarten (ServerMap::file):
//   - maps:   an diesen Tagen im Command Center wählbar, auch wenn sie nicht
//             dauerhaft freigegeben sind
//   - locked: an diesen Tagen gesperrt, auch wenn sie freigegeben sind
//   Die Mondkarte (Ziel 0 im Command Center) ist immer wählbar. Gelten an
//   einem Tag mehrere Events, zählen alle zusammen; gesperrt geht vor.
//
// Kontorollen: Bits, damit ein Konto mehrere haben kann. Die erste ist
// "Event Manager" (darf Events einrichten; im Spiel folgt das mit dem
// Event-Editor). Neue Rollen nur hinten anfügen: Die Bits stehen als Namen
// in accounts.txt und sollen später auch über das Netzwerk gehen.
//
// Wer benutzt die Datei? Der Server: Accounts (events.txt, Rollen in
// accounts.txt), GameServer (Ziele im Command Center je Tag) und WebServer
// (Bereiche "Events" und "Rollen" der Verwaltungsseite). Später auch der
// Event-Editor im Spiel.
//
// Abhängigkeiten: nur die Standardbibliothek (kein raylib.h), damit auch
// WebServer.cpp sie einbinden kann.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <vector>

constexpr int MAX_EVENT_NAME = 40;   // Zeichen, wie ein Kartenname (mapgrid::MAX_NAME)
constexpr int MAX_EVENT_MAPS = 20;   // Karten je Liste, wie die Ziele im Command Center (net::MAX_MISSION_MAPS)

struct MapEvent {
    int id = 0;                         // fortlaufende Nummer (> 0), bleibt beim Bearbeiten gleich
    std::string name;
    std::string from, to;               // JJJJ-MM-TT, from <= to
    std::vector<std::string> maps;      // zusätzlich wählbare Einsatzkarten
    std::vector<std::string> locked;    // gesperrte Einsatzkarten
    std::string by;                     // eingerichtet von: Kontoname oder "Verwaltungsseite"
    int64_t created = 0;                // Unix-Zeit
};

// Gültiger Tag im Format JJJJ-MM-TT (mit Schaltjahren), Jahr 2000..2999.
// Weil das Format fest ist, lassen sich Tage als Text vergleichen ("<").
inline bool ValidEventDay(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (d[i] < '0' || d[i] > '9') return false;
    const int y = std::stoi(d.substr(0, 4)), m = std::stoi(d.substr(5, 2)), day = std::stoi(d.substr(8, 2));
    if (y < 2000 || y > 2999 || m < 1 || m > 12 || day < 1) return false;
    static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return day <= DAYS[m - 1] + (m == 2 && leap ? 1 : 0);
}

// Gilt das Event an diesem Tag?
inline bool EventOnDay(const MapEvent& e, const std::string& day) { return e.from <= day && day <= e.to; }

// --- Kontorollen ---------------------------------------------------------------
enum AccountRoleBit : uint32_t {
    ROLE_EVENT_MANAGER = 1u << 0,   // darf Events einrichten
};

struct AccountRoleInfo {
    uint32_t bit;
    const char* key;      // in accounts.txt ("roles=event") und auf der Webseite
    const char* label;    // Anzeige
};

inline constexpr AccountRoleInfo ACCOUNT_ROLES[] = {
    {ROLE_EVENT_MANAGER, "event", "Event Manager"},
};

// nullptr, wenn es die Rolle nicht gibt
inline const AccountRoleInfo* FindAccountRole(const std::string& key) {
    for (const AccountRoleInfo& r : ACCOUNT_ROLES)
        if (key == r.key) return &r;
    return nullptr;
}
