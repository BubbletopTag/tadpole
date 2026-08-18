/* Tadpole — the microphone backend, on Android.
 *
 * The whole of it is a bridge to org.tadpole.view.MicSource: AudioRecord is
 * Java-only (there is no NDK equivalent short of AAudio, which is API 26+ and
 * built for low-latency playback rather than for this), and the work on this
 * side is a write into a FIFO. See tadpole/viewer/tadpole_mic.c for what the
 * FIFO is and why its being open is also the on switch.
 */

#include <jni.h>
#include <android/log.h>
#include <SDL.h>

#include "../../tadpole/viewer/tadpole_mic.h"

#define TAG "tadpole"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

JNIEXPORT void JNICALL
Java_org_tadpole_view_MicSource_nativeMicData(JNIEnv *env, jclass cls,
                                              jbyteArray arr, jint len)
{
	jbyte *p;

	(void)cls;
	if (!arr || len <= 0)
		return;
	/* Critical: this runs at the recording rate on a thread of its own, and
	 * the array is reused every read, so a copy would be pure overhead. */
	p = (*env)->GetPrimitiveArrayCritical(env, arr, NULL);
	if (!p)
		return;
	tad_mic_data(p, (int)len);
	(*env)->ReleasePrimitiveArrayCritical(env, arr, p, JNI_ABORT);
}

static jclass mic_class(JNIEnv *env)
{
	static jclass cls;
	jclass local;
	if (cls) return cls;
	local = (*env)->FindClass(env, "org/tadpole/view/MicSource");
	if (!local) { (*env)->ExceptionClear(env); return NULL; }
	cls = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	return cls;
}

void tad_mic_plat_start(int rate, int channels)
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jclass cls;
	jmethodID mid;

	if (!env || !(cls = mic_class(env))) return;
	mid = (*env)->GetStaticMethodID(env, cls, "start", "(II)Z");
	if (!mid) { (*env)->ExceptionClear(env); return; }
	(*env)->CallStaticBooleanMethod(env, cls, mid, rate, channels);
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		LOGE("mic: MicSource.start threw");
	}
}

void tad_mic_plat_stop(void)
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jclass cls;
	jmethodID mid;

	if (!env || !(cls = mic_class(env))) return;
	mid = (*env)->GetStaticMethodID(env, cls, "stop", "()V");
	if (!mid) { (*env)->ExceptionClear(env); return; }
	(*env)->CallStaticVoidMethod(env, cls, mid);
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
	}
}
