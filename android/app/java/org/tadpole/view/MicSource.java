package org.tadpole.view;

import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.util.Log;

/**
 * The tablet's microphone, for the LeapPad2's.
 *
 * <p>AudioRecord and not MediaRecorder: MediaRecorder writes an encoded file
 * and the guest wants raw PCM frames it can put through its own encoder — the
 * Camera app muxes MJPEG video and PCM audio into an AVI with libavformat, and
 * the microphone widget writes a WAV with libsndfile. Both want samples.
 *
 * <p>VOICE_RECOGNITION rather than MIC as the source: it asks the platform for
 * the least processing, and processing is exactly what is not wanted here. The
 * LeapPad2's own microphone is a plain electret and the firmware's noise
 * handling assumes it; an aggressive AGC or echo canceller on the way in would
 * be a second, invisible one. If the device has no such source the constructor
 * falls back to MIC.
 *
 * <p>The thread reads and hands the bytes straight to C, which writes them into
 * the FIFO the guest is reading. Nothing is buffered here: the shim's staging
 * buffer is the place for that and it is sized for it.
 */
final class MicSource {
    private static final String TAG = "tadpole";
    private static MicSource live;

    private AudioRecord rec;
    private Thread thread;
    private volatile boolean stop;

    private MicSource() {}

    static synchronized boolean start(int rate, int channels) {
        stop();
        MicSource m = new MicSource();
        if (!m.open(rate, channels)) return false;
        live = m;
        return true;
    }

    static synchronized void stop() {
        MicSource m = live;
        live = null;
        if (m != null) m.close();
    }

    private boolean open(int rate, int channels) {
        int cfg = (channels >= 2) ? AudioFormat.CHANNEL_IN_STEREO
                                  : AudioFormat.CHANNEL_IN_MONO;
        int min = AudioRecord.getMinBufferSize(rate, cfg,
                                               AudioFormat.ENCODING_PCM_16BIT);
        if (min <= 0) {
            Log.e(TAG, "mic: " + rate + " Hz x" + channels + " not supported");
            return false;
        }
        /* Four times the minimum: the reader is a plain thread competing with
         * the render loop, and an underspecified buffer here shows up as
         * clicks in the recording rather than as an error anywhere. */
        final int buf = min * 4;
        try {
            rec = new AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION,
                                  rate, cfg, AudioFormat.ENCODING_PCM_16BIT, buf);
            if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
                rec.release();
                rec = new AudioRecord(MediaRecorder.AudioSource.MIC, rate, cfg,
                                      AudioFormat.ENCODING_PCM_16BIT, buf);
            }
        } catch (Throwable t) {
            Log.e(TAG, "mic: could not open AudioRecord", t);
            rec = null;
            return false;
        }
        if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "mic: AudioRecord did not initialise — is"
                       + " android.permission.RECORD_AUDIO granted?");
            rec.release();
            rec = null;
            return false;
        }

        stop = false;
        final int chunk = min;
        thread = new Thread(new Runnable() {
            public void run() {
                byte[] b = new byte[chunk];
                while (!stop) {
                    int n = rec.read(b, 0, b.length);
                    if (n > 0) nativeMicData(b, n);
                    else if (n < 0) break;
                }
            }
        }, "tadpole-mic");
        rec.startRecording();
        thread.start();
        Log.i(TAG, "mic: recording " + rate + " Hz x" + channels
                   + ", buffer " + buf + " chunk " + chunk);
        return true;
    }

    private void close() {
        stop = true;
        try {
            if (rec != null) {
                rec.stop();
                rec.release();
            }
        } catch (Throwable t) {
            Log.e(TAG, "mic: release failed", t);
        }
        rec = null;
        if (thread != null) {
            try { thread.join(1000); } catch (InterruptedException e) { }
            thread = null;
        }
        Log.i(TAG, "mic: stopped");
    }

    /** Implemented in android/viewer/tadpole_mic_android.c. */
    private static native void nativeMicData(byte[] pcm, int len);
}
