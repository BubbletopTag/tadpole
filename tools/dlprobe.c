/* dlopen probe: each argument is dlopen'd with RTLD_NOW|RTLD_GLOBAL ("@name"
 * for RTLD_LAZY) and the outcome printed, so a failing closure can be bisected
 * from inside the guest. */
extern void *dlopen(const char *, int);
extern char *dlerror(void);
extern int   printf(const char *, ...);
extern void  exit(int);
void start_c(long *sp);
__attribute__((naked)) void _start(void) { __asm__ volatile("mov r0, sp\n\tb start_c"); }
void start_c(long *sp)
{
	int argc = (int)sp[0], i;
	char **argv = (char **)(sp + 1);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		int flags = 2 | 0x100;
		void *h;
		if (a[0] == '@') { flags = 1 | 0x100; a++; }
		h = dlopen(a, flags);
		if (h) printf("OK   %s\n", a);
		else { char *e = dlerror(); printf("FAIL %s: %s\n", a, e ? e : "(none)"); }
	}
	exit(0);
}
