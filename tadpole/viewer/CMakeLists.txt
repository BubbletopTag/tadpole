# The viewer, for platforms where the GNU Makefile is the wrong tool — which
# today means Windows under MSYS2. The Makefile remains the Linux build; this
# file must therefore stay in agreement with its VIEW_SRC list, which is short
# enough to keep by hand.
cmake_minimum_required(VERSION 3.20)
project(tadpole-viewer C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# WHICH BUILD THIS IS — AND THIS FILE USED TO THROW IT AWAY.
#
# tools/build-windows.sh has always configured the native MSYS2 build with
# -DTADPOLE_VERSION="$VERSION", and nothing here ever read it. CMake does not
# treat an unused -D as an error; it mentions it once, at the bottom of a
# hundred lines of configure output, as
#
#     Manually-specified variables were not used by this project:
#       TADPOLE_VERSION
#
# and then builds a viewer whose tadpole_view.c and tadpole_ui.c fall through
# to their own `#ifndef TADPOLE_VERSION / #define TADPOLE_VERSION "dev"`. So a
# native Windows build could not report its version no matter what it was told,
# and every one of them called itself an unreleased build — which the update
# checker reads as "no version at all", so it can never say "up to date".
#
# The default stays "dev" because a working copy genuinely is one; what changes
# is that asking for a version now gets you one.
set(TADPOLE_VERSION "dev" CACHE STRING
    "Release version baked into the viewer, as DDMMYYYY-NNNN")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(WIN32)
    set(ZLIB_USE_STATIC_LIBS ON)   # or the one-file .exe grows a zlib1.dll
endif()
find_package(ZLIB REQUIRED)   # ui screenshots and the games index

# THE STARTUP ANIMATION'S DECODER, OPTIONAL, AND STATIC HERE.
#
# The device's animations are Ogg Theora with Vorbis audio. Optional because
# Fast Boot skips them by default and a viewer without the three libraries
# still shows the boot logo; see the matching block in the GNU Makefile, which
# this file has to stay in agreement with.
#
# NOTHING TO SHIP BESIDE THE EXE. The Windows viewer already links -static, so
# these arrive as .a inside tadpole-view.exe rather than as three more DLLs to
# copy — which is the same policy as SDL2 and zlib here and the reason this is
# a one-file build at all. pkg-config --static is what makes that work: the
# non-static --libs omits the transitive -logg that libtheoradec needs.
pkg_check_modules(THEORA ogg theoradec vorbis)

add_executable(tadpole-view tadpole_view.c tadpole_ui.c tadpole_hle.c
               tadpole_cam.c tadpole_mic.c
                            tadpole_boot.c tadpole_pad.c)
add_executable(hle-probe hle_probe.c)

foreach(t tadpole-view hle-probe)
    target_include_directories(${t} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(${t} PRIVATE ${SDL2_CFLAGS_OTHER})
    target_link_libraries(${t} PRIVATE ZLIB::ZLIB)
endforeach()

# The definition carries the quotes, the same shape the Makefile passes. CMake
# needs no version-stamp file of its own: a compile definition is part of the
# command line it records per object, so changing it makes the object stale and
# the translation unit is recompiled. That is exactly what the Makefile could
# not do from timestamps alone, and why it grew .tadpole-version.
target_compile_definitions(tadpole-view PRIVATE
                           TADPOLE_VERSION="${TADPOLE_VERSION}")

if(THEORA_FOUND)
    target_compile_definitions(tadpole-view PRIVATE TADPOLE_THEORA)
    target_include_directories(tadpole-view PRIVATE ${THEORA_INCLUDE_DIRS})
    if(WIN32)
        target_link_libraries(tadpole-view PRIVATE ${THEORA_STATIC_LDFLAGS})
    else()
        target_link_libraries(tadpole-view PRIVATE ${THEORA_LDFLAGS})
    endif()
else()
    message(STATUS
        "no ogg/theoradec/vorbis: the startup animation will be skipped and "
        "Fast Boot off shows the logo alone")
endif()

if(WIN32)
    # The double-click: a tiny GUI-subsystem exe that finds the tree and
    # starts the viewer. Copy it to the repo root as tadpole.exe.
    add_executable(tadpole WIN32 tadpole_launcher.c tadpole.rc)
    target_link_options(tadpole PRIVATE -static -municode)

    # Static SDL2 and a static runtime: one copyable .exe, no MinGW or SDL
    # DLLs beside it — same policy as glasspole. opengl32 supplies the GL-1.1
    # entry points still bound at link time; everything newer resolves through
    # SDL_GL_GetProcAddress.
    #
    # -mconsole LAST, because sdl2.pc smuggles -mwindows into its link flags
    # and the subsystem is decided by whichever comes last. The viewer is a
    # GUI, but a GUI whose printf diagnostics still land somewhere during
    # bring-up; flipping to -mwindows is a release decision, not a build fact.
    foreach(t tadpole-view hle-probe)
        target_link_options(${t} PRIVATE -static)
        target_link_libraries(${t} PRIVATE ${SDL2_STATIC_LDFLAGS} opengl32 -mconsole)
    endforeach()
else()
    foreach(t tadpole-view hle-probe)
        target_link_libraries(${t} PRIVATE ${SDL2_LDFLAGS} GL m)
    endforeach()
endif()
