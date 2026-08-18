/* Tadpole — the camera backend, on Android.
 *
 * WHY THIS IS NOT THE NDK. The obvious implementation is libcamera2ndk:
 * AImageReader hands the frames straight to C, on a thread of its own, with no
 * Java in the path at all. It was written that way first and it enumerates
 * nothing on this tablet —
 *
 *     I/tadpole: camera: 0 device(s) on this tablet
 *
 * — while the platform plainly has two. The reason is in dumpsys:
 *
 *     == Camera HAL device device@1.0/internal/0 (v1.0) static information: ==
 *     == Camera HAL device device@1.0/internal/1 (v1.0) static information: ==
 *
 * Both are HAL 1.0, which camera2 presents as hardware level LEGACY, and
 * ACameraManager_getCameraIdList filters LEGACY devices out — the NDK API is
 * defined against HAL3 and will not pretend. So the frames come through
 * org.tadpole.view.CameraSource and the old android.hardware.Camera API, which
 * talks to HAL1 directly. Not a fallback: on this hardware it is the only
 * route, and it is the more compatible one everywhere else.
 *
 * WHAT THIS FILE DOES is the pixel work: NV21 in, planar I420 out, rotated,
 * mirrored, centre-cropped and scaled to whatever the guest asked S_FMT for,
 * in one pass with no intermediate buffer.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <jni.h>

#include <android/log.h>
#include <SDL.h>

#include "../../tadpole/viewer/tadpole_cam.h"

#define TAG "tadpole"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct cam {
	int            running;
	int            w, h;          /* what the guest asked for */
	unsigned char *i420;
	size_t         cap;
};
static struct cam g_cam[TAD_CAM_N];

/* ---- rotate, mirror, centre-crop and scale ------------------------------
 *
 * One pass, nearest neighbour, as a sampler rather than as separate rotate,
 * scale and crop stages: each destination pixel is written exactly once and
 * nothing is allocated in between. Cropped rather than letterboxed, because a
 * letterbox would put black bars into a photograph.
 */
struct samp {
	int sw, sh;          /* source                     */
	int rot;             /* 0/90/180/270, clockwise    */
	int mirror;          /* applied after rotation     */
	int rw, rh;          /* rotated dimensions         */
	int cx, cy, cw, ch;  /* crop rect in rotated space */
};

static void samp_init(struct samp *s, int sw, int sh, int rot, int mirror,
                      int dw, int dh)
{
	s->sw = sw; s->sh = sh; s->rot = rot; s->mirror = mirror;
	s->rw = (rot == 90 || rot == 270) ? sh : sw;
	s->rh = (rot == 90 || rot == 270) ? sw : sh;
	if ((long)s->rw * dh > (long)s->rh * dw) {
		s->ch = s->rh;
		s->cw = (int)((long)s->rh * dw / dh);
	} else {
		s->cw = s->rw;
		s->ch = (int)((long)s->rw * dh / dw);
	}
	if (s->cw < 1) s->cw = 1;
	if (s->ch < 1) s->ch = 1;
	s->cx = (s->rw - s->cw) / 2;
	s->cy = (s->rh - s->ch) / 2;
}

static void samp_map(const struct samp *s, int dx, int dy, int dw, int dh,
                     int *sx, int *sy)
{
	int rx = s->cx + (int)((long)dx * s->cw / dw);
	int ry = s->cy + (int)((long)dy * s->ch / dh);
	if (s->mirror) rx = s->rw - 1 - rx;
	if (rx < 0) rx = 0; else if (rx >= s->rw) rx = s->rw - 1;
	if (ry < 0) ry = 0; else if (ry >= s->rh) ry = s->rh - 1;
	switch (s->rot) {
	case 90:  *sx = ry;             *sy = s->sh - 1 - rx; break;
	case 180: *sx = s->sw - 1 - rx; *sy = s->sh - 1 - ry; break;
	case 270: *sx = s->sw - 1 - ry; *sy = rx;             break;
	default:  *sx = rx;             *sy = ry;             break;
	}
}

/* NV21: a full-resolution Y plane, then one interleaved VU plane at half
 * resolution in both directions. Not NV12 — the V comes first. */
static void nv21_to_i420(const unsigned char *nv21, int sw, int sh,
                         unsigned char *dst, int dw, int dh,
                         int rot, int mirror)
{
	const unsigned char *sy = nv21;
	const unsigned char *svu = nv21 + (size_t)sw * sh;
	unsigned char *dy = dst;
	unsigned char *du = dst + (size_t)dw * dh;
	unsigned char *dv = du + (size_t)((dw + 1) / 2) * ((dh + 1) / 2);
	int cdw = (dw + 1) / 2, cdh = (dh + 1) / 2;
	int csw = sw / 2;
	struct samp s;
	int x, y;

	samp_init(&s, sw, sh, rot, mirror, dw, dh);
	for (y = 0; y < dh; y++)
		for (x = 0; x < dw; x++) {
			int px, py;
			samp_map(&s, x, y, dw, dh, &px, &py);
			dy[(size_t)y * dw + x] = sy[(size_t)py * sw + px];
		}
	for (y = 0; y < cdh; y++)
		for (x = 0; x < cdw; x++) {
			int px, py;
			samp_map(&s, x * 2, y * 2, dw, dh, &px, &py);
			px /= 2; py /= 2;
			if (px >= csw) px = csw - 1;
			if (py >= sh / 2) py = sh / 2 - 1;
			dv[(size_t)y * cdw + x] = svu[(size_t)py * sw + px * 2 + 0];
			du[(size_t)y * cdw + x] = svu[(size_t)py * sw + px * 2 + 1];
		}
}

/* ---- what CameraSource calls -------------------------------------------- */

JNIEXPORT void JNICALL
Java_org_tadpole_view_CameraSource_nativeFrame(JNIEnv *env, jclass cls,
                                               jint idx, jbyteArray arr,
                                               jint sw, jint sh,
                                               jint rot, jboolean mirror)
{
	struct cam *c;
	jbyte *src;
	size_t need;
	int dw, dh;

	(void)cls;
	if (idx < 0 || idx >= TAD_CAM_N || !arr || sw <= 0 || sh <= 0)
		return;
	c = &g_cam[idx];
	if (!c->running)
		return;
	dw = c->w; dh = c->h;
	need = (size_t)dw * dh + 2 * (size_t)((dw + 1) / 2) * ((dh + 1) / 2);
	if (c->cap < need) {
		unsigned char *n = realloc(c->i420, need);
		if (!n) return;
		c->i420 = n;
		c->cap = need;
	}

	/* Critical, not GetByteArrayElements: this is the hot path and the array
	 * is a callback buffer the camera reuses, so a copy per frame would be a
	 * 460 KB memcpy for nothing. */
	src = (*env)->GetPrimitiveArrayCritical(env, arr, NULL);
	if (!src) return;
	nv21_to_i420((const unsigned char *)src, sw, sh, c->i420, dw, dh,
	             (int)rot, mirror ? 1 : 0);
	(*env)->ReleasePrimitiveArrayCritical(env, arr, src, JNI_ABORT);

	tad_cam_submit((int)idx, c->i420, dw, dh);
}

/* ---- the platform hooks tadpole_cam.c calls ------------------------------ */

static jclass source_class(JNIEnv *env)
{
	static jclass cls;
	jclass local;
	if (cls) return cls;
	local = (*env)->FindClass(env, "org/tadpole/view/CameraSource");
	if (!local) { (*env)->ExceptionClear(env); return NULL; }
	cls = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	return cls;
}

static int call_static(const char *name, const char *sig, int a, int b, int c)
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jclass cls;
	jmethodID mid;
	jint r = 0;

	if (!env || !(cls = source_class(env))) return 0;
	mid = (*env)->GetStaticMethodID(env, cls, name, sig);
	if (!mid) { (*env)->ExceptionClear(env); return 0; }
	if (sig[1] == ')')
		r = (*env)->CallStaticIntMethod(env, cls, mid);
	else if (sig[1] == 'I' && sig[2] == ')')
		(*env)->CallStaticVoidMethod(env, cls, mid, a);
	else
		r = (*env)->CallStaticBooleanMethod(env, cls, mid, a, b, c);
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		return 0;
	}
	return (int)r;
}

void tad_cam_plat_start(int idx, int w, int h)
{
	if (idx < 0 || idx >= TAD_CAM_N) return;
	g_cam[idx].w = w > 0 ? w : 640;
	g_cam[idx].h = h > 0 ? h : 480;
	g_cam[idx].running = call_static("start", "(III)Z", idx,
	                                 g_cam[idx].w, g_cam[idx].h);
	if (!g_cam[idx].running)
		LOGE("camera %d: CameraSource.start refused", idx);
}

void tad_cam_plat_stop(int idx)
{
	if (idx < 0 || idx >= TAD_CAM_N) return;
	g_cam[idx].running = 0;
	call_static("stop", "(I)V", idx, 0, 0);
}

int tad_cam_plat_running(int idx)
{
	if (idx < 0 || idx >= TAD_CAM_N) return 0;
	return g_cam[idx].running;
}

