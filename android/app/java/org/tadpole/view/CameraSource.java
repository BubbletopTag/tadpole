package org.tadpole.view;

import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.util.List;

/**
 * The tablet's cameras, for the LeapPad2's.
 *
 * <p>WHY THE OLD API. The first version of this was the NDK's camera2
 * ({@code libcamera2ndk}), which would have kept the frames in C and needed no
 * Java at all. It returns an empty camera list on this device, and the reason
 * is not a bug in it:
 *
 * <pre>
 *   $ adb shell dumpsys media.camera | grep 'Camera HAL device'
 *   == Camera HAL device device@1.0/internal/0 (v1.0) static information: ==
 *   == Camera HAL device device@1.0/internal/1 (v1.0) static information: ==
 * </pre>
 *
 * <p>Both cameras are HAL 1.0, which the platform presents to camera2 as
 * hardware level LEGACY — and {@code ACameraManager_getCameraIdList} filters
 * LEGACY devices out entirely, because the NDK API is defined against HAL3.
 * The Java {@code android.hardware.Camera} API talks to HAL1 directly, so it
 * is not a fallback here, it is the only route.
 *
 * <p>It also hands over exactly what is wanted: {@code setPreviewCallbackWith
 * Buffer} delivers NV21 into a buffer we own and re-queue, so there is no
 * allocation per frame and no copy before the one that matters.
 *
 * <p>ON ITS OWN THREAD. Camera1 delivers its callbacks to the Looper of the
 * thread that opened the device; opening it from the SDL render thread, which
 * has no Looper, would send every frame to the main thread instead and put a
 * 640x480 colour conversion in front of the UI.
 */
final class CameraSource {
    private static final String TAG = "tadpole";
    private static final CameraSource[] SRC = new CameraSource[2];

    private final int idx;
    private HandlerThread thread;
    private Handler handler;
    private Camera cam;
    private SurfaceTexture tex;
    private int rot;
    private boolean mirror;

    private CameraSource(int idx) { this.idx = idx; }

    /* BOTH OF THESE BLOCK UNTIL THEY HAVE ACTUALLY HAPPENED, and that is not
     * fussiness. The camera work has to run on a thread with a Looper, so the
     * first version posted it and returned — which broke the switch button
     * twice over. start() said "started" while the open was still queued, so
     * the C side believed a camera was running that had in fact thrown; and
     * stop() returned before the release, so the next open raced it and got
     *
     *     java.lang.RuntimeException: Fail to connect to camera service
     *
     * because these are HAL 1.0 devices and only one may be held at a time.
     * The caller is the viewer's render thread and this is a button press, so
     * a frame's worth of stall is the right trade. */
    static synchronized boolean start(int idx, int w, int h) {
        if (idx < 0 || idx >= SRC.length) return false;
        stop(idx);
        CameraSource s = new CameraSource(idx);
        SRC[idx] = s;
        return s.open(w, h);
    }

    static synchronized void stop(int idx) {
        if (idx < 0 || idx >= SRC.length) return;
        CameraSource s = SRC[idx];
        SRC[idx] = null;
        if (s != null) s.close();
    }

    /** How many cameras the tablet has. */
    static int count() {
        try { return Camera.getNumberOfCameras(); }
        catch (Throwable t) { return 0; }
    }

    /* video0 is the LeapPad2's rear camera and video1 its front one, which is
     * how CameraWidget's switch button is wired; map them the same way here. */
    private static int deviceFor(int idx) {
        int want = (idx == 0) ? Camera.CameraInfo.CAMERA_FACING_BACK
                              : Camera.CameraInfo.CAMERA_FACING_FRONT;
        Camera.CameraInfo info = new Camera.CameraInfo();
        int n = count();
        for (int i = 0; i < n; i++) {
            Camera.getCameraInfo(i, info);
            if (info.facing == want) return i;
        }
        /* One camera only: use it for both, rather than leaving the switch
         * button showing a dead viewfinder. */
        return n > 0 ? 0 : -1;
    }

    private final Object done = new Object();
    private boolean finished;

    private void signal() {
        synchronized (done) { finished = true; done.notifyAll(); }
    }

    private void await(long ms) {
        long end = System.currentTimeMillis() + ms;
        synchronized (done) {
            while (!finished) {
                long left = end - System.currentTimeMillis();
                if (left <= 0) return;
                try { done.wait(left); } catch (InterruptedException e) { return; }
            }
        }
    }

    private boolean open(final int w, final int h) {
        final int dev = deviceFor(idx);
        if (dev < 0) {
            Log.e(TAG, "camera " + idx + ": this tablet has no camera");
            return false;
        }
        thread = new HandlerThread("tadpole-cam" + idx);
        thread.start();
        handler = new Handler(thread.getLooper());
        finished = false;
        handler.post(new Runnable() {
            public void run() { openOn(dev, w, h); signal(); }
        });
        await(4000);
        return cam != null;
    }

    private void openOn(int dev, int w, int h) {
        try {
            Camera.CameraInfo info = new Camera.CameraInfo();
            Camera.getCameraInfo(dev, info);

            /* THE LEAPPAD'S CAMERA APP IS LANDSCAPE, so the frame has to be
             * upright for a device held on its side — 90 degrees from this
             * tablet's natural portrait. That is the standard Camera1 display
             * formula with degrees = 90, and the front camera is mirrored on
             * top of it, which is what every camera app does and what anyone
             * looking at their own face expects. */
            final int degrees = 90;
            if (info.facing == Camera.CameraInfo.CAMERA_FACING_FRONT) {
                rot = (360 - ((info.orientation + degrees) % 360)) % 360;
                mirror = true;
            } else {
                rot = (info.orientation - degrees + 360) % 360;
                mirror = false;
            }

            cam = Camera.open(dev);
            Camera.Parameters p = cam.getParameters();
            int[] size = pickSize(p.getSupportedPreviewSizes(), w, h);
            p.setPreviewSize(size[0], size[1]);
            p.setPreviewFormat(ImageFormat.NV21);
            List<int[]> fps = p.getSupportedPreviewFpsRange();
            if (fps != null && !fps.isEmpty()) {
                int[] best = fps.get(0);
                for (int[] r : fps) if (r[1] >= 15000 && r[1] <= best[1]) best = r;
                p.setPreviewFpsRange(best[0], best[1]);
            }
            cam.setParameters(p);

            /* A preview target is mandatory even when the frames come back
             * through the callback: without one, startPreview() throws. The
             * SurfaceTexture is never drawn and never has updateTexImage()
             * called on it, so the texture name it is given does not have to
             * exist. */
            tex = new SurfaceTexture(0);
            cam.setPreviewTexture(tex);

            int bytes = size[0] * size[1] * ImageFormat.getBitsPerPixel(
                            ImageFormat.NV21) / 8;
            final int sw = size[0], sh = size[1];
            cam.setPreviewCallbackWithBuffer(new Camera.PreviewCallback() {
                public void onPreviewFrame(byte[] data, Camera c) {
                    if (data != null)
                        nativeFrame(idx, data, sw, sh, rot, mirror);
                    if (c != null) c.addCallbackBuffer(data);
                }
            });
            for (int i = 0; i < 3; i++) cam.addCallbackBuffer(new byte[bytes]);
            cam.startPreview();
            Log.i(TAG, "camera " + idx + ": device " + dev + " facing "
                       + info.facing + " orientation " + info.orientation
                       + " -> " + sw + "x" + sh + " NV21, rotate " + rot
                       + (mirror ? " mirrored" : ""));
        } catch (Throwable t) {
            Log.e(TAG, "camera " + idx + ": could not start", t);
            closeOn();
        }
    }

    private static int[] pickSize(List<Camera.Size> sizes, int w, int h) {
        if (sizes == null || sizes.isEmpty()) return new int[] { 640, 480 };
        Camera.Size best = null;
        long bestErr = Long.MAX_VALUE;
        for (Camera.Size s : sizes) {
            /* Nearest by area, preferring one at least as big as asked so the
             * frame is downscaled rather than blown up. */
            long err = Math.abs((long) s.width * s.height - (long) w * h);
            if (s.width < w || s.height < h) err += 1L << 40;
            if (err < bestErr) { bestErr = err; best = s; }
        }
        return new int[] { best.width, best.height };
    }

    private void close() {
        if (handler != null) {
            finished = false;
            handler.post(new Runnable() {
                public void run() { closeOn(); signal(); }
            });
            await(3000);
        }
        if (thread != null) {
            thread.quitSafely();
            try { thread.join(1000); } catch (InterruptedException e) { }
            thread = null;
        }
        handler = null;
    }

    private void closeOn() {
        try {
            if (cam != null) {
                cam.setPreviewCallbackWithBuffer(null);
                cam.stopPreview();
                cam.release();
            }
        } catch (Throwable t) {
            Log.e(TAG, "camera " + idx + ": release failed", t);
        }
        cam = null;
        if (tex != null) { tex.release(); tex = null; }
        Log.i(TAG, "camera " + idx + ": stopped");
    }

    /** Implemented in android/viewer/tadpole_cam_android.c. */
    private static native void nativeFrame(int idx, byte[] nv21, int w, int h,
                                           int rot, boolean mirror);
}
