# Glasspole.
#
# CMake rather than the Makefile the rest of Tadpole uses, for two reasons that
# both come from outside: dynarmic is a CMake project, and the Windows build
# will be a MinGW cross-compile driven by a toolchain file. Fighting either of
# those with hand-written rules is not a good use of anyone's afternoon.
cmake_minimum_required(VERSION 3.20)
project(glasspole C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- dynarmic -------------------------------------------------------------
set(DYNARMIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/dynarmic")
if(NOT EXISTS "${DYNARMIC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "dynarmic is missing — run ./fetch-deps.sh first")
endif()

# A32 only: the guest is 32-bit ARM and the A64 frontend is a third of the
# build for code we will never execute.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
# dynarmic's bundled externals still declare cmake_minimum_required(VERSION 3.x)
# below what CMake 4 will accept without this.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
add_subdirectory("${DYNARMIC_DIR}" EXCLUDE_FROM_ALL)

# ---- the host backend -----------------------------------------------------
# Exactly one of these is compiled. That is the whole design: if a second file
# ever needs an #ifdef to choose between them, the interface in host.h is wrong.
if(WIN32)
    set(GP_HOST src/host_win32.c)
else()
    set(GP_HOST src/host_posix.c)
endif()

add_executable(glasspole src/cpu.cpp src/syscall.cpp src/signal.cpp src/cp15.cpp
                         src/kuser.cpp src/elf.c ${GP_HOST})
target_include_directories(glasspole PRIVATE src)
target_link_libraries(glasspole PRIVATE dynarmic)
if(WIN32)
    # bcrypt: gp_random. winmm: timeBeginPeriod, so nanosleep is ~1 ms rather
    # than ~15.6. Both are system DLLs on every Windows since 7.
    target_link_libraries(glasspole PRIVATE bcrypt winmm)
    # Static runtime: one file someone can copy, with no MinGW DLLs beside it.
    target_link_options(glasspole PRIVATE -static)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(glasspole PRIVATE Threads::Threads)
endif()

# ---- the guest-side test programs -----------------------------------------
# Built with clang's ARM target, matching the flags tadpole/Makefile already
# uses for the shim: ARMv7-A, VFP, softfp — i.e. the real console.
find_program(GP_CLANG clang)
find_program(GP_OBJCOPY NAMES llvm-objcopy objcopy)

if(GP_CLANG AND GP_OBJCOPY)
    # -fuse-ld=lld because the host's GNU ld is built for x86 only and answers
    # an ARM link with "unrecognised emulation mode: armelf_linux_eabi". lld is
    # multi-target by construction, so this needs no cross-binutils install.
    set(GP_ARMFLAGS --target=armv7-unknown-linux-gnueabi
                    -march=armv7-a -mfpu=vfp -mfloat-abi=softfp -fuse-ld=lld)
    foreach(gp_test hello thread threads-many abort segv)
    add_custom_command(
        OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/${gp_test}.bin
        COMMAND ${GP_CLANG} ${GP_ARMFLAGS} -nostdlib -static
                -Wl,-Ttext=0x00010000 -Wl,-e,_start
                -o ${CMAKE_CURRENT_BINARY_DIR}/${gp_test}.elf
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/${gp_test}.S
        # -j .text, and it matters. Without it objcopy also carries
        # .ARM.attributes, which sits at VMA 0, so the flat binary comes out
        # with 64 KB of zeros ahead of the code — and those zeros decode as
        # `andeq r0, r0, r0`. The guest then runs a 16,384-instruction NOP sled
        # and falls into _start by luck. It works, which is the problem.
        COMMAND ${GP_OBJCOPY} -O binary -j .text
                ${CMAKE_CURRENT_BINARY_DIR}/${gp_test}.elf
                ${CMAKE_CURRENT_BINARY_DIR}/${gp_test}.bin
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${gp_test}.S
        COMMENT "Assembling the ARM guest test program ${gp_test}")
    list(APPEND gp_test_bins ${CMAKE_CURRENT_BINARY_DIR}/${gp_test}.bin)
    endforeach()
    add_custom_target(gp-tests ALL DEPENDS ${gp_test_bins})
else()
    message(WARNING "clang or objcopy missing — guest test programs will not build")
endif()
