/* Tadpole — feasibility probe for host-GPU replay (HLE).
 *
 * Answers the one question the whole HLE design rests on, before any of it gets
 * built: can the VIEWER, which already owns an SDL_Renderer on its window,
 * separately obtain a compatibility-profile GL context, draw with the FIXED
 * FUNCTION pipeline into an offscreen target, and read the pixels back for the
 * existing compositor?
 *
 * Fixed function matters because GLES 1.x has no shaders. If the host only
 * offered a core profile we would have to reimplement the whole fixed-function
 * pipeline in GLSL. On the Linux box `glxinfo` reports "4.6 (Compatibility
 * Profile)"; on Windows the operating system's fallback renderer is GDI
 * OpenGL 1.1 with no FBOs at all, so nothing here may be assumed — this checks
 * the claim rather than trusting it.
 *
 * Readback is the other question. Rendering on the GPU is pointless if
 * glReadPixels costs more than the 78 ms/frame the software rasteriser spends,
 * so the probe times it.
 *
 *   Linux:   cc -O2 -o hle_probe hle_probe.c $(pkg-config --cflags --libs sdl2 gl)
 *   Windows: gcc -O2 -o hle_probe.exe hle_probe.c $(pkg-config --cflags sdl2) \
 *                -lmingw32 -lSDL2main -lSDL2 -lopengl32
 *            (no -mwindows: the probe's output IS its product)
 */
/* Everything past OpenGL 1.1 is resolved at runtime through
 * SDL_GL_GetProcAddress. This is mandatory on Windows, where opengl32.dll
 * exports only 1.1 and the rest live behind wglGetProcAddress, and harmless on
 * Linux. It is also the honest probe: a resolution failure is a finding, not a
 * link error on the developer's machine. The PFN typedefs come from
 * SDL_opengl_glext.h, which SDL_opengl.h pulls in. */
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 480
#define H 272

static PFNGLGENFRAMEBUFFERSPROC            p_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC            p_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC       p_glFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC     p_glCheckFramebufferStatus;
static PFNGLBLITFRAMEBUFFERPROC            p_glBlitFramebuffer;
static PFNGLGENRENDERBUFFERSPROC           p_glGenRenderbuffers;
static PFNGLBINDRENDERBUFFERPROC           p_glBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC        p_glRenderbufferStorage;
static PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC p_glRenderbufferStorageMultisample;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC    p_glFramebufferRenderbuffer;
static PFNGLDELETERENDERBUFFERSPROC        p_glDeleteRenderbuffers;

static double now_ms(void)
{
	return (double)SDL_GetPerformanceCounter() * 1000.0
	     / (double)SDL_GetPerformanceFrequency();
}

int main(int argc, char **argv)
{
	SDL_Window *win;
	SDL_GLContext ctx;
	GLuint fbo = 0, colour = 0, depth = 0, tex = 0;
	unsigned char *px = malloc(W * H * 4);
	int i, ok = 1, compat = 1;

	(void)argc; (void)argv;
	setvbuf(stdout, NULL, _IONBF, 0);   /* a crash must not eat the output */
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		printf("FAIL SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	/* Compatibility profile: that is where glVertexPointer, glMatrixMode,
	 * glTexEnv, glAlphaFunc and the rest still exist. */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
	                    SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

	/* HIDDEN window: the real viewer keeps its SDL_Renderer on the visible
	 * one, so the replay context must live somewhere else entirely. */
	win = SDL_CreateWindow("tadpole hle probe", 0, 0, 64, 64,
	                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	if (!win) { printf("FAIL CreateWindow: %s\n", SDL_GetError()); return 1; }
	ctx = SDL_GL_CreateContext(win);
	if (!ctx) {
		/* Some drivers reject an explicit profile request outright.
		 * Retry with no profile attribute before concluding anything. */
		printf("  compatibility-profile request REFUSED: %s\n", SDL_GetError());
		compat = 0;
		SDL_GL_ResetAttributes();
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
		ctx = SDL_GL_CreateContext(win);
		if (!ctx) { printf("FAIL GL context: %s\n", SDL_GetError()); return 1; }
	}

	printf("  GL_VERSION  %s\n", (const char *)glGetString(GL_VERSION));
	printf("  GL_VENDOR   %s\n", (const char *)glGetString(GL_VENDOR));
	printf("  GL_RENDERER %s\n", (const char *)glGetString(GL_RENDERER));
	{
		const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
		printf("  GL_SHADING_LANGUAGE_VERSION %s\n", sl ? sl : "(none — pre-2.0)");
	}
	printf("  compatibility profile context: %s\n",
	       compat ? "obtained as requested" : "NOT honoured, driver default used");
	{
		GLint maxtex = 0, maxsamp = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
		glGetError();  /* GL_MAX_SAMPLES is 3.0+; swallow the error below it */
		glGetIntegerv(GL_MAX_SAMPLES, &maxsamp);
		if (glGetError() != GL_NO_ERROR) maxsamp = 0;
		printf("  GL_MAX_TEXTURE_SIZE %d   GL_MAX_SAMPLES %d\n", maxtex, maxsamp);
	}

	/* ---- fixed function actually present? ------------------------------- */
	{
		void *p1 = SDL_GL_GetProcAddress("glVertexPointer");
		void *p2 = SDL_GL_GetProcAddress("glMatrixMode");
		void *p3 = SDL_GL_GetProcAddress("glTexEnvi");
		void *p4 = SDL_GL_GetProcAddress("glAlphaFunc");
		printf("  fixed function: glVertexPointer=%s glMatrixMode=%s "
		       "glTexEnvi=%s glAlphaFunc=%s\n",
		       p1?"yes":"NO", p2?"yes":"NO", p3?"yes":"NO", p4?"yes":"NO");
		if (!p1 || !p2 || !p3 || !p4) ok = 0;
	}

	/* ---- FBO entry points actually resolvable? -------------------------- */
	p_glGenFramebuffers        = (PFNGLGENFRAMEBUFFERSPROC)
		SDL_GL_GetProcAddress("glGenFramebuffers");
	p_glBindFramebuffer        = (PFNGLBINDFRAMEBUFFERPROC)
		SDL_GL_GetProcAddress("glBindFramebuffer");
	p_glFramebufferTexture2D   = (PFNGLFRAMEBUFFERTEXTURE2DPROC)
		SDL_GL_GetProcAddress("glFramebufferTexture2D");
	p_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)
		SDL_GL_GetProcAddress("glCheckFramebufferStatus");
	p_glBlitFramebuffer        = (PFNGLBLITFRAMEBUFFERPROC)
		SDL_GL_GetProcAddress("glBlitFramebuffer");
	p_glGenRenderbuffers       = (PFNGLGENRENDERBUFFERSPROC)
		SDL_GL_GetProcAddress("glGenRenderbuffers");
	p_glBindRenderbuffer       = (PFNGLBINDRENDERBUFFERPROC)
		SDL_GL_GetProcAddress("glBindRenderbuffer");
	p_glRenderbufferStorage    = (PFNGLRENDERBUFFERSTORAGEPROC)
		SDL_GL_GetProcAddress("glRenderbufferStorage");
	p_glRenderbufferStorageMultisample = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)
		SDL_GL_GetProcAddress("glRenderbufferStorageMultisample");
	p_glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)
		SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
	p_glDeleteRenderbuffers    = (PFNGLDELETERENDERBUFFERSPROC)
		SDL_GL_GetProcAddress("glDeleteRenderbuffers");
	printf("  FBO entry points: glGenFramebuffers=%s glFramebufferTexture2D=%s "
	       "glBlitFramebuffer=%s glCheckFramebufferStatus=%s\n",
	       p_glGenFramebuffers?"yes":"NO", p_glFramebufferTexture2D?"yes":"NO",
	       p_glBlitFramebuffer?"yes":"NO", p_glCheckFramebufferStatus?"yes":"NO");
	if (!p_glGenFramebuffers || !p_glBindFramebuffer
	 || !p_glFramebufferTexture2D || !p_glCheckFramebufferStatus
	 || !p_glGenRenderbuffers || !p_glBindRenderbuffer
	 || !p_glRenderbufferStorage || !p_glFramebufferRenderbuffer) {
		printf("FAIL no framebuffer objects — GDI 1.1 fallback? Ship Mesa.\n");
		return 1;
	}

	/* ---- which MSAA sample counts actually complete an FBO? ------------- */
	if (p_glRenderbufferStorageMultisample) {
		static const int counts[] = { 2, 4, 8, 16 };
		unsigned n;
		printf("  MSAA renderbuffer counts that complete:");
		for (n = 0; n < sizeof counts / sizeof counts[0]; n++) {
			GLuint mfbo = 0, mrb = 0;
			p_glGenFramebuffers(1, &mfbo);
			p_glBindFramebuffer(GL_FRAMEBUFFER, mfbo);
			p_glGenRenderbuffers(1, &mrb);
			p_glBindRenderbuffer(GL_RENDERBUFFER, mrb);
			glGetError();
			p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, counts[n],
			                                   GL_RGBA8, W, H);
			p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                            GL_RENDERBUFFER, mrb);
			if (glGetError() == GL_NO_ERROR &&
			    p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
				printf(" %d", counts[n]);
			p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
			p_glDeleteRenderbuffers(1, &mrb);
		}
		printf("\n");
	} else {
		printf("  MSAA: glRenderbufferStorageMultisample NOT resolvable\n");
	}

	/* ---- offscreen target at the panel's size --------------------------- */
	p_glGenFramebuffers(1, &fbo);
	p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenTextures(1, &colour);
	glBindTexture(GL_TEXTURE_2D, colour);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, NULL);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                         GL_TEXTURE_2D, colour, 0);
	p_glGenRenderbuffers(1, &depth);
	p_glBindRenderbuffer(GL_RENDERBUFFER, depth);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	                            GL_RENDERBUFFER, depth);
	if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("FAIL incomplete FBO\n");
		return 1;
	}
	printf("  FBO %dx%d with 16-bit depth: complete\n", W, H);

	/* ---- a 2x2 texture, the way a title's upload would arrive ----------- */
	{
		static const unsigned char t[16] = {
			255,0,0,255,   0,255,0,255,
			0,0,255,255,   255,255,0,255 };
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
		             GL_UNSIGNED_BYTE, t);
	}

	/* ---- draw exactly like a GLES1 title: vertex arrays, no shaders ----- */
	static const unsigned short idx[] = { 0,1,2, 0,2,3 };
	{
		static const float verts[] = {
			 40.0f,  40.0f,   440.0f,  40.0f,
			440.0f, 232.0f,    40.0f, 232.0f };
		static const float uvs[] = {
			0.0f,0.0f,  1.0f,0.0f,  1.0f,1.0f,  0.0f,1.0f };

		glViewport(0, 0, W, H);
		glClearColor(0.05f, 0.12f, 0.08f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, W, H, 0, -1, 1);          /* GLES1 2D titles do exactly this */
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		glEnable(GL_TEXTURE_2D);
		glColor4f(1, 1, 1, 1);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, verts);
		glTexCoordPointer(2, GL_FLOAT, 0, uvs);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
		glFinish();
	}

	/* ---- readback cost: the whole design hinges on this being cheap ----- */
	{
		double t0, t1;
		int n = 60;
		glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, px);  /* warm */
		t0 = now_ms();
		for (i = 0; i < n; i++)
			glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, px);
		t1 = now_ms();
		printf("  glReadPixels %dx%d BGRA: %.3f ms/frame  (software raster is"
		       " 78 ms)\n", W, H, (t1 - t0) / n);
	}

	/* ---- did it actually draw? ------------------------------------------ */
	{
		int centre = ((H/2) * W + (W/2)) * 4;
		int corner = (4 * W + 4) * 4;
		printf("  centre pixel BGRA %3d,%3d,%3d,%3d  (expect a texture colour)\n",
		       px[centre], px[centre+1], px[centre+2], px[centre+3]);
		printf("  corner pixel BGRA %3d,%3d,%3d,%3d  (expect the clear colour)\n",
		       px[corner], px[corner+1], px[corner+2], px[corner+3]);
		if (px[centre] == px[corner] && px[centre+1] == px[corner+1])
			{ printf("FAIL centre and corner identical — nothing drew\n"); ok = 0; }
	}

	/* ---- full-frame draw+readback rate --------------------------------- */
	{
		double t0 = now_ms();
		int n = 120;
		/* Re-use the real index array: passing NULL here with no element
		 * buffer bound is a null dereference inside the driver, not a
		 * "draw nothing". */
		for (i = 0; i < n; i++) {
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
			glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, px);
		}
		t0 = now_ms() - t0;
		printf("  clear+draw+readback: %.3f ms/frame -> %.0f fps ceiling\n",
		       t0 / n, 1000.0 / (t0 / n));
	}

	/* ---- a REALISTIC frame ----------------------------------------------
	 *
	 * Two triangles says nothing useful about a real title. The Clam Prix menu
	 * was measured at 28 draw calls and ~93000 painted pixels per frame, so
	 * match the DRAW COUNT exactly and deliberately overshoot the pixels: 28
	 * half-window quads is ~914000 pixels, roughly 10x a real frame. The result
	 * is therefore a conservative bound, not a like-for-like replica.
	 *
	 * Per-DRAW-CALL cost is what matters under HLE. The whole point of moving
	 * rasterisation to the host is trading O(pixels) work in the guest for
	 * O(calls) work, so the number to know is what 28 calls actually cost.
	 */
	{
		float quad[8];
		static const float uvs2[] = { 0,0, 1,0, 1,1, 0,1 };
		double t0;
		int n = 60, k;

		const long px_per_frame = 28L * (W / 2) * (H / 2);
		glTexCoordPointer(2, GL_FLOAT, 0, uvs2);
		t0 = now_ms();
		for (i = 0; i < n; i++) {
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			for (k = 0; k < 28; k++) {
				/* Half-window quads: 28 of them is ~2x overdraw, matching
				 * the measured 93000 pixels into a 76800-pixel window. */
				float x0f = (float)((k * 37) % (W / 2));
				float y0f = (float)((k * 53) % (H / 2));
				quad[0] = x0f;             quad[1] = y0f;
				quad[2] = x0f + W / 2.0f;  quad[3] = y0f;
				quad[4] = x0f + W / 2.0f;  quad[5] = y0f + H / 2.0f;
				quad[6] = x0f;             quad[7] = y0f + H / 2.0f;
				glVertexPointer(2, GL_FLOAT, 0, quad);
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
			}
			glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, px);
		}
		t0 = now_ms() - t0;
		printf("  FRAME-SHAPED load: 28 draws, %ld px (~10x a real frame),"
		       " + readback\n", px_per_frame);
		printf("    %.3f ms/frame -> %.0f fps if the guest were free\n",
		       t0 / n, 1000.0 / (t0 / n));
		printf("    throughput %.0f Mpx/s   (software rasteriser: 1.07 Mpx/s)\n",
		       px_per_frame / (t0 / n) / 1000.0);
		printf("    a real 93000 px frame would cost about %.3f ms host-side\n",
		       (t0 / n) * 93194.0 / (double)px_per_frame);
	}

	printf("%s\n", ok ? "PASS host-GPU replay is viable"
	                  : "FAIL see above");
	free(px);
	SDL_GL_DeleteContext(ctx);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return ok ? 0 : 1;
}
