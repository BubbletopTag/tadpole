# Tadpole — LeapPad2 emulator
#
#   make            build shim (ARM) + viewer (host)
#   make viewer     host viewer only — no cross toolchain needed
#   make shim       ARM LD_PRELOAD shim  (needs clang + lld)
#
# The shim is cross-compiled with clang. It needs an ARM-capable linker;
# system GNU ld is x86-only here, so install lld:  sudo pacman -S lld

CLANG   ?= clang
CC      ?= cc

ARCH_TRIPLE := armv7-unknown-linux-gnueabi
# uClibc 0.9.32, ARMv7-A, VFPv2, SOFT-FLOAT calling convention.
# Confirmed by readelf on AppManager: Tag_ABI_VFP_args is absent, so floats
# pass in core registers. This is NOT armhf — do not "fix" it to hard-float.
ARMFLAGS := --target=$(ARCH_TRIPLE) -march=armv7-a -mfpu=vfp -mfloat-abi=softfp

# INJECTION: WE CANNOT USE LD_PRELOAD.
# LeapFrog's uClibc 0.9.32 was built WITHOUT __LDSO_PRELOAD_ENV_SUPPORT__.
# The guest loader never even opens an LD_PRELOAD path — verified with a
# known-good library, so it is not a problem with our object. (The string
# "LD_PRELOAD" does appear in ld-uClibc, but only in the list of variables
# it scrubs for setuid binaries.)
#
# Instead we impersonate libdl.so.0, which sits at position 22 in
# AppManager's DT_NEEDED list — comfortably before libc.so.0 at 33, so our
# open/ioctl definitions win symbol lookup. The real libdl is copied aside
# with its SONAME patched to libdl.so.9 (same length, so it patches in
# place) and becomes a dependency of ours, which means dlopen/dlsym/dlclose
# resolve straight through to it and we get dlsym(RTLD_NEXT, ...) for free.
GUESTLIBS := ../runtime/libs
ROOTFS_LIB := ../rootfs/stock-4.6.0.784/1221351650/ubi_rfs/usr/lib
SHIMLIBS  := ../runtime/shimlibs
SHIM_SONAME := libdl.so.0
REAL_DL     := libdl.so.9

SHIM_SRC := shim/tadpole_shim.c shim/tadpole_crash.c
SHIM_OUT := $(SHIMLIBS)/libdl.so.0
# Second variant: the display tools (imager-fb, drawtext-fb, flipbook-fb,
# fbtest) link no libdl at all — libz.so.1 is their first NEEDED entry.
SHIMLIBS_Z := ../runtime/shimlibs-z
SHIM_OUT_Z := $(SHIMLIBS_Z)/libz.so.1
REAL_Z     := libz.so.9
# Third variant: a COMPLETE REPLACEMENT for libasound.so.2, not an
# impersonation. There is nothing to chain through to — qemu-user does not
# forward the ALSA control ioctls, so the real libasound cannot reach a device
# even if we let it try. We implement the 26 snd_pcm_* symbols libAudio.so
# imports and pipe the PCM to the viewer instead.
ASOUND_SRC := shim/tadpole_asound.c
ASOUND_OUT := $(SHIMLIBS)/libasound.so.2

EGL_SRC   := shim/tadpole_egl.c
GLES_SRC  := shim/tadpole_gles_core.c shim/tadpole_gles_stubs.c \
             shim/tadpole_gles_hle.c shim/tadpole_gles_debug.c
SHIMLIBS_GL := ../runtime/shimlibs-gl
# CHAIN, DO NOT REPLACE. The stock libGLESv1_CM.so.1.1 is a 1.9 MB blob that
# exports 697 symbols, only 180 of which are GL — it also carries Flash Lite
# helpers like DataConvert_to_enumv/_intv that LeapFrogPlugin.so links against.
# Swapping it out wholesale makes the plugin fail to load, every LF.CSystem call
# return undefined, and the whole UI go black. So keep the real library in the
# process under another name and let our gl* definitions win by being the
# direct dependency; everything else falls through. Same trick as libdl.so.0 ->
# libdl.so.9 (HANDOVER 4.1), and easier here because it has no SONAME, so the
# copy can simply be renamed.
REAL_GLES := libGLESreal.so
EGL_OUT   := $(SHIMLIBS_GL)/libEGL.so
GLES_OUT  := $(SHIMLIBS_GL)/libGLESv1_CM.so

TONE_SRC := shim/tone_test.c
TONE_OUT := ../runtime/tone

VIEW_SRC := viewer/tadpole_view.c viewer/tadpole_ui.c viewer/tadpole_hle.c
VIEW_HDR := viewer/tadpole_ui.h viewer/tadpole_font.h viewer/tadpole_hle.h \
            shim/tadpole_glcmd.h
VIEW_OUT := viewer/tadpole-view

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

.PHONY: all shim shimz asound gl tone viewer hle-probe hle-selftest clean check realdl realz

all: shim shimz asound gl viewer

shim: $(SHIM_OUT)

asound: $(ASOUND_OUT)

# GL: replace the stock EGL/GLES1 front-ends at the standard API boundary.
# Freestanding — these call nothing but each other.
gl: gl-links

$(EGL_OUT): $(EGL_SRC) $(GLES_OUT)
	@mkdir -p $(SHIMLIBS_GL)
	$(CLANG) $(ARMFLAGS) -fPIC -shared -nostdlib -O2 -Wall \
		-Wno-incompatible-library-redeclaration \
		-fno-stack-protector -fno-builtin \
		-fuse-ld=lld -Wl,-soname,libEGL.so.1 -o $@ $< \
		-L$(SHIMLIBS_GL) -l:libGLESv1_CM.so $(GUESTLIBS)/libc.so.0
	@echo "built $@"

$(GLES_OUT): $(GLES_SRC) shim/tadpole_glcmd.h shim/tadpole_gles_debug.h
	@mkdir -p $(SHIMLIBS_GL)
	$(CLANG) $(ARMFLAGS) -fPIC -shared -nostdlib -O2 -Wall \
		-fno-stack-protector -fno-builtin \
		-fuse-ld=lld -Wl,-soname,libGLESv1_CM.so.1 -o $@ $(GLES_SRC) \
		$(GUESTLIBS)/libc.so.0
	@echo "built $@"
	@$(MAKE) --no-print-directory check-undefined LIB=$@

# NOTHING MAY BE LEFT UNDEFINED EXCEPT libc AND THE ARM EABI HELPERS.
#
# -shared does not object to an undefined symbol, so a function that is declared
# and called but never DEFINED links perfectly and then fails in the guest — at
# LOAD time, killing the title outright, with a message about our library rather
# than about the missing helper:
#     AppManager: can't resolve symbol 'tad_gl_fatal'
# That is the same failure the whole gen-gl-stubs.py machinery exists to prevent
# for GL entry points, and it applies just as much to our own internals. This
# caught exactly that: a rewrite dropped tad_gl_fatal's body while leaving two
# calls to it, and the build said nothing.
#
# --no-undefined cannot be used instead: __aeabi_uidiv and __aeabi_idivmod come
# from the guest's libgcc at runtime and are legitimately unresolved here.
.PHONY: check-undefined
check-undefined:
	@readelf --dyn-syms -W $(LIB) | awk '$$7=="UND"{print $$8}' \
	  | sed 's/@.*//' | sort -u > /tmp/tad-und.$$$$; \
	readelf --dyn-syms -W $(GUESTLIBS)/libc.so.0 \
	  | awk '$$7!="UND" && $$8!=""{print $$8}' | sed 's/@.*//' | sort -u \
	  > /tmp/tad-libc.$$$$; \
	bad=$$(comm -23 /tmp/tad-und.$$$$ /tmp/tad-libc.$$$$ | grep -v '^__aeabi_'); \
	rm -f /tmp/tad-und.$$$$ /tmp/tad-libc.$$$$; \
	if [ -n "$$bad" ]; then \
	  echo "ERROR: $(LIB) references symbols nothing provides:" >&2; \
	  echo "$$bad" | sed 's/^/  /' >&2; \
	  echo "  (the guest loader treats each of these as fatal at load time)" >&2; \
	  exit 1; \
	fi
	@echo "  undefined symbols: libc + __aeabi_* only  OK"

# THE ALIAS NAMES ARE A SEPARATE, ALWAYS-CHECKED TARGET.
#
# These used to be side effects of the recipe above, which only runs when the
# .so is out of date — so a link that was missing (or never created, as
# libopengles_lite.so never was) could not be restored by any number of
# rebuilds. `make gl` would print nothing and fix nothing.
gl-links: $(GLES_OUT) $(EGL_OUT)
	@# The loader resolves DT_NEEDED by SONAME as a FILENAME, so libEGL's
	@# dependency on libGLESv1_CM.so.1 needs that exact name on disk.
	@ln -sf libGLESv1_CM.so $(SHIMLIBS_GL)/libGLESv1_CM.so.1
	@ln -sf libEGL.so $(SHIMLIBS_GL)/libEGL.so.1
	@# libopengles_lite.so IS libGLESv1_CM.so — a symlink, on the real device.
	@# Native titles link THAT name (Clam Prix's DT_NEEDED says
	@# libopengles_lite.so, not libGLESv1_CM.so.1), and because the loader
	@# searches by filename, its absence here meant the search fell through
	@# shimlibs-gl to runtime/libs and loaded the STOCK VR5 driver instead of
	@# ours. That driver then cannot resolve its own EGL::* C++ internals,
	@# because our libEGL replaced the stock one:
	@#     symbol '_ZN3EGL6Object9OnReleaseEi': can't resolve symbol
	@# So those games ran on a half-linked dead driver and rendered nothing.
	@# Verified with a test binary linked exactly as Clam Prix is: without this
	@# link it fails as above, with it glGetString(GL_VENDOR) returns "Tadpole".
	@ln -sf libGLESv1_CM.so $(SHIMLIBS_GL)/libopengles_lite.so
	@echo "gl aliases: libGLESv1_CM.so.1 libEGL.so.1 libopengles_lite.so"
.PHONY: gl-links


tone: $(TONE_OUT)

$(TONE_OUT): $(TONE_SRC) $(ASOUND_OUT)
	$(CLANG) $(ARMFLAGS) -O2 -Wall -nostdlib -fno-builtin \
		-fno-stack-protector -fuse-ld=lld -o $@ $< \
		-Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
		-L$(SHIMLIBS) -L$(GUESTLIBS) -l:libasound.so.2 -l:libc.so.0
	@echo "built $@" 


$(ASOUND_OUT): $(ASOUND_SRC)
	@mkdir -p $(SHIMLIBS)
	$(CLANG) $(ARMFLAGS) -fPIC -shared -nostdlib -O2 -Wall \
		-Wno-incompatible-library-redeclaration \
		-fno-stack-protector -fno-builtin \
		-fuse-ld=lld -Wl,-soname,libasound.so.2 -o $@ $< \
		$(GUESTLIBS)/libc.so.0
	@echo "built $@"


$(SHIM_OUT): $(SHIM_SRC) $(SHIMLIBS)/$(REAL_DL)
	$(CLANG) $(ARMFLAGS) -fPIC -shared -nostdlib -O2 -Wall \
		-fno-stack-protector -fno-builtin \
		-fuse-ld=lld -Wl,-soname,$(SHIM_SONAME) -o $@ $(SHIM_SRC) \
		$(SHIMLIBS)/$(REAL_DL) $(GUESTLIBS)/libc.so.0
	@echo "built $@"
	@echo -n "  SONAME: "; readelf -d $@ | grep SONAME | sed 's/.*\[\(.*\)\]/\1/'
	@echo -n "  NEEDED: "; readelf -d $@ | grep NEEDED | sed 's/.*\[\(.*\)\]/\1/' | tr '\n' ' '; echo

# The real libdl, renamed so it can coexist with our impersonator.
$(SHIMLIBS)/$(REAL_DL):
	@echo "run: make realdl" && false

.PHONY: realdl
realdl:
	@mkdir -p $(SHIMLIBS)
	@python3 -c "import pathlib;\
s=pathlib.Path('../rootfs/stock-4.6.0.784/1221351650/ubi_rfs/lib/libdl-0.9.32.1-git.so');\
d=pathlib.Path('$(SHIMLIBS)/$(REAL_DL)');\
d.write_bytes(s.read_bytes().replace(b'libdl.so.0\x00', b'libdl.so.9\x00'));\
print('wrote', d)"

shimz: $(SHIM_OUT_Z)

$(SHIM_OUT_Z): $(SHIM_SRC) $(SHIMLIBS_Z)/$(REAL_Z)
	$(CLANG) $(ARMFLAGS) -fPIC -shared -nostdlib -O2 -Wall \
		-fno-stack-protector -fno-builtin \
		-fuse-ld=lld -Wl,-soname,libz.so.1 -o $@ $(SHIM_SRC) \
		$(SHIMLIBS_Z)/$(REAL_Z) $(SHIMLIBS)/$(REAL_DL) $(GUESTLIBS)/libc.so.0
	@echo "built $@"

$(SHIMLIBS_Z)/$(REAL_Z):
	@mkdir -p $(SHIMLIBS_Z)
	@python3 -c "import pathlib;\
b=pathlib.Path('../rootfs/stock-4.6.0.784/1221351650/ubi_rfs/usr/lib');\
s=(b/'libz.so.1').resolve();\
d=pathlib.Path('$(SHIMLIBS_Z)/$(REAL_Z)');\
d.write_bytes(s.read_bytes().replace(b'libz.so.1\x00', b'libz.so.9\x00'));\
print('wrote', d)"

# Feasibility/regression check for host-GPU replay. Not part of `all`: it needs
# a GL context, so it cannot run headless in CI.
# End-to-end round trip: the real encoder and the real replayer in one process,
# no guest and no visible window. Catches wire-format and context bugs in a
# second instead of a five-minute boot.
hle-selftest: viewer/hle_selftest.c viewer/tadpole_hle.c shim/tadpole_gles_hle.c \
              shim/tadpole_glcmd.h viewer/tadpole_hle.h
	$(CC) -O2 -Wall -o viewer/hle-selftest viewer/hle_selftest.c \
	      viewer/tadpole_hle.c shim/tadpole_gles_hle.c \
	      $(SDL_CFLAGS) $(SDL_LIBS) $(shell pkg-config --libs gl)
	@echo "built viewer/hle-selftest"

hle-probe: viewer/hle_probe.c
	$(CC) -O2 -Wall -o viewer/hle-probe $< $(SDL_CFLAGS) $(SDL_LIBS) \
	      $(shell pkg-config --cflags --libs gl)
	@echo "built viewer/hle-probe — run it to re-check the HLE assumptions"

# -std=gnu17 IS NOT A STYLE CHOICE — it is what lets this run on a distribution
# older than the one it was built on.
#
# With the compiler's default (gnu23 on GCC 13+), calls to strtol, sscanf and
# friends bind to glibc 2.38's __isoc23_* variants, because C23 changed how
# those parse binary literals. Four symbols is all it takes:
#
#     __isoc23_fscanf  __isoc23_sscanf  __isoc23_strtoll  __isoc23_strtol
#
# and the finished binary then REQUIRES glibc 2.38 — Ubuntu 23.10 or newer.
# On Linux Mint 21 (glibc 2.35) it does not start, and the failure names a
# library rather than a version, which reads as "SDL is missing" to anyone who
# has not seen it before. Building as gnu17 uses the ordinary symbols and drops
# the floor to 2.34, which is Ubuntu 22.04 and Mint 21.
#
# Going below 2.34 needs an older build host; 2.34 is where glibc merged
# libpthread and every binary built since references __libc_start_main@2.34.
HOST_STD := -std=gnu17
#
# AND _GNU_SOURCE HAS TO GO WITH IT. sdl2.pc puts -D_GNU_SOURCE=1 in its
# cflags, and glibc re-enables the C23 redirects whenever that is defined,
# whatever -std says. Measured on a two-line program:
#
#     <default>                        2 __isoc23_ symbols
#     -std=gnu17                       0
#     -std=gnu17 -D_GNU_SOURCE=1       2      <- what sdl2.pc gives us
#     -std=c17                         0
#
# So the flag is filtered out of SDL's cflags rather than fought with. The
# viewer uses no GNU extensions; SDL's headers define what they need.
SDL_CFLAGS_PORTABLE := $(filter-out -D_GNU_SOURCE=1 -D_GNU_SOURCE,$(SDL_CFLAGS))

viewer: $(VIEW_OUT)

$(VIEW_OUT): $(VIEW_SRC) $(VIEW_HDR)
	$(CC) -O2 -Wall $(HOST_STD) -o $@ $(VIEW_SRC) $(SDL_CFLAGS_PORTABLE) \
	      $(SDL_LIBS) -lz $(shell pkg-config --libs gl)
	@echo "built $@"

# The UI font is generated from ASCII art so the glyphs stay reviewable.
viewer/tadpole_font.h: ../tools/genfont.py
	python3 $< $@

check:
	@printf 'clang:  '; command -v $(CLANG) || echo MISSING
	@printf 'lld:    '; command -v ld.lld   || echo 'MISSING  -> sudo pacman -S lld'
	@printf 'sdl2:   '; pkg-config --modversion sdl2 2>/dev/null || echo MISSING
	@printf 'qemu:   '; command -v qemu-arm || echo MISSING

clean:
	rm -f $(SHIM_OUT) $(SHIM_OUT_Z) $(VIEW_OUT)
