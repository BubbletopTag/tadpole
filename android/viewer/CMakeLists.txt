# Tadpole's viewer, cross-compiled for Android.
#
# This deliberately does NOT reuse tadpole/viewer/CMakeLists.txt. That file is
# the Windows/MSYS2 build and it does `find_package(PkgConfig REQUIRED)` and
# `pkg_check_modules(SDL2 REQUIRED sdl2)`, neither of which means anything with
# the NDK toolchain — there is no pkg-config in an Android sysroot and SDL2 is
# a thing we built ourselves five minutes ago, not a system package. Sharing it
# would mean bending it around a third platform; a separate file means the
# desktop build cannot be broken from here.
#
# It builds a SHARED library, not an executable. An Android app has no main():
# SDLActivity loads libmain.so and calls SDL_main through JNI. That is a
# structural fact about the platform and it is the first thing the viewer
# collides with.
cmake_minimum_required(VERSION 3.22)
project(tadpole-view-android C)

set(CMAKE_C_STANDARD 11)

set(VIEWER ${CMAKE_CURRENT_SOURCE_DIR}/../../tadpole/viewer)
set(SDL_PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/../../build/android/sdl-prefix/${ANDROID_ABI})

add_library(main SHARED
    ${VIEWER}/tadpole_view.c
    ${VIEWER}/tadpole_ui.c
    ${VIEWER}/tadpole_hle.c
    ${VIEWER}/tadpole_boot.c
    ${VIEWER}/tadpole_pad.c
    ${VIEWER}/tadpole_cam.c
    ${VIEWER}/tadpole_mic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tadpole_cam_android.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tadpole_mic_android.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tadpole_gles_compat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/jni/tadpole_jni.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/jni/tadpole_probe.c
)

# Both, because the viewer spells it two ways: tadpole_view.c has
# <SDL2/SDL.h> and tadpole_hle.c has <SDL_opengl.h>. On the desktop pkg-config
# hands over -I/usr/include/SDL2 and the first form works off the plain include
# root; here we have to supply both halves ourselves.
target_include_directories(main PRIVATE
    ${SDL_PREFIX}/include ${SDL_PREFIX}/include/SDL2 ${VIEWER})
target_compile_definitions(main PRIVATE TADPOLE_VERSION="android-dev")

# No ogg/theoradec/vorbis for Android yet, so TADPOLE_THEORA stays undefined and
# tadpole_boot.c compiles to its "no decoder" path — same as a desktop build
# without the three libraries.

# libz is in the NDK sysroot, so zlib needs no cross-build. GLESv1_CM is there
# too, which matters more than it looks: the viewer's GL is fixed-function
# (glMatrixMode, glOrthof, glEnableClientState, client-side arrays) because it
# is replaying the guest's GLES1, so the natural Android target is GLES1 and not
# a shader port.
# No camera2ndk: both of this tablet's cameras are HAL 1.0, which the NDK
# camera API hides. The frames come through Java instead — see the header of
# android/viewer/tadpole_cam_android.c.
target_link_libraries(main PRIVATE
    ${SDL_PREFIX}/lib/libSDL2.so
    GLESv1_CM EGL z log android m)

# A real executable, shipped in the APK as lib/<abi>/libtadpoleexec.so. See the
# long note in android/app/jni/tadpole_execprobe.c: an Android app may not
# execute a file it wrote, but it MAY execute one the package installer put in
# the native library directory, and that is the only route by which qemu-arm or
# any guest binary could ever run. This proves the route works.
add_executable(tadpoleexec ${CMAKE_CURRENT_SOURCE_DIR}/../app/jni/tadpole_execprobe.c)
set_target_properties(tadpoleexec PROPERTIES OUTPUT_NAME tadpoleexec)
