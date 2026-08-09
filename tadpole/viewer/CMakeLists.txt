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

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(WIN32)
    set(ZLIB_USE_STATIC_LIBS ON)   # or the one-file .exe grows a zlib1.dll
endif()
find_package(ZLIB REQUIRED)   # ui screenshots and the games index

add_executable(tadpole-view tadpole_view.c tadpole_ui.c tadpole_hle.c)
add_executable(hle-probe hle_probe.c)

foreach(t tadpole-view hle-probe)
    target_include_directories(${t} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(${t} PRIVATE ${SDL2_CFLAGS_OTHER})
    target_link_libraries(${t} PRIVATE ZLIB::ZLIB)
endforeach()

if(WIN32)
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
