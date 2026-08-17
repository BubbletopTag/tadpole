/* Tadpole — the GL command stream shared by the guest encoder and the host
 * replayer. Included by BOTH sides, so it must stay free of libc and of any
 * assumption about which architecture is reading it.
 *
 * WHY THIS EXISTS
 * ---------------
 * The software rasteriser is 90% of a frame (78.4 ms of 87.0) and its ceiling is
 * architectural: it is ARM code inside qemu, so every pixel is paid for through
 * the JIT with soft-float on top. Measured, it manages 1.07 Mpx/s. The same work
 * on the host GPU measured 1563 Mpx/s — ~134x — and the readback that worried me
 * costs 0.071 ms. See viewer/hle_probe.c.
 *
 * The guest cannot call host Mesa; it is ARM code and there is no host boundary
 * to cross from inside it. So SERIALISE instead: the guest writes GL calls into a
 * ring in shared memory and the native viewer replays them. Same shape as virgl,
 * one layer up — at the GLES1 API rather than at virtio.
 *
 * LAYOUT
 * ------
 * One file, $TADPOLE_DIR/glcmd.bin: a fixed header followed by TADGL_RING bytes
 * of packet data. Every packet is
 *
 *     u16 op ; u16 pad ; u32 len ; <len bytes payload, padded to 4>
 *
 * A fixed 8-byte header keeps every payload 4-byte aligned, which matters because
 * the guest is 32-bit ARM and unaligned loads are not free.
 *
 * head and tail are MONOTONIC byte counters, never indices. Wrapping is done by
 * the reader and writer with `% TADGL_RING`. That makes full and empty
 * unambiguous — `head == tail` is empty and `head - tail == TADGL_RING` is full,
 * with no ambiguous case and no need for a spare slot. The audio ring in
 * tadpole_view.c uses the same convention for the same reason.
 *
 * ENDIANNESS AND WIDTH. Guest and host are both little-endian here, and every
 * field is a fixed-width u32/i32/float, so the stream is exchanged as-is. There
 * is deliberately no pointer in any payload: an address means nothing across the
 * boundary, so array references travel as (buffer name, byte offset) and pixel
 * and vertex data travel by value.
 */
#ifndef TADPOLE_GLCMD_H
#define TADPOLE_GLCMD_H

#define TADGL_MAGIC   0x4C474454u      /* 'TDGL' little-endian */
#define TADGL_VERSION 1u

/* 8 MB of payload. A steady-state frame is ~28 draw calls of state and offsets,
 * but a texture upload can be 320x240x4 = 300 KB and a buffer upload 48 KB, so
 * the ring has to absorb a burst of them at title load without stalling. */
#define TADGL_RING (8u << 20)

enum tadgl_op {
	TADGL_NOP = 0,

	/* ---- frame ---- */
	TADGL_PRESENT,          /* no payload; end of frame */
	TADGL_CLEAR,            /* u32 mask, u32 argb, float depth */
	TADGL_VIEWPORT,         /* i32 x, y, w, h */

	/* ---- fixed-function state ---- */
	TADGL_ENABLE,           /* u32 cap */
	TADGL_DISABLE,          /* u32 cap */
	TADGL_BLENDFUNC,        /* u32 src, dst */
	TADGL_DEPTHFUNC,        /* u32 func */
	TADGL_DEPTHMASK,        /* u32 on */
	TADGL_ALPHAFUNC,        /* u32 func, float ref */
	TADGL_TEXENV,           /* u32 target, pname, i32 value */
	TADGL_COLOR,            /* float r,g,b,a */
	TADGL_CULLFACE,         /* u32 mode */
	TADGL_FRONTFACE,        /* u32 mode */
	TADGL_SHADEMODEL,       /* u32 mode */

	/* ---- matrices (always sent as float, never GLfixed) ---- */
	TADGL_MATRIXMODE,       /* u32 mode */
	TADGL_LOADIDENTITY,     /* no payload */
	TADGL_LOADMATRIX,       /* float m[16] */
	TADGL_MULTMATRIX,       /* float m[16] */
	TADGL_PUSHMATRIX,
	TADGL_POPMATRIX,
	TADGL_ORTHO,            /* float l,r,b,t,n,f */
	TADGL_FRUSTUM,          /* float l,r,b,t,n,f */
	TADGL_TRANSLATE,        /* float x,y,z */
	TADGL_ROTATE,           /* float a,x,y,z */
	TADGL_SCALE,            /* float x,y,z */

	/* ---- textures ---- */
	TADGL_BINDTEXTURE,      /* u32 name */
	TADGL_ACTIVETEXTURE,    /* u32 unit */
	TADGL_TEXIMAGE2D,       /* u32 name,w,h  then w*h*4 bytes ARGB8888 */
	TADGL_TEXSUBIMAGE2D,    /* u32 name,x,y,w,h then w*h*4 ARGB8888 */
	TADGL_TEXPARAM,         /* u32 pname, i32 value */
	TADGL_DELETETEXTURE,    /* u32 name */

	/* ---- buffer objects, mirrored on the host as plain bytes ---- */
	TADGL_BUFFERDATA,       /* u32 name, u32 size, then size bytes */
	TADGL_BUFFERSUBDATA,    /* u32 name, u32 offset, u32 size, then bytes */
	TADGL_DELETEBUFFER,     /* u32 name */

	/* ---- vertex arrays: a reference, never a pointer ---- */
	TADGL_ARRAYPOINTER,     /* u32 which, buffer, i32 size, u32 type,
	                         * i32 stride, u32 offset */
	TADGL_CLIENTSTATE,      /* u32 which, u32 on */

	/* ---- draws ---- */
	TADGL_DRAWARRAYS,       /* u32 mode, i32 first, i32 count */
	TADGL_DRAWELEMENTS,     /* u32 mode, i32 count, u32 type,
	                         * u32 elembuf, u32 offset */

	/* ---- context teardown ---- */
	/* No payload. Drop every mirrored texture, buffer and array.
	 *
	 * AppManager does not exit between games — it dlopen()s App.so, runs it,
	 * and UnloadModule()s it — so the shim's GL tables are process-lifetime and
	 * used to survive into the next title. Game A would fill all 192 texture
	 * slots, then game B's glGenTextures found none free and returned name 0,
	 * so it drew untextured; each title after that was worse than the last.
	 * Real hardware never showed this because its driver tears the context
	 * down, which is exactly what Brio was complaining about in the one log
	 * line that appears here and never on the device:
	 *     ExitPopUnloadApp: OGL context still active after unloading
	 * Appended last so existing opcode numbers are unchanged. */
	TADGL_RESET,

	/* GL_TEXTURE_ENV_COLOR — four floats, which TADGL_TEXENV cannot carry: its
	 * payload is a single i32 because every other TexEnv parameter is an enum.
	 * Only reached by glTexEnvfv/glTexEnvxv with that one pname. Appended after
	 * TADGL_RESET for the same reason RESET was appended after everything else:
	 * existing opcode numbers must not move, or a viewer and a shim built at
	 * different times disagree about what every packet means. */
	TADGL_TEXENVCOLOR,      /* float r,g,b,a */

	/* ---- per-fragment and per-primitive state -------------------------
	 * All appended, never inserted — see the note on TADGL_RESET. Each of
	 * these was a no-op stub the installed titles import in numbers:
	 * glScissor 23 titles, glPolygonOffsetx 20, glPointSizex 19,
	 * glLineWidthx 32, glColorMask 12 (tools/gl-demand.py). */
	TADGL_SCISSOR,          /* i32 x, y, w, h */
	TADGL_COLORMASK,        /* u32 r, g, b, a  (each 0 or 1) */
	TADGL_LINEWIDTH,        /* float w */
	TADGL_POINTSIZE,        /* float s */
	TADGL_POLYGONOFFSET,    /* float factor, units */

	/* ---- fixed-function lighting --------------------------------------
	 * The host evaluates it; desktop GL's compatibility profile has the whole
	 * pipeline. Each carries a count because these parameters are 1, 3 or 4
	 * components depending on pname, and sending a fixed 4 would make
	 * GL_SPOT_DIRECTION read one float past what the guest set.
	 *
	 * SENT IN CALL ORDER, NOT BATCHED: GL_POSITION and GL_SPOT_DIRECTION are
	 * transformed by the modelview matrix in force at the moment of the call,
	 * and matrix commands share this ring, so order is what makes the host's
	 * transform match the guest's. */
	TADGL_LIGHT,            /* u32 light, pname, count; then count floats */
	TADGL_MATERIAL,         /* u32 face,  pname, count; then count floats */
	TADGL_LIGHTMODEL,       /* u32 pname, count;        then count floats */
	TADGL_NORMAL,           /* float x, y, z */

	TADGL_OP_COUNT
};

/* `which` for TADGL_ARRAYPOINTER / TADGL_CLIENTSTATE. Small dense values rather
 * than the GL enums, so the host can index an array with them.
 *
 * TEXCOORD1 IS A SEPARATE SLOT, not a parameter on TEXCOORD, because the host
 * binds each unit's array with its own glTexCoordPointer under its own
 * glClientActiveTexture — they are two independent arrays that happen to share
 * an entry point, exactly as GL models them. Appending it keeps every existing
 * opcode's encoding byte-identical; the host bounds-checks against
 * TADGL_ARR_COUNT, so an older host simply ignores a slot it does not know.
 *
 * Two units because the device advertises GL_MAX_TEXTURE_UNITS = 2. */
enum tadgl_array { TADGL_ARR_VERTEX = 0, TADGL_ARR_COLOR, TADGL_ARR_TEXCOORD,
                   TADGL_ARR_NORMAL, TADGL_ARR_TEXCOORD1, TADGL_ARR_COUNT };

struct tadgl_hdr {
	unsigned int magic, version, ring_bytes;
	/* Monotonic byte counters — see the note above. */
	volatile unsigned int head;         /* guest writes */
	volatile unsigned int tail;         /* host reads */
	volatile unsigned int frames_sent;  /* guest bumps on PRESENT */
	volatile unsigned int frames_done;  /* host bumps after replaying one */
	/* Host heartbeat, as a COUNTER not a flag. The host bumps it on every pump.
	 * A flag cannot distinguish "the viewer is alive but had a slow frame" from
	 * "the viewer died and left a stale 1 behind", and that distinction is the
	 * whole difference between waiting a moment and abandoning GPU rendering for
	 * the rest of the session. */
	volatile unsigned int host_alive;
	/* Host -> guest repair request. A one-shot resync at attach is not enough:
	 * the host's mirror of buffers and textures can end up incomplete for
	 * reasons that vary run to run, and a draw whose element buffer is missing
	 * is silently skipped. Rather than guess which upload went astray, the host
	 * ASKS for the state again and the guest replays its tables. Also covers a
	 * host that restarts mid-session. */
	volatile unsigned int want_resync;
	/* Set by the guest when it stops encoding, so the front end can SAY SO.
	 * Falling back silently looks exactly like "HLE is not working" and cost a
	 * confusing round of testing. */
	volatile unsigned int guest_fellback;
	unsigned int pad[6];
};

struct tadgl_pkt { unsigned short op, pad; unsigned int len; };

#define TADGL_DATA(h) ((unsigned char *)(h) + sizeof(struct tadgl_hdr))
#define TADGL_FILE_BYTES (sizeof(struct tadgl_hdr) + TADGL_RING)

#endif /* TADPOLE_GLCMD_H */
