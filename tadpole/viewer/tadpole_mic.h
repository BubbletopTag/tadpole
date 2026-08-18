/* Tadpole — host side of the LeapPad2's microphone. See tadpole_mic.c. */
#ifndef TADPOLE_MIC_H
#define TADPOLE_MIC_H

/* `dir` is TADPOLE_DIR. Creates the FIFO the guest will read from. */
void tad_mic_init(const char *dir);

/* Once per rendered frame. Starts the platform recorder when the guest opens
 * its end of the FIFO and stops it when the guest lets go. */
void tad_mic_pump(void);

/* One block of PCM in the negotiated format, from the backend's thread. */
void tad_mic_data(const void *pcm, int bytes);

/* Per platform; the generic ones are no-ops. */
void tad_mic_plat_start(int rate, int channels);
void tad_mic_plat_stop(void);

#endif
