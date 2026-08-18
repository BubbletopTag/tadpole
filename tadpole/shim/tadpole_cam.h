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
	unsigned int slot_size;   /* staging slot / V4L2 buffer stride, paged   */
	unsigned int fps;         /* frames per second the guest asked for      */
	unsigned int want;        /* bumped whenever the guest waits in DQBUF   */
	unsigned int seq;         /* HOST bumps this after staging a frame      */
	unsigned int bytes[2];    /* staged length, per slot                    */
	unsigned int host;        /* host backend has a camera bound            */
	unsigned int host_err;    /* non-zero: host could not open the camera   */
	/* Counters, because "the viewfinder is green" has four possible causes
	 * and they are indistinguishable from the outside: the guest never
	 * queued a buffer, never dequeued one, dequeued and got nothing, or got
	 * a frame that was wrong. One read of state.bin separates them. */
	unsigned int n_qbuf;      /* QBUFs the guest issued                     */
	unsigned int n_dqbuf;     /* DQBUFs the guest issued                    */
	unsigned int n_frames;    /* frames actually handed over                */
	unsigned int reserved[3];
};

#endif
