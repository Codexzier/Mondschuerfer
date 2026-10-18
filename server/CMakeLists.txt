# ---------------------------------------------------------------------------
# Mehrspieler-Server (ohne Grafik). Wird vom CMakeLists.txt im Hauptordner
# eingebunden, wenn MONDSCHUERFER_SERVER=ON gesetzt ist.
#
# Abhängigkeiten liegen alle im Projekt (kein Download nötig):
#   third_party/enet      Netzwerk (UDP)
#   third_party/httplib   Webserver für die Verwaltungsseite (Header-only)
#   third_party/raylib    nur raylib.h und raymath.h (Datentypen und Mathe,
#                         die die gemeinsame Spiellogik in shared/ benutzt)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_executable(mondschuerfer-server
        src/main.cpp
        src/GameServer.cpp
        src/Accounts.cpp
        src/WebServer.cpp
        ${MONDSCHUERFER_SHARED_SOURCES}
)

target_include_directories(mondschuerfer-server PRIVATE
        src
        ${PROJECT_SOURCE_DIR}/shared
        ${PROJECT_SOURCE_DIR}/third_party/raylib
        ${PROJECT_SOURCE_DIR}/third_party/httplib
        ${MONDSCHUERFER_ENET_INCLUDE})
target_link_libraries(mondschuerfer-server PRIVATE enet Threads::Threads)

if (MSVC)
    target_compile_options(mondschuerfer-server PRIVATE /W3 /utf-8)
    target_compile_definitions(mondschuerfer-server PRIVATE _CRT_SECURE_NO_WARNINGS)
else ()
    target_compile_options(mondschuerfer-server PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif ()

if (WIN32)
    target_link_libraries(mondschuerfer-server PRIVATE ws2_32 winmm)
endif ()

# Webseite (HTML/JS/CSS) neben die ausführbare Datei kopieren
add_custom_command(TARGET mondschuerfer-server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/web
        $<TARGET_FILE_DIR:mondschuerfer-server>/web
        COMMENT "Kopiere Webseite zum Server")

# Lasttest: startet viele simulierte Spieler (Bots), die sich über das Netzwerk
# mit einem laufenden Server verbinden. Nur zum Messen, nicht für den Betrieb.
add_executable(mondschuerfer-loadtest
        src/LoadTest.cpp
        ${MONDSCHUERFER_SHARED_SOURCES}
)
target_include_directories(mondschuerfer-loadtest PRIVATE
        src
        ${PROJECT_SOURCE_DIR}/shared
        ${PROJECT_SOURCE_DIR}/third_party/raylib
        ${MONDSCHUERFER_ENET_INCLUDE})
target_link_libraries(mondschuerfer-loadtest PRIVATE enet Threads::Threads)
if (NOT MSVC)
    target_compile_options(mondschuerfer-loadtest PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif ()
if (WIN32)
    target_link_libraries(mondschuerfer-loadtest PRIVATE ws2_32 winmm)
endif ()
