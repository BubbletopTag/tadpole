/* Tadpole — GLES 2.0 on the guest side: shaders, programs, uniforms and
 * generic vertex attributes.
 *
 * WHY THIS EXISTS. The LeapTV's whole system UI — GlasgowUI, OutOfBox,
 * ParentSettings, VideoWidget, ErrorWidget — is a GLES2 program with fourteen
 * small ESSL 1.00 shaders, and its titles link libGLESv2 too. Everything
 * before it was fixed-function GLES 1.1, which is what tadpole_gles_core.c
 * implements; this file adds the programmable half beside it, in the same
 * library, so the one object that answers to libGLESv2.so (see the Makefile's
 * shimegl-links) carries both APIs.
 *
 * NOTHING IS RASTERISED HERE. There is no software path for shaders and none is
 * planned: with a program in use the core's draw entry points hand the draw to
 * tad_g2_send_attribs() and the encoder, and the host replays it against real
 * GL with real shaders (viewer/tadpole_hle.c). Without a host attached, a
 * shader draw is dropped.
 *
 * THE ONE HARD PROBLEM IS THAT THERE IS NO REPLY CHANNEL. glGetUniformLocation
 * and glGetAttribLocation return numbers the host would normally choose at link
 * time, and the guest cannot ask it. So the guest CHOOSES THEM ITSELF: it reads
 * the `attribute` and `uniform` declarations out of the shader source, numbers
 * them in declaration order, and tells the host the name behind each number —
 * attributes with glBindAttribLocation before the link (so the host's linker is
 * made to agree), uniforms by name after it (so the host can look up its own
 * location for each of ours). From then on both sides speak in our numbers.
 * The parser is small because ESSL 1.00 is small: no blocks, no layout
 * qualifiers, no structs worth the trouble here.
 *
 * Built like the core: no libc headers, its own declarations, nothing that
 * assumes anything about the host.
 */
#include "tadpole_gles_debug.h"

typedef unsigned int   u32;
typedef int            i32;
typedef unsigned char  u8;
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef char           GLchar;
typedef unsigned int   GLbitfield;

#define NULL ((void *)0)

extern void *malloc(u32 n);
extern void  free(void *p);
extern void *memcpy(void *d, const void *s, u32 n);
extern void *memset(void *d, int c, u32 n);
extern int   snprintf(char *s, u32 n, const char *fmt, ...);

/* From tadpole_gles_core.c. */
extern int  tad_core_hle_ready(void);
extern u32  tad_core_bound_array(void);

/* From tadpole_gles_hle.c. */
extern void hle_shadersource(u32 shader, u32 type, const char *src, u32 len);
extern void hle_deleteshader(u32 shader);
extern void hle_attachshader(u32 program, u32 shader);
extern void hle_detachshader(u32 program, u32 shader);
extern void hle_bindattrib(u32 program, u32 index, const char *name, u32 len);
extern void hle_linkprogram(u32 program);
extern void hle_uniformloc(u32 program, u32 loc, const char *name, u32 len);
extern void hle_useprogram(u32 program);
extern void hle_deleteprogram(u32 program);
extern void hle_uniform(u32 loc, u32 kind, u32 count, const void *data, u32 words);
extern void hle_attribpointer(u32 index, u32 buf, i32 size, u32 type, u32 norm,
                              i32 stride, u32 off);
extern void hle_attribenable(u32 index, u32 on);
extern void hle_attribvalue(u32 index, const float *xyzw);
extern void hle_bufferdata(u32 name, u32 size, const void *data);

#define GL_NO_ERROR_          0
#define TAD_INVALID_ENUM      0x0500
#define TAD_INVALID_VALUE     0x0501
#define TAD_INVALID_OPERATION 0x0502

#define GL_VERTEX_SHADER   0x8B31
#define GL_FRAGMENT_SHADER 0x8B30

#define GL_BYTE_           0x1400
#define GL_UNSIGNED_BYTE_  0x1401
#define GL_SHORT_          0x1402
#define GL_UNSIGNED_SHORT_ 0x1403
#define GL_FLOAT_          0x1406
#define GL_FIXED_          0x140C

/* GLSL type enums, for glGetActiveAttrib/Uniform. */
#define T_FLOAT 0x1406
#define T_VEC2  0x8B50
#define T_VEC3  0x8B51
#define T_VEC4  0x8B52
#define T_INT   0x1404
#define T_IVEC2 0x8B53
#define T_IVEC3 0x8B54
#define T_IVEC4 0x8B55
#define T_BOOL  0x8B56
#define T_BVEC2 0x8B57
#define T_BVEC3 0x8B58
#define T_BVEC4 0x8B59
#define T_MAT2  0x8B5A
#define T_MAT3  0x8B5B
#define T_MAT4  0x8B5C
#define T_SAMPLER2D   0x8B5E
#define T_SAMPLERCUBE 0x8B60

static void tr2(const char *msg, int a, int b)   { tad_gl_trace(msg, a, b); }
static void warn2(const char *msg, int a, int b) { tad_gl_warn(msg, a, b); }

/* ---- tiny string helpers: the shim has no string.h ---------------------- */

static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int is_id(char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* ---- tables ------------------------------------------------------------- */

#define G2_MAX_SHADERS  256
#define G2_MAX_PROGRAMS 64
#define G2_MAX_ATTRIBS  16
#define G2_MAX_VARS     64
#define G2_NAME         64

struct g2_shader { u32 name; u32 type; char *src; u32 len; };

struct g2_attr_decl { char name[G2_NAME]; u32 index; u32 gltype; u32 slots; };

/* One uniform. `loc` is OUR base location; an array of `count` elements owns
 * loc .. loc+count-1. `vals` caches whatever the title last set, so a host that
 * (re)attaches mid-session can be brought up to date — see tad_g2_resync(). */
struct g2_uniform {
	char name[G2_NAME];
	u32  loc, count, gltype;
	u32  kind, ucount, nwords;    /* last glUniform* call, as sent */
	u32 *vals;
};

struct g2_program {
	u32 name, vs, fs;
	int linked;
	struct g2_attr_decl attrs[G2_MAX_ATTRIBS];  u32 nattrs;
	struct g2_attr_decl bound[G2_MAX_ATTRIBS];  u32 nbound;   /* pre-link requests */
	struct g2_uniform   unis[G2_MAX_VARS];      u32 nunis, nlocs;
};

struct g2_attr {
	int on;
	const u8 *ptr; u32 buf; i32 size; u32 type; u32 norm; i32 stride;
	float value[4]; int value_set;
};

static struct g2_shader  g_sh[G2_MAX_SHADERS];
static struct g2_program g_pr[G2_MAX_PROGRAMS];
static struct g2_attr    g_attr[G2_MAX_ATTRIBS];
static u32 g_cur;                  /* program in use, 0 = fixed function */

/* Staging buffer names for client-side attribute arrays: one per attribute,
 * above the core's HLE_CLIENT_* (4000..4005) and below the host's MAX_BUF. */
#define G2_CLIENT_BASE 4016u

static struct g2_shader *sh_find(u32 n)
{ if (!n || n > G2_MAX_SHADERS) return NULL; return g_sh[n-1].name ? &g_sh[n-1] : NULL; }
static struct g2_program *pr_find(u32 n)
{ if (!n || n > G2_MAX_PROGRAMS) return NULL; return g_pr[n-1].name ? &g_pr[n-1] : NULL; }

static int hle(void) { return tad_core_hle_ready(); }

/* ---- shader source parsing ---------------------------------------------- */

/* A copy with comments blanked to spaces (newlines kept, so #define lines
 * still end where they did). */
static char *strip_comments(const char *s, u32 n)
{
	char *o = malloc(n + 1);
	u32 i = 0, j = 0;
	if (!o) return NULL;
	while (i < n) {
		if (s[i] == '/' && i + 1 < n && s[i+1] == '/') {
			while (i < n && s[i] != '\n') { o[j++] = ' '; i++; }
		} else if (s[i] == '/' && i + 1 < n && s[i+1] == '*') {
			i += 2; o[j++] = ' '; o[j++] = ' ';
			while (i < n && !(s[i] == '*' && i + 1 < n && s[i+1] == '/')) {
				o[j++] = (s[i] == '\n') ? '\n' : ' '; i++;
			}
			if (i < n) { i += 2; o[j++] = ' '; o[j++] = ' '; }
		} else o[j++] = s[i++];
	}
	o[j] = 0;
	return o;
}

static u32 type_enum(const char *t)
{
	if (seq(t, "float")) return T_FLOAT;
	if (seq(t, "vec2")) return T_VEC2;   if (seq(t, "vec3")) return T_VEC3;
	if (seq(t, "vec4")) return T_VEC4;   if (seq(t, "int")) return T_INT;
	if (seq(t, "ivec2")) return T_IVEC2; if (seq(t, "ivec3")) return T_IVEC3;
	if (seq(t, "ivec4")) return T_IVEC4; if (seq(t, "bool")) return T_BOOL;
	if (seq(t, "bvec2")) return T_BVEC2; if (seq(t, "bvec3")) return T_BVEC3;
	if (seq(t, "bvec4")) return T_BVEC4; if (seq(t, "mat2")) return T_MAT2;
	if (seq(t, "mat3")) return T_MAT3;   if (seq(t, "mat4")) return T_MAT4;
	if (seq(t, "sampler2D")) return T_SAMPLER2D;
	if (seq(t, "samplerCube")) return T_SAMPLERCUBE;
	return T_FLOAT;
}

/* Resolve an array size: a literal, or a #define'd one. Anything else is 1. */
static u32 array_size(const char *src, const char *tok, u32 toklen)
{
	u32 v = 0, i;
	if (tok[0] >= '0' && tok[0] <= '9') {
		for (i = 0; i < toklen && tok[i] >= '0' && tok[i] <= '9'; i++)
			v = v * 10 + (u32)(tok[i] - '0');
		return v ? v : 1;
	}
	{
		const char *p = src;
		while (*p) {
			if (*p == '#') {
				const char *q = p + 1;
				while (is_ws(*q) && *q != '\n') q++;
				if (q[0]=='d'&&q[1]=='e'&&q[2]=='f'&&q[3]=='i'&&q[4]=='n'&&q[5]=='e'&&is_ws(q[6])) {
					q += 6; while (is_ws(*q) && *q != '\n') q++;
					if (slen(q) >= toklen) {
						for (i = 0; i < toklen && q[i] == tok[i]; i++) ;
						if (i == toklen && !is_id(q[toklen])) {
							q += toklen; while (is_ws(*q) && *q != '\n') q++;
							v = 0;
							while (*q >= '0' && *q <= '9') v = v * 10 + (u32)(*q++ - '0');
							return v ? v : 1;
						}
					}
				}
			}
			while (*p && *p != '\n') p++;
			if (*p) p++;
		}
	}
	return 1;
}

/* Walk every `attribute`/`uniform` declaration at statement level and hand each
 * declarator to cb(name, gltype, count). */
typedef void (*decl_cb)(struct g2_program *pr, const char *name, u32 gltype,
                        u32 count);

static void scan_decls(struct g2_program *pr, const char *raw, u32 rawlen,
                       const char *kw, decl_cb cb)
{
	char *src = strip_comments(raw, rawlen);
	u32 kwlen = slen(kw);
	const char *p;
	if (!src) return;
	for (p = src; *p; p++) {
		const char *q, *prev;
		char tok[G2_NAME];
		u32 gltype, tl;
		/* whole word */
		if (*p != kw[0]) continue;
		for (tl = 0; tl < kwlen && p[tl] == kw[tl]; tl++) ;
		if (tl != kwlen || is_id(p[kwlen])) continue;
		if (p != src && is_id(p[-1])) continue;
		/* at statement level: preceded by ; { } or the start of the file */
		prev = p;
		while (prev > src && is_ws(prev[-1])) prev--;
		if (prev != src && prev[-1] != ';' && prev[-1] != '{' && prev[-1] != '}')
			continue;
		q = p + kwlen;
		while (is_ws(*q)) q++;
		/* optional precision qualifier */
		for (;;) {
			for (tl = 0; is_id(q[tl]) && tl < G2_NAME - 1; tl++) tok[tl] = q[tl];
			tok[tl] = 0;
			if (seq(tok, "lowp") || seq(tok, "mediump") || seq(tok, "highp") ||
			    seq(tok, "invariant")) {
				q += tl; while (is_ws(*q)) q++;
				continue;
			}
			break;
		}
		if (!tl) continue;
		gltype = type_enum(tok);
		q += tl;
		/* declarators */
		for (;;) {
			u32 count = 1;
			while (is_ws(*q)) q++;
			for (tl = 0; is_id(q[tl]) && tl < G2_NAME - 1; tl++) tok[tl] = q[tl];
			tok[tl] = 0;
			if (!tl) break;
			q += tl;
			while (is_ws(*q)) q++;
			if (*q == '[') {
				const char *a;
				u32 al;
				q++; while (is_ws(*q)) q++;
				a = q; for (al = 0; is_id(q[al]); al++) ;
				count = array_size(src, a, al);
				q += al; while (is_ws(*q)) q++;
				if (*q == ']') q++;
				while (is_ws(*q)) q++;
			}
			cb(pr, tok, gltype, count);
			if (*q == ',') { q++; continue; }
			break;
		}
		p = q;
		if (!*p) break;
	}
	free(src);
}

static void add_attr(struct g2_program *pr, const char *name, u32 gltype, u32 count)
{
	u32 i, slots;
	(void)count;
	for (i = 0; i < pr->nattrs; i++)
		if (seq(pr->attrs[i].name, name)) return;
	if (pr->nattrs >= G2_MAX_ATTRIBS) return;
	slots = (gltype == T_MAT2) ? 2 : (gltype == T_MAT3) ? 3 : (gltype == T_MAT4) ? 4 : 1;
	snprintf(pr->attrs[pr->nattrs].name, G2_NAME, "%s", name);
	pr->attrs[pr->nattrs].gltype = gltype;
	pr->attrs[pr->nattrs].slots = slots;
	pr->attrs[pr->nattrs].index = 0xFFFFFFFFu;
	pr->nattrs++;
}

static void add_uniform(struct g2_program *pr, const char *name, u32 gltype, u32 count)
{
	u32 i;
	struct g2_uniform *u;
	for (i = 0; i < pr->nunis; i++)
		if (seq(pr->unis[i].name, name)) return;
	if (pr->nunis >= G2_MAX_VARS) return;
	u = &pr->unis[pr->nunis++];
	memset(u, 0, sizeof *u);
	snprintf(u->name, G2_NAME, "%s", name);
	u->gltype = gltype;
	u->count = count;
	u->loc = pr->nlocs;
	pr->nlocs += count;
}

/* Number every attribute: explicit glBindAttribLocation requests first, then
 * the rest in declaration order into the lowest free slots. */
static void assign_attrs(struct g2_program *pr)
{
	u32 used = 0, i, j;
	for (i = 0; i < pr->nattrs; i++) {
		pr->attrs[i].index = 0xFFFFFFFFu;
		for (j = 0; j < pr->nbound; j++)
			if (seq(pr->bound[j].name, pr->attrs[i].name)) {
				u32 k;
				pr->attrs[i].index = pr->bound[j].index;
				for (k = 0; k < pr->attrs[i].slots; k++)
					used |= 1u << (pr->bound[j].index + k);
				break;
			}
	}
	for (i = 0; i < pr->nattrs; i++) {
		u32 idx;
		if (pr->attrs[i].index != 0xFFFFFFFFu) continue;
		for (idx = 0; idx + pr->attrs[i].slots <= G2_MAX_ATTRIBS; idx++) {
			u32 k, ok = 1;
			for (k = 0; k < pr->attrs[i].slots; k++)
				if (used & (1u << (idx + k))) ok = 0;
			if (ok) break;
		}
		if (idx + pr->attrs[i].slots > G2_MAX_ATTRIBS) idx = 0;
		pr->attrs[i].index = idx;
		for (j = 0; j < pr->attrs[i].slots; j++) used |= 1u << (idx + j);
	}
}

/* Send the host what it needs to link this program the way we numbered it:
 * every attribute binding, the link itself, then every uniform's name. */
static void send_link(struct g2_program *pr)
{
	u32 i, k;
	char nm[G2_NAME + 16];
	for (i = 0; i < pr->nattrs; i++)
		hle_bindattrib(pr->name, pr->attrs[i].index, pr->attrs[i].name,
		               slen(pr->attrs[i].name));
	hle_linkprogram(pr->name);
	for (i = 0; i < pr->nunis; i++) {
		struct g2_uniform *u = &pr->unis[i];
		if (u->count == 1) {
			hle_uniformloc(pr->name, u->loc, u->name, slen(u->name));
			continue;
		}
		for (k = 0; k < u->count; k++) {
			snprintf(nm, sizeof nm, "%s[%u]", u->name, k);
			hle_uniformloc(pr->name, u->loc + k, nm, slen(nm));
		}
	}
}

/* ---- shaders ------------------------------------------------------------ */

GLuint glCreateShader(GLenum type)
{
	int i;
	if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
		tad_gl_error(TAD_INVALID_ENUM, "glCreateShader"); return 0; }
	for (i = 0; i < G2_MAX_SHADERS; i++)
		if (!g_sh[i].name) {
			g_sh[i].name = (u32)(i + 1);
			g_sh[i].type = type;
			g_sh[i].src = NULL; g_sh[i].len = 0;
			tr2("glCreateShader name/type", i + 1, (int)type);
			/* Create the host object now, with its type; the source follows. */
			if (hle()) hle_shadersource(g_sh[i].name, type, "", 0);
			return g_sh[i].name;
		}
	warn2("glCreateShader EXHAUSTED", G2_MAX_SHADERS, 0);
	return 0;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length)
{
	struct g2_shader *s = sh_find(shader);
	u32 total = 0, off = 0;
	GLsizei i;
	if (!s) { tad_gl_error(TAD_INVALID_VALUE, "glShaderSource"); return; }
	if (count < 0 || !string) { tad_gl_error(TAD_INVALID_VALUE, "glShaderSource"); return; }
	for (i = 0; i < count; i++) {
		if (!string[i]) continue;
		total += (length && length[i] >= 0) ? (u32)length[i] : slen(string[i]);
	}
	if (s->src) free(s->src);
	s->src = malloc(total + 1);
	if (!s->src) { s->len = 0; return; }
	for (i = 0; i < count; i++) {
		u32 n;
		if (!string[i]) continue;
		n = (length && length[i] >= 0) ? (u32)length[i] : slen(string[i]);
		memcpy(s->src + off, string[i], n);
		off += n;
	}
	s->src[total] = 0;
	s->len = total;
	tr2("glShaderSource name/len", (int)shader, (int)total);
	if (hle()) hle_shadersource(s->name, s->type, s->src, s->len);
}

/* The host compiles; we cannot know the outcome and say so optimistically.
 * A genuine compile failure shows in the viewer's log with the info log. */
void glCompileShader(GLuint shader)
{
	if (!sh_find(shader)) tad_gl_error(TAD_INVALID_VALUE, "glCompileShader");
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
	struct g2_shader *s = sh_find(shader);
	if (!params) return;
	if (!s) { tad_gl_error(TAD_INVALID_VALUE, "glGetShaderiv"); *params = 0; return; }
	switch (pname) {
	case 0x8B4F: *params = (GLint)s->type; break;        /* GL_SHADER_TYPE */
	case 0x8B80: *params = 0; break;                     /* GL_DELETE_STATUS */
	case 0x8B81: *params = 1; break;                     /* GL_COMPILE_STATUS */
	case 0x8B84: *params = 0; break;                     /* GL_INFO_LOG_LENGTH */
	case 0x8B88: *params = s->src ? (GLint)s->len + 1 : 0; break; /* SOURCE_LENGTH */
	default: tad_gl_error(TAD_INVALID_ENUM, "glGetShaderiv"); *params = 0; break;
	}
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *log)
{
	(void)shader;
	if (bufsize > 0 && log) log[0] = 0;
	if (length) *length = 0;
}

void glGetShaderSource(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *out)
{
	struct g2_shader *s = sh_find(shader);
	u32 n;
	if (!s) { tad_gl_error(TAD_INVALID_VALUE, "glGetShaderSource"); return; }
	n = s->src ? s->len : 0;
	if (bufsize <= 0 || !out) { if (length) *length = 0; return; }
	if (n > (u32)bufsize - 1) n = (u32)bufsize - 1;
	if (n) memcpy(out, s->src, n);
	out[n] = 0;
	if (length) *length = (GLsizei)n;
}

void glDeleteShader(GLuint shader)
{
	struct g2_shader *s = sh_find(shader);
	if (!s) return;
	/* Programs keep a name reference; the host keeps the object alive while
	 * it is attached, exactly as GL does. */
	if (hle()) hle_deleteshader(shader);
	if (s->src) free(s->src);
	s->src = NULL; s->len = 0; s->name = 0;
}

GLboolean glIsShader(GLuint shader) { return sh_find(shader) ? 1 : 0; }

void glReleaseShaderCompiler(void) {}

void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype,
                                GLint *range, GLint *precision)
{
	(void)shadertype;
	/* 0x8DF0..0x8DF2 LOW/MEDIUM/HIGH_FLOAT, 0x8DF3..0x8DF5 the INT trio. */
	if (precisiontype >= 0x8DF3 && precisiontype <= 0x8DF5) {
		if (range) { range[0] = 31; range[1] = 30; }
		if (precision) *precision = 0;
	} else {
		if (range) { range[0] = 127; range[1] = 127; }
		if (precision) *precision = 23;
	}
}

void glShaderBinary(GLsizei n, const GLuint *shaders, GLenum fmt, const void *bin, GLsizei len)
{ (void)n; (void)shaders; (void)fmt; (void)bin; (void)len;
  tad_gl_error(TAD_INVALID_ENUM, "glShaderBinary"); }

/* ---- programs ----------------------------------------------------------- */

static void pr_clear(struct g2_program *pr)
{
	u32 i;
	for (i = 0; i < pr->nunis; i++) if (pr->unis[i].vals) free(pr->unis[i].vals);
	memset(pr, 0, sizeof *pr);
}

GLuint glCreateProgram(void)
{
	int i;
	for (i = 0; i < G2_MAX_PROGRAMS; i++)
		if (!g_pr[i].name) {
			pr_clear(&g_pr[i]);
			g_pr[i].name = (u32)(i + 1);
			tr2("glCreateProgram", i + 1, 0);
			return g_pr[i].name;
		}
	warn2("glCreateProgram EXHAUSTED", G2_MAX_PROGRAMS, 0);
	return 0;
}

void glAttachShader(GLuint program, GLuint shader)
{
	struct g2_program *pr = pr_find(program);
	struct g2_shader *s = sh_find(shader);
	if (!pr || !s) { tad_gl_error(TAD_INVALID_VALUE, "glAttachShader"); return; }
	if (s->type == GL_VERTEX_SHADER) pr->vs = shader; else pr->fs = shader;
	if (hle()) hle_attachshader(program, shader);
}

void glDetachShader(GLuint program, GLuint shader)
{
	struct g2_program *pr = pr_find(program);
	if (!pr) { tad_gl_error(TAD_INVALID_VALUE, "glDetachShader"); return; }
	if (pr->vs == shader) pr->vs = 0;
	if (pr->fs == shader) pr->fs = 0;
	if (hle()) hle_detachshader(program, shader);
}

void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
	struct g2_program *pr = pr_find(program);
	u32 i;
	if (!pr || !name) { tad_gl_error(TAD_INVALID_VALUE, "glBindAttribLocation"); return; }
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glBindAttribLocation"); return; }
	for (i = 0; i < pr->nbound; i++)
		if (seq(pr->bound[i].name, name)) { pr->bound[i].index = index; return; }
	if (pr->nbound >= G2_MAX_ATTRIBS) return;
	snprintf(pr->bound[pr->nbound].name, G2_NAME, "%s", name);
	pr->bound[pr->nbound].index = index;
	pr->nbound++;
}

void glLinkProgram(GLuint program)
{
	struct g2_program *pr = pr_find(program);
	struct g2_shader *vs, *fs;
	u32 i;
	if (!pr) { tad_gl_error(TAD_INVALID_VALUE, "glLinkProgram"); return; }
	vs = sh_find(pr->vs); fs = sh_find(pr->fs);
	/* Re-derive everything from the sources as they stand now. */
	for (i = 0; i < pr->nunis; i++) if (pr->unis[i].vals) free(pr->unis[i].vals);
	pr->nattrs = 0; pr->nunis = 0; pr->nlocs = 0;
	if (vs && vs->src) {
		scan_decls(pr, vs->src, vs->len, "attribute", add_attr);
		scan_decls(pr, vs->src, vs->len, "uniform", add_uniform);
	}
	if (fs && fs->src)
		scan_decls(pr, fs->src, fs->len, "uniform", add_uniform);
	assign_attrs(pr);
	pr->linked = (vs && fs) ? 1 : 0;
	tr2("glLinkProgram attrs/uniforms", (int)pr->nattrs, (int)pr->nunis);
	if (hle()) send_link(pr);
}

void glUseProgram(GLuint program)
{
	if (program && !pr_find(program)) { tad_gl_error(TAD_INVALID_VALUE, "glUseProgram"); return; }
	g_cur = program;
	if (hle()) hle_useprogram(program);
}

void glDeleteProgram(GLuint program)
{
	struct g2_program *pr = pr_find(program);
	if (!pr) return;
	if (hle()) hle_deleteprogram(program);
	if (g_cur == program) g_cur = 0;
	pr_clear(pr);
}

void glValidateProgram(GLuint program) { (void)program; }
GLboolean glIsProgram(GLuint program) { return pr_find(program) ? 1 : 0; }

void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
	struct g2_program *pr = pr_find(program);
	u32 i, m;
	if (!params) return;
	if (!pr) { tad_gl_error(TAD_INVALID_VALUE, "glGetProgramiv"); *params = 0; return; }
	switch (pname) {
	case 0x8B80: *params = 0; break;                          /* DELETE_STATUS */
	case 0x8B82: *params = pr->linked; break;                 /* LINK_STATUS */
	case 0x8B83: *params = 1; break;                          /* VALIDATE_STATUS */
	case 0x8B84: *params = 0; break;                          /* INFO_LOG_LENGTH */
	case 0x8B85: *params = (pr->vs ? 1 : 0) + (pr->fs ? 1 : 0); break;
	case 0x8B86: *params = (GLint)pr->nunis; break;           /* ACTIVE_UNIFORMS */
	case 0x8B87:                                              /* ..._MAX_LENGTH */
		for (m = 0, i = 0; i < pr->nunis; i++) if (slen(pr->unis[i].name) + 1 > m) m = slen(pr->unis[i].name) + 1;
		*params = (GLint)m; break;
	case 0x8B89: *params = (GLint)pr->nattrs; break;          /* ACTIVE_ATTRIBUTES */
	case 0x8B8A:
		for (m = 0, i = 0; i < pr->nattrs; i++) if (slen(pr->attrs[i].name) + 1 > m) m = slen(pr->attrs[i].name) + 1;
		*params = (GLint)m; break;
	default: tad_gl_error(TAD_INVALID_ENUM, "glGetProgramiv"); *params = 0; break;
	}
}

void glGetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei *length, GLchar *log)
{
	(void)program;
	if (bufsize > 0 && log) log[0] = 0;
	if (length) *length = 0;
}

static void copy_name(const char *src, GLsizei bufsize, GLsizei *length, GLchar *out)
{
	u32 n = slen(src);
	if (bufsize <= 0 || !out) { if (length) *length = 0; return; }
	if (n > (u32)bufsize - 1) n = (u32)bufsize - 1;
	memcpy(out, src, n); out[n] = 0;
	if (length) *length = (GLsizei)n;
}

void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufsize, GLsizei *length,
                       GLint *size, GLenum *type, GLchar *name)
{
	struct g2_program *pr = pr_find(program);
	if (!pr || index >= pr->nattrs) { tad_gl_error(TAD_INVALID_VALUE, "glGetActiveAttrib"); return; }
	if (size) *size = 1;
	if (type) *type = pr->attrs[index].gltype;
	copy_name(pr->attrs[index].name, bufsize, length, name);
}

void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufsize, GLsizei *length,
                        GLint *size, GLenum *type, GLchar *name)
{
	struct g2_program *pr = pr_find(program);
	if (!pr || index >= pr->nunis) { tad_gl_error(TAD_INVALID_VALUE, "glGetActiveUniform"); return; }
	if (size) *size = (GLint)pr->unis[index].count;
	if (type) *type = pr->unis[index].gltype;
	copy_name(pr->unis[index].name, bufsize, length, name);
}

void glGetAttachedShaders(GLuint program, GLsizei maxcount, GLsizei *count, GLuint *shaders)
{
	struct g2_program *pr = pr_find(program);
	GLsizei n = 0;
	if (!pr) { tad_gl_error(TAD_INVALID_VALUE, "glGetAttachedShaders"); return; }
	if (pr->vs && n < maxcount && shaders) shaders[n++] = pr->vs;
	if (pr->fs && n < maxcount && shaders) shaders[n++] = pr->fs;
	if (count) *count = n;
}

GLint glGetAttribLocation(GLuint program, const GLchar *name)
{
	struct g2_program *pr = pr_find(program);
	u32 i;
	if (!pr || !name) { tad_gl_error(TAD_INVALID_VALUE, "glGetAttribLocation"); return -1; }
	for (i = 0; i < pr->nattrs; i++)
		if (seq(pr->attrs[i].name, name)) return (GLint)pr->attrs[i].index;
	tr2("glGetAttribLocation: not found (program)", (int)program, 0);
	return -1;
}

GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
	struct g2_program *pr = pr_find(program);
	char base[G2_NAME];
	u32 i, n, idx = 0;
	if (!pr || !name) { tad_gl_error(TAD_INVALID_VALUE, "glGetUniformLocation"); return -1; }
	for (n = 0; name[n] && name[n] != '[' && n < G2_NAME - 1; n++) base[n] = name[n];
	base[n] = 0;
	if (name[n] == '[') {
		const char *q = name + n + 1;
		while (*q >= '0' && *q <= '9') idx = idx * 10 + (u32)(*q++ - '0');
	}
	for (i = 0; i < pr->nunis; i++)
		if (seq(pr->unis[i].name, base)) {
			if (idx >= pr->unis[i].count) return -1;
			return (GLint)(pr->unis[i].loc + idx);
		}
	tr2("glGetUniformLocation: not found (program)", (int)program, 0);
	return -1;
}

/* ---- uniforms ----------------------------------------------------------- */

static u32 kind_words(u32 kind)
{
	switch (kind) {
	case 0: case 4: return 1;  case 1: case 5: return 2;
	case 2: case 6: return 3;  case 3: case 7: return 4;
	case 8: return 4;  case 9: return 9;  case 10: return 16;
	}
	return 1;
}

/* Record and forward one glUniform* call. `loc` may address an element inside
 * an array uniform, in which case the cache is updated from that element on. */
static void uniform_set(GLint loc, u32 kind, GLsizei count, const void *v)
{
	struct g2_program *pr = pr_find(g_cur);
	u32 i, w = kind_words(kind), words;
	if (loc < 0) return;                       /* -1 is silently ignored, per spec */
	if (!pr) { tad_gl_error(TAD_INVALID_OPERATION, "glUniform"); return; }
	if (count < 0 || !v) { tad_gl_error(TAD_INVALID_VALUE, "glUniform"); return; }
	words = (u32)count * w;
	for (i = 0; i < pr->nunis; i++) {
		struct g2_uniform *u = &pr->unis[i];
		if ((u32)loc >= u->loc && (u32)loc < u->loc + u->count) {
			u32 off = (u32)loc - u->loc;
			u32 need = u->count * w;
			if (u->kind != kind || u->nwords != need) {
				if (u->vals) free(u->vals);
				u->vals = malloc(need * 4);
				if (u->vals) memset(u->vals, 0, need * 4);
				u->kind = kind; u->nwords = need; u->ucount = u->count;
			}
			if (u->vals && off * w + words <= need)
				memcpy(u->vals + off * w, v, words * 4);
			break;
		}
	}
	if (hle()) hle_uniform((u32)loc, kind, (u32)count, v, words);
}

void glUniform1f(GLint l, GLfloat x) { uniform_set(l, 0, 1, &x); }
void glUniform2f(GLint l, GLfloat x, GLfloat y) { float v[2]={x,y}; uniform_set(l, 1, 1, v); }
void glUniform3f(GLint l, GLfloat x, GLfloat y, GLfloat z) { float v[3]={x,y,z}; uniform_set(l, 2, 1, v); }
void glUniform4f(GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { float v[4]={x,y,z,w}; uniform_set(l, 3, 1, v); }
void glUniform1i(GLint l, GLint x) { uniform_set(l, 4, 1, &x); }
void glUniform2i(GLint l, GLint x, GLint y) { int v[2]={x,y}; uniform_set(l, 5, 1, v); }
void glUniform3i(GLint l, GLint x, GLint y, GLint z) { int v[3]={x,y,z}; uniform_set(l, 6, 1, v); }
void glUniform4i(GLint l, GLint x, GLint y, GLint z, GLint w) { int v[4]={x,y,z,w}; uniform_set(l, 7, 1, v); }
void glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { uniform_set(l, 0, n, v); }
void glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { uniform_set(l, 1, n, v); }
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { uniform_set(l, 2, n, v); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { uniform_set(l, 3, n, v); }
void glUniform1iv(GLint l, GLsizei n, const GLint *v) { uniform_set(l, 4, n, v); }
void glUniform2iv(GLint l, GLsizei n, const GLint *v) { uniform_set(l, 5, n, v); }
void glUniform3iv(GLint l, GLsizei n, const GLint *v) { uniform_set(l, 6, n, v); }
void glUniform4iv(GLint l, GLsizei n, const GLint *v) { uniform_set(l, 7, n, v); }
/* GLES2 requires transpose == GL_FALSE; a GL_TRUE is INVALID_VALUE there. */
void glUniformMatrix2fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{ if (t) { tad_gl_error(TAD_INVALID_VALUE, "glUniformMatrix2fv"); return; } uniform_set(l, 8, n, v); }
void glUniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{ if (t) { tad_gl_error(TAD_INVALID_VALUE, "glUniformMatrix3fv"); return; } uniform_set(l, 9, n, v); }
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{ if (t) { tad_gl_error(TAD_INVALID_VALUE, "glUniformMatrix4fv"); return; } uniform_set(l, 10, n, v); }

void glGetUniformfv(GLuint program, GLint loc, GLfloat *out)
{
	struct g2_program *pr = pr_find(program);
	u32 i;
	if (!pr || !out) return;
	for (i = 0; i < pr->nunis; i++) {
		struct g2_uniform *u = &pr->unis[i];
		if ((u32)loc >= u->loc && (u32)loc < u->loc + u->count && u->vals) {
			u32 w = kind_words(u->kind);
			memcpy(out, u->vals + ((u32)loc - u->loc) * w, w * 4);
			return;
		}
	}
}
void glGetUniformiv(GLuint program, GLint loc, GLint *out)
{ glGetUniformfv(program, loc, (GLfloat *)out); }

/* ---- generic vertex attributes ------------------------------------------ */

static u32 attr_bytes(u32 type, i32 size)
{
	u32 b = (type == GL_BYTE_ || type == GL_UNSIGNED_BYTE_) ? 1
	      : (type == GL_SHORT_ || type == GL_UNSIGNED_SHORT_) ? 2 : 4;
	return b * (u32)(size > 0 ? size : 1);
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm,
                           GLsizei stride, const void *ptr)
{
	struct g2_attr *a;
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glVertexAttribPointer"); return; }
	if (size < 1 || size > 4 || stride < 0) { tad_gl_error(TAD_INVALID_VALUE, "glVertexAttribPointer"); return; }
	a = &g_attr[index];
	a->ptr = ptr; a->buf = tad_core_bound_array();
	a->size = size; a->type = type; a->norm = norm ? 1 : 0; a->stride = stride;
	tr2("glVertexAttribPointer index/size", (int)index, size);
}

void glEnableVertexAttribArray(GLuint index)
{
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glEnableVertexAttribArray"); return; }
	g_attr[index].on = 1;
	if (hle()) hle_attribenable(index, 1);
}

void glDisableVertexAttribArray(GLuint index)
{
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glDisableVertexAttribArray"); return; }
	g_attr[index].on = 0;
	if (hle()) hle_attribenable(index, 0);
}

static void attrib_value(GLuint index, float x, float y, float z, float w)
{
	struct g2_attr *a;
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glVertexAttrib"); return; }
	a = &g_attr[index];
	a->value[0] = x; a->value[1] = y; a->value[2] = z; a->value[3] = w;
	a->value_set = 1;
	if (hle()) hle_attribvalue(index, a->value);
}
void glVertexAttrib1f(GLuint i, GLfloat x) { attrib_value(i, x, 0, 0, 1); }
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { attrib_value(i, x, y, 0, 1); }
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { attrib_value(i, x, y, z, 1); }
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { attrib_value(i, x, y, z, w); }
void glVertexAttrib1fv(GLuint i, const GLfloat *v) { if (v) attrib_value(i, v[0], 0, 0, 1); }
void glVertexAttrib2fv(GLuint i, const GLfloat *v) { if (v) attrib_value(i, v[0], v[1], 0, 1); }
void glVertexAttrib3fv(GLuint i, const GLfloat *v) { if (v) attrib_value(i, v[0], v[1], v[2], 1); }
void glVertexAttrib4fv(GLuint i, const GLfloat *v) { if (v) attrib_value(i, v[0], v[1], v[2], v[3]); }

void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params)
{
	struct g2_attr *a;
	if (!params) return;
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glGetVertexAttribiv"); return; }
	a = &g_attr[index];
	switch (pname) {
	case 0x8622: *params = a->on; break;             /* ARRAY_ENABLED */
	case 0x8623: *params = a->size; break;           /* ARRAY_SIZE */
	case 0x8624: *params = a->stride; break;         /* ARRAY_STRIDE */
	case 0x8625: *params = (GLint)a->type; break;    /* ARRAY_TYPE */
	case 0x886A: *params = (GLint)a->norm; break;    /* ARRAY_NORMALIZED */
	case 0x889F: *params = (GLint)a->buf; break;     /* ARRAY_BUFFER_BINDING */
	default: tad_gl_error(TAD_INVALID_ENUM, "glGetVertexAttribiv"); break;
	}
}
void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params)
{
	if (!params) return;
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glGetVertexAttribfv"); return; }
	if (pname == 0x8626) { memcpy(params, g_attr[index].value, 16); return; }   /* CURRENT_VERTEX_ATTRIB */
	{ GLint v = 0; glGetVertexAttribiv(index, pname, &v); params[0] = (GLfloat)v; }
}
void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **ptr)
{
	(void)pname;
	if (!ptr) return;
	if (index >= G2_MAX_ATTRIBS) { tad_gl_error(TAD_INVALID_VALUE, "glGetVertexAttribPointerv"); return; }
	*ptr = (void *)g_attr[index].ptr;
}

/* ---- what the core asks of us ------------------------------------------- */

int tad_g2_in_use(void) { return g_cur != 0; }

/* Before a draw with a program in use: every enabled attribute array goes
 * across, buffer-backed ones by reference and client-side ones by value —
 * the same rule the core applies to its fixed-function arrays. `nverts` is
 * how many vertices the draw touches, which decides how much to copy. */
void tad_g2_send_attribs(u32 nverts)
{
	u32 i;
	for (i = 0; i < G2_MAX_ATTRIBS; i++) {
		struct g2_attr *a = &g_attr[i];
		u32 stride;
		if (!a->on) continue;
		if (a->buf) {
			hle_attribpointer(i, a->buf, a->size, a->type, a->norm, a->stride,
			                  (u32)(unsigned long)a->ptr);
			continue;
		}
		if (!a->ptr) continue;
		stride = a->stride ? (u32)a->stride : attr_bytes(a->type, a->size);
		hle_bufferdata(G2_CLIENT_BASE + i, nverts * stride, a->ptr);
		hle_attribpointer(i, G2_CLIENT_BASE + i, a->size, a->type, a->norm,
		                  (i32)stride, 0);
	}
}

/* GLES2 limits and bindings glGetIntegerv is asked for. Returns 1 if answered. */
int tad_g2_get_integer(GLenum p, GLint *v)
{
	switch (p) {
	case 0x8869: *v = G2_MAX_ATTRIBS; return 1;   /* MAX_VERTEX_ATTRIBS */
	case 0x8872: *v = 8;  return 1;               /* MAX_TEXTURE_IMAGE_UNITS */
	case 0x8B4C: *v = 4;  return 1;               /* MAX_VERTEX_TEXTURE_IMAGE_UNITS */
	case 0x8B4D: *v = 16; return 1;               /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
	case 0x8DFB: *v = 256; return 1;              /* MAX_VERTEX_UNIFORM_VECTORS */
	case 0x8DFC: *v = 15; return 1;               /* MAX_VARYING_VECTORS */
	case 0x8DFD: *v = 256; return 1;              /* MAX_FRAGMENT_UNIFORM_VECTORS */
	case 0x8B8D: *v = (GLint)g_cur; return 1;     /* CURRENT_PROGRAM */
	case 0x851C: *v = 2048; return 1;             /* MAX_CUBE_MAP_TEXTURE_SIZE */
	case 0x84E8: *v = 4096; return 1;             /* MAX_RENDERBUFFER_SIZE */
	case 0x8B9A: *v = GL_UNSIGNED_BYTE_; return 1;/* IMPLEMENTATION_COLOR_READ_TYPE */
	case 0x8B9B: *v = 0x1908; return 1;           /* ..._FORMAT: GL_RGBA */
	case 0x8DF9: *v = 0; return 1;                /* NUM_SHADER_BINARY_FORMATS */
	case 0x8DFA: *v = 1; return 1;                /* SHADER_COMPILER */
	case 0x8CA6: *v = 0; return 1;                /* FRAMEBUFFER_BINDING */
	case 0x8CA7: *v = 0; return 1;                /* RENDERBUFFER_BINDING */
	case 0x0D57: *v = 0; return 1;                /* STENCIL_BITS */
	case 0x8B60: return 0;
	}
	return 0;
}

/* A host that has just attached, or asked for its mirror again: everything it
 * needs to draw the next frame, in the order it needs it. */
void tad_g2_resync(void)
{
	u32 i, k;
	for (i = 0; i < G2_MAX_SHADERS; i++)
		if (g_sh[i].name)
			hle_shadersource(g_sh[i].name, g_sh[i].type,
			                 g_sh[i].src ? g_sh[i].src : "", g_sh[i].len);
	for (i = 0; i < G2_MAX_PROGRAMS; i++) {
		struct g2_program *pr = &g_pr[i];
		if (!pr->name) continue;
		if (pr->vs) hle_attachshader(pr->name, pr->vs);
		if (pr->fs) hle_attachshader(pr->name, pr->fs);
		if (pr->linked) {
			send_link(pr);
			hle_useprogram(pr->name);
			for (k = 0; k < pr->nunis; k++) {
				struct g2_uniform *u = &pr->unis[k];
				if (u->vals && u->nwords)
					hle_uniform(u->loc, u->kind, u->ucount, u->vals, u->nwords);
			}
		}
	}
	hle_useprogram(g_cur);
	for (i = 0; i < G2_MAX_ATTRIBS; i++) {
		hle_attribenable(i, g_attr[i].on ? 1u : 0u);
		if (g_attr[i].value_set) hle_attribvalue(i, g_attr[i].value);
	}
}

/* Context teardown: forget everything, exactly as the core forgets its
 * textures and buffers. The host drops its side on TADGL_RESET. */
void tad_g2_reset(void)
{
	u32 i;
	for (i = 0; i < G2_MAX_SHADERS; i++) {
		if (g_sh[i].src) free(g_sh[i].src);
		g_sh[i].src = NULL; g_sh[i].len = 0; g_sh[i].name = 0;
	}
	for (i = 0; i < G2_MAX_PROGRAMS; i++) pr_clear(&g_pr[i]);
	memset(g_attr, 0, sizeof g_attr);
	g_cur = 0;
}

/* ---- GLES2 entry points that are core names for things the GLES1 side
 * already has under an OES suffix, or that need only resolve --------------- */

void glGenerateMipmap(GLenum target) { (void)target; }

/* Mali's own extensions, imported by the LF3000 Brio display module and never
 * reached on the fbdev path we give it. They only have to resolve. */
void glEnableSpecialMode(void) {}
void glDisableSpecialMode(void) {}
void glSetSpecialModeParam(void) {}
void glEGLImageTargetTexture2DOES(GLenum target, void *image) { (void)target; (void)image; }
