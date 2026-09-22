/* Tadpole — the Didj's libCartridgeMPI.so, which is not a cartridge library.
 *
 * AN EXPERIMENT FOR THE LFHACKS BACKPORTERS, and nothing a stock Didj needs.
 * A LeapsterExplorer title dropped into /Didj/ProgramFiles fails in dlopen:
 * its App.so names one library the Didj does not have and imports sixteen
 * symbols the Didj's 2009 Brio and Lightning framework never exported. The
 * measurement is in docs/DIDJ.md ("what an Explorer title is missing"): every
 * import of Ni Hao, Kai-lan's App.so minus every export of the Didj, with a
 * native title as the control. 217 of 236 resolve. This file is the rest.
 *
 * WHY IT IS CALLED libCartridgeMPI.so. App.so has that name in DT_NEEDED and
 * takes not one symbol from it, so the loader wants a file and nothing else
 * from it. Give it this file, and the loader brings it into the process — at
 * which point the sixteen definitions below resolve the remaining imports.
 * One library, in the device's own /Didj/Base/Brio/lib, and no change to
 * the title, the shell, or any device that has a real one (this is only
 * ever installed into the Didj's tree).
 *
 * WHAT EACH STUB DOES was read out of the Leapster GS's own implementations
 * (llvm-objdump on its libLightningJSON.so and libButtonMPI.so) and matched
 * to what the Didj already exports, so that every object a stub hands back
 * is one the Didj's own code can then be given:
 *
 *   tPackageType, tUploadDataType   sixteen-byte character buffers; the GS
 *                                   ctor is memset(16) + strncpy(15). Same.
 *   CGameAreaStart/Exit,            log events the Didj's analytics never
 *   CAchievementEarned              had; built as the Didj's own CGameStart,
 *                                   CGameExit and CLevelUp, so CLogFile::
 *                                   Append and ~CLogData see a real record.
 *   CMicroDownloads()               the GS default ctor derives its path
 *                                   from CSystemData::GetMDLsPath(); so do
 *                                   we, then call the Didj's ctor(path).
 *   CSystemData::GetBaseTutorialPath  the Didj's GetBasePath(): a real dir.
 *   CMilestones                     a singleton with no milestones: Add is a
 *                                   no-op and GetMatch an empty vector.
 *   CPlayerProfile::AddBadge        no badges on a Didj; reports false.
 *   CTouchEventQueue                a Brio IEventListener with no events to
 *                                   listen to, constructed through the
 *                                   Didj's own base ctor so the EventMPI can
 *                                   hold it, whose queue is always empty.
 *                                   The Didj has no touchscreen, and the
 *                                   title constructs this but does not need
 *                                   it to play.
 *
 * Freestanding C (sixteen stubs for Kai-lan, thirteen more for Clam Prix below).
 * The C++ names are attached with asm labels, which is what
 * lets a C file define _ZN12tPackageTypeC1EPKc without a compiler that knows
 * what a tPackageType is. Sizes and slot offsets are the GS's, so a title
 * that allocated the Explorer's sizeof gets an object no larger than it made
 * room for. */

typedef unsigned int u32;
typedef unsigned short u16;

extern void *memset(void *, int, u32);
extern char *strncpy(char *, const char *, u32);

/* ---- the Didj's own exports these stubs build on ------------------------ */
extern void  ustring_dtor(void *s)                    __asm__("_ZN4Glib7ustringD1Ev");
extern void *sysdata_instance(void)                   __asm__("_ZN11CSystemData8InstanceEv");
extern void  sysdata_basepath(void *sret, void *self) __asm__("_ZN11CSystemData11GetBasePathEv");
extern void  sysdata_mdlspath(void *sret, void *self) __asm__("_ZN11CSystemData11GetMDLsPathEv");
extern void  microdl_ctor_path(void *self, void *ustring_tmp)
                                                      __asm__("_ZN3LTM15CMicroDownloadsC1EN4Glib7ustringE");
extern void  gamestart_ctor(void *self, char *name)   __asm__("_ZN7LogData9Lightning10CGameStartC1EPc");
extern void  gameexit_ctor(void *self)                __asm__("_ZN7LogData9Lightning9CGameExitC1Ev");
extern void  levelup_ctor(void *self, char *name)     __asm__("_ZN7LogData9Lightning8CLevelUpC1EPc");
extern void  listener_ctor(void *self, const u32 *types, u32 count)
                                                      __asm__("_ZN8LeapFrog4Brio14IEventListenerC2EPKmm");
extern void  listener_dtor(void *self)                __asm__("_ZN8LeapFrog4Brio14IEventListenerD2Ev");
extern char  listener_typeinfo[]                      __asm__("_ZTIN8LeapFrog4Brio14IEventListenerE");
extern void  op_delete(void *p)                       __asm__("_ZdlPv");

/* ---- tPackageType / tUploadDataType: 16-byte strings -------------------- */
void pkgtype_ctor_s(char *self, const char *s) __asm__("_ZN12tPackageTypeC1EPKc");
void pkgtype_ctor_s(char *self, const char *s)
{ memset(self, 0, 16); if (s) strncpy(self, s, 15); }
void pkgtype_ctor(char *self) __asm__("_ZN12tPackageTypeC1Ev");
void pkgtype_ctor(char *self) { memset(self, 0, 16); }

void uploadtype_ctor_s(char *self, const char *s) __asm__("_ZN15tUploadDataTypeC1EPKc");
void uploadtype_ctor_s(char *self, const char *s)
{ memset(self, 0, 16); if (s) strncpy(self, s, 15); }
void uploadtype_ctor(char *self) __asm__("_ZN15tUploadDataTypeC1Ev");
void uploadtype_ctor(char *self) { memset(self, 0, 16); }

/* ---- analytics events the Didj never had --------------------------------- */
void gamearea_start(void *self, char *name) __asm__("_ZN7LogData9Lightning14CGameAreaStartC1EPc");
void gamearea_start(void *self, char *name) { gamestart_ctor(self, name); }
void gamearea_exit(void *self) __asm__("_ZN7LogData9Lightning13CGameAreaExitC1Ev");
void gamearea_exit(void *self) { gameexit_ctor(self); }
static char g_achievement[] = "achievement";
void achievement_earned(void *self, u16 id) __asm__("_ZN7LogData9Lightning18CAchievementEarnedC1Et");
void achievement_earned(void *self, u16 id) { (void)id; levelup_ctor(self, g_achievement); }

/* ---- micro-downloads, tutorial path ------------------------------------- */
void microdl_ctor(void *self) __asm__("_ZN3LTM15CMicroDownloadsC1Ev");
void microdl_ctor(void *self)
{
	/* A Glib::ustring is one pointer on this libstdc++; leave it room. */
	u32 path[8];
	sysdata_mdlspath(path, sysdata_instance());
	microdl_ctor_path(self, path);          /* by value: a pointer to our temp */
	ustring_dtor(path);
}

void *tutorial_path(void *sret, void *self) __asm__("_ZN11CSystemData19GetBaseTutorialPathEv");
void *tutorial_path(void *sret, void *self) { sysdata_basepath(sret, self); return sret; }

/* ---- milestones and badges ---------------------------------------------- */
static u32 g_milestones[16];
void *milestones_instance(void) __asm__("_ZN11CMilestones8InstanceEv");
void *milestones_instance(void) { return g_milestones; }
int milestones_add(void *self, void *ustring_tmp) __asm__("_ZN11CMilestones3AddEN4Glib7ustringE");
int milestones_add(void *self, void *ustring_tmp) { (void)self; (void)ustring_tmp; return 0; }
/* Returns a std::vector by value: three words through the hidden pointer,
 * all zero, which is an empty vector the caller can walk and destroy. */
void *milestones_getmatch(u32 *sret, void *self, void *vec_tmp)
	__asm__("_ZN11CMilestones8GetMatchESt6vectorIN4Glib7ustringESaIS2_EE");
void *milestones_getmatch(u32 *sret, void *self, void *vec_tmp)
{ (void)self; (void)vec_tmp; sret[0] = sret[1] = sret[2] = 0; return sret; }

int add_badge(void *self, u32 badge) __asm__("_ZN3LTM14CPlayerProfile8AddBadgeEm");
int add_badge(void *self, u32 badge) { (void)self; (void)badge; return 0; }

/* ---- CTouchEventQueue: a listener with nothing to hear ------------------ */
/* Layout follows the GS's: vptr @0, IEventListener's pimpl @4, a CKernelMPI
 * @8, a mutex @16, two std::vector<tTouchData> @0x28 and @0x34, the front
 * index @0x40. Only the base and the vectors are real here; the rest is
 * zero, and GetQueue never touches it. */
#define TQ_BYTES 0x44
#define TQ_VEC   0x28

static void tq_dtor(void *self) { listener_dtor(self); }
static void tq_dtor_del(void *self) { listener_dtor(self); op_delete(self); }
static u32  tq_notify(void *self, const void *msg) { (void)self; (void)msg; return 0; }
/* Itanium layout: offset-to-top, typeinfo, then the virtuals in the base's
 * order — which the Didj's own vtable (read from its relocations) gives as
 * ~D1, ~D0, Notify. The typeinfo is the base's: this object claims to be
 * exactly an IEventListener, which for the EventMPI's purposes it is. */
static void *tq_vtable[5] = { 0, listener_typeinfo,
                              (void *)tq_dtor, (void *)tq_dtor_del, (void *)tq_notify };

void tq_ctor(void *self) __asm__("_ZN8LeapFrog4Brio16CTouchEventQueueC1Ev");
void tq_ctor(void *self)
{
	listener_ctor(self, 0, 0);                    /* no event types at all */
	*(void ***)self = &tq_vtable[2];
	memset((char *)self + 8, 0, TQ_BYTES - 8);
}
void tq_dtor_pub(void *self) __asm__("_ZN8LeapFrog4Brio16CTouchEventQueueD1Ev");
void tq_dtor_pub(void *self) { tq_dtor(self); }
void *tq_getqueue(void *self) __asm__("_ZN8LeapFrog4Brio16CTouchEventQueue8GetQueueEv");
void *tq_getqueue(void *self) { return (char *)self + TQ_VEC; }

/* ======================================================================
 * THE SECOND TITLE: SpongeBob SquarePants: The Clam Prix (LST3-0x00180025)
 * ======================================================================
 *
 * Its App.so imports thirteen more names the Didj lacks, and seven that
 * exist nowhere in the Leapster GS either — which is how it was learned
 * that the Didj's shell dlopens lazily: Clam Prix LOADED with twenty
 * symbols unresolved, ran its init, uploaded eighteen textures, and only
 * died at the first CALL into one of them:
 *
 *     AppManager: can't resolve symbol 'CSystemData::GetCurrentPlayerID()'
 *
 * So a function the title never calls costs nothing, and the seven that
 * are nowhere (an NvTriStrip stripifier, a matrix inverse, a template
 * instantiation, a touch-message accessor) are exactly the ones the GS
 * gets away with too. These thirteen are the ones it does call, each
 * again read out of the GS's implementation and matched to the Didj's:
 *
 *   CSystemData::GetCurrentPlayerID   the GS asks LTM::CSystem; the Didj
 *                                     has the same class and method. Same.
 *   CSystemData::GetCurrentGamePackageID  a ustring member on the GS; the
 *                                     Didj's GetCurrentGameID() is the
 *                                     nearest thing with the same shape.
 *   CSystemData::GetLocalDataPath(pid, game)  the Didj's
 *                                     GetDataProfileGamePath(pid): the
 *                                     per-player save directory.
 *   CMilestones::Get                  an empty vector, like GetMatch.
 *   CPlayerProfile::GetBadges         a bitmask on the GS; none here.
 *   CCyo::Get{Audio,Value}(id)        ustrings by hidden pointer; empty.
 *   CCyo::GetStatus/SetStatus/GetCompletionParameters  the GS itself
 *                                     returns nothing for these when its
 *                                     "Didj legacy" flag is set — the
 *                                     Explorer's own path for running Didj
 *                                     titles — so nothing is what they
 *                                     get here as well.
 *   fopenAtomic / fcloseAtomic        libdftp's write-to-temp-and-rename;
 *                                     plain fopen and fclose do the job.
 */
extern void  ustring_ctor_empty(void *s)             __asm__("_ZN4Glib7ustringC1Ev");
extern void  csystem_ctor(void *s)                   __asm__("_ZN3LTM7CSystemC1Ev");
extern void  csystem_dtor(void *s)                   __asm__("_ZN3LTM7CSystemD1Ev");
extern u32   csystem_player(void *s)                 __asm__("_ZN3LTM7CSystem18GetCurrentPlayerIDEv");
extern void  sysdata_gameid(void *sret, void *self)  __asm__("_ZN11CSystemData16GetCurrentGameIDEv");
extern void  sysdata_profilegame(void *sret, void *self, u32 pid)
                                                     __asm__("_ZN11CSystemData22GetDataProfileGamePathE9tPlayerID");
extern void *fopen(const char *, const char *);
extern int   fclose(void *);

u32 sd_current_player(void *self) __asm__("_ZN11CSystemData18GetCurrentPlayerIDEv");
u32 sd_current_player(void *self)
{
	u32 sys[16], id;                 /* LTM::CSystem is small; leave room */
	(void)self;
	csystem_ctor(sys);
	id = csystem_player(sys);
	csystem_dtor(sys);
	return id;
}

void *sd_game_package_id(void *sret, void *self) __asm__("_ZN11CSystemData23GetCurrentGamePackageIDEv");
void *sd_game_package_id(void *sret, void *self) { sysdata_gameid(sret, self); return sret; }

void *sd_local_data_path(void *sret, void *self, u32 pid, void *game_tmp)
	__asm__("_ZN11CSystemData16GetLocalDataPathE9tPlayerIDN4Glib7ustringE");
void *sd_local_data_path(void *sret, void *self, u32 pid, void *game_tmp)
{ (void)game_tmp; sysdata_profilegame(sret, self, pid); return sret; }

void *milestones_get(u32 *sret, void *self) __asm__("_ZN11CMilestones3GetEv");
void *milestones_get(u32 *sret, void *self)
{ (void)self; sret[0] = sret[1] = sret[2] = 0; return sret; }

u32 profile_badges(void *self) __asm__("_ZN3LTM14CPlayerProfile9GetBadgesEv");
u32 profile_badges(void *self) { (void)self; return 0; }

void *cyo_completion(void *self) __asm__("_ZN3LTM4CCyo23GetCompletionParametersEv");
void *cyo_completion(void *self) { (void)self; return 0; }

void *cyo_audio(void *sret, void *self, u32 id) __asm__("_ZN3LTM4CCyo8GetAudioEm");
void *cyo_audio(void *sret, void *self, u32 id)
{ (void)self; (void)id; ustring_ctor_empty(sret); return sret; }

void *cyo_value(void *sret, void *self, u32 id) __asm__("_ZN3LTM4CCyo8GetValueEm");
void *cyo_value(void *sret, void *self, u32 id)
{ (void)self; (void)id; ustring_ctor_empty(sret); return sret; }

u32 cyo_status(void *self, u32 id) __asm__("_ZN3LTM4CCyo9GetStatusEm");
u32 cyo_status(void *self, u32 id) { (void)self; (void)id; return 0; }

u32 cyo_set_status(void *self, u32 id, u32 st) __asm__("_ZN3LTM4CCyo9SetStatusEmNS_10tCyoStatusE");
u32 cyo_set_status(void *self, u32 id, u32 st) { (void)self; (void)id; (void)st; return 0; }

void *fopen_atomic(const char *path, const char *mode) __asm__("_Z11fopenAtomicPKcS0_");
void *fopen_atomic(const char *path, const char *mode) { return fopen(path, mode); }

int fclose_atomic(void *f) __asm__("_Z12fcloseAtomicP19__STDIO_FILE_STRUCT");
int fclose_atomic(void *f) { return f ? fclose(f) : -1; }
