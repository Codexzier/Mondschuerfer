cmake_minimum_required(VERSION 3.16)

project(Mondschuerfer
        VERSION 1.1
        DESCRIPTION "Weltraum-Bergbau-Spiel mit dem Kenney Space Kit"
        LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Was soll gebaut werden?
#   MONDSCHUERFER_CLIENT  das Spiel (Einzelspieler + Mehrspieler-Client), braucht raylib
#   MONDSCHUERFER_SERVER  der Mehrspieler-Server (ohne Grafik), z. B. für Docker:
#                         cmake -S . -B build -DMONDSCHUERFER_CLIENT=OFF -DMONDSCHUERFER_SERVER=ON
# ---------------------------------------------------------------------------
option(MONDSCHUERFER_CLIENT "Spiel (Client) bauen" ON)
option(MONDSCHUERFER_SERVER "Mehrspieler-Server bauen" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build-Typ" FORCE)
endif ()

# ---------------------------------------------------------------------------
# Gemeinsamer Code (Spiellogik, Protokoll, Kryptografie): Client UND Server
# ---------------------------------------------------------------------------
set(MONDSCHUERFER_SHARED_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/World.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/WorldGen.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/GameMap.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/MapAnim.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/Ai.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/Models.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/GameSettings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/Protocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/Crypto.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/Util.cpp
)

# ENet (Netzwerk über UDP) liegt im Projekt unter third_party/enet (MIT-Lizenz)
add_subdirectory(third_party/enet EXCLUDE_FROM_ALL)
set(MONDSCHUERFER_ENET_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/enet/include)

if (MONDSCHUERFER_SERVER)
    add_subdirectory(server)
endif ()

if (NOT MONDSCHUERFER_CLIENT)
    return()
endif ()

# ---------------------------------------------------------------------------
# raylib: zuerst eine installierte Version suchen, sonst automatisch laden.
# ---------------------------------------------------------------------------
find_package(raylib 5.5 QUIET)

if (NOT raylib_FOUND)
    include(FetchContent)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()
    set(FETCHCONTENT_QUIET OFF)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
            raylib
            URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
    )
    FetchContent_MakeAvailable(raylib)
endif ()

# pl_mpeg (Videoclips der Missionen: MPEG-1 mit MP2-Ton) liegt im Projekt unter
# third_party/pl_mpeg (MIT-Lizenz). Eigene Bibliothek, damit die Warnungen des
# Spiels nicht für den fremden Code gelten.
add_library(pl_mpeg STATIC third_party/pl_mpeg/pl_mpeg.c)
target_include_directories(pl_mpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pl_mpeg)

# ---------------------------------------------------------------------------
# Spiel
# ---------------------------------------------------------------------------
add_executable(Mondschuerfer
        src/main.cpp
        src/Assets.cpp
        src/Audio.cpp
        src/ClipPlayer.cpp
        src/Render.cpp
        src/Hud.cpp
        src/Ui.cpp
        src/Save.cpp
        src/NetClient.cpp
        src/Multiplayer.cpp
        src/ClientConfig.cpp
        src/Options.cpp
        src/TiltShift.cpp
        src/Controls.cpp
        src/MapFiles.cpp
        src/MapEditor.cpp
        src/Chat.cpp
        ${MONDSCHUERFER_SHARED_SOURCES}
)

target_include_directories(Mondschuerfer PRIVATE src shared ${MONDSCHUERFER_ENET_INCLUDE})
target_link_libraries(Mondschuerfer PRIVATE raylib enet pl_mpeg)

if (MSVC)
    target_compile_options(Mondschuerfer PRIVATE /W3 /utf-8)
    target_compile_definitions(Mondschuerfer PRIVATE _CRT_SECURE_NO_WARNINGS)
else ()
    target_compile_options(Mondschuerfer PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif ()

if (WIN32)
    target_link_libraries(Mondschuerfer PRIVATE winmm ws2_32)
    if (MINGW)
        # Statisch linken, damit die .exe auch ausserhalb von CLion startet
        # (keine libstdc++-6.dll / libgcc_s_seh-1.dll noetig).
        #
        # Manche Toolchains (z. B. die von CLion mitgelieferte, GCC 15) haben eine
        # statische libwinpthread, die gegen die UCRT gebaut ist, waehrend libstdc++
        # gegen msvcrt gebaut ist. Dann schlaegt "-static" fehl
        # (__intrinsic_setjmpex / __ms_vsnprintf). In dem Fall wird nur winpthread
        # dynamisch gelinkt und die noetigen DLLs neben die .exe kopiert.
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_LINK_OPTIONS -static)
        check_cxx_source_compiles("
            #include <thread>
            int main() { std::thread t([] {}); t.join(); return 0; }"
                MINGW_FULL_STATIC_OK)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)

        if (MINGW_FULL_STATIC_OK)
            target_link_options(Mondschuerfer PRIVATE -static -static-libgcc -static-libstdc++)
        else ()
            message(STATUS "MinGW: vollstaendig statischer Link nicht moeglich, libwinpthread wird mitkopiert")
            target_link_options(Mondschuerfer PRIVATE -static-libgcc -static-libstdc++)
            get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
            foreach (dll libwinpthread-1.dll libgcc_s_seh-1.dll)
                if (EXISTS ${MINGW_BIN_DIR}/${dll})
                    add_custom_command(TARGET Mondschuerfer POST_BUILD
                            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${MINGW_BIN_DIR}/${dll} $<TARGET_FILE_DIR:Mondschuerfer>)
                endif ()
            endforeach ()
        endif ()
    endif ()
    # Im Release-Build kein Konsolenfenster anzeigen
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        set_target_properties(Mondschuerfer PROPERTIES WIN32_EXECUTABLE TRUE)
        if (MSVC)
            target_link_options(Mondschuerfer PRIVATE /ENTRY:mainCRTStartup)
        endif ()
    endif ()
endif ()

if (APPLE)
    target_link_libraries(Mondschuerfer PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif ()

# Assets (Kenney Space Kit, Sci-Fi Sounds, Interface Sounds, Musik, Clips) neben die ausfuehrbare Datei kopieren
add_custom_command(TARGET Mondschuerfer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:Mondschuerfer>/assets
        COMMENT "Kopiere Assets zum Spiel")
