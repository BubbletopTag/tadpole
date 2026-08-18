/* Tadpole — the camera control block, shared between the guest shim, the
 * viewer and the Android capture backend.
 *
 * It lives inside struct tadpole_state (state.bin), which every side already
 * maps. The PIXELS do not: they travel in $TADPOLE_DIR/cam0.bin and cam1.bin,
 * the same "plain host file, mmapped by both ends" arrangement the framebuffer
 * uses, pointing the other way — the host writes and the guest reads.
 *
 * LAYOUT OF camN.bin — DERIVED FROM slot_size, NEVER HARDCODED
 *
 *     [0]                            staging slot 0
 *     [slot_size]                    staging slot 1
 *     [2*slot_size + i*slot_size]    V4L2 buffer i
 *
 * slot_size is the negotiated frame size rounded up to a page, published here
 * by the shim when the guest calls S_FMT. It has to be dynamic because the
 * frame size is: CameraWidget offers 320x240 through 1600x900 and the guest
 * asks for planar YUV420, so one frame is anything from 115 KB to 2.1 MB.
 *
 * TWO STAGING SLOTS, NOT ONE, and that is the whole locking story. The host
 * writes the slot the guest is NOT reading — (seq+1)&1 — fills in bytes[] for
 * it, and only then bumps seq. A reader that latches seq first therefore has a
 * slot nobody is writing. One shared word would have needed a lock between a
 * root chroot process and an Android app; this needs none.
 *
 * The V4L2 buffers are separate from the staging slots because the guest mmaps
 * them and keeps the mappings for the life of the stream: DQBUF copies the
 * staged frame into whichever buffer the guest queued.
 */
#ifndef TADPOLE_CAM_H
#define TADPOLE_CAM_H

/* /dev/video0 and /dev/video1 — libCameraVIP.so and libCameraUSB.so both carry
 * exactly these two strings and CameraWidget's "switch camera" button moves
 * between them. There is no V4L2 input index involved. */
#define TAD_CAM_N       2
#define TAD_CAM_BUFMAX  8              /* most REQBUFS we will honour */
#define TAD_CAM_MAXFRAME (8u*1024*1024) /* refuse anything sillier than this */

/* HOW MUCH OF THE ARENA THE CAPTURE BUFFER MAY HAVE.
 *
 * The VIP capture buffers live at video memory offset 0 — the guest computes
 * their addresses as fb2_mapping + width*i and never reads QUERYBUF's offset —
 * and in this emulator all three framebuffers share one arena, so that is
 * arena offset 0. Brio's own allocator hands out its first surface at 0xFF000,
 * two full 480x272x32 screens in, and everything on screen is above that. So
 * this is the whole budget for one captured frame, and writing past it means
 * writing over the picture. */
#define TAD_CAM_HEADROOM 0xFF000u

/* The fourccs this pair understands. The guest picks: CVIPCameraModule's
 * built-in default mode is YUV420 and that is what S_FMT actually asks for. */
#define TAD_FOURCC_YU12  0x32315559u   /* 'Y','U','1','2' — planar I420 */
#define TAD_FOURCC_YUYV  0x56595559u
#define TAD_FOURCC_422P  0x50323234u
#define TAD_FOURCC_MJPG  0x47504A4Du

struct tad_cam_state {
	unsigned int open;        /* guest holds the node open                  */
	unsigned int streaming;   /* guest called STREAMON                      */
	unsigned int width;       /* what S_FMT settled on                      */
	unsigned int height;
	unsigned int pixfmt;      /* fourcc the guest expects us to produce     */
	unsigned int sizeimage;   /* exact bytes in one frame of that fourcc    */
	unsigned int slot_size;   /* staging slot stride in camN.bin, paged     */
	unsigned int fps;         /* frames per second the guest asked for      */
	unsigned int want;        /* bumped whenever the guest waits in DQBUF   */
	unsigned int seq;         /* HOST bumps this after staging a frame      */
	unsigned int bytes[2];    /* staged length, per slot                    */
	unsigned int host;        /* host backend has a camera bound            */
	unsigned int host_err;    /* non-zero: host could not open the camera   */

	/* THE VIEWFINDER, WHICH THE HOST HAS TO DRAW.
	 *
	 * On the VIP path the preview is not a stream the guest reads: it points
	 * VIDIOC_S_FBUF at a display surface, turns on VIDIOC_OVERLAY, and on real
	 * hardware the capture block then DMAs into the MLC's video plane while
	 * the application does nothing at all. Measured: with the viewfinder up,
	 * AppManager is at 3% CPU and state.bin's vsync_count does not move — the
	 * guest issues no framebuffer ioctl of any kind, so there is no clock on
	 * the guest side to hang a preview off. The host has one.
	 *
	 * These five fields are where to draw and in what shape; the shim fills
	 * them in from the S_FBUF the guest issued, translating its physical base
	 * into an offset in the arena both sides map.
	 */
	unsigned int ov_on;       /* overlay enabled                            */
	unsigned int ov_off;      /* byte offset of the surface inside fb0.bin  */
	unsigned int ov_w, ov_h;  /* surface size                               */
	unsigned int ov_pitch;    /* its bytesperline: the panel pitch          */

	/* Counters, because "the viewfinder is green" has several possible causes
	 * and they are indistinguishable from the outside. */
	unsigned int n_qbuf;      /* QBUFs the guest issued                     */
	unsigned int n_dqbuf;     /* DQBUFs the guest issued                    */
	unsigned int n_frames;    /* frames actually handed over                */
	/* Padded to 32 words so the block is a round 128 bytes: fbshot.py and
	 * anything else that walks state.bin by offset does simpler arithmetic. */
	unsigned int pitch;       /* the capture pitch actually used            */
	unsigned int polled;      /* the recorder's PollFrame loop has been seen*/
	unsigned int reserved[8];
};

#endif
