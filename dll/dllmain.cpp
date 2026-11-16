// V Ultra spellbook pool mod (config-driven).
// The spellbook spell pool = the set of skills enabled in vultramod.ini [spellbook_pool].
// Normal spellbook spells are skills 0..15; Portal(17), Telekinesis(25), Psychic Push(26) are
// otherwise-disabled higher-slot spells that this mod can re-add. When the game rolls a spellbook
// spell, the hook replaces it with a uniform pick from the enabled pool (disabled spells never
// appear; enabled high-slot spells do). The game's own level-gate still re-rolls per requirement.
//
// Hooks are resident; behavior is gated on g_pool so an in-game overlay can change the pool live
// (applies to the next spellbook generated; no restart). First install requires the game closed.
// Injected via a proxy openal32.dll that forwards all exports to openal32_real.dll.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <intrin.h>
#include "exports.h"
#pragma comment(lib, "user32.lib")   // GetAsyncKeyState

static const uintptr_t RVA_SET_SKILL          = 0x00223680;
static const uintptr_t RVA_SET_SKILL_CAP      = 0x002236b1;
// The ONE random-roll call site, inside generateNewRandomSpellBook's gate-retry loop
// (`call setSkill`). Redirecting only this site means the pool re-roll happens at
// generation time; Item::load / setEntityState / pickup re-apply the already-stored
// skill unchanged, so dropping & re-grabbing a book keeps its spell.
static const uintptr_t RVA_GEN_SETSKILL_CALL  = 0x00243d3e;
// The `call setSkill` inside setNameAndDescription that re-derives a book's skill from its
// randIndex on every refresh. Redirected to a sanitizer so widening can't misread old books.
static const uintptr_t RVA_NAMEDESC_SETSKILL_CALL = 0x0022255c;
// ImGui (statically linked in vagante.exe; __cdecl). Used for the in-game overlay.
static const uintptr_t RVA_IM_RENDER     = 0x00084c00;
static const uintptr_t RVA_IM_BEGIN      = 0x00087cd0;
static const uintptr_t RVA_IM_END        = 0x0008a700;
static const uintptr_t RVA_IM_CHECKBOX   = 0x0008fa60;
static const uintptr_t RVA_IM_TEXT       = 0x0008b8c0;
static const uintptr_t RVA_IM_SEPARATOR  = 0x0009abd0;
static const int       OVERLAY_HOTKEY    = VK_F10;
// New Game Plus / "The Cycle" looping. GameSave::initGameState(GameEngine*) reads
// GameSave.m_newGamePlus (+0x3a6) and, when set, applies the harder NG+ scaling via
// LevelGenerator::setNewGamePlus. Forcing the flag here turns the run into NG+.
static const uintptr_t RVA_INIT_GAMESTATE = 0x001d1a10; // GameSave::initGameState(GameEngine*) const
static const size_t    OFF_NEWGAMEPLUS    = 0x3a6;      // GameSave::m_newGamePlus (byte)
static const size_t    OFF_GE_NEWGAMEPLUS = 0x2f4;      // GameEngine::newGamePlus (live, byte)
// Win-path tracer targets (logging only) to capture the win->restart transition.
static const uintptr_t RVA_CREATE_SAVESTATE = 0x001d0180; // GameSave::createSaveState(GameEngine*)
static const uintptr_t RVA_NEWGAME          = 0x001c1570; // GameEngine::newGame(...)
static const uintptr_t RVA_STEP_GOOD_ENDING = 0x00057150; // Credits::stepGoodEnding (true ending)
static const uintptr_t RVA_CREDITS_STEP     = 0x00055df0; // Credits::step (any credits/ending)
static const uintptr_t RVA_TRANSITION_LEVEL = 0x001c4e70; // GameEngine::transitionLevel
static const uintptr_t RVA_STEP_INTERMISSION = 0x001c5e80; // GameEngine::stepIntermission (ecx=GameEngine)
static const size_t    OFF_FLOORINDEX        = 0x3c0;      // GameEngine::floorIndex (int)
static const uintptr_t RVA_LOADNEXTLEVEL     = 0x001c23b0; // GameEngine::loadNextLevel(Level*)
// The `inc [edi+0x3c0]` (floorIndex++) inside loadNextLevel, where descending advances the
// floor. edi = GameEngine here. We wrap past the final floor to loop instead of winning.
static const uintptr_t RVA_FLOOR_INC         = 0x001c3d52; // inc dword [edi+0x3c0]  (6 bytes)
static const uintptr_t RVA_FLOOR_INC_RESUME  = 0x001c3d58; // next instruction after the inc
static const int       FINAL_FLOOR           = 12;         // observed last floor (win after this)
// The win-vs-continue decision in the Client state machine (edi=Client):
//   cmp [edi+0x4c5],0 ; je end   -- flag!=0 => initGameState (set up a run); flag==0 => loadNextLevel(NULL) (end game)
// We redirect the "end" branch to the "init" branch when looping, so a win restarts the run.
static const uintptr_t RVA_WIN_DECISION_JE   = 0x00160aec; // je 0x560b8b  (6 bytes)
static const uintptr_t RVA_WIN_INIT_PATH     = 0x00160af2; // init branch (je not taken)
static const uintptr_t RVA_WIN_END_PATH      = 0x00160b8b; // end branch  (je taken)
static const size_t    OFF_CLIENT_NEWGAMEPLUS = 0x318;     // Client+0x24(GameEngine)+0x2f4 = newGamePlus
// Same-character loop primitive, lifted from the devs' own debug warp in Client::handleDebugInput:
//   mov byte [Client+0x3ee],1 ; push 0 ; mov ecx,Client ; call Client::loadNextLevel(NULL)
// Client+0x3ee == GameEngine+0x3ca is the level-transition "mode" byte the loadNextLevel selector
// reads to regenerate the next dungeon floor *on the live party* (players persist across floor
// transitions, so this is a true same-character restart). We add: floorIndex=start + NG+.
static const uintptr_t RVA_CLIENT_LOADNEXTLEVEL = 0x00150a00; // Client::loadNextLevel(Level*) thiscall, ret 4
static const uintptr_t RVA_CLIENT_STEP          = 0x0015e500; // Client::step (per-frame, ecx=Client)
static const size_t    OFF_GE_WARPMODE          = 0x3ca;      // GameEngine+0x3ca (=Client+0x3ee) warp/advance byte
static const size_t    OFF_GE_INTERMISSION      = 0x3c8;      // GameEngine+0x3c8: !=0 => next loadNextLevel is a campfire intermission
static const size_t    OFF_CLIENT_GE            = 0x24;       // GameEngine subobject within Client
// Multiplayer: the HOST runs a separate authoritative Server (own thread) whose level transitions
// net-sync every client. The client-side loop only warps the local view, so MP needs the loop driven
// from the server. Server::step runs on the server thread (ecx=Server); Server::loadNextLevel(no-arg
// thiscall) regenerates the floor on the server GameEngine (Server+0xd20) AND syncs all clients.
static const uintptr_t RVA_SERVER_STEP          = 0x00364460; // Server::step (server thread, ecx=Server)
// Inside Server::step, immediately AFTER the server's GameEngine::step returns (the call @0x764496
// that sets gameComplete on the boss-win) and BEFORE the later sendGameState broadcast. ebx=Server
// here. Stealing the 7-byte `cmp byte[ebx+0x10ef],0` lets us catch the win pre-broadcast.
static const uintptr_t RVA_SERVER_POST_GESTEP   = 0x0036449b; // Server::step, just after GE::step
static const uintptr_t RVA_SERVER_LOADNEXTLEVEL = 0x00364120; // Server::loadNextLevel() thiscall, no args
static const size_t    OFF_SERVER_GE            = 0xd20;      // GameEngine subobject within Server
// Cutscene::trigger(Scene) __thiscall(ecx=this, [esp+4]=scene). BossFinal::hurt calls this to play the
// final-boss-defeat ending cutscene, which is the ONLY thing that sets Level::endOfCampaign (+0x1126)
// -> credits. Scene enum: 1=ELEVATOR, 2=FINAL_BOSS_DEFEATED, 3=FINAL_BOSS_DEFEATED_FINAL_FORM.
static const uintptr_t RVA_CUTSCENE_TRIGGER     = 0x0005ced0; // Cutscene::trigger(Scene)
static const int CUTSCENE_FINAL_BOSS_DEFEATED            = 2;
static const int CUTSCENE_FINAL_BOSS_DEFEATED_FINAL_FORM = 3;
static const int       LOOP_TEST_HOTKEY         = VK_F11;     // mid-run warp test (ini [loop] test_warp)
// NG+ difficulty scaling lives on the LevelGenerator (a GameEngine subobject), armed by
// LevelGenerator::setNewGamePlus() (sets this+0x50=1). initGameState normally calls it; the warp
// skips initGameState, so we call it directly so looped floors actually spawn harder.
static const uintptr_t RVA_SET_NEWGAMEPLUS      = 0x00295780; // LevelGenerator::setNewGamePlus() thiscall, no args
static const size_t    OFF_GE_LEVELGEN          = 0x2a4;      // LevelGenerator subobject within GameEngine
static const size_t    OFF_CLIENT_GAMESAVE      = 0xa744;     // GameSave subobject within Client
// The TRUE win signal: Client::step gates the ending on `cmp byte[Credits+0xf3b],0` (Credits* is
// at Client+0xab0c). This flag is set only when the boss is beaten / the ending begins — NOT at
// the pre-boss intermission (which shares floorIndex 12), so it's the unambiguous "won" trigger.
static const size_t    OFF_CLIENT_CREDITS       = 0xab0c;     // Credits* within Client
static const size_t    OFF_CREDITS_ACTIVE       = 0xf3b;      // Credits "ending active" flag (byte)
static const size_t    OFF_CREDITS_ACTIVE2      = 0xf3d;      // secondary ending flag checked after step
// Boss-win "victory" state to clear on loop so players aren't stuck invincible / auto-walking.
// Player::step enters the invincible victory walk when Player.gameOver(+0x800) is set; the win
// also sets GameEngine.gameOver/gameComplete/gameOverTimer. Players live in a vector<shared_ptr>
// at GameEngine+0x874..0x878 (8-byte elements; the Player* is the first 4 bytes of each).
static const size_t    OFF_GE_GAMEOVER          = 0x43a;
static const size_t    OFF_GE_GAMECOMPLETE       = 0x43b;
static const size_t    OFF_GE_GAMEOVERTIMER      = 0x43c;
static const size_t    OFF_GE_PLAYERS_BEGIN      = 0x874;
static const size_t    OFF_GE_PLAYERS_END        = 0x878;
static const size_t    OFF_GE_LEVEL              = 0x870;   // current Level*
static const size_t    PLAYER_VEC_STRIDE         = 8;
static const size_t    OFF_PLAYER_GAMEOVER        = 0x800;  // victory-walk flag (set every frame by GE::step)
static const size_t    OFF_PLAYER_INVULFRAMES     = 0x78d8; // hurt() skips damage while != 0
static const size_t    OFF_PLAYER_INCORPOREAL     = 0x78dc; // hurt() skips damage while != 0
static const size_t    OFF_PLAYER_IMMORTAL        = 0x13e;  // Entity::immortal: boss-death victory-walk immunity
// GameEngine::step's victory block re-stamps every player's gameOver each frame while the current
// level's victory-trigger byte (Level+0x1126) is set. Clearing that + the immunity timers ends it.
static const size_t    OFF_LEVEL_VICTORY          = 0x1126;
// Background re-enable (Wisp Curse, cut from the character-select list).
static const uintptr_t RVA_GET_AVAIL_BG  = 0x003bdcb0; // Stats::getAvailableBackgrounds(int,bool)
static const uintptr_t RVA_OP_NEW        = 0x004ccfa2; // operator new(size_t)
static const uintptr_t RVA_OP_DELETE     = 0x004ce566; // operator delete(void*)
static const int       BG_WISPCURSE      = 12;
static const uint32_t  ITEMTYPE_SPELLBOOK = 0xD8;
static const size_t    OFF_ITEMTYPE = 0x20C;
static const int       STEAL = 5;

static uintptr_t  g_base = 0;
static uintptr_t  g_realSetSkill = 0;   // VA of the genuine (un-hooked) Item::setSkill

// ---- the configurable spell set ----------------------------------------
struct SpellDef { int id; const char* key; const char* name; int def; };
static SpellDef g_spells[] = {
    {  0, "dash",            "Dash",            1 },
    {  1, "fire_shield",     "Fire Shield",     1 },
    {  2, "teleport",        "Teleport",        1 },
    {  3, "ice_bolt",        "Ice Bolt",        1 },
    {  4, "fireball",        "Fireball",        1 },
    {  5, "lightning",       "Lightning",       1 },
    {  6, "chain_lightning", "Chain Lightning", 1 },
    {  7, "spirits",         "Spirits",         1 },
    {  8, "magic_missile",   "Magic Missile",   1 },
    {  9, "shockwave",       "Shockwave",       1 },
    { 10, "elec_lance",      "Elec Lance",      1 },
    { 11, "frost_nova",      "Frost Nova",      1 },
    { 12, "flame_pillar",    "Flame Pillar",    1 },
    { 13, "dark_metamorph",  "Evil Transformation", 1 },
    { 14, "charm",           "Charm",           1 },
    { 15, "summon_monster",  "Summon Monster",  1 },
    { 17, "portal",          "Portal",          1 },  // re-enabled higher-slot spell
    { 25, "telekinesis",     "Telekinesis",     1 },  // re-enabled higher-slot spell
    { 26, "psychic_push",    "Psychic Push",    1 },  // re-enabled higher-slot spell
};
static const int N_SPELLS = sizeof(g_spells) / sizeof(g_spells[0]);

static bool g_enabled[N_SPELLS];   // mirrors g_spells, by index
static int  g_pool[N_SPELLS];      // enabled skill ids
static int  g_poolN = 0;
static char g_iniPath[MAX_PATH] = {0};
static bool g_wideningApplied = false;

static void host_dir_file(const char* fname, char* out) {
    GetModuleFileNameA(GetModuleHandleW(nullptr), out, MAX_PATH);
    char* slash = strrchr(out, '\\');
    if (slash) strcpy(slash + 1, fname); else strcpy(out, fname);
}
static char g_logTag[16] = "";   // test harness co-op runs: "[host] " / "[client] ", since both games share vultra.log
static void logmsg(const char* fmt, ...) {
    char path[MAX_PATH]; host_dir_file("vultra.log", path);
    FILE* f = fopen(path, "a"); if (!f) return;
    if (g_logTag[0]) fputs(g_logTag, f);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void rebuild_pool() {
    g_poolN = 0;
    for (int i = 0; i < N_SPELLS; i++) if (g_enabled[i]) g_pool[g_poolN++] = g_spells[i].id;
}

static uint32_t g_rng = 0;
static inline uint32_t rng_next() {
    if (!g_rng) g_rng = (uint32_t)__rdtsc() | 1u;
    uint32_t x = g_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; g_rng = x; return x;
}

// ---- the hook -----------------------------------------------------------
// Only the generation call site is redirected here (NOT setSkill's entry), so the
// re-roll happens exactly once, when a spellbook is generated. The chosen skill is
// stored in the item's randIndex; every later setSkill (load, pickup, setEntityState)
// re-applies that stored value verbatim -> drop & re-grab keeps the same spell.
static void reload_if_ini_changed();   // defined below
static void apply_random_tree_count(); // defined below

// Replacement for the rolled skill: a uniform pick from the enabled pool.
static int __cdecl pick_pool_skill(int rolled) {
    reload_if_ini_changed();   // pick up live edits from the manager / hand-edited ini
    int picked = (g_poolN > 0) ? g_pool[rng_next() % (uint32_t)g_poolN] : rolled;
    logmsg("[roll] gen spellbook: rolled=%d poolN=%d -> picked=%d", rolled, g_poolN, picked);  // DIAG
    return picked;
}

// ---- DIAGNOSTIC: log-only entry hook on setSkill (set DIAG 0 to remove) ----
#define DIAG 0       // set to 1 to log every setSkill (caller, before/after) for debugging
#define DIAG_WIN 1   // set to 1 to trace the win/new-game/save path (for wiring looping)
#if DIAG
typedef void (__fastcall *setSkill_t)(void* self, void* edx, int skill);
static setSkill_t g_ssTramp = nullptr;
static void __fastcall det_log_setSkill(void* self, void* edx, int skill) {
    void* ret = _ReturnAddress();
    uint8_t* s = reinterpret_cast<uint8_t*>(self);
    uint8_t ri = s ? s[0x280] : 0;
    uint32_t ty = s ? *reinterpret_cast<uint32_t*>(s + 0x20c) : 0;
    g_ssTramp(self, edx, skill);
    uint8_t after = s ? s[0x280] : ri;
    if (s && after != ri)
        logmsg("[ss] *CHANGED* item=%p caller_rva=0x%x type=0x%x randIndex 0x%02x->0x%02x  (skill4 %d->%d, skill5 %d->%d) new_arg=%d",
               self, (unsigned)((uintptr_t)ret - g_base), ty, ri, after, ri & 0xf, after & 0xf, ri & 0x1f, after & 0x1f, skill);
    else
        logmsg("[ss] same    item=%p caller_rva=0x%x randIndex=0x%02x skill4=%d skill5=%d new_arg=%d",
               self, (unsigned)((uintptr_t)ret - g_base), ri, ri & 0xf, ri & 0x1f, skill);
}
// Mark every Item::drop so we can correlate the drop event with any setSkill change by item ptr.
static void* g_dropTramp = nullptr;
static void __cdecl log_item_drop(void* item) {
    uint8_t* it = reinterpret_cast<uint8_t*>(item);
    logmsg("[drop] Item::drop item=%p type=0x%x randIndex=0x%02x (skill4=%d skill5=%d)",
           item, it ? *reinterpret_cast<uint32_t*>(it + 0x20c) : 0,
           it ? it[0x280] : 0, it ? it[0x280] & 0xf : 0, it ? it[0x280] & 0x1f : 0);
}
__declspec(naked) static void det_log_drop() {
    __asm {
        pushad
        push ecx                 // this (Item)
        call log_item_drop
        add  esp, 4
        popad
        jmp  dword ptr [g_dropTramp]
    }
}
#endif

// Stands in for `call setSkill` at RVA_GEN_SETSKILL_CALL.
// On entry: ecx = item (this), [esp+4] = rolled skill, [esp] = return addr (0x243d43).
// We overwrite the stack skill arg with the pool pick, then tail-jmp the real setSkill
// so it cleans its own arg (ret 4) and returns to the original caller.
__declspec(naked) static void thunk_genSetSkill() {
    __asm {
        pushad
        mov  eax, [esp+0x24]      // rolled skill (pushed before the call)
        push eax
        call pick_pool_skill
        add  esp, 4
        mov  [esp+0x24], eax      // replace with the pool pick
        popad
        jmp  dword ptr [g_realSetSkill]
    }
}

// ---- bulletproofing: sanitize the spellbook's re-derived skill ----------
// With 5-bit widening on (a high-slot spell enabled), the book's appearance bit (0x10)
// can collide with the widened skill read, so a book NOT generated under widening (old
// save / pre-toggle) could be misread as a higher spell number. setNameAndDescription
// re-derives the skill from randIndex on every refresh; we route that through here so an
// invalid 5-bit read falls back to the real 4-bit skill (the game then rewrites it clean).
static bool is_valid_skill5(int s) {
    if (s >= 0 && s < 16) return true;                       // ordinary spellbook spells
    for (int i = 0; i < N_SPELLS; i++) if (g_spells[i].id == s) return true;  // 17/25/26
    return false;
}
static int __cdecl sanitize_skill(int randIndex) {
    if (!g_wideningApplied) return randIndex & 0xf;          // 4-bit world: no high slots
    int low5 = randIndex & 0x1f;
    return is_valid_skill5(low5) ? low5 : (randIndex & 0xf);
}

// Stands in for `call setSkill` inside setNameAndDescription (RVA_NAMEDESC_SETSKILL_CALL).
// On entry: ecx = item (this), [esp+4] = the skill the game derived, [esp] = return addr.
// We recompute a sanitized skill from item->randIndex, overwrite the arg, tail-jmp setSkill.
__declspec(naked) static void thunk_nameDescSetSkill() {
    __asm {
        pushad
        mov  eax, [ecx+0x280]     // item->randIndex (ecx = this, still valid)
        push eax
        call sanitize_skill       // SAFE: idempotent; never changes an in-use book's spell
        add  esp, 4
        mov  [esp+0x24], eax      // overwrite the derived skill with the sanitized one
        popad
        jmp  dword ptr [g_realSetSkill]
    }
}

// ---- creation-only pool filter for shop / chest / boss spellbooks --------
// Floor spellbooks are pooled by the roll loop inside generateNewRandomSpellBook (hooked at
// RVA_GEN_SETSKILL_CALL). The other drop paths -- generateNewShopItem, generateNewRandomChestItem,
// generateNewBossRewardItem -- all funnel through ItemGenerator::generateNewRandomItemOfType but
// SKIP that loop, so their spellbook keeps an unfiltered random spell. We hook the single exit of
// generateNewRandomItemOfType: if it just built a spellbook (item type 0xd8), re-pick its spell
// from the pool -- exactly what the floor loop does, but on the freshly-built item only (never an
// in-use/carried book), so it can't desync a book mid-action the way the old display-path remap did.
// At the exit (0x243c4b) esi = the shared_ptr<Item> return buffer, so *(esi) = Item* (raw ptr).
typedef void (__fastcall *setSkill_fp)(void* self, void* edx, int skill);
static const uint32_t ITEM_TYPE_SPELLBOOK = 0xd8;   // Item type field at +0x20c
static uintptr_t g_genRandItemResume = 0;           // = g_base + 0x243c50 (instr after the stolen bytes)

static void __cdecl pool_filter_new_item(void* item) {
    if (!item || g_poolN <= 0) return;
    uint8_t* it = static_cast<uint8_t*>(item);
    if (*reinterpret_cast<uint32_t*>(it + 0x20c) != ITEM_TYPE_SPELLBOOK) return;  // only spellbooks
    int rolled = sanitize_skill(it[0x280]);                  // current effective spell id (for the log)
    int picked = pick_pool_skill(rolled);                    // uniform pool pick, same as the floor loop
    reinterpret_cast<setSkill_fp>(g_realSetSkill)(item, 0, picked);  // setSkill(item, picked) -- refreshes name/desc too
}

__declspec(naked) static void det_genRandItem() {
    __asm {
        pushad
        mov  eax, [esi]                 // Item* from the returned shared_ptr (esi = return buffer)
        push eax
        call pool_filter_new_item
        add  esp, 4
        popad
        mov  eax, esi                   // -- original stolen bytes --
        mov  ecx, dword ptr [ebp-0xc]
        jmp  dword ptr [g_genRandItemResume]
    }
}

// ---- patch helpers ------------------------------------------------------
static bool patch_jmp(uint8_t* at, void* dest) {
    DWORD oldp;
    if (!VirtualProtect(at, STEAL, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    at[0] = 0xE9;
    *reinterpret_cast<int32_t*>(at + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(dest) - (at + 5));
    VirtualProtect(at, STEAL, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), at, STEAL);
    return true;
}
static bool patch_site(const char* name, uintptr_t rva, const uint8_t* expect, int len, int off, uint8_t newb) {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + rva);
    if (memcmp(p, expect, len) != 0) { logmsg("[vultra] %s: mismatch @ %p - skipped", name, p); return false; }
    DWORD oldp;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    p[off] = newb;
    VirtualProtect(p, len, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}
// Replace `len` bytes at rva with newb[], but only if they currently match expect[].
static bool patch_replace(const char* name, uintptr_t rva, const uint8_t* expect, const uint8_t* newb, int len) {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + rva);
    if (memcmp(p, expect, len) != 0) { logmsg("[vultra] %s: mismatch @ %p - skipped", name, p); return false; }
    DWORD oldp;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    memcpy(p, newb, len);
    VirtualProtect(p, len, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}

// Mage option flags (read from the ini at startup in load_config; used by the detours below).
static bool g_wandSkillTree = false;     // mage: give the Wand skill tree instead of the Rod skill tree
static bool g_randomWandWeapon = false;  // mage: start with a random Wand weapon instead of the Rod
static bool g_wandZeroCharges = false;   // mage: the starting wand begins with 0 charges (more balanced)
static volatile bool g_randWeaponMatch = false;  // [player] random_match_weapon: Random char start weapon = first weapon skill tree
static volatile bool g_randNoTreeDefault = true; // [player] random_no_tree_default: no weapon tree -> default weapon (off = fists)
static volatile bool g_randArcheryBow = true;    // [player] random_archery_bow: Archery skill tree also grants a bow + 30 arrows
static volatile bool g_randHealth = false;       // [player] random_health: Random char starts with a randomized max HP (60..100)
// The Woodsman: a new class (Player::PlayerClass 7) that borrows the Houndmaster's look. See "Woodsman class".
static const int CLASS_BEASTMASTER = 4;          // the Houndmaster ("Ranger" in the game's files)
static const int CLASS_WOODSMAN    = 7;          // one past CLASS_RANDOM (6); the net state has 4 bits for it
static volatile int g_woodsmanAxeLevel = 0;      // [player] woodsman_axe_level: Axe tree starts at this level (0-4, testing)
static volatile int g_woodsmanBomberLevel = 0;   // [player] woodsman_bomber_level: Bomber tree starts at this level (0-4, testing)
static volatile int g_woodsmanBrutalistLevel = 0; // [player] woodsman_brutalist_level: Brutalist tree start level (0-4, testing)
static volatile int g_woodsmanScavengerLevel = 0; // [player] woodsman_scavenger_level: Scavenger tree start level (0-3, testing)
static volatile int g_startSkillPoints = 0;      // [player] start_skill_points: every new character starts with this many skill points (testing)
static volatile bool g_startRegens = false;      // [player] start_regens: 999 Potions of Regeneration at creation (testing)
static volatile bool g_startAllBooks = false;    // [player] start_all_spellbooks: one spellbook of each spell (testing)
static volatile uint32_t g_startWeaponCats = 0;  // [player] start_weapons_*: one of each weapon in those categories (bit per WEAPON_KIT entry, testing)
// Debug weapon kit categories: ini key, log name, item types (0-terminated). Bows also get 99 arrows.
static const struct WeaponKit { const char* key; const char* name; int types[10]; } WEAPON_KIT[] = {
    { "start_weapons_daggers", "daggers",        { 45, 46, 47 } },
    { "start_weapons_swords",  "swords",         { 49, 50, 51, 52, 53 } },
    { "start_weapons_clubs",   "clubs/hammers",  { 55, 56, 57, 58, 59, 60, 61, 62 } },     // + flail
    { "start_weapons_axes",    "axes",           { 64, 65 } },
    { "start_weapons_fists",   "fist weapons",   { 85, 86, 87 } },                         // caestus, claw, qatar
    { "start_weapons_bows",    "bows",           { 93, 94, 95 } },                         // short, long, crossbow
    { "start_weapons_wands",   "wands",          { 68, 69, 70, 71, 72, 73, 74, 75 } },
    { "start_weapons_rods",    "rods",           { 79, 80, 81 } },
    { "start_weapons_special", "special",        { 89, 90, 83, 96 } },                     // blink dagger, Thunder God hammer, spear, arm cannon
};
static const int N_WEAPON_KIT = sizeof(WEAPON_KIT) / sizeof(WEAPON_KIT[0]);
static volatile bool g_randWoodsmanTrees = false; // [player] random_woodsman_trees: Random chars can roll the Woodsman's trees
static const int ITEMTYPE_AXE_BROWN = 64;        // ITEMTYPE_AXE (the low-tier axe; AXE2 = 65 is the upgrade)
// [player] random_tree_count: how many skill trees (affinities) a Random character rolls. Stock is a
// hardcoded 4 in PlayerMenu::randomizeAffinities (it shuffles a 16-entry pool of distinct affinity
// types -- 0,1,2,3,5,11,12,13,14,15,16,18,19,20,21,25 -- and takes the first N). We rewrite that site,
// so 0..16 is valid. The roll happens on the machine of the player who picks Random, using THAT
// machine's ini; the rolled list is then sent to the host (Client::sendInputToServer: count byte +
// one byte per type), and the character is built from the list on both machines. For 0 trees see
// AFFINITY_NO_TREES below. The character-select UI is laid out for 4 slots (>4 is cosmetic only).
static volatile int g_randomTreeCount = 4;

// ---- Multiplayer: never stop new players from joining (and spectating) mid-game ----------
// Server::onClientBeginAuthentication has two overloads (the host picks one via byte[Server+0x78]).
// Each first rejects when the lobby is full ("Server is full."), then branches on the game-started
// flag byte[Server+0xd21] (= gameEngine+1): in the lobby it assigns every connector a fresh slot;
// once the run starts it only matches an existing SteamID (reconnect) and otherwise rejects a new
// joiner with "That game has already started.". We turn that one rejection into "assign a free slot"
// by redirecting each overload's reject-builder entry to its own assign-free-slot path. Reconnect
// matching is left untouched, so the existing rejoin still works; a brand-new joiner lands in a slot
// with no character = spectator (the behavior that already happens by accident in the join race).
// Applied at startup only (not live-toggleable): set the ini and restart the host.
static volatile bool g_midgameSpectators = false;  // [multiplayer] allow_midgame_spectators
// Keep the lobby advertised as joinable so non-modded clients still see the JOIN button mid-run.
// Server::updateLobbyInformation publishes lobby-data "joinable"="1" only while both [Server+0x10e8]
// and [Server+0x10e9] are set; once the run starts one clears and it publishes "joinable"="" (stock
// clients' lobby browser then hides JOIN). NOP the two je's that take the empty-string path so the
// host always advertises "joinable"="1". Host-side + respects the host's public/friends privacy
// (SetLobbyType is unchanged); a genuinely full lobby still answers "Server is full." on join.
static volatile bool g_keepLobbyJoinable = false;  // [multiplayer] keep_lobby_joinable
static volatile bool g_portalCoop = true;          // [multiplayer] portal_coop (declared early; used in load_config)
static volatile bool g_portalTwoWay = true;        // [multiplayer] portal_two_way (both ends interactable, teleport to the opposite end); default on
static void* g_specPathA1 = nullptr;   // overload1 assign-free-slot entry (g_base + 0x36b3dd)
static void* g_specPathA2 = nullptr;   // overload2 assign-free-slot entry (g_base + 0x36bc52)
static volatile long g_specLogged1 = 0, g_specLogged2 = 0;
static void __cdecl spec_admit_log(int which) {
    if (which == 1) { if (g_specLogged1) return; g_specLogged1 = 1; }
    else            { if (g_specLogged2) return; g_specLogged2 = 1; }
    logmsg("[midspec] admitted a new mid-game joiner to a free slot via auth overload %d "
           "(was 'That game has already started.')", which);
}
// overload1's reject builder is reached with ebx=Server; its path A re-inits its own slot index, so
// we just jump there. (eax is clobbered, but path A overwrites eax with its first instruction.)
__declspec(naked) static void stub_spec_ov1() {
    __asm {
        pushad
        push 1
        call spec_admit_log
        add  esp, 4
        popad
        mov  eax, dword ptr [g_specPathA1]
        jmp  eax
    }
}
// overload2's reject builder is reached with edi=Server and esi = the slot-scan index it left off at;
// its path A expects esi=0, so zero it before jumping. (eax clobber is safe, same as above.)
__declspec(naked) static void stub_spec_ov2() {
    __asm {
        pushad
        push 2
        call spec_admit_log
        add  esp, 4
        popad
        xor  esi, esi
        mov  eax, dword ptr [g_specPathA2]
        jmp  eax
    }
}

// ---- Mage Rod->Wand options (all LIVE-toggleable; see sync_mage_patches) -
// All three apply at the NEXT character creation. The detour reads its flag live; the two byte-patch
// options are (re)applied/reverted to match the live flag in sync_mage_patches(), which runs whenever
// the ini changes -- so toggling in the manager + re-creating the mage works with no game restart.

// (1) Random Wand weapon. setStartingItems builds the mage's (class 0x10) weapon as
// `mov dword[ebp-0x6c], ITEMTYPE_ROD(0x4f)` @0x30b3b1. The detour (installed unconditionally) writes
// either a random wand type or, when the flag is off, ITEMTYPE_ROD(0x4f) -- so it's vanilla when off.
static uintptr_t g_mageWandResume = 0;
static int __cdecl pick_mage_weapon_type() {
    if (!g_randomWandWeapon) return 0x4f;          // ITEMTYPE_ROD (vanilla mage weapon)
    static uint32_t s = 0;
    if (!s) s = GetTickCount() ^ 0x9e3779b9u;
    s = s * 1103515245u + 12345u;
    return 0x44 + (int)((s >> 16) % 8);            // ITEMTYPE_WAND(68) .. ITEMTYPE_WAND8(75)
}
// ---- Random-class: start with the weapon for your first weapon skill tree ----
// When a "Random" character is rolled the game resolves it to a concrete class (Player+0x350
// randomizedClass=1) and hands out that class's fixed starting weapon. With g_randWeaponMatch on,
// we instead give the weapon matching the player's FIRST weapon affinity (skill tree). Each class
// branch of setStartingItems writes its weapon ItemType to a frame local just before make_shared;
// we detour that write (mage-style) and substitute the affinity weapon. The affinities live in a
// std::vector<Affinity*> at Player+0x870(begin)/+0x874(end); Affinity::affinityType is at +0x4.
// Wand tree start weapon: any of the 7 wands except the Wand of Healing (75).
static int random_start_wand() {
    static uint32_t s = 0;
    if (!s) s = GetTickCount() ^ 0x85ebca6bu;
    s = s * 1103515245u + 12345u;
    return 68 + (int)((s >> 16) % 7);              // ITEMTYPE_WAND(68) .. ITEMTYPE_WAND7(74)
}
static int affinity_to_weapon_type(int at) {
    switch (at) {
        // ARCHERY(2) is handled separately (bow + arrows, additive) -- see give_bow_and_arrows.
        case 12: return 45;   // DAGGER  -> ITEMTYPE_DAGGER
        case 13: return 49;   // SWORD   -> ITEMTYPE_SWORD
        case 14: return random_start_wand();   // WAND -> any wand but Healing
        case 18: return 85;   // CQC     -> ITEMTYPE_CAESTUS (fists)
        case 21: return 79;   // ROD     -> ITEMTYPE_ROD
        case 25: return 55;   // CLUB    -> ITEMTYPE_CLUB
        case 26: return 64;   // AXE (the mod's tree) -> ITEMTYPE_AXE
        default: return -1;   // not a weapon skill tree
    }
}
// Weapon ItemType of the player's first weapon skill tree, or -1 if they rolled none.
static int first_weapon_affinity_type(void* player) {
    void** begin = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(player) + 0x870);
    void** end   = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(player) + 0x874);
    if (!begin || !end || end <= begin) return -1;
    for (void** p = begin; p < end; ++p) {
        void* a = *p;
        if (!a) continue;
        int wt = affinity_to_weapon_type(*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(a) + 4));
        if (wt >= 0) return wt;   // first weapon skill tree wins
    }
    return -1;
}
// Returns the weapon ItemType to actually create. Off / non-random / no weapon affinity -> defaultType.
static int __cdecl pick_random_class_weapon(void* player, int defaultType) {
    if (!g_randWeaponMatch || !player) return defaultType;
    if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(player) + 0x350) == 0) return defaultType; // not a Random char
    int wt = first_weapon_affinity_type(player);
    if (wt >= 0) {
        logmsg("[randwpn] random-class weapon: default=%d -> %d", defaultType, wt);
        return wt;
    }
    // No weapon skill tree: use the class default weapon, or (toggle off) start with fists.
    int noTree = g_randNoTreeDefault ? defaultType : 85;   // 85 = ITEMTYPE_CAESTUS (fists)
    logmsg("[randwpn] random-class no weapon tree: default=%d -> %d", defaultType, noTree);
    return noTree;
}

__declspec(naked) static void det_mageWand() {
    __asm {
        push eax
        push ecx
        push edx
        call pick_mage_weapon_type            // eax = ITEMTYPE_ROD or a random wand type (the default)
        push eax                              // defaultType
        push dword ptr [ebp+8]                // player (this)
        call pick_random_class_weapon         // eax = affinity weapon (random char) or the default
        add  esp, 8
        mov  dword ptr [ebp-0x6c], eax         // overwrite the weapon type in the game's frame
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_mageWandResume]      // resume just past the replaced 7-byte mov
    }
}
// The other concrete classes hand out a fixed weapon; with the toggle on, a Random character of that
// class gets their first-weapon-affinity weapon instead. Each detour reads the player (setStartingItems
// arg at [ebp+8]), passes the class's vanilla weapon as the default, and writes the chosen type back to
// that branch's frame local before resuming past the replaced mov.
static uintptr_t g_knightWpnResume = 0, g_rogueWpnResume = 0, g_deprivedWpnResume = 0, g_beastWpnResume = 0;
__declspec(naked) static void det_knightWeapon() {        // KNIGHT: mov [ebp-0x84], 0x31 (SWORD), 10 bytes
    __asm {
        push eax
        push ecx
        push edx
        push 0x31
        push dword ptr [ebp+8]
        call pick_random_class_weapon
        add  esp, 8
        mov  dword ptr [ebp-0x84], eax
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_knightWpnResume]
    }
}
__declspec(naked) static void det_rogueWeapon() {         // ROGUE: mov [ebp-0x8c], 0x2d (DAGGER), 10 bytes
    __asm {
        push eax
        push ecx
        push edx
        push 0x2d
        push dword ptr [ebp+8]
        call pick_random_class_weapon
        add  esp, 8
        mov  dword ptr [ebp-0x8c], eax
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_rogueWpnResume]
    }
}
__declspec(naked) static void det_deprivedWeapon() {      // DEPRIVED: mov [ebp-0x6c], 0x55 (CAESTUS), 7 bytes
    __asm {
        push eax
        push ecx
        push edx
        push 0x55
        push dword ptr [ebp+8]
        call pick_random_class_weapon
        add  esp, 8
        mov  dword ptr [ebp-0x6c], eax
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_deprivedWpnResume]
    }
}
__declspec(naked) static void det_beastWeapon() {         // BEASTMASTER: mov [ebp-0x6c], 0x37 (CLUB), 7 bytes
    __asm {
        push eax
        push ecx
        push edx
        push 0x37
        push dword ptr [ebp+8]
        call pick_random_class_weapon
        add  esp, 8
        mov  dword ptr [ebp-0x6c], eax
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_beastWpnResume]
    }
}
// addRandomizedPlayer builds the character via initializePlayer (which runs setStartingItems and creates
// the weapon) and only sets randomizedClass (Player+0x350)=1 AFTERWARD, so our weapon detours never saw
// the flag. We redirect that `call initializePlayer` (@0x308a3f, player ptr in edi) through this thunk,
// which sets the flag FIRST, then tail-jmps to the real initializePlayer (same stack -> returns directly
// to addRandomizedPlayer). The game re-sets +0x350 later, so this is just an earlier, harmless write.
static uintptr_t g_initPlayerReal = 0;
__declspec(naked) static void det_initRandomized() {
    __asm {
        mov  byte ptr [edi+0x350], 1
        jmp  dword ptr [g_initPlayerReal]
    }
}
// ---- Random-class: randomized max HP (60..100), forced every frame after Player::updateStats ----
// Earlier attempts (writing maxHp once at creation, or editing the base vitality stat) didn't stick: the
// real gameplay player object is (re)built with default class stats after our creation hooks, and the
// per-frame Player::updateStats (rva 0x2cb380, __thiscall(self,a,b)) recomputes maxHp (Entity+0x110) each
// frame. The reliable approach is to WRAP updateStats and overwrite maxHp afterwards for a Random char.
// We roll a target HP once per player object (first time we see it), keep it for the run, and pin maxHp to
// it every frame; hp is set to full only on the first roll, then clamped to <= maxHp. randomizedClass
// (+0x350) is set on the gameplay player too (confirmed via probe), so this targets only Random chars.
// Tradeoff: a leveled-up vitality point won't raise the cap (we pin it) -- it's a fixed random max per run.
struct HpSlot { void* player; float target; int fillFrames; };
static HpSlot g_hpSlots[8] = {};
static float roll_rand_hp() {
    static uint32_t s = 0;
    if (!s) s = GetTickCount() ^ 0x9e3779b9u;
    s = s * 1103515245u + 12345u;
    return (float)(60 + (int)((s >> 16) % 41));      // 60..100 inclusive (1-HP granularity)
}
// Returns the slot for this player, rolling a fresh target (and a short hp-fill window) the first time.
static HpSlot* hp_slot_for(void* player) {
    for (HpSlot& sl : g_hpSlots) if (sl.player == player) return &sl;
    HpSlot* sl = nullptr;
    for (HpSlot& s2 : g_hpSlots) if (s2.player == nullptr) { sl = &s2; break; }
    if (!sl) sl = &g_hpSlots[(reinterpret_cast<uintptr_t>(player) >> 4) & 7];  // table full: evict by hash
    sl->player = player;
    sl->target = roll_rand_hp();
    sl->fillFrames = 8;                              // re-fill hp for a few frames to beat spawn ordering
    logmsg("[randhp] rolled random-class maxHp=%.0f for player=%p", sl->target, player);
    return sl;
}
typedef void (__thiscall *updateStats_t)(void* self, int a, int b);
static updateStats_t g_updateStatsTramp = nullptr;   // trampoline to the real Player::updateStats
static void woodsman_stats(uint8_t* p);             // Brutalist/Scavenger per-frame stats (below)
static void __fastcall det_updateStats(void* self, void* /*edx*/, int a, int b) {
    g_updateStatsTramp(self, a, b);                  // run the real stat recompute first
    if (!self) return;
    int cls = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + 0x34c);
    if (cls < 0 || cls > 7) return;                                           // sanity: a real player
    float* maxHp = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x110);
    if (*maxHp < 5.0f) return;                                                // skip uninitialized/doll objects
    if (g_randHealth && *(reinterpret_cast<uint8_t*>(self) + 0x350) != 0 && cls <= 6) {   // Random chars only
        HpSlot* sl = hp_slot_for(self);
        *maxHp = sl->target;                                                  // pin max HP every frame
        float* hp = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x10c);
        if (sl->fillFrames > 0) { *hp = sl->target; sl->fillFrames--; }       // start full (re-fill window)
        else if (*hp > sl->target) *hp = sl->target;                         // otherwise just clamp to max
    }
    woodsman_stats(static_cast<uint8_t*>(self));     // after the pin, so Scavenger's +5s add on top
}
// The DEPRIVED branch only builds its weapon (caestus) when background(+0x824)==0x12; otherwise it
// `jne 0x70b667` and the deprived ends up unarmed -- so for most backgrounds there's no weapon for our
// type-detour to override. We replace that jne: for a Random char (with match on) always fall through
// and create the weapon (our det_deprivedWeapon then sets the affinity type); otherwise keep vanilla
// behavior (jump when background != 0x12). The C helper recomputes the branch so we don't juggle flags.
static uintptr_t g_deprivedGateResume = 0;   // 0x30c145: create the weapon
static uintptr_t g_deprivedGateSkip   = 0;   // 0x70b667: original "skip" target
static volatile long g_depJump = 0;
static int __cdecl should_jump_deprived(void* player) {
    if (g_randWeaponMatch && player && *(reinterpret_cast<uint8_t*>(player) + 0x350) != 0) {
        if (first_weapon_affinity_type(player) >= 0) return 0;   // has a weapon tree -> create it
        // No weapon tree: the deprived's true default IS no weapon, so "use default" leaves it unarmed
        // (jump/skip). Only the explicit fists fallback (toggle off) creates the caestus.
        return g_randNoTreeDefault ? 1 : 0;
    }
    if (!player) return 1;
    return (*(reinterpret_cast<uint8_t*>(player) + 0x824) != 0x12) ? 1 : 0;                          // vanilla
}
__declspec(naked) static void det_deprivedGate() {
    __asm {
        pushad
        mov  eax, [ebp+8]            // player
        push eax
        call should_jump_deprived
        add  esp, 4
        mov  g_depJump, eax
        popad
        cmp  dword ptr g_depJump, 0
        jne  dep_skip
        jmp  dword ptr [g_deprivedGateResume]
    dep_skip:
        jmp  dword ptr [g_deprivedGateSkip]
    }
}

// The Deprived branch then gives whatever weapon it made three ItemAttributes (elec 0x29, fire 0x2b,
// ice 0x2a, via Item::addAttribute) -- meant for the enchanted-caestus background (0x12), but a Random
// char's matched weapon (sword, wand, axe...) got them too. Detour at 0x30c1b2 (the first attribute's
// `push 0xd0`): keep them only for background 0x12 with the Caestus, else jump past all three (0x30c236).
static uintptr_t g_depEnchKeep = 0, g_depEnchSkip = 0;
static volatile long g_depEnch = 0;
static int __cdecl keep_deprived_enchants(void* player, void* item) {
    if (!player || !item) return 1;
    const bool bg = *(reinterpret_cast<uint8_t*>(player) + 0x824) == 0x12;
    const int type = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(item) + 0x20c);
    if (bg && type == 85) return 1;                       // ITEMTYPE_CAESTUS: the background's own weapon
    logmsg("[randwpn] deprived: no elemental enchants on weapon type %d (background %d)", type,
           *(reinterpret_cast<uint8_t*>(player) + 0x824));
    return 0;
}
__declspec(naked) static void det_deprivedEnchants() {
    __asm {
        pushad
        push ebx                     // the weapon Item*
        push dword ptr [ebp+8]       // player
        call keep_deprived_enchants
        add  esp, 8
        mov  g_depEnch, eax
        popad
        cmp  dword ptr g_depEnch, 0
        je   ench_skip
        push 0xd0                    // the stolen instruction
        jmp  dword ptr [g_depEnchKeep]
    ench_skip:
        jmp  dword ptr [g_depEnchSkip]
    }
}

// (2) Wand skill tree. setStartingAffinities builds AffinityRod; AffinityWand is built identically
// (same 0x1e8 size, same base ctor) so we swap four values. The vtable is an ABSOLUTE address the
// loader RELOCATES, so compare/write it relative to g_base. Idempotent + reversible: writes whichever
// of the two known states matches `wand`, only if currently in the other recognized state.
static void set_mage_affinity(bool wand) {
    uint8_t* p32c = reinterpret_cast<uint8_t*>(g_base + 0x0030a32c);  // push <ctor arg2>
    uint8_t* p32e = reinterpret_cast<uint8_t*>(g_base + 0x0030a32e);  // push <affinity type>
    uint8_t* p33b = reinterpret_cast<uint8_t*>(g_base + 0x0030a33b);  // mov [edi], <vtable>
    uint8_t* p341 = reinterpret_cast<uint8_t*>(g_base + 0x0030a341);  // push <tail arg>
    uint32_t rodVtbl  = static_cast<uint32_t>(g_base + 0x00716604);
    uint32_t wandVtbl = static_cast<uint32_t>(g_base + 0x0071a820);
    if (p33b[0] != 0xC7 || p33b[1] != 0x07) return;                  // not the construction site
    uint32_t cur = *reinterpret_cast<uint32_t*>(p33b + 2);
    if (cur != rodVtbl && cur != wandVtbl) return;                   // unrecognized -> never touch
    if ((cur == wandVtbl) == wand) return;                           // already in the target state
    DWORD o;
    uint8_t* r = reinterpret_cast<uint8_t*>(g_base + 0x0030a32c);
    if (!VirtualProtect(r, 0x1c, PAGE_EXECUTE_READWRITE, &o)) return;
    if (wand) { p32c[1]=0x04; p32e[1]=0x0E; *reinterpret_cast<uint32_t*>(p33b+2)=wandVtbl; p341[1]=0x0A; }
    else      { p32c[1]=0x05; p32e[1]=0x15; *reinterpret_cast<uint32_t*>(p33b+2)=rodVtbl;  p341[1]=0x14; }
    VirtualProtect(r, 0x1c, o, &o);
    FlushInstructionCache(GetCurrentProcess(), r, 0x1c);
}

// (3) Zero starting charges. The mage's weapon is filled to full at 0x30b467 via
// `mov al,[edi+0x52d](maxWandCharges)`. Swap the source to `mov al,0` so the fill writes 0 (flag-safe:
// the following jne reads earlier flags, so we never use a flag-affecting op). Idempotent + reversible.
static void set_mage_wand_zero(bool zero) {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + 0x0030b467);
    bool isFill = (p[0]==0x8A && p[1]==0x87 && p[2]==0x2D);          // mov al,[edi+0x52d]
    bool isZero = (p[0]==0xB0 && p[1]==0x00);                        // mov al,0
    if (!isFill && !isZero) return;                                 // unrecognized -> never touch
    if (isZero == zero) return;                                     // already in target state
    DWORD o;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &o)) return;
    if (zero) { const uint8_t n[]={0xB0,0x00,0x90,0x90,0x90,0x90}; memcpy(p,n,6); }
    else      { const uint8_t n[]={0x8A,0x87,0x2D,0x05,0x00,0x00}; memcpy(p,n,6); }
    VirtualProtect(p, 6, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
}

// Re-sync the two byte-patch mage options to the current flags (the weapon detour reads its flag live).
// Called at install and on every live ini reload, so manager toggles take effect on the next mage.
static void sync_mage_patches() {
    set_mage_affinity(g_wandSkillTree);
    set_mage_wand_zero(g_wandZeroCharges);
}

// Repoint an existing `call rel32` (opcode E8) to a new target, after verifying its
// current target is the expected function. Keeps the opcode (still a call), so the
// callee returns to the original site.
static bool redirect_call(uintptr_t rva, uintptr_t expectTarget, void* newTarget) {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + rva);
    if (p[0] != 0xE8) { logmsg("[vultra] redirect_call: %p is not a call (0x%02X)", p, p[0]); return false; }
    uintptr_t cur = reinterpret_cast<uintptr_t>(p + 5) + *reinterpret_cast<int32_t*>(p + 1);
    if (cur != expectTarget) { logmsg("[vultra] redirect_call: %p targets %p, expected %p", p, (void*)cur, (void*)expectTarget); return false; }
    DWORD oldp;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(newTarget) - (p + 5));
    VirtualProtect(p, 5, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
static void apply_widening_if_needed() {
    if (g_wideningApplied) return;
    bool need = false;
    for (int i = 0; i < N_SPELLS; i++) if (g_enabled[i] && g_spells[i].id >= 16) need = true;
    if (!need) return;
    const uint8_t cap_e[] = {0x83,0xFF,0x0F}, chk_e[] = {0x24,0x0F};
    const uint8_t rnd_e[] = {0x81,0xE1,0x0F,0x00,0x00,0x80}, sgn_e[] = {0x83,0xC9,0xF0}, snd_e[] = {0x24,0x0F};
    bool ok = true;
    ok &= patch_site("skill cap",       RVA_SET_SKILL_CAP, cap_e, 3, 2, 0x1D);
    ok &= patch_site("setSkill mask",   0x00223796,        chk_e, 2, 1, 0x1F);
    ok &= patch_site("store randmask",  0x002237a5,        rnd_e, 6, 2, 0x1F);
    ok &= patch_site("store signfix",   0x002237ae,        sgn_e, 3, 2, 0xE0);
    ok &= patch_site("setNameDesc mask",0x00222556,        snd_e, 2, 1, 0x1F);
    g_wideningApplied = ok;
    logmsg("[vultra] skill-field widening %s", ok ? "applied" : "FAILED");
}

// ---- config -------------------------------------------------------------
static bool g_wispCurse = true;          // re-enable the Wisp Curse background
static int  g_replaceBg = 5;             // sacrifice background to overwrite (5 = Illiterate)
static volatile bool g_forceWispCurse = false;  // force the Wisp Curse floor effect for everyone (host)
static volatile int  g_fairyMode = 0;           // caged fairy: 0=Default(2/5/8/11), 1=None, 2=All levels
static bool g_loop = false;              // force New Game Plus ("The Cycle") scaling
static int  g_startDifficulty = 2;       // run starts at this difficulty tier: 1=normal first playthrough,
                                         // 2=first loop (NG+ on), 3+=NG+ plus our extra scaling. Default 2.
static bool g_loopTestWarp = false;      // F11 = warp-restart now (mechanism test; off in releases)
static int  g_loopStartFloor = 0;        // floorIndex the loop restarts at (0 = start)
static int  g_warpFloor = 0;             // F11 warp destination floor (separate from the loop restart)
static bool g_campfireOnWrap = false;    // route the loop restart through a campfire intermission first
// (mage option flags g_wandSkillTree / g_randomWandWeapon / g_wandZeroCharges declared earlier)
static int  g_loopFinalFloor = 13;       // loop only when leaving a floor >= this (boss-win = 13)
static float g_harderByPct = 50.0f;      // escalating loops: extra monster HP & damage % added per difficulty tier (host-side)
static int  g_scalingStartsAt = 3;       // difficulty tier where the extra (beyond-NG+) monster scaling begins
static volatile int g_spawnIncreasePct      = 0;  // +N% enemies per level per loop: DUPLICATE each enemy (host)
static volatile int g_spawnIncreaseStartsAt = 3;  // loop number both enemy increases begin at
static volatile int g_spawnIncreaseMaxPct   = 0;  // cap on the escalated DUPLICATE increase (0 = no cap)
static volatile int g_spawnSpreadPct        = 0;  // +N% per loop: SPREAD via cheaper spawn weight (more distinct spawns)
static volatile int g_spawnSpreadStartsAt   = 3;  // loop number the SPREAD increase begins at (independent of duplicate)
static volatile int g_spawnSpreadMaxPct     = 0;  // cap on the escalated SPREAD increase (0 = no cap)
static volatile bool g_cursedBomb           = false;  // [player] god_cursed_bomb: spawn each player with a cursed bomb
// g_randWeaponMatch declared up with the mage weapon flags (used by det_mageWand)
static volatile long g_loopCount = 0;    // loops completed THIS run (0 on a fresh run); loop number = g_loopCount + 1
static volatile int  g_fairyStack = 0;        // stack campfire fairy blessings: +5 max HP per FAIRY, not per biome
static volatile long g_fairyBlessCount = 0;   // total fairies blessed THIS run; drives the campfire bonus when stacking
static volatile int  g_fairyHeal = 5;         // max HP each fairy adds at the campfire (vanilla 5)
static volatile int  g_fairyHealDelta = 0;    // = g_fairyHeal - 5; added per fairy after the vanilla *5 (0 = vanilla)
// Loop numbering: 1 = first playthrough, 2 = first loop (after one boss-win), 3 = second loop, ...
// g_loopCount = loops completed (0 on a fresh run), so the current loop number is g_loopCount + 1.
// The two difficulty thresholds are independent loop numbers: NG+ turns on at g_startDifficulty, the
// extra monster scaling at g_scalingStartsAt. Defaults (2 / 3): loop 1 normal, loop 2 NG+, loop 3+ scaling.
static long current_loop_number() { return (long)g_loopCount + 1; }
static bool ngplus_active()        { return current_loop_number() >= (long)g_startDifficulty; }
static FILETIME g_iniMtime = {0, 0};
static bool ini_mtime(FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &d)) return false;
    *out = d.ftLastWriteTime; return true;
}
// ---- Host settings sync: config overrides --------------------------------
// In multiplayer the host publishes its settings in the Steam lobby (see "Host settings sync" below),
// and a modded client with the same VULTRA_VERSION uses them in place of its own ini. The client's
// ini file is never written. Every setting load_config reads goes through cfg_int / cfg_str:
//   - while overrides are active (client in a modded host's lobby), a synced key returns the host's
//     value; everything else still comes from the local ini.
//   - every read is also recorded into g_cfgSnap (key -> effective value), which is exactly what a
//     host publishes. So the synced-key list is "whatever load_config reads", minus g_cfgNoSync.
//   Keys are unique across sections (the manager relies on that too), so overrides key on name only.
static volatile DWORD g_lastServerStep = 0;   // updated every Server::step; recent => this instance is the MP host
static void* volatile g_lastServer = nullptr;  // the Server that ran that step (its GameEngine is the authoritative one)
static const char* const VULTRA_VERSION = "1.5.0-dev";   // bump on any change to what syncs or how
struct CfgKV { char key[40]; char val[24]; };
static const int CFG_MAX = 128;
static CfgKV g_cfgSnap[CFG_MAX]; static int g_cfgSnapN = 0;   // last load_config's effective values
static CfgKV g_cfgOvr[CFG_MAX];  static int g_cfgOvrN = 0;    // host's values (client only)
static volatile bool g_cfgOvrActive = false;
static SRWLOCK g_cfgLock = SRWLOCK_INIT;                      // guards load_config + both tables
static volatile bool g_hostSync = true;                       // [multiplayer] host_settings_sync
// Never synced: debug hotkeys, host-only startup options, and the sync switch itself.
static const char* const g_cfgNoSync[] = {
    "test_warp", "warp_floor", "allow_midgame_spectators", "keep_lobby_joinable", "host_settings_sync",
};
static bool cfg_is_synced(const char* key) {
    for (const char* k : g_cfgNoSync) if (_stricmp(k, key) == 0) return false;
    return true;
}
static const char* cfg_ovr_find(const char* key) {
    if (!g_cfgOvrActive || !cfg_is_synced(key)) return nullptr;
    for (int i = 0; i < g_cfgOvrN; i++) if (_stricmp(g_cfgOvr[i].key, key) == 0) return g_cfgOvr[i].val;
    return nullptr;
}
static void cfg_snap_put(const char* key, const char* val) {
    if (!cfg_is_synced(key) || g_cfgSnapN >= CFG_MAX) return;
    CfgKV& e = g_cfgSnap[g_cfgSnapN++];
    strncpy_s(e.key, key, _TRUNCATE); strncpy_s(e.val, val, _TRUNCATE);
}
// Same argument order as GetPrivateProfileIntA / GetPrivateProfileStringA, so load_config reads alike.
static UINT cfg_int(const char* sec, const char* key, int def, const char* path) {
    const char* o = cfg_ovr_find(key);
    int v = o ? atoi(o) : (int)GetPrivateProfileIntA(sec, key, def, path);
    char b[16]; sprintf_s(b, "%d", v); cfg_snap_put(key, b);
    return (UINT)v;
}
static void cfg_str(const char* sec, const char* key, const char* def, char* out, DWORD n, const char* path) {
    const char* o = cfg_ovr_find(key);
    if (o) strncpy_s(out, n, o, _TRUNCATE);
    else   GetPrivateProfileStringA(sec, key, def, out, n, path);
    cfg_snap_put(key, out);
}

static void load_config() {
    g_cfgSnapN = 0;
    host_dir_file("vultramod.ini", g_iniPath);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath);  // flush Win ini cache -> read fresh
    for (int i = 0; i < N_SPELLS; i++)
        g_enabled[i] = cfg_int("spellbook_pool", g_spells[i].key, g_spells[i].def, g_iniPath) != 0;
    g_wispCurse = cfg_int("backgrounds", "wisp_curse", 0, g_iniPath) != 0;  // opt-in
    g_replaceBg = (int)cfg_int("backgrounds", "replace_bg", 5, g_iniPath);
    g_forceWispCurse = cfg_int("backgrounds", "force_wisp_curse", 0, g_iniPath) != 0;  // opt-in (host)
    g_fairyMode = (int)cfg_int("npc", "fairy_mode", 0, g_iniPath);             // 0=Default, 1=None, 2=All levels
    g_fairyStack = cfg_int("npc", "fairy_stack_blessing", 0, g_iniPath) != 0;  // +5 max HP per fairy (host)
    g_fairyHeal = (int)cfg_int("npc", "fairy_heal", 5, g_iniPath);              // max HP each fairy adds (host)
    if (g_fairyHeal < 0)    g_fairyHeal = 0;
    if (g_fairyHeal > 1000) g_fairyHeal = 1000;
    g_fairyHealDelta = g_fairyHeal - 5;
    g_loop = cfg_int("loop", "new_game_plus", 0, g_iniPath) != 0;            // opt-in
    g_startDifficulty = (int)cfg_int("loop", "start_difficulty", 2, g_iniPath);  // run starts at this tier (1..100)
    if (g_startDifficulty < 1)   g_startDifficulty = 1;
    if (g_startDifficulty > 100) g_startDifficulty = 100;
    g_loopTestWarp  = cfg_int("loop", "test_warp", 0, g_iniPath) != 0;       // dev/test only (F11 warp)
    g_loopStartFloor = (int)cfg_int("loop", "loop_start_floor", 0, g_iniPath);  // where the loop restarts
    g_warpFloor      = (int)cfg_int("loop", "warp_floor", 0, g_iniPath);        // F11 destination
    g_campfireOnWrap = cfg_int("loop", "campfire_on_wrap", 0, g_iniPath) != 0;  // campfire before restart
    g_loopFinalFloor = (int)cfg_int("loop", "final_floor", 13, g_iniPath);
    {   // escalating loops: "harder by" is a float percent; read as a string and parse
        char buf[32];
        cfg_str("loop", "loop_scale_pct", "50", buf, sizeof(buf), g_iniPath);
        g_harderByPct = (float)atof(buf);
        if (g_harderByPct < 0.0f)    g_harderByPct = 0.0f;
        if (g_harderByPct > 1000.0f) g_harderByPct = 1000.0f;
    }
    g_scalingStartsAt = (int)cfg_int("loop", "scaling_starts_at", 3, g_iniPath);  // tier the extra scaling begins
    if (g_scalingStartsAt < 1)   g_scalingStartsAt = 1;
    if (g_scalingStartsAt > 100) g_scalingStartsAt = 100;
    g_spawnIncreasePct = (int)cfg_int("loop", "spawn_increase_pct", 0, g_iniPath);   // +N% enemies/level/loop
    if (g_spawnIncreasePct < 0)     g_spawnIncreasePct = 0;
    if (g_spawnIncreasePct > 10000) g_spawnIncreasePct = 10000;
    g_spawnIncreaseStartsAt = (int)cfg_int("loop", "spawn_increase_starts_at", 3, g_iniPath);
    if (g_spawnIncreaseStartsAt < 1)   g_spawnIncreaseStartsAt = 1;
    if (g_spawnIncreaseStartsAt > 100) g_spawnIncreaseStartsAt = 100;
    g_spawnIncreaseMaxPct = (int)cfg_int("loop", "spawn_increase_max_pct", 0, g_iniPath);  // 0 = no cap
    if (g_spawnIncreaseMaxPct < 0)      g_spawnIncreaseMaxPct = 0;
    if (g_spawnIncreaseMaxPct > 100000) g_spawnIncreaseMaxPct = 100000;
    g_spawnSpreadPct = (int)cfg_int("loop", "spawn_spread_pct", 0, g_iniPath);  // spread-out density
    if (g_spawnSpreadPct < 0)     g_spawnSpreadPct = 0;
    if (g_spawnSpreadPct > 10000) g_spawnSpreadPct = 10000;
    g_spawnSpreadStartsAt = (int)cfg_int("loop", "spawn_spread_starts_at", 3, g_iniPath);
    if (g_spawnSpreadStartsAt < 1)   g_spawnSpreadStartsAt = 1;
    if (g_spawnSpreadStartsAt > 100) g_spawnSpreadStartsAt = 100;
    g_spawnSpreadMaxPct = (int)cfg_int("loop", "spawn_spread_max_pct", 0, g_iniPath);  // 0 = no cap
    if (g_spawnSpreadMaxPct < 0)      g_spawnSpreadMaxPct = 0;
    if (g_spawnSpreadMaxPct > 100000) g_spawnSpreadMaxPct = 100000;
    g_cursedBomb = cfg_int("player", "god_cursed_bomb", 0, g_iniPath) != 0;   // spawn loadout (host)
    g_randWeaponMatch = cfg_int("player", "random_match_weapon", 0, g_iniPath) != 0;  // live-toggleable
    g_randNoTreeDefault = cfg_int("player", "random_no_tree_default", 1, g_iniPath) != 0;  // 1=default weapon, 0=fists
    g_randArcheryBow = cfg_int("player", "random_archery_bow", 1, g_iniPath) != 0;  // Archery tree -> +bow +30 arrows
    g_randHealth = cfg_int("player", "random_health", 0, g_iniPath) != 0;  // random max HP 60..100 for Random chars
    g_woodsmanAxeLevel = (int)cfg_int("player", "woodsman_axe_level", 0, g_iniPath);  // testing: Woodsman's Axe tree starts at 0-4
    if (g_woodsmanAxeLevel < 0) g_woodsmanAxeLevel = 0;
    if (g_woodsmanAxeLevel > 4) g_woodsmanAxeLevel = 4;
    g_woodsmanBomberLevel = (int)cfg_int("player", "woodsman_bomber_level", 0, g_iniPath);  // testing: Bomber tree starts at 0-4
    if (g_woodsmanBomberLevel < 0) g_woodsmanBomberLevel = 0;
    if (g_woodsmanBomberLevel > 4) g_woodsmanBomberLevel = 4;
    g_woodsmanBrutalistLevel = (int)cfg_int("player", "woodsman_brutalist_level", 0, g_iniPath);
    if (g_woodsmanBrutalistLevel < 0) g_woodsmanBrutalistLevel = 0;
    if (g_woodsmanBrutalistLevel > 4) g_woodsmanBrutalistLevel = 4;
    g_woodsmanScavengerLevel = (int)cfg_int("player", "woodsman_scavenger_level", 0, g_iniPath);
    if (g_woodsmanScavengerLevel < 0) g_woodsmanScavengerLevel = 0;
    if (g_woodsmanScavengerLevel > 3) g_woodsmanScavengerLevel = 3;
    g_startSkillPoints = (int)cfg_int("player", "start_skill_points", 0, g_iniPath);  // testing: skill points at creation
    if (g_startSkillPoints < 0) g_startSkillPoints = 0;
    if (g_startSkillPoints > 99) g_startSkillPoints = 99;
    g_startRegens     = cfg_int("player", "start_regens", 0, g_iniPath) != 0;
    g_startAllBooks   = cfg_int("player", "start_all_spellbooks", 0, g_iniPath) != 0;
    {
        uint32_t cats = 0;
        for (int i = 0; i < N_WEAPON_KIT; i++)
            if (cfg_int("player", WEAPON_KIT[i].key, 0, g_iniPath) != 0) cats |= 1u << i;
        g_startWeaponCats = cats;
    }
    g_randWoodsmanTrees = cfg_int("player", "random_woodsman_trees", 0, g_iniPath) != 0;
    g_randomTreeCount = (int)cfg_int("player", "random_tree_count", 4, g_iniPath);  // # skill trees a Random char rolls (stock 4)
    if (g_randomTreeCount < 0)  g_randomTreeCount = 0;
    if (g_randomTreeCount > 16) g_randomTreeCount = 16;   // candidate pool is 16 distinct affinity types
    g_midgameSpectators = cfg_int("multiplayer", "allow_midgame_spectators", 0, g_iniPath) != 0;  // applied at startup (host)
    g_keepLobbyJoinable = cfg_int("multiplayer", "keep_lobby_joinable", 0, g_iniPath) != 0;       // applied at startup (host)
    g_portalCoop = cfg_int("multiplayer", "portal_coop", 1, g_iniPath) != 0;  // any player can use placed portals (both need the mod); live-toggleable
    g_portalTwoWay = cfg_int("multiplayer", "portal_two_way", 1, g_iniPath) != 0;  // interact on either portal end to teleport to the other; live-toggleable
    g_wandSkillTree    = cfg_int("mage", "wand_skill_tree",    0, g_iniPath) != 0;  // live: sync_mage_patches re-applies it to the next mage built
    g_randomWandWeapon = cfg_int("mage", "random_wand_weapon", 0, g_iniPath) != 0;  // live: sync_mage_patches re-applies it to the next mage built
    g_wandZeroCharges  = cfg_int("mage", "wand_zero_charges",  0, g_iniPath) != 0;  // live: sync_mage_patches re-applies it to the next mage built
    g_hostSync = GetPrivateProfileIntA("multiplayer", "host_settings_sync", 1, g_iniPath) != 0;  // never synced (local choice)
    rebuild_pool();
    ini_mtime(&g_iniMtime);
}
// Re-read the config (ini + any host overrides) and re-apply everything that can change live.
// Caller holds g_cfgLock exclusively.
static void host_publish_locked(const char* why);   // defined with the sync events below
static void client_clear_overrides(const char* why);   // defined with the sync events below
static void apply_config_now(const char* why) {
    load_config();
    apply_widening_if_needed();
    sync_mage_patches();                 // re-apply/revert the mage Rod<->Wand options
    apply_random_tree_count();           // re-apply the Random-character skill-tree count
    logmsg("[vultra] config applied (%s); pool size %d, host overrides %s",
           why, g_poolN, g_cfgOvrActive ? "ON" : "off");
    host_publish_locked(why);            // MP host: push the change to the lobby right away
}
// Event-driven ini reload: a background thread waits on a folder change notification and flags the
// ini as possibly changed; the game thread then compares the file's timestamp. Other files in the
// folder (like vultra.log) also wake it, but they don't change the ini's timestamp. If the watch
// can't be set up, reload_if_ini_changed falls back to checking the timestamp every frame.
static volatile LONG g_iniDirty = 1;     // set by ini_watch_thread on a folder change
static volatile bool g_iniWatchOk = false;
static DWORD WINAPI ini_watch_thread(LPVOID) {
    char dir[MAX_PATH]; strcpy_s(dir, g_iniPath);
    char* slash = strrchr(dir, '\\'); if (slash) *slash = 0;
    HANDLE h = FindFirstChangeNotificationA(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (h == INVALID_HANDLE_VALUE) { logmsg("[vultra] ini watch failed; checking every frame"); return 0; }
    g_iniWatchOk = true;
    for (;;) {
        if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) break;
        InterlockedExchange(&g_iniDirty, 1);
        if (!FindNextChangeNotification(h)) break;
    }
    g_iniWatchOk = false;
    FindCloseChangeNotification(h);
    return 0;
}
// Live reload: if vultramod.ini changed on disk (e.g. the manager's "Save changes"),
// re-read it so a running game picks up the new pool on the next spellbook it rolls.
static void reload_if_ini_changed() {
    if (g_iniWatchOk && InterlockedExchange(&g_iniDirty, 0) == 0) return;   // no change notification
    FILETIME t;
    if (!ini_mtime(&t)) return;
    if (t.dwLowDateTime == g_iniMtime.dwLowDateTime && t.dwHighDateTime == g_iniMtime.dwHighDateTime) return;
    AcquireSRWLockExclusive(&g_cfgLock);
    apply_config_now("ini changed");
    ReleaseSRWLockExclusive(&g_cfgLock);
    if (!g_hostSync && g_cfgOvrActive) client_clear_overrides("sync turned off");   // client opted out
}

// ---- Host settings sync: Steam lobby glue --------------------------------
// Vagante runs multiplayer through a Steam lobby. The host stores two lobby-data keys:
//   vultra_ver = VULTRA_VERSION      vultra_cfg = "key=value;key=value;..." (its effective settings)
// and re-publishes whenever they change (e.g. the manager saves mid-lobby or mid-run). Each modded
// client stores member-data vultra_ver, so the host can tell who has the mod and which version.
// A modded client whose version matches applies vultra_cfg as overrides (see cfg_int), live, the
// moment it changes. On a version mismatch, an unmodded host, or leaving the lobby, it drops the
// overrides and goes back to its own ini. Unmodded players never read these keys, so they're unaffected.
//   We call steam_api.dll's flat C API directly (the game's own copy). Lobby IDs come from the game:
// Server+0xd10 (host; see Server::updateLobbyInformation) and Client+0x1f1a (client; unaligned, see
// Client::onSteamLobbyJoined).
static const size_t OFF_SERVER_LOBBY = 0xd10;    // Server: CSteamID of the hosted lobby (uint64)
static const size_t OFF_CLIENT_LOBBY = 0x1f1a;   // Client: CSteamID of the joined lobby (uint64, unaligned)
typedef void*       (__cdecl *SteamMM_t)();
typedef bool        (__cdecl *SetLobbyData_t)(void*, uint64_t, const char*, const char*);
typedef const char* (__cdecl *GetLobbyData_t)(void*, uint64_t, const char*);
typedef void        (__cdecl *SetLobbyMemberData_t)(void*, uint64_t, const char*, const char*);
typedef const char* (__cdecl *GetLobbyMemberData_t)(void*, uint64_t, uint64_t, const char*);
typedef int         (__cdecl *GetNumLobbyMembers_t)(void*, uint64_t);
typedef uint64_t    (__cdecl *GetLobbyMemberByIndex_t)(void*, uint64_t, int);
typedef uint64_t    (__cdecl *GetLobbyOwner_t)(void*, uint64_t);
static struct {
    bool tried = false, ok = false;
    SteamMM_t mm; SetLobbyData_t setData; GetLobbyData_t getData;
    SetLobbyMemberData_t setMember; GetLobbyMemberData_t getMember;
    GetNumLobbyMembers_t numMembers; GetLobbyMemberByIndex_t memberAt; GetLobbyOwner_t owner;
} g_sl;
static bool steam_lobby_api() {
    if (g_sl.tried) return g_sl.ok;
    g_sl.tried = true;
    HMODULE h = GetModuleHandleA("steam_api.dll");
    if (!h) { logmsg("[sync] steam_api.dll not loaded; host settings sync disabled"); return false; }
    g_sl.mm         = (SteamMM_t)              GetProcAddress(h, "SteamAPI_SteamMatchmaking_v009");
    g_sl.setData    = (SetLobbyData_t)         GetProcAddress(h, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    g_sl.getData    = (GetLobbyData_t)         GetProcAddress(h, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    g_sl.setMember  = (SetLobbyMemberData_t)   GetProcAddress(h, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
    g_sl.getMember  = (GetLobbyMemberData_t)   GetProcAddress(h, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
    g_sl.numMembers = (GetNumLobbyMembers_t)   GetProcAddress(h, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    g_sl.memberAt   = (GetLobbyMemberByIndex_t)GetProcAddress(h, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    g_sl.owner      = (GetLobbyOwner_t)        GetProcAddress(h, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    g_sl.ok = g_sl.mm && g_sl.setData && g_sl.getData && g_sl.setMember && g_sl.getMember &&
              g_sl.numMembers && g_sl.memberAt && g_sl.owner;
    logmsg("[sync] steam lobby api %s (version %s)", g_sl.ok ? "ready" : "MISSING EXPORTS", VULTRA_VERSION);
    return g_sl.ok;
}
// A lobby we're actually in (Steam reports 0 members for lobbies we aren't a member of).
static void* lobby_mm(uint64_t lobby) {
    if (!lobby || !steam_lobby_api()) return nullptr;
    void* m = g_sl.mm();
    return (m && g_sl.numMembers(m, lobby) > 0) ? m : nullptr;
}

// ---- Host settings sync: events ------------------------------------------
// Everything here is driven by events, not timers:
//   host   - publishes when its lobby is ready (Server::onSteamLobbyCreated / updateLobbyInformation)
//            and whenever its settings are re-applied (apply_config_now, i.e. the ini changed).
//   client - reads the host's settings once on join (Client::onSteamLobbyJoined), then again only
//            when Steam reports the lobby data changed (LobbyDataUpdate_t callback). It drops the
//            overrides on Client::disconnect.
// Mid-game changes need nothing extra: every feature already reads its setting live, so a new value
// takes effect the next time that code runs (next spellbook, next boss win, next floor, ...).
static volatile uint64_t g_hostLobby = 0;          // lobby we host (from the Server)
static volatile uint64_t g_hostPublishedLobby = 0; // lobby we last published to
static volatile uint64_t g_clientLobby = 0;        // lobby we joined as a client

// Host: write version + settings to the lobby if they changed. Caller holds g_cfgLock (any mode).
static void host_publish_locked(const char* why) {
    uint64_t lobby = g_hostLobby;
    void* m = lobby_mm(lobby);
    if (!m) return;
    static char lastCfg[4096] = {0};
    char cfg[4096] = {0};
    if (g_hostSync) {
        size_t len = 0;
        for (int i = 0; i < g_cfgSnapN; i++) {
            int w = sprintf_s(cfg + len, sizeof(cfg) - len, "%s%s=%s", len ? ";" : "", g_cfgSnap[i].key, g_cfgSnap[i].val);
            if (w < 0) break;
            len += (size_t)w;
        }
    }
    if (lobby == g_hostPublishedLobby && strcmp(cfg, lastCfg) == 0) return;   // nothing new
    if (!g_sl.setData(m, lobby, "vultra_ver", VULTRA_VERSION)) return;     // not the owner (yet)
    g_sl.setData(m, lobby, "vultra_cfg", cfg);                              // "" = host turned sync off
    strcpy_s(lastCfg, cfg);
    g_hostPublishedLobby = lobby;
    logmsg("[sync] host published %s settings (%u bytes) to lobby %llx (%s)",
           cfg[0] ? "its" : "NO", (unsigned)strlen(cfg), lobby, why);
}
static void client_clear_overrides(const char* why);
// Host: the Server's lobby exists or changed. Called after the game updates its lobby info.
static void host_on_lobby(void* server) {
    if (!server) return;
    uint64_t lobby = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(server) + OFF_SERVER_LOBBY);
    if (!lobby || (lobby == g_hostLobby && lobby == g_hostPublishedLobby)) return;   // already done
    g_hostLobby = lobby;
    if (g_clientLobby == lobby) g_clientLobby = 0;   // our own client joined first: it isn't a client of ours
    client_clear_overrides("hosting");       // a host plays with its own settings
    AcquireSRWLockShared(&g_cfgLock);
    host_publish_locked("lobby ready");
    ReleaseSRWLockShared(&g_cfgLock);
}
// Host: a member's data changed (a client announced its version). Log who has the mod.
static void host_on_member(uint64_t lobby, uint64_t member) {
    void* m = lobby_mm(lobby);
    if (!m || member == g_sl.owner(m, lobby)) return;
    const char* v = g_sl.getMember(m, lobby, member, "vultra_ver");
    if (!v || !*v)                         logmsg("[sync] member %llx does NOT have VUltra", member);
    else if (!strcmp(v, VULTRA_VERSION))   logmsg("[sync] member %llx has VUltra %s (in sync)", member, v);
    else logmsg("[sync] member %llx has VUltra %.20s, host has %s (NOT synced)", member, v, VULTRA_VERSION);
}

// Client: drop the host's overrides and go back to the local ini.
static void client_clear_overrides(const char* why) {
    if (!g_cfgOvrActive) return;
    AcquireSRWLockExclusive(&g_cfgLock);
    g_cfgOvrActive = false;
    g_cfgOvrN = 0;
    apply_config_now(why);
    ReleaseSRWLockExclusive(&g_cfgLock);
}
// Parse "k=v;k=v" strictly: a key must be one this build reads (it's in our own snapshot) and a
// value must be a plain number. Anything else is skipped. load_config clamps the ranges afterwards.
static int parse_host_cfg(const char* s, CfgKV* out, int max) {
    int n = 0;
    while (*s && n < max) {
        const char* end = strchr(s, ';');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        const char* eq = (const char*)memchr(s, '=', len);
        if (eq) {
            size_t kl = (size_t)(eq - s), vl = len - kl - 1;
            bool ok = kl > 0 && kl < sizeof(out[0].key) && vl > 0 && vl < sizeof(out[0].val);
            for (size_t i = 0; ok && i < kl; i++) ok = isalnum((unsigned char)s[i]) || s[i] == '_';
            for (size_t i = 0; ok && i < vl; i++) ok = isdigit((unsigned char)eq[1 + i]) || strchr("-+.", eq[1 + i]);
            if (ok) {
                memcpy(out[n].key, s, kl); out[n].key[kl] = 0;
                memcpy(out[n].val, eq + 1, vl); out[n].val[vl] = 0;
                bool known = false;
                for (int i = 0; i < g_cfgSnapN && !known; i++) known = _stricmp(g_cfgSnap[i].key, out[n].key) == 0;
                if (known && cfg_is_synced(out[n].key)) n++;
            }
        }
        if (!end) break;
        s = end + 1;
    }
    return n;
}
// Client: read the host's lobby data and apply it if it changed.
static void client_apply_from_lobby(const char* why) {
    uint64_t lobby = g_clientLobby;
    void* m = lobby_mm(lobby);
    if (!m) return;
    if (!g_hostSync) { client_clear_overrides("sync turned off"); return; }
    const char* ver = g_sl.getData(m, lobby, "vultra_ver");
    const char* cfg = g_sl.getData(m, lobby, "vultra_cfg");
    char note[64] = {0};
    if (!ver || !*ver)                       strcpy_s(note, "host has no VUltra");
    else if (strcmp(ver, VULTRA_VERSION))    sprintf_s(note, "host has VUltra %.20s, we have %s", ver, VULTRA_VERSION);
    else if (!cfg || !*cfg)                  strcpy_s(note, "host turned sync off");
    if (note[0]) {
        logmsg("[sync] not syncing (%s): %s", why, note);
        client_clear_overrides(note);
        return;
    }
    static char lastCfg[4096] = {0};
    if (g_cfgOvrActive && strcmp(cfg, lastCfg) == 0) return;   // nothing new for us
    static CfgKV parsed[CFG_MAX];
    AcquireSRWLockExclusive(&g_cfgLock);
    int n = parse_host_cfg(cfg, parsed, CFG_MAX);
    memcpy(g_cfgOvr, parsed, sizeof(CfgKV) * n);
    g_cfgOvrN = n;
    g_cfgOvrActive = true;
    apply_config_now("host settings");
    ReleaseSRWLockExclusive(&g_cfgLock);
    strncpy_s(lastCfg, cfg, _TRUNCATE);
    logmsg("[sync] applied %d host settings from lobby %llx (%s)", n, lobby, why);
}
// Client: we entered a lobby. Announce our version, then do the one-time initial read.
static void client_on_joined(uint64_t lobby) {
    if (!lobby) return;
    if (lobby == g_hostLobby) return;          // our own hosted lobby: nothing to take
    g_hostLobby = 0;                           // no longer hosting anything
    g_clientLobby = lobby;
    void* m = lobby_mm(lobby);
    if (!m) { logmsg("[sync] joined lobby %llx but the Steam lobby api is unavailable", lobby); return; }
    g_sl.setMember(m, lobby, "vultra_ver", VULTRA_VERSION);
    logmsg("[sync] joined lobby %llx; announced VUltra %s", lobby, VULTRA_VERSION);
    client_apply_from_lobby("joined");
}
static void client_on_left() {
    if (!g_clientLobby && !g_cfgOvrActive) return;
    logmsg("[sync] left lobby %llx", (uint64_t)g_clientLobby);
    g_clientLobby = 0;
    client_clear_overrides("left lobby");
}

// Steam LobbyDataUpdate_t (callback 505): fires when lobby data (member == lobby) or a member's
// data (member == that player) changes. We register our own listener with SteamAPI_RegisterCallback.
// The class mirrors the Steamworks SDK's CCallbackBase exactly (same virtuals, same order), so MSVC
// lays out the same vtable steam_api expects.
#pragma pack(push, 8)
struct LobbyDataUpdate_t { uint64_t lobby; uint64_t member; uint8_t success; };
#pragma pack(pop)
class SteamCallbackBase {
public:
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
    virtual int GetCallbackSizeBytes() = 0;
    uint8_t m_nCallbackFlags = 0;
    int m_iCallback = 0;
};
class LobbyDataListener : public SteamCallbackBase {
public:
    void Run(void* p) override {
        const LobbyDataUpdate_t* u = static_cast<const LobbyDataUpdate_t*>(p);
        if (!u || !u->success) return;
        if (u->lobby == g_clientLobby && u->lobby != g_hostLobby && u->member == u->lobby)
            client_apply_from_lobby("host changed settings");
        else if (u->lobby == g_hostLobby && u->member != u->lobby) host_on_member(u->lobby, u->member);
    }
    void Run(void* p, bool, uint64_t) override { Run(p); }
    int GetCallbackSizeBytes() override { return (int)sizeof(LobbyDataUpdate_t); }
};
static LobbyDataListener g_lobbyDataListener;
static const int CB_LOBBY_DATA_UPDATE = 505;   // k_iSteamMatchmakingCallbacks (500) + 5
static bool register_lobby_listener() {
    HMODULE h = GetModuleHandleA("steam_api.dll");
    typedef void (__cdecl *RegisterCallback_t)(void*, int);
    RegisterCallback_t reg = h ? (RegisterCallback_t)GetProcAddress(h, "SteamAPI_RegisterCallback") : nullptr;
    if (!reg) return false;
    g_lobbyDataListener.m_iCallback = CB_LOBBY_DATA_UPDATE;
    reg(&g_lobbyDataListener, CB_LOBBY_DATA_UPDATE);
    return true;
}

// Game hooks feeding the events above.
static const uintptr_t RVA_CLIENT_LOBBY_JOINED   = 0x0014cc50;  // Client::onSteamLobbyJoined(LobbyEnter_t*, bool)
static const uintptr_t RVA_CLIENT_DISCONNECT     = 0x0015de80;  // Client::disconnect()
static const uintptr_t RVA_SERVER_LOBBY_CREATED  = 0x0036ae20;  // Server::onSteamLobbyCreated(LobbyCreated_t*, bool)
static const uintptr_t RVA_SERVER_UPDATE_LOBBY   = 0x0036aaa0;  // Server::updateLobbyInformation()
typedef void (__fastcall *thisArg2_t)(void* self, void* edx, void* p, int io);
typedef void (__fastcall *thisArg0_t)(void* self, void* edx);
static thisArg2_t g_lobbyJoinedTramp = nullptr, g_lobbyCreatedTramp = nullptr;
static thisArg0_t g_clientDisconnectTramp = nullptr, g_updateLobbyTramp = nullptr;
static void __fastcall det_lobbyJoined(void* self, void* edx, void* p, int io) {
    g_lobbyJoinedTramp(self, edx, p, io);
    // LobbyEnter_t: m_ulSteamIDLobby @+0, m_EChatRoomEnterResponse @+0x10 (1 = success)
    if (p && *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(p) + 0x10) == 1)
        client_on_joined(*reinterpret_cast<uint64_t*>(p));
}
static void __fastcall det_clientDisconnect(void* self, void* edx) {
    client_on_left();
    g_clientDisconnectTramp(self, edx);
}
static void __fastcall det_lobbyCreated(void* self, void* edx, void* p, int io) {
    g_lobbyCreatedTramp(self, edx, p, io);
    host_on_lobby(self);
}
static void __fastcall det_updateLobby(void* self, void* edx) {
    g_updateLobbyTramp(self, edx);
    host_on_lobby(self);
}

static void save_config() {
    for (int i = 0; i < N_SPELLS; i++)
        WritePrivateProfileStringA("spellbook_pool", g_spells[i].key, g_enabled[i] ? "1" : "0", g_iniPath);
}
static void apply_changes() { rebuild_pool(); apply_widening_if_needed(); save_config(); }

// Overlay/runtime API (same process):
extern "C" __declspec(dllexport) int  VUltra_Count()        { return N_SPELLS; }
extern "C" __declspec(dllexport) const char* VUltra_Name(int i)  { return (i>=0&&i<N_SPELLS)?g_spells[i].name:""; }
extern "C" __declspec(dllexport) const char* VUltra_Key(int i)   { return (i>=0&&i<N_SPELLS)?g_spells[i].key:""; }
extern "C" __declspec(dllexport) int  VUltra_Get(int i)     { return (i>=0&&i<N_SPELLS)?(g_enabled[i]?1:0):0; }
extern "C" __declspec(dllexport) void VUltra_Set(int i,int v){ if(i>=0&&i<N_SPELLS) g_enabled[i]=(v!=0); }
extern "C" __declspec(dllexport) void VUltra_Apply() { apply_changes(); logmsg("[vultra] applied: pool size %d", g_poolN); }

// ---- in-game ImGui overlay (#2) ----------------------------------------
typedef bool (__cdecl *ImBegin_t)(const char*, bool*, int);
typedef void (__cdecl *ImEnd_t)();
typedef bool (__cdecl *ImCheckbox_t)(const char*, bool*);
typedef void (__cdecl *ImText_t)(const char*, ...);
typedef void (__cdecl *ImSeparator_t)();
typedef void (__cdecl *ImRender_t)();
static ImBegin_t imBegin = nullptr; static ImEnd_t imEnd = nullptr;
static ImCheckbox_t imCheckbox = nullptr; static ImText_t imText = nullptr; static ImSeparator_t imSeparator = nullptr;
static ImRender_t g_renderTramp = nullptr;
static bool g_overlayShown = false;

static void draw_overlay() {
    if (imBegin("V Ultra", &g_overlayShown, 0)) {
        bool changed = false;
        imText("Spellbook pool. Changes apply to the next room/level.");
        imSeparator();
        imText("Standard spells");
        for (int i = 0; i < N_SPELLS; i++)
            if (g_spells[i].id < 16 && imCheckbox(g_spells[i].name, &g_enabled[i])) changed = true;
        imSeparator();
        imText("Re-enabled (cut) spells");
        for (int i = 0; i < N_SPELLS; i++)
            if (g_spells[i].id >= 16 && imCheckbox(g_spells[i].name, &g_enabled[i])) changed = true;
        imSeparator();
        imText("F10 toggles this window.");
        if (changed) apply_changes();
    }
    imEnd();   // ImGui requires End() regardless of Begin()'s return
}

static void __cdecl det_Render() {
    static bool prev = false;
    bool down = (GetAsyncKeyState(OVERLAY_HOTKEY) & 0x8000) != 0;
    if (down && !prev) g_overlayShown = !g_overlayShown;
    prev = down;
    if (g_overlayShown && imBegin) draw_overlay();
    g_renderTramp();   // original ImGui::Render
}

// Generic inline detour (steal >= 5). Returns trampoline (orig prologue + jmp back), or null.
static void* install_detour(uintptr_t rva, void* detour, int steal, const uint8_t* expect, int expectLen) {
    uint8_t* target = reinterpret_cast<uint8_t*>(g_base + rva);
    if (expect && memcmp(target, expect, expectLen) != 0) { logmsg("[vultra] detour %p prologue mismatch", target); return nullptr; }
    uint8_t* tramp = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, steal + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    memcpy(tramp, target, steal);
    tramp[steal] = 0xE9;
    *reinterpret_cast<int32_t*>(tramp + steal + 1) = static_cast<int32_t>((target + steal) - (tramp + steal + 5));
    DWORD oldp;
    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &oldp)) return nullptr;
    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(detour) - (target + 5));
    for (int i = 5; i < steal; i++) target[i] = 0x90;   // pad to instruction boundary
    VirtualProtect(target, steal, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    return tramp;
}

static void install_overlay() {
    imBegin     = reinterpret_cast<ImBegin_t>(g_base + RVA_IM_BEGIN);
    imEnd       = reinterpret_cast<ImEnd_t>(g_base + RVA_IM_END);
    imCheckbox  = reinterpret_cast<ImCheckbox_t>(g_base + RVA_IM_CHECKBOX);
    imText      = reinterpret_cast<ImText_t>(g_base + RVA_IM_TEXT);
    imSeparator = reinterpret_cast<ImSeparator_t>(g_base + RVA_IM_SEPARATOR);
    const uint8_t render_pro[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};  // push ebp; mov ebp,esp; and esp,~7
    g_renderTramp = reinterpret_cast<ImRender_t>(install_detour(RVA_IM_RENDER, (void*)&det_Render, 6, render_pro, 6));
    logmsg("[vultra] overlay %s (F10)", g_renderTramp ? "installed" : "FAILED");
}

// ---- Wisp Curse background re-enable ------------------------------------
typedef void* (__cdecl *opnew_t)(size_t);
typedef void  (__cdecl *opdel_t)(void*);
static opnew_t g_opnew = nullptr;
static opdel_t g_opdel = nullptr;
static void*   g_origGetBg = nullptr;   // trampoline to original getAvailableBackgrounds

// In the returned vector<pair<BackgroundType,bool>> (2-byte elems), overwrite the sacrifice
// background's type with BG_WISPCURSE (reuse its slot). TEST: tells us if the card converts.
static void __cdecl append_wisp(void* vp) {
    reload_if_ini_changed();   // pick up live toggles from the manager while at the menu
    if (!g_wispCurse || !vp) return;
    uint8_t** v = reinterpret_cast<uint8_t**>(vp);     // v[0]=first, v[1]=last, v[2]=end
    uint8_t* first = v[0], *last = v[1];
    if (!first) return;
    for (uint8_t* p = first; p + 2 <= last; p += 2) {
        if (p[0] == (uint8_t)g_replaceBg) {
            p[0] = (uint8_t)BG_WISPCURSE; p[1] = 1;
            logmsg("[vultra] replaced background %d with Wisp Curse in list", g_replaceBg);
            return;
        }
    }
}

// Detour: ABI is ecx=return-vector ptr, edx=class int, [stack]=bool, returns eax=ret ptr, no cleanup.
__declspec(naked) static void det_getBg() {
    __asm {
        push ebp
        mov  ebp, esp
        sub  esp, 4
        mov  [ebp-4], ecx           // save return-vector ptr
        push dword ptr [ebp+8]      // forward bool arg
        mov  ecx, [ebp-4]           // ecx = ret ptr  (edx still = class int)
        mov  eax, g_origGetBg
        call eax                    // run original; eax = ret ptr
        add  esp, 4                 // (original does not clean the stack arg)
        push dword ptr [ebp-4]
        call append_wisp            // append Wisp Curse
        add  esp, 4
        mov  eax, [ebp-4]           // return the vector ptr
        mov  esp, ebp
        pop  ebp
        ret
    }
}

// ---- Wisp Curse: force the floor effect without anyone having the background ----
// GameEngine::loadNextLevel computes, every floor, a local "wisp curse active" flag by scanning
// all players for background 12 (Player+0x824) and storing the result to GameEngine+0x2f0. The
// init store `mov byte[ebp-0x8d],0` (RVA 0x1c3adb, 7 bytes) seeds that flag to 0 before the scan.
// We replace it: write g_forceWispCurse instead of 0. Off -> 0 (vanilla: a real cursed player can
// still trigger it). On -> 1, which makes +0x2f0=1 AND takes the curse-setup branch for everyone.
// Because the HOST's authoritative Server GameEngine runs this same function, forcing it there makes
// the cursed floor generate + net-sync to all clients -- host-only, no client mod (like the loop).
static const uintptr_t RVA_WISP_INIT_FLAG = 0x001c3adb;  // mov byte[ebp-0x8d],0  (C6 85 73 FF FF FF 00)
static uintptr_t g_wispResume = 0;                       // = g_base + 0x1c3ae2 (next instr, `test edx,edx`)
// Reload the ini FIRST so a lobby toggle takes effect on the very next floor generated. (The
// existing reload happens later in loadNextLevel, after this flag is seeded -> would lag one floor.)
static char __cdecl wisp_force_value() {
    reload_if_ini_changed();
    return g_forceWispCurse ? 1 : 0;
}
__declspec(naked) static void det_wispForce() {
    __asm {
        pushad                                 // preserve the live frame regs (edx = player count, etc.)
        call wisp_force_value                  // al = current force flag (reloads ini)
        mov  byte ptr [ebp-0x8d], al           // seed the flag (ebp = loadNextLevel's live frame)
        popad
        jmp  dword ptr [g_wispResume]           // skip the original init store, resume after it
    }
}

// ---- Caged-fairy mode: Default / None / All levels / One in first biome --
// LevelGenerator::generateTreasure gates the FairyCage spawn on the floor index:
// `mov eax,[ebx+0xa4]; cmp eax,1/4/7/0xa` (0-based floors 1,4,7,10 = the 2nd floor of each
// biome) -> sets a biome index (ecx 0..3 = Caves/Forest/Catacombs/Rift, which picks the fairy
// TYPE) -> per-biome dedup `test [eax],edx; jne skip` @0x29423f -> `call FairyCage @0x5b9fb0`.
// Two hooks implement the modes:
//   (A) det_fairyMode replaces the floor load and rewrites eax (the value fed to the cmp chain):
//       1 None -> eax=-1 (matches nothing -> skip);
//       2 All  -> map the REAL floor to its biome's representative value 1/4/7/10 (floor/3) so
//                 every floor spawns ITS OWN biome's fairy (Caves in caves, Forest in forest...);
//                 floor 12 (boss) -> none. (Forcing eax=1 would put the Caves fairy everywhere.)
//       3 First -> spawn only on Caves' 2nd floor (real floor 1); every other floor -> skip;
//       0 Default -> real floor (vanilla: 1/4/7/10, each biome's own fairy, one each).
//   (B) det_fairyDedup replaces the dedup `jne skip`: in All mode always fall through to spawn (so
//       the once-per-biome bit can't block later floors of the SAME biome); otherwise behave exactly
//       like the original jne (preserving the test's flags). Default/None/First are vanilla-dedup.
static const uintptr_t RVA_FAIRY_FLOOR_LOAD = 0x002941ec;  // mov eax,[ebx+0xa4]  (8B 83 A4 00 00 00)
static const uintptr_t RVA_FAIRY_DEDUP_JNE  = 0x0029423f;  // jne 0x69457d        (0F 85 38 03 00 00)
static uintptr_t g_fairyResume    = 0;                     // = g_base + 0x2941f2 (cmp eax,1)
static uintptr_t g_fairyDedupFall = 0;                     // = g_base + 0x294245 (continue -> spawn)
static uintptr_t g_fairyDedupSkip = 0;                     // = g_base + 0x29457d (skip / no spawn)
__declspec(naked) static void det_fairyMode() {
    __asm {
        mov  eax, dword ptr [ebx+0xa4]         // vanilla: eax = real floor index
        cmp  dword ptr [g_fairyMode], 1
        je   fm_none
        cmp  dword ptr [g_fairyMode], 2
        je   fm_all
        cmp  dword ptr [g_fairyMode], 3
        je   fm_first
        jmp  fm_done                           // Default(0) -> real floor
    fm_none:
        mov  eax, -1                           // None -> matches none of 1/4/7/10 -> no fairy
        jmp  fm_done
    fm_first:
        cmp  eax, 1                            // First -> only Caves' 2nd floor (real floor 1)
        je   fm_done                           //   eax already 1 -> Caves fairy
        mov  eax, -1                           //   any other floor -> skip
        jmp  fm_done
    fm_all:                                    // map real floor -> its biome's value (1/4/7/10)
        cmp  eax, 3
        jl   fm_a0                             // 0,1,2  -> Caves
        cmp  eax, 6
        jl   fm_a1                             // 3,4,5  -> Forest
        cmp  eax, 9
        jl   fm_a2                             // 6,7,8  -> Catacombs
        cmp  eax, 12
        jl   fm_a3                             // 9,10,11-> Rift
        mov  eax, -1                           // 12+ (final boss) -> no fairy
        jmp  fm_done
    fm_a0:
        mov  eax, 1
        jmp  fm_done
    fm_a1:
        mov  eax, 4
        jmp  fm_done
    fm_a2:
        mov  eax, 7
        jmp  fm_done
    fm_a3:
        mov  eax, 10
    fm_done:
        jmp  dword ptr [g_fairyResume]
    }
}
__declspec(naked) static void det_fairyDedup() {
    __asm {
        // ZF here = result of the preceding `test [eax],edx` (bit set => ZF=0 => original skips).
        pushfd                                 // preserve the test's flags
        cmp  dword ptr [g_fairyMode], 2        // All? (clobbers flags)
        jne  fd_orig
        add  esp, 4                            // All: drop saved flags, always spawn
        jmp  dword ptr [g_fairyDedupFall]
    fd_orig:
        popfd                                  // restore the test's flags
        jne  fd_skip                           // original: bit already set -> skip
        jmp  dword ptr [g_fairyDedupFall]      // bit clear -> spawn
    fd_skip:
        jmp  dword ptr [g_fairyDedupSkip]
    }
}

// ---- Fairy blessing stacking: +5 max HP per FAIRY, not per biome ----------
// Vanilla records a rescued fairy as a single BIT keyed by FairyType (0..3) in the level's
// collected-types set (Level+0xc8). When you rest at the campfire, Bonfire::step popcounts that
// set into Bonfire+0x53c, and the rest bonus = (count + 4) * 5 (so 4 biomes = +20). Because it's a
// 4-bit set, a 2nd same-biome fairy re-sets an already-set bit and adds nothing.
//   Fix (host-authoritative: both sites run in the game sim, so unmodded clients get it too):
//   (A) det_fairyBlessMark counts EVERY fairy that finishes its campfire blessing (one-shot on the
//       finishedAnimating 0->1 edge) into g_fairyBlessCount.
//   (B) det_bonfireBless overrides the popcount the rest-bonus reads with that true per-fairy count
//       (max of the accumulator and the vanilla popcount).
//   Reset: g_fairyBlessCount is cleared per RUN in the GameEngine::newGame hook (wlog_newGame) and per
//   LOOP in loop_prep_core. It is NOT reset on an empty bonfire read -- an earlier version did that
//   ("popcount 0 == fresh run") but it broke multiplayer: in MP the bonfire is read with popcount 0
//   constantly (an empty/mirror read every frame), which kept nuking the accumulator so same-biome
//   fairies never stacked. An empty read now just yields no bonus for that read and leaves the count
//   intact.
static const uintptr_t RVA_FAIRY_BLESS_MARK   = 0x001b782f;  // mov byte[edi+0x33c],1 (Fairy finishedAnimating) 7 bytes
static const uintptr_t RVA_BONFIRE_BLESS_LOAD = 0x0010062f;  // mov eax,[edi+0x53c]  (Bonfire bless popcount) 6 bytes
static uintptr_t g_fairyBlessResume   = 0;   // = g_base + 0x001b7836 (after the stolen mov)
static uintptr_t g_bonfireBlessResume = 0;   // = g_base + 0x00100635 (after the stolen mov)

static void __cdecl fairy_bless_oneshot(void* fairy) {
    if (!g_fairyStack || !fairy) return;
    if (*((uint8_t*)fairy + 0x33c) == 0)            // finishedAnimating still 0 => first completion
        InterlockedIncrement(&g_fairyBlessCount);
}
__declspec(naked) static void det_fairyBlessMark() {
    __asm {
        pushad
        push edi                                    // Fairy* (this)
        call fairy_bless_oneshot
        add  esp, 4
        popad
        mov  byte ptr [edi+0x33c], 1                // stolen write (finishedAnimating = 1)
        jmp  dword ptr [g_fairyBlessResume]
    }
}
static int __cdecl bonfire_bless_count(void* bonfire) {
    int vanilla = *(int*)((uint8_t*)bonfire + 0x53c);
    if (!g_fairyStack) return vanilla;              // feature off: vanilla popcount (no change)
    // An empty bonfire read (popcount 0) means no bonus FOR THIS read -- but do NOT reset the
    // accumulator here. In multiplayer the bonfire is read with popcount 0 constantly (an empty/mirror
    // read every frame), which previously kept nuking g_fairyBlessCount so stacks never grew. The
    // run-start reset now lives in the GameEngine::newGame hook (and loop_prep_core for loops).
    if (vanilla == 0) return 0;
    int total = (int)g_fairyBlessCount;
    return total > vanilla ? total : vanilla;       // +5 per fairy (>= the distinct-biome popcount)
}
__declspec(naked) static void det_bonfireBless() {
    __asm {
        push edi                                    // Bonfire* (this); cdecl preserves edi
        call bonfire_bless_count                    // eax = count to use
        add  esp, 4
        jmp  dword ptr [g_bonfireBlessResume]
    }
}

// Configurable per-fairy heal amount. Vanilla computes the rest bonus as (count + 4) * 5: across all
// branches (campfire/character variants) the per-fairy term is exactly count*5, with +4 giving the
// base 20. We hook right AFTER the *5 multiply (where ecx = final bonus, eax = count) and add
// count*(g_fairyHeal - 5), turning the per-fairy amount into g_fairyHeal while leaving the base 20
// (and the branch variants) untouched. At the default of 5 the delta is 0 -> bit-for-bit vanilla.
// The stolen 7-byte cmp is re-run last so its flags drive the downstream je correctly.
static const uintptr_t RVA_BONFIRE_HEAL = 0x0010065d;  // cmp byte[esi+0x808],0  (after the bless *5) 7 bytes
static uintptr_t g_bonfireHealResume = 0;              // = g_base + 0x00100664 (the mov [ebp-0x20],ecx)
__declspec(naked) static void det_bonfireHeal() {
    __asm {
        push edx
        mov  edx, g_fairyHealDelta                  // edx = g_fairyHeal - 5 (may be negative)
        imul edx, eax                               // edx = count * delta
        add  ecx, edx                               // ecx = base + count*5 + count*delta = base + count*heal
        pop  edx
        cmp  byte ptr [esi+0x808], 0                // stolen cmp (sets flags for the downstream je)
        jmp  dword ptr [g_bonfireHealResume]
    }
}

// ---- Co-op portal fix: let any player use a placed portal -----------------
// The Portal spellbook places two networked SpellEntities: SPELL_PORTAL_ENTRANCE(2) and
// SPELL_PORTAL(1). SpellEntity::enterPortal teleports the entering entity to the EXIT, reading the
// destination from the CASTING player's fields Player.portal(+0x7878) and Player.portalEntrance
// (+0x7880). Those two Player fields are NOT network-synced (absent from Snapshot::writeDeltaEntity),
// so on every machine except the caster's they are null. enterPortal then takes its "no destination"
// branch: it still swaps the portal art (a replicated entity-state toggle) but skips the move. Net
// result the player reported: only the caster teleports; for everyone else the art flips but nobody
// moves.
//   Both portal endpoints ARE replicated (the SpellEntity ctor sets Entity.networked=1) and each one
// resolves its owner pointer (+0x218) on every machine. So on whatever machine runs the teleport we
// can rebuild the missing link from the live entity list: scan Level.entities for the two portal
// SpellEntities owned by this portal's owner and borrow them into owner.portal / owner.portalEntrance
// just for the duration of the original call, then restore. The borrow uses raw pointers and touches
// no refcounts: the entities are kept alive by the level's own strong references, and stock
// enterPortal makes its own ref-counted copies of the link while it runs. With the link present the
// unmodified enterPortal teleports whoever entered, on host and client alike. Both players need the
// mod (the teleport runs authoritatively on the host AND is predicted on the entering client).
// g_lastServerStep is defined earlier (host settings sync reads it too).
static const uintptr_t RVA_ENTER_PORTAL    = 0x003a48a0;  // SpellEntity::enterPortal(shared_ptr<Entity>)
static const uintptr_t RVA_SPELLENTITY_VFT = 0x0071ef58;  // ??_7SpellEntity@@6B@ (identifies a SpellEntity)
static const size_t OFF_ENT_OWNER        = 0x218;   // SpellEntity owner shared_ptr (raw ptr; ctrl at +0x21c)
static const size_t OFF_ENT_SPELLTYPE    = 0x224;   // SpellEntity::SpellType (1=PORTAL exit, 2=PORTAL_ENTRANCE)
static const size_t OFF_ENT_LEVEL        = 0x1a0;   // Entity::level (Level*)
static const size_t OFF_PLAYER_PORTAL    = 0x7878;  // Player.portal          shared_ptr<SpellEntity> (the exit)
static const size_t OFF_PLAYER_PORTALENT = 0x7880;  // Player.portalEntrance  shared_ptr<SpellEntity> (the entrance)
static const size_t OFF_LEVEL_ENTITIES   = 0xfd0;   // Level.entities  vector<shared_ptr<Entity>> (8-byte elems)
static const int SPELLTYPE_PORTAL          = 1;     // the EXIT endpoint
static const int SPELLTYPE_PORTAL_ENTRANCE = 2;     // the ENTRANCE endpoint
// g_portalCoop is declared up with the other [multiplayer] flags (load_config reads it before here).

typedef void (__fastcall *enterPortal_t)(void* self, void* edx, void* spPtr, void* spCtrl);
static enterPortal_t g_enterPortalTramp = nullptr;

// The Player link slots we temporarily filled, so we can put them back after the stock call.
struct PortalBorrow { void** slot[2]; void* saved[2][2]; int n; };

static bool portal_link_borrow(void* self, PortalBorrow* b) {
    b->n = 0;
    if (!g_portalCoop || !self) return false;
    uint8_t* ent = reinterpret_cast<uint8_t*>(self);
    void* spellVft = reinterpret_cast<void*>(g_base + RVA_SPELLENTITY_VFT);
    if (*reinterpret_cast<void**>(ent) != spellVft) return false;            // not a SpellEntity
    uint8_t* owner = *reinterpret_cast<uint8_t**>(ent + OFF_ENT_OWNER);
    if (!owner) return false;
    void** pPortal   = reinterpret_cast<void**>(owner + OFF_PLAYER_PORTAL);     // {ptr, ctrl}
    void** pEntrance = reinterpret_cast<void**>(owner + OFF_PLAYER_PORTALENT);  // {ptr, ctrl}
    if (pPortal[0] && pEntrance[0]) return false;   // already linked (caster's machine): nothing to do
    uint8_t* level = *reinterpret_cast<uint8_t**>(ent + OFF_ENT_LEVEL);
    if (!level) return false;
    // Level::entities (+0xfd0) is a POINTER to a std::vector<shared_ptr<Entity>> (12 bytes: begin/end/cap),
    // NOT an inline vector. Dereference once to reach the vector, then read begin/end.
    uint8_t* entvec = *reinterpret_cast<uint8_t**>(level + OFF_LEVEL_ENTITIES);
    if (!entvec) return false;
    void** vbegin = *reinterpret_cast<void***>(entvec + 0);
    void** vend   = *reinterpret_cast<void***>(entvec + 4);
    if (!vbegin || vend < vbegin) return false;
    void* exitPtr = nullptr, *exitCtrl = nullptr, *entrPtr = nullptr, *entrCtrl = nullptr;
    for (void** it = vbegin; it < vend; it += 2) {       // 8-byte shared_ptr elements: {raw, ctrl}
        uint8_t* e = reinterpret_cast<uint8_t*>(it[0]);
        if (!e) continue;
        if (*reinterpret_cast<void**>(e) != spellVft) continue;                 // SpellEntity only
        if (*reinterpret_cast<void**>(e + OFF_ENT_OWNER) != owner) continue;    // same caster
        int type = *reinterpret_cast<int*>(e + OFF_ENT_SPELLTYPE);
        if      (type == SPELLTYPE_PORTAL)          { exitPtr = it[0]; exitCtrl = it[1]; }
        else if (type == SPELLTYPE_PORTAL_ENTRANCE) { entrPtr = it[0]; entrCtrl = it[1]; }
    }
    if (!pPortal[0] && exitPtr) {
        b->slot[b->n] = pPortal;   b->saved[b->n][0] = pPortal[0];   b->saved[b->n][1] = pPortal[1];
        pPortal[0] = exitPtr;      pPortal[1] = exitCtrl;            b->n++;
    }
    if (!pEntrance[0] && entrPtr) {
        b->slot[b->n] = pEntrance; b->saved[b->n][0] = pEntrance[0]; b->saved[b->n][1] = pEntrance[1];
        pEntrance[0] = entrPtr;    pEntrance[1] = entrCtrl;          b->n++;
    }
    return b->n > 0;
}
static void portal_link_restore(PortalBorrow* b) {
    for (int i = 0; i < b->n; i++) { b->slot[i][0] = b->saved[i][0]; b->slot[i][1] = b->saved[i][1]; }
    b->n = 0;
}
// Swap the {x,y} positions of the two portal endpoints (mirrors the caster-only owner-branch swap in
// stock enterPortal). Entity position is at +0xac (x) / +0xb0 (y).
static void portal_swap_positions(void* a, void* c) {
    if (!a || !c || a == c) return;
    float* ax = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a) + 0xac);  // x
    float* ay = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a) + 0xb0);  // y
    float* cx = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(c) + 0xac);
    float* cy = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(c) + 0xb0);
    float t;
    t = *ax; *ax = *cx; *cx = t;
    t = *ay; *ay = *cy; *cy = t;
}
// Pre-hook: supply the missing entrance<->exit link (when this isn't the caster's machine), run the
// stock teleport, then put the borrowed Player fields back. A rare same-owner concurrent use (server
// thread + main thread) can null the borrow mid-call: the worst case is one missed teleport, retried
// on the next press. No refcounts are touched, so the borrowed entities can never be freed underneath.
//
// Stock enterPortal only swaps the entrance<->exit positions when the CASTER uses the portal (owner
// branch); a teammate just teleports. To keep co-op behaviour identical to solo, when a teammate uses
// the portal we perform that swap ourselves -- but only on the MP HOST (the portals are host-owned, so
// the swap must be authoritative to sync to everyone) and only once per use (a short per-portal cooldown
// absorbs the collision-driven repeat calls while the player still overlaps).
static void* g_lastSwapPortal = nullptr;
static DWORD g_lastSwapTime   = 0;
static void __fastcall det_enterPortal(void* self, void* edx, void* spPtr, void* spCtrl) {
    if (!g_enterPortalTramp) return;
    PortalBorrow b;
    bool borrowed = portal_link_borrow(self, &b);
    // Capture the two endpoints + the teammate/host decision while the link is valid (borrow filled it).
    uint8_t* owner = self ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(self) + OFF_ENT_OWNER) : nullptr;
    bool teammate  = owner && spPtr && spPtr != owner;
    bool isHost    = (GetTickCount() - g_lastServerStep) < 500;   // Server::step ran recently => MP host
    void** pPortalSlot = owner ? reinterpret_cast<void**>(owner + OFF_PLAYER_PORTAL) : nullptr;      // {ptr, ctrl}
    void** pEntrSlot   = owner ? reinterpret_cast<void**>(owner + OFF_PLAYER_PORTALENT) : nullptr;   // {ptr, ctrl}
    void* pExit = pPortalSlot ? pPortalSlot[0] : nullptr;   // stock teleport destination is always owner.portal
    void* pEntr = pEntrSlot   ? pEntrSlot[0]   : nullptr;
    // Two-way: stock always sends you to owner.portal (the exit). If two-way is on and you entered the
    // EXIT end, point owner.portal at the ENTRANCE for this call so you teleport to the opposite end.
    // Restored right after the stock call (independent of the borrow, which may not own this slot).
    bool redir = false; void* savedPortal[2] = { nullptr, nullptr };
    if (g_portalTwoWay && pPortalSlot && pEntrSlot && pExit && pEntr && self == pExit) {
        savedPortal[0] = pPortalSlot[0]; savedPortal[1] = pPortalSlot[1];
        pPortalSlot[0] = pEntrSlot[0];   pPortalSlot[1] = pEntrSlot[1];
        redir = true;
    }
    g_enterPortalTramp(self, edx, spPtr, spCtrl);   // stock teleport (now it sees the link)
    if (redir) { pPortalSlot[0] = savedPortal[0]; pPortalSlot[1] = savedPortal[1]; }
    if (g_portalCoop && isHost && teammate && pExit && pEntr && pExit != pEntr) {
        DWORD now = GetTickCount();
        if (self != g_lastSwapPortal || now - g_lastSwapTime > 500) {   // once per fresh use
            portal_swap_positions(pExit, pEntr);
            g_lastSwapPortal = self;
            g_lastSwapTime   = now;
        }
    }
    if (borrowed) portal_link_restore(&b);
}

// ---- Two-way portal: make the EXIT end interactable too ------------------
// Player::doCollision registers the touched portal as the interactable one (Player+0x7798) ONLY for
// SpellType 2 (PORTAL_ENTRANCE); the exit (type 1) is skipped, so you can only press interact on the
// entrance. The site is `cmp [ebp-0x20], 2 ; jne 0x6f839d` at 0x2f7d14/0x2f7d18. We overwrite the jne
// with a jump to this naked handler: with two_way on it registers type 1 OR 2; off, it reproduces the
// stock "type 2 only" behaviour (so the patch is safe to install unconditionally + live-toggle).
static const uintptr_t RVA_PORTAL_INTERACT_JNE = 0x002f7d18;  // jne 0x6f839d (0F 85 7F 06 00 00)
static uintptr_t g_portalRegVA  = 0;   // 0x2f7d1e: fall-through that stores Player+0x7798 = this portal
static uintptr_t g_portalSkipVA = 0;   // 0x2f839d: not a portal to register
__declspec(naked) static void det_portalInteract() {
    __asm {
        cmp  byte ptr g_portalTwoWay, 0
        jne  two_way
        cmp  dword ptr [ebp-0x20], 2        // stock: entrance only
        je   go_reg
        jmp  go_skip
    two_way:
        cmp  dword ptr [ebp-0x20], 1        // exit
        je   go_reg
        cmp  dword ptr [ebp-0x20], 2        // entrance
        je   go_reg
    go_skip:
        jmp  dword ptr [g_portalSkipVA]
    go_reg:
        jmp  dword ptr [g_portalRegVA]
    }
}
static bool install_portal_twoway() {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + RVA_PORTAL_INTERACT_JNE);
    static const uint8_t expect[6] = {0x0F, 0x85, 0x7F, 0x06, 0x00, 0x00};   // jne 0x6f839d
    if (memcmp(p, expect, 6) != 0) { logmsg("[vultra] portal two-way: byte mismatch, skipped"); return false; }
    g_portalRegVA  = g_base + 0x002f7d1e;
    g_portalSkipVA = g_base + 0x002f839d;
    DWORD oldp;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&det_portalInteract) - (p + 5));
    p[5] = 0x90;
    VirtualProtect(p, 6, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}

// ---- Random character skill-tree count ----------------------------------
// PlayerMenu::randomizeAffinities shuffles a 16-entry pool of affinity types and copies the first N
// into the rolled loadout via `mov esi, 4` (BE 04 00 00 00). We rewrite that imm8 to g_randomTreeCount.
// Idempotent (only the immediate changes; the 0xBE opcode is the guard), so it is safe to re-apply on
// live ini reload. The pool is 16 distinct, so 1..16 never reads past the array.
static const uintptr_t RVA_RANDOMIZE_AFFINITIES = 0x0031dfe0;  // PlayerMenu::randomizeAffinities (thiscall, no args)
static const uintptr_t RVA_RANDOM_TREE_COUNT    = 0x0031e3e4;  // `mov esi, 4` (BE 04 00 00 00) in PlayerMenu::randomizeAffinities
static const uintptr_t RVA_POPULATE_AFFINITIES  = 0x0030a8c0;  // PlayerCustomizer::populateAffinities (cdecl)
static const size_t    OFF_MENU_ROLLED_AFFS     = 0x33bc;      // PlayerMenu: vector<AffinityType> rolled list (begin/end/cap)
// The take-N copy loop right after this is a do-while, so a raw `mov esi, 0` would underflow (esi--
// from 0) and run ~4 billion times -> crash. And an EMPTY rolled list is no good either: the
// character-build path (initializePlayer) treats an empty custom affinity list as "fixed class" and
// falls back to the class-default trees (setStartingAffinities). So for 0 trees we still roll ONE
// entry here (`mov esi, 1`) to keep the list non-empty -> the build takes the populate branch (no
// class defaults). det_randomizeAffinities then overwrites that entry with the AFFINITY_NO_TREES
// marker, and det_populateAffinities drops every marker, so zero real trees land.
//   The marker lives IN the list, so the decision travels with the character (the list is what the
// client sends to the host). Every machine that builds the character strips it the same way, whatever
// its own random_tree_count says. Before, each machine decided from its own ini, so a host set to 0
// wiped every Random client's trees, and a client set to 0 showed up on the host with 1 tree.
//   Type 17 is never in the roll pool, and it's a real type (the hidden baseline affinity that
// populateAffinities appends five of), so an unmodded machine that receives the marker builds one
// extra hidden baseline instead of crashing (Affinity::getNewAffinity has a case for 0..25).
static const int AFFINITY_NO_TREES = 17;
static void apply_random_tree_count() {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + RVA_RANDOM_TREE_COUNT);
    // Accept the stock site (mov esi, imm32) or our own prior rewrite (mov esi, n / legacy jmp-skip).
    bool known = (p[0] == 0xBE && p[3] == 0x00 && p[4] == 0x00) || (p[0] == 0xEB && p[1] == 0x2D);
    if (!known) { logmsg("[vultra] random-tree-count site mismatch (op=0x%02x)", p[0]); return; }
    int n = g_randomTreeCount;
    if (n < 0) n = 0; if (n > 16) n = 16;           // pool is 16 distinct affinity types (0..15)
    uint8_t cnt = (n == 0) ? 1 : (uint8_t)n;         // 0 rolls 1 placeholder; det_randomizeAffinities marks it
    uint8_t want[5] = { 0xBE, cnt, 0x00, 0x00, 0x00 };   // mov esi, cnt
    if (memcmp(p, want, 5) == 0) return;            // already set
    DWORD oldp;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldp)) return;
    memcpy(p, want, 5);
    VirtualProtect(p, 5, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logmsg("[vultra] random skill-tree count = %d", n);
}

// Post-hook on the roll: with random_tree_count == 0 the list holds the single placeholder entry
// (see above). Overwrite it with the AFFINITY_NO_TREES marker. If the count was changed between the
// patch and this roll, the list length won't be 1, so we leave it alone.
typedef void (__fastcall *randomizeAff_t)(void* self, void* edx);
static void reroll_with_woodsman_trees(void* menu);   // Woodsman trees in the Random pool (below)
static randomizeAff_t g_randomizeAffTramp = nullptr;
static void __fastcall det_randomizeAffinities(void* self, void* edx) {
    g_randomizeAffTramp(self, edx);
    reroll_with_woodsman_trees(self);
    if (g_randomTreeCount != 0 || !self) return;
    int** vec = reinterpret_cast<int**>(reinterpret_cast<uint8_t*>(self) + OFF_MENU_ROLLED_AFFS);
    if (vec[0] && vec[1] == vec[0] + 1) vec[0][0] = AFFINITY_NO_TREES;
}

// ---- New Game Plus / "The Cycle" looping --------------------------------
// GameSave::initGameState(GameEngine*) const: thiscall, ecx=this(GameSave). It reads
// this->m_newGamePlus and, when set, applies the harder NG+ scaling. We force the flag
// on entry when the loop toggle is enabled so the run plays as New Game Plus.
typedef void (__fastcall *initGameState_t)(void* self, void* edx, void* ge);
static initGameState_t g_igsTramp = nullptr;
static void __fastcall det_initGameState(void* self, void* edx, void* ge) {
    reload_if_ini_changed();
    // By default we do NOT force m_newGamePlus here — for the Looping toggle, NG+ must not begin
    // until the player actually loops (loop_prep sets the persisted flag at the win, so later
    // initGameState calls pick it up naturally; a fresh run starts normal).
    // NG+ turns on once the current loop number reaches "start_difficulty" (the loop # where NG+
    // begins). On a fresh run the loop number is 1, so only start_difficulty == 1 forces NG+ from
    // floor 1; the default (2) leaves the first playthrough normal until the player loops.
    if (ngplus_active()) {
        if (self) *(reinterpret_cast<uint8_t*>(self) + OFF_NEWGAMEPLUS)    = 1;   // GameSave::m_newGamePlus
        if (ge)   *(reinterpret_cast<uint8_t*>(ge)   + OFF_GE_NEWGAMEPLUS) = 1;   // GameEngine::newGamePlus
    }
#if DIAG_WIN
    {
        void* ret = _ReturnAddress();
        uint8_t m = self ? *(reinterpret_cast<uint8_t*>(self) + OFF_NEWGAMEPLUS) : 0xFF;
        uint8_t g = ge   ? *(reinterpret_cast<uint8_t*>(ge)   + OFF_GE_NEWGAMEPLUS) : 0xFF;
        logmsg("[win] initGameState gs=%p ge=%p m_newGamePlus=%d ge.newGamePlus=%d caller_rva=0x%x loop=%d",
               self, ge, m, g, (unsigned)((uintptr_t)ret - g_base), g_loop);
    }
#endif
    g_igsTramp(self, edx, ge);
}

// ---- LOOP: same-character warp restart (the dev debug-warp primitive) ---
// A live GameEngine pointer, captured every frame from Client::step. GameEngine is a subobject
// of the singleton Client, so its address is stable for the whole process once captured.
static void*     g_ge = nullptr;
static uintptr_t g_clientLoadNextLevelVA = 0;   // Client::loadNextLevel
static uintptr_t g_setNewGamePlusVA      = 0;   // LevelGenerator::setNewGamePlus

// Invoke Client::loadNextLevel(this=client, level) — __thiscall, callee cleans the one arg.
static void __declspec(noinline) call_client_loadNextLevel(void* client, void* level) {
    __asm {
        mov  ecx, client
        mov  eax, level
        push eax
        mov  eax, g_clientLoadNextLevelVA
        call eax                 // ret 4: the pushed arg is cleaned by the callee
    }
}
// Invoke LevelGenerator::setNewGamePlus(this=levelgen) — __thiscall, no args.
static void __declspec(noinline) call_setNewGamePlus(void* levelgen) {
    __asm {
        mov  ecx, levelgen
        mov  eax, g_setNewGamePlusVA
        call eax
    }
}
static uintptr_t g_serverLoadNextLevelVA = 0;   // Server::loadNextLevel
// Invoke Server::loadNextLevel(this=server) — __thiscall, no args. Regenerates the floor on the
// server's GameEngine and net-syncs all clients. MUST be called on the server thread.
static void __declspec(noinline) call_server_loadNextLevel(void* server) {
    __asm {
        mov  ecx, server
        mov  eax, g_serverLoadNextLevelVA
        call eax
    }
}
// MP F11 warp handshake: F11 on the client thread sets g_mpWarpRequest; the server thread consumes
// it in its Server::step hook and warps the authoritative GameEngine (synced to every client).
// g_lastServerStep lets the client tell host (server active) from single-player (no server) so SP
// still falls back to the client-side warp. (The boss-win LOOP path does not use this flag; it calls
// server_loop_now directly from the win detection.)
static volatile LONG g_mpWarpRequest = 0;
// g_lastServerStep defined earlier (near the portal hook) so det_enterPortal can read it.
// Boss-defeat loop handshake: instead of letting the final boss trigger its ending cutscene (which
// sets Level::endOfCampaign and syncs the win to every client -> credits), the Cutscene::trigger hook
// SKIPS the FINAL_BOSS_DEFEATED[_FINAL_FORM] cutscene and sets this flag. The boss never "wins" the
// game, so no client (modded or not) ever rolls credits. The server thread (MP host) or step_frame
// (single-player) then consumes the flag and loops to the start floor. Set on the host's sim thread.
static volatile LONG g_bossLoopRequest = 0;

// Prepare a same-character loop restart: reset to the start floor, mark New Game Plus on the
// GameEngine + GameSave (persists), and arm the LevelGenerator's NG+ scaling so floors spawn
// harder. Sets the transition "mode" byte so the next loadNextLevel regenerates the floor on
// the existing party (players persist across floor transitions). Does NOT itself load.
// Core prep that operates purely on a GameEngine (client's OR server's): reset floor, NG+ live flag,
// arm scaling, set the regenerate-next byte. No GameSave write (that offset is only valid relative to
// a Client; the server GameEngine is not a Client subobject).
// Shared transition prep: set the destination floor + the regenerate-next byte; optionally arm NG+.
// NG+ is for the real loop restart (harder); the F11 debug warp passes ngplus=false so it's a clean
// warp that does NOT bump difficulty.
static void prep_transition_core(void* ge, int floor, bool ngplus) {
    if (!ge) return;
    uint8_t* g = reinterpret_cast<uint8_t*>(ge);
    *reinterpret_cast<int*>(g + OFF_FLOORINDEX) = floor;                     // destination floor
    if (ngplus) {
        *(g + OFF_GE_NEWGAMEPLUS) = 1;                                       // live flag: The Cycle
        call_setNewGamePlus(g + OFF_GE_LEVELGEN);                            // arm NG+ scaling
    }
    *(g + OFF_GE_WARPMODE) = 1;                                              // engine: regenerate next
    if (g_campfireOnWrap) *(g + OFF_GE_INTERMISSION) = 1;                    // (currently a no-op; see notes)
}
// Loop restart prep: NG+ ON, restart at the loop start floor. Each call = one completed cycle, so
// bump the loop counter that drives the escalating monster scaling. Called once per real boss-win
// loop (SP via loop_prep, MP via server_loop_now); the win detection clears the victory flag so it
// can't double-fire. NOT called by the F11 debug warp (that uses prep_transition_core directly).
static void loop_prep_core(void* ge) {
    InterlockedIncrement(&g_loopCount);            // we are now entering the next loop number
    g_fairyBlessCount = 0;                          // clear stacked fairy blessings each cycle (no unbounded growth)
    bool ng = ngplus_active();                      // NG+ only once the loop number reaches start_difficulty
    logmsg("[loop] entering loop #%ld (NG+=%d, scaling starts loop #%d, harderBy=%.1f%%)",
           current_loop_number(), ng ? 1 : 0, g_scalingStartsAt, g_harderByPct);
    prep_transition_core(ge, g_loopStartFloor, ng);
}
static void loop_prep(void* ge) {
    if (!ge) return;
    uint8_t* g = reinterpret_cast<uint8_t*>(ge);
    loop_prep_core(ge);
    *(g - OFF_CLIENT_GE + OFF_CLIENT_GAMESAVE + OFF_NEWGAMEPLUS) = 1;        // persisted flag (client only)
}

// ---- Start a FRESH run already at New Game Plus difficulty ----------------
// Why this can't ride on det_initGameState: GameSave::initGameState (the only place that reads
// m_newGamePlus and calls setNewGamePlus) is invoked from EXACTLY ONE site (0x560b84), which only
// runs on the post-win / save-load transition (Client+0x4c5 != 0). A brand-new "New Game" takes
// the ordinary path (Client+0x4c5 == 0 -> loadNextLevel), so initGameState never fires and forcing
// the flag there is a no-op for a fresh run.
// Hooking GameEngine::newGame to arm it was tried and CRASHED (the detour didn't survive newGame's
// server-thread call site). The working approach is fully self-driven from step_frame: see the
// levelgen+0x50 check there. No newGame hook, no extra trampoline.

// Clear the boss-win victory state so looped players aren't stuck invincible / auto-walking.
// Must be called AFTER the floor regenerates (so it targets the new level + still-live players);
// GE::step otherwise re-stamps gameOver every frame while the level's victory-trigger is set.
static void clear_victory_state(void* ge) {
    if (!ge) return;
    uint8_t* g = reinterpret_cast<uint8_t*>(ge);
    *(g + OFF_GE_GAMEOVER) = 0;
    *(g + OFF_GE_GAMECOMPLETE) = 0;
    *reinterpret_cast<int*>(g + OFF_GE_GAMEOVERTIMER) = 0;
    uint8_t* lvl = *reinterpret_cast<uint8_t**>(g + OFF_GE_LEVEL);
    int v1126 = lvl ? *(lvl + OFF_LEVEL_VICTORY) : -1;
    if (lvl) *(lvl + OFF_LEVEL_VICTORY) = 0;                                 // stop the victory block re-firing
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_END);
    int n = 0, p0go = -1, p0iv = -1, p0imm = -1;
    for (uint8_t* e = pb; pb && pe && e < pe; e += PLAYER_VEC_STRIDE) {
        uint8_t* p = *reinterpret_cast<uint8_t**>(e);
        if (!p) continue;
        if (n == 0) { p0go = *(p + OFF_PLAYER_GAMEOVER); p0iv = *reinterpret_cast<int*>(p + OFF_PLAYER_INVULFRAMES);
                      p0imm = *(p + OFF_PLAYER_IMMORTAL); }
        *(p + OFF_PLAYER_GAMEOVER)   = 0;
        *reinterpret_cast<int*>(p + OFF_PLAYER_INVULFRAMES)  = 0;
        *reinterpret_cast<int*>(p + OFF_PLAYER_INCORPOREAL)  = 0;
        *(p + OFF_PLAYER_IMMORTAL)   = 0;   // Entity::immortal: set by the boss-death victory walk; this is the fix
        n++;
    }
    logmsg("[loop] clear_victory: lvl=%p lvl+0x1126=%d players=%d p0.gameOver=%d p0.invul=%d p0.immortal=%d",
           lvl, v1126, n, p0go, p0iv, p0imm);
}
// Diagnostic: write the full player-0 struct to a file so the F11 (hittable) and boss-win
// (invincible) post-loop states can be diffed byte-for-byte. After the loop both paths place
// the player at the same dark-caves-1 spawn, so the only meaningful difference is the bug flag.
static void dump_player_full(void* ge, const char* fname) {
    if (!g_loopTestWarp || !ge) return;
    uint8_t* g = reinterpret_cast<uint8_t*>(ge);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    if (!pb) return;
    uint8_t* p = *reinterpret_cast<uint8_t**>(pb);
    if (!p) return;
    char path[MAX_PATH]; sprintf_s(path, "%s\\%s", "J:\\SteamLibrary\\steamapps\\common\\Vagante", fname);
    FILE* f = nullptr; fopen_s(&f, path, "wb");
    if (!f) { logmsg("[dump] could not open %s", path); return; }
    fwrite(p, 1, 0x7a00, f); fclose(f);
    logmsg("[dump] wrote %s (player=%p, 0x7a00 bytes)", fname, p);
}
// F11 test path: prep + actually trigger the regeneration via the dev debug-warp primitive.
static void do_loop_warp(void* ge) {
    if (!ge) return;
    prep_transition_core(ge, g_warpFloor, false); // F11: warp to the chosen floor, NO NG+ (clean debug warp)
    logmsg("[loop] F11 WARP: floorIndex<-%d, NG+ off, ge=%p", g_warpFloor, ge);
    call_client_loadNextLevel(reinterpret_cast<uint8_t*>(ge) - OFF_CLIENT_GE, nullptr);
    clear_victory_state(ge);                      // after regen: drop any victory immunity
    dump_player_full(ge, "player_f11.bin");       // hittable baseline
}

// Same-character LOOP on the CLIENT GameEngine (single-player path): restart at the loop start floor
// with NG+, regenerate, then clear any victory state. Used by the boss-defeat loop (g_bossLoopRequest)
// and shared with win_maybe_loop's body.
static void client_loop_now(void* client) {
    if (!client) return;
    void* ge = reinterpret_cast<uint8_t*>(client) + OFF_CLIENT_GE;
    loop_prep(ge);
    call_client_loadNextLevel(client, nullptr);
    clear_victory_state(ge);
}

// Win -> loop: the boss-win and the pre-boss intermission share floorIndex 12, so floor number
// can't tell them apart. The reliable signal is the ending/credits flag (Credits+0xf3b), set only
// on a real win. When we see it (and looping is on), clear it and warp-restart instead of credits.
static bool win_maybe_loop(void* client) {
    if (!g_loop || !client) return false;
    void* credits = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(client) + OFF_CLIENT_CREDITS);
    if (!credits) return false;
    uint8_t* active = reinterpret_cast<uint8_t*>(credits) + OFF_CREDITS_ACTIVE;
    if (!*active) return false;
    *active = 0;                                                        // suppress credits
    *(reinterpret_cast<uint8_t*>(credits) + OFF_CREDITS_ACTIVE2) = 0;
    void* ge = reinterpret_cast<uint8_t*>(client) + OFF_CLIENT_GE;
    loop_prep(ge);
    logmsg("[loop] WIN (ending flag) -> looping to %d (+NG+)", g_loopStartFloor);
    call_client_loadNextLevel(client, nullptr);
    clear_victory_state(ge);                      // after regen: drop the victory invincibility
    dump_player_full(ge, "player_boss.bin");      // invincible case
    return true;
}

// Diagnostic: dump player 0's remaining Player::hurt damage-skip guards so we can see which one
// is stuck while the player is walking around invincible. (Logged ~once/sec when test_warp is on.)
static void dump_player_guards(void* ge) {
    if (!ge) return;
    uint8_t* g = reinterpret_cast<uint8_t*>(ge);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_END);
    if (!pb || pb >= pe) return;
    uint8_t* p = *reinterpret_cast<uint8_t**>(pb);
    if (!p) return;
    uint8_t* m = *reinterpret_cast<uint8_t**>(p + 0x1a0);
    logmsg("[diag] p0 enter=%d exit=%d teleport=%d invul=%d incorp=%d noclip=%d gameOver=%d a3=%d p990=%d pc80=%d p1a0=%p p1a0+4=%d",
           *(p + 0x7f1), *(p + 0x7f4), *(p + 0x78c0), *reinterpret_cast<int*>(p + 0x78d8),
           *reinterpret_cast<int*>(p + 0x78dc), *(p + 0x8a4), *(p + 0x800), *(p + 0xa3),
           *(p + 0x990), *reinterpret_cast<int*>(p + 0xc80),
           m, m ? *(m + 4) : -1);
}

// Diagnostic hook on Player::hurt: log the damage-skip guards at the instant a hit is processed.
// If the invincible player IS hit (hurt called) we see which guard fires; if hurt is never called
// on a hit, the immunity is enemy/collision-side, not a player flag.
static const uintptr_t RVA_PLAYER_HURT = 0x002f9020;
static void* g_hurtTramp = nullptr;
// Dump labeled dword windows of the Player struct so the invincible (boss-win) and hittable (F11)
// states can be diffed offline. The single field that differs is the victory-invincibility flag.
static void dump_player_window(uint8_t* u) {
    struct { unsigned off, n; } win[] = {
        {0x00a0, 8}, {0x0460, 8}, {0x07c0, 18}, {0x0800, 16},
        {0x08a0, 18}, {0x0980, 16}, {0x0af0, 18}, {0x0b30, 16},
        {0x0c70, 8}, {0x77d0, 18},
    };
    for (auto& w : win) {
        char line[512]; int o = 0;
        o += sprintf_s(line + o, sizeof(line) - o, "[pdump] +0x%04x:", w.off);
        for (unsigned i = 0; i < w.n; i++)
            o += sprintf_s(line + o, sizeof(line) - o, " %08x", *reinterpret_cast<uint32_t*>(u + w.off + i * 4));
        logmsg("%s", line);
    }
}
static void __cdecl hurt_log(void* p) {
    if (!g_loopTestWarp || !p) return;
    static DWORD last = 0; DWORD now = GetTickCount();
    if (now - last < 250) return; last = now;     // throttle: a few lines/sec is plenty
    static DWORD lastDump = 0;
    if (now - lastDump >= 1500) { lastDump = now; dump_player_window(reinterpret_cast<uint8_t*>(p)); }
    uint8_t* u = reinterpret_cast<uint8_t*>(p);
    uint8_t* m = *reinterpret_cast<uint8_t**>(u + 0x1a0);
    int geGC = -1, geGO = -1, geGOT = -1;
    if (g_ge) { uint8_t* g = reinterpret_cast<uint8_t*>(g_ge);
        geGO = *(g + OFF_GE_GAMEOVER); geGC = *(g + OFF_GE_GAMECOMPLETE); geGOT = *reinterpret_cast<int*>(g + OFF_GE_GAMEOVERTIMER); }
    logmsg("[hurt] p=%p enter=%d exit=%d teleport=%d invul=%d incorp=%d noclip=%d pGameOver=%d a3=%d 1a0+4=%d | p990=%d pc80=%d | ge.gameOver=%d ge.gameComplete=%d ge.goTimer=%d",
           p, *(u + 0x7f1), *(u + 0x7f4), *(u + 0x78c0), *reinterpret_cast<int*>(u + 0x78d8),
           *reinterpret_cast<int*>(u + 0x78dc), *(u + 0x8a4), *(u + 0x800), *(u + 0xa3), m ? *(m + 4) : -1,
           *(u + 0x990), *reinterpret_cast<int*>(u + 0xc80),
           geGO, geGC, geGOT);
}
static bool bomber_spares_player(const uint8_t* player, const uint8_t* meta);   // Bomber tree (below)
// Player::hurt(meta) (thiscall, ret 4). Returns nonzero when the hit is dropped before Player::hurt runs.
static int __cdecl player_hurt_pre(void* p, const uint8_t* meta) {
    hurt_log(p);
    return bomber_spares_player(static_cast<const uint8_t*>(p), meta) ? 1 : 0;
}
__declspec(naked) static void det_hurt() {
    __asm {
        pushad
        push dword ptr [esp+0x24] // meta (past the 8 saved registers and our return address)
        push ecx                 // this (Player)
        call player_hurt_pre
        add  esp, 8
        test eax, eax
        jnz  skip
        popad
        jmp  dword ptr [g_hurtTramp]
    skip:
        popad
        xor  eax, eax            // not hurt
        ret  4
    }
}

// Per-frame hook on Client::step: capture the live GameEngine and poll the test hotkey.
typedef void (__fastcall *clientStep_t)(void* self, void* edx);
static clientStep_t g_stepTramp = nullptr;
static void flush_enemy_dups(void* ge);   // defined below (enemy-duplication section)
static void sunder_tick();                // Brutalist tree (below)
static void woodsman_tick();
static void test_frame(void* client);     // testharness.h: runs one step of a test script (inert without one)
static void __cdecl step_frame(void* client) {
    reload_if_ini_changed();   // pick up manager "Save" live (e.g. in the lobby) -> re-syncs mage options
    if (client) g_ge = reinterpret_cast<uint8_t*>(client) + OFF_CLIENT_GE;
    test_frame(client);
    flush_enemy_dups(g_ge);    // spawn any queued enemy copies into the live level (deferred, safe here)
    sunder_tick();             // Brutalist: put back enemy defense as Sunder stacks run out
    woodsman_tick();           // Brutalist: Momentum streaks and the Party afterimage
    // Difficulty model (self-driven; NO newGame hook — that crashed). Loop number = g_loopCount + 1:
    // NG+ turns on at loop # start_difficulty, the extra monster scaling at loop # scaling_starts_at
    // (defaults 2 / 3: loop 1 normal, loop 2 NG+, loop 3+ scaling). Two steps, ORDER MATTERS:
    //  (1) RESET the cycle count on a genuine new game. newGame clears newGamePlus (it resets the
    //      levelgen), a looped run keeps it set, so zeroing while the flag is clear lands exactly on a
    //      fresh run. Must run BEFORE the arming below — otherwise arming would set NG+=1 this same
    //      frame and the reset would never see the clear window. SP only (no recent server step); an MP
    //      host resets on the server side in server_step_frame so the two never fight.
    if (g_ge && (GetTickCount() - g_lastServerStep > 500)
        && *(reinterpret_cast<uint8_t*>(g_ge) + OFF_GE_NEWGAMEPLUS) == 0)
        g_loopCount = 0;
    //  (2) ARM NG+ for the starting tier. The game never arms NG+ on a fresh run (initGameState, which
    //      would, doesn't run on the ordinary new-game path), so we arm it here on the live client GE —
    //      the exact engine/state F11's setNewGamePlus arms safely. levelgen+0x50 is 0 on a fresh run
    //      and stays 1 once set, so this fires ONCE: as soon as a level exists (the lobby), well before
    //      Caves 1 generates. Only arm when the loop number has reached start_difficulty: with the
    //      default (2) a fresh run (loop 1) stays normal; the loop machinery turns NG+ on at loop 2.
    if (ngplus_active() && g_ge) {
        uint8_t* g = reinterpret_cast<uint8_t*>(g_ge);
        uint8_t* lvl = *reinterpret_cast<uint8_t**>(g + OFF_GE_LEVEL);   // null at the main menu
        uint8_t* lg  = g + OFF_GE_LEVELGEN;
        if (lvl && *(lg + 0x50) == 0) {
            *(g + OFF_GE_NEWGAMEPLUS) = 1;                  // live engine flag
            call_setNewGamePlus(lg);                        // arm levelgen NG+ scaling (+0x50)
            logmsg("[ngplus] armed on live client GE=%p (loop#=%ld, start_difficulty=%d)",
                   g_ge, current_loop_number(), g_startDifficulty);
        }
    }
    // Boss defeated with looping on -> the Cutscene::trigger hook skipped the ending and set this flag.
    // Single-player loops here on the client GE; an MP host instead consumes it on the server thread
    // (gated SP-only via g_lastServerStep so the two never both fire).
    if (g_loop && g_bossLoopRequest && (GetTickCount() - g_lastServerStep > 500) && client) {
        InterlockedExchange(&g_bossLoopRequest, 0);
        logmsg("[loop] boss defeated (cutscene skipped) -> SP client loop");
        client_loop_now(client);
        return;
    }
    if (win_maybe_loop(client)) return;          // boss-win this frame -> looped instead of credits
    if (!g_loopTestWarp) return;
    if (g_ge) {                                  // diagnostic: surface the stuck damage-skip guard
        static int fc = 0;
        if (++fc >= 120) { fc = 0; dump_player_guards(g_ge); }
    }
    static bool prev = false;
    bool down = (GetAsyncKeyState(LOOP_TEST_HOTKEY) & 0x8000) != 0;
    if (down && !prev) { InterlockedExchange(&g_mpWarpRequest, 1); logmsg("[loop] F11 -> warp requested"); }
    prev = down;
    // Single-player fallback: no server has stepped recently => not an MP host => warp the client
    // GameEngine directly. On an MP host the server thread consumes the request first (sync path).
    if (g_mpWarpRequest && (GetTickCount() - g_lastServerStep > 500) && g_ge) {
        InterlockedExchange(&g_mpWarpRequest, 0);
        do_loop_warp(g_ge);
    }
    // F10: simulate the boss victory (F11 can't reproduce the invincibility; only a real win does).
    // Setting the current level's victory-trigger makes GameEngine::step run its victory block, so
    // we can observe the stuck damage guard via [diag] without fighting the boss. (F12 = Steam
    // screenshot, so this lives on F10 — the overlay that used it is disabled.)
    static bool prevSim = false;
    bool sim = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (sim && !prevSim && g_ge) {
        uint8_t* lvl = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(g_ge) + OFF_GE_LEVEL);
        if (lvl) { *(lvl + OFF_LEVEL_VICTORY) = 1; logmsg("[loop] F10 sim victory: set level+0x1126=1 lvl=%p", lvl); }
    }
    prevSim = sim;
    // F8: snapshot player-0 to snapN.bin. Dump just BEFORE the killing blow (hittable) and just
    // AFTER the boss dies (invincible) without moving -- same arena/position, so the diff is only
    // the won-state. (Far cleaner than the post-loop dump, where random floor regen moves the spawn.)
    static bool prevSnap = false; static int snapN = 0;
    bool snap = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (snap && !prevSnap && g_ge) {
        char fn[64]; sprintf_s(fn, "snap%d.bin", snapN++);
        dump_player_full(g_ge, fn);
    }
    prevSnap = snap;
}
__declspec(naked) static void det_step() {
    __asm {
        pushad
        push ecx                 // this (Client)
        call step_frame
        add  esp, 4
        popad
        jmp  dword ptr [g_stepTramp]
    }
}

// MP host: the F11 DEBUG WARP on the SERVER. Mirrors server_loop_now, but a CLEAN warp to the chosen
// floor (g_warpFloor) with NO NG+ and NO loop-count bump -- the level-transition sync then carries
// every client to that floor together. This is the multiplayer counterpart of do_loop_warp.
static void server_warp_now(void* server) {
    if (!server) return;
    void* sge = reinterpret_cast<uint8_t*>(server) + OFF_SERVER_GE;
    prep_transition_core(sge, g_warpFloor, false);   // warp to the chosen floor, NO NG+ (clean debug warp)
    logmsg("[loop] MP server warp: server=%p sge=%p floor<-%d (NG+ off)", server, sge, g_warpFloor);
    call_server_loadNextLevel(server);               // regenerate + net-sync all clients
    clear_victory_state(sge);                        // drop any victory immortal/walk on server players
}
// MP host: perform the loop on the SERVER (its own thread / authoritative GameEngine), so the
// built-in level-transition sync carries every client to the start floor together. Mirrors the
// client warp but targets Server+0xd20 and uses Server::loadNextLevel (which syncs).
static void server_loop_now(void* server) {
    if (!server) return;
    void* sge = reinterpret_cast<uint8_t*>(server) + OFF_SERVER_GE;
    loop_prep_core(sge);                          // floor=0, NG+, warpmode on the server GameEngine
    clear_victory_state(sge);                     // clear gameComplete/victory BEFORE the sync so the
                                                  // loadNextLevel broadcast can't carry the win flag down
    logmsg("[loop] MP server loop: server=%p sge=%p floor<-%d", server, sge, g_loopStartFloor);
    call_server_loadNextLevel(server);            // regenerate + net-sync all clients (clean state)
    clear_victory_state(sge);                     // and again on the NEW level (re-fire guard + players)
}
// Detect the authoritative boss-win on the server's own GameEngine. The server's GameEngine::step
// (first thing Server::step does, before any sendGameState broadcast) runs the victory block that
// sets gameComplete the frame the boss dies. Catching it here -- on the host, before the ending is
// ever computed/synced -- means we loop instead of the ending and NO client (modded or not) gets
// pushed into the credits / invincible victory-walk. We clear immortal on the server players and the
// regen + cleared state syncs down via the normal level-transition broadcast.
static bool server_won(void* sge) {
    uint8_t* ge = reinterpret_cast<uint8_t*>(sge);
    if (*(ge + OFF_GE_GAMECOMPLETE) != 0) return true;          // victory block fired (boss dead)
    void* lvl = *reinterpret_cast<void**>(ge + OFF_GE_LEVEL);    // current Level*
    if (lvl && *(reinterpret_cast<uint8_t*>(lvl) + OFF_LEVEL_VICTORY) != 0) return true; // victory trigger set
    return false;
}
// Server::step ENTRY hook (server thread). Records liveness and consumes a pending F11 loop request.
// The boss-win detection is NOT here anymore: at entry we'd only see LAST frame's gameComplete, which
// the previous frame's sendGameState already broadcast to clients -> they roll the credits before our
// loop lands (exactly the reported bug). Win detection now runs mid-step, pre-broadcast (see below).
typedef void (__fastcall *serverStep_t)(void* self, void* edx);
static serverStep_t g_serverStepTramp = nullptr;
static void __cdecl server_step_frame(void* server) {
    g_lastServerStep = GetTickCount();            // tell the client thread a server is active (= MP host)
    g_lastServer = server;
    // MP escalating-loop reset on the authoritative server GE: a fresh host run clears newGamePlus,
    // a looped run keeps it set (server_loop_now arms it). Keeps the cycle count correct on the host.
    if (server && *(reinterpret_cast<uint8_t*>(server) + OFF_SERVER_GE + OFF_GE_NEWGAMEPLUS) == 0)
        g_loopCount = 0;
    if (InterlockedExchange(&g_mpWarpRequest, 0) == 1) server_warp_now(server);   // F11 debug warp (MP)
    // Boss defeated (the Cutscene::trigger hook skipped the ending and set this): loop authoritatively
    // on the server so the regen + clean state syncs to every client. No endOfCampaign was ever set.
    if (g_loop && InterlockedExchange(&g_bossLoopRequest, 0) == 1) {
        logmsg("[loop] boss defeated (cutscene skipped) -> MP server loop");
        server_loop_now(server);
    }
}
__declspec(naked) static void det_server_step() {
    __asm {
        pushad
        push ecx                 // this (Server)
        call server_step_frame
        add  esp, 4
        popad
        jmp  dword ptr [g_serverStepTramp]
    }
}

// THE FIX: catch the boss-win on the server WITHIN the same frame it happens -- after the server's
// GameEngine::step has set gameComplete, but BEFORE Server::step reaches sendGameState. Looping here
// regenerates the start floor and clears the victory state, so the broadcast that frame carries the
// looped state, not the win -> no client (modded or not) ever enters the credits / victory-walk.
// Hook point = 0x76449b (right after the GE::step call @0x764496); ebx=Server. The game itself calls
// Server::loadNextLevel from inside Server::step, so doing the loop mid-step is its own pattern.
static void* g_serverPostTramp = nullptr;
static void __cdecl server_postwin(void* server) {
    if (!g_loop || !server) return;
    void* sge = reinterpret_cast<uint8_t*>(server) + OFF_SERVER_GE;
    if (server_won(sge)) {
        logmsg("[loop] MP win caught pre-broadcast -> looping host before sendGameState");
        server_loop_now(server);
    }
}
__declspec(naked) static void det_server_postgestep() {
    __asm {
        pushad
        push ebx                 // Server (ebx live at 0x76449b)
        call server_postwin
        add  esp, 4
        popad
        jmp  dword ptr [g_serverPostTramp]   // tramp runs the stolen cmp, then returns to 0x7644a2
    }
}

// ---- BOSS NEVER WINS: skip the final-boss-defeat cutscene, loop instead --------------------------
// BossFinal::hurt plays Cutscene::trigger(FINAL_BOSS_DEFEATED / _FINAL_FORM) when the boss is beaten;
// that cutscene is the ONLY writer of Level::endOfCampaign (-> credits, and it net-syncs to clients).
// When looping is on we intercept that trigger: don't start the ending cutscene, just request a loop.
// The boss is beaten on the host's sim thread (server in MP, client GE in SP), so no endOfCampaign is
// ever set anywhere -> NO client (modded or unmodded) can roll credits. The non-defeat scenes
// (ELEVATOR) pass through untouched. __thiscall(ecx=this, [esp+4]=Scene); cdecl-clean caller, ret 4.
static void* g_cutsceneTrigTramp = nullptr;
__declspec(naked) static void det_cutscene_trigger() {
    __asm {
        mov  eax, [esp+4]                      // Scene argument
        cmp  eax, CUTSCENE_FINAL_BOSS_DEFEATED
        je   maybe_skip
        cmp  eax, CUTSCENE_FINAL_BOSS_DEFEATED_FINAL_FORM
        jne  run_original                      // any other scene -> play it normally
    maybe_skip:
        cmp  byte ptr [g_loop], 0
        je   run_original                      // looping off -> let the ending play (vanilla win)
        mov  dword ptr [g_bossLoopRequest], 1  // looping on -> request the loop, skip the cutscene
        ret  4                                 // swallow the trigger (Scene arg is caller-pushed)
    run_original:
        jmp  dword ptr [g_cutsceneTrigTramp]
    }
}

// ---- ESCALATING DIFFICULTY: per-tier monster scaling ---------------------
// Monster::setLevel (0x2b37c0) finalizes the monster's healthScaleFactor (+0x334) and
// damageScaleFactor (+0x338) from the level-indexed MODIFIERS_SCALE arrays, then a vtable call
// (right after, @0x2b3930) applies them to the monster's stats. We hook the 9 bytes right AFTER both
// stores (@0x2b3925) and multiply both factors BEFORE that apply-call, so the boosted values feed the
// stat computation. esi = the Monster* throughout setLevel. The extra scaling (on TOP of NG+) only
// kicks in once the loop number (g_loopCount + 1) reaches g_scalingStartsAt, then adds g_harderByPct%
// per loop beyond, linearly, forever. This runs on the
// HOST's authoritative monster spawns (server GE in MP, client GE in SP), so the harder stats sync
// down to unmodded clients exactly like the rest of the loop. Installed unconditionally and gated at
// runtime, so manager changes take effect without a reinstall.
static const uintptr_t RVA_MONSTER_SETLEVEL_TAIL = 0x002b3925;
static const size_t    OFF_MON_HEALTHSCALE       = 0x334;
static const size_t    OFF_MON_DAMAGESCALE       = 0x338;
static void* g_setLevelTramp = nullptr;
static void __cdecl scale_monster_loop(void* mon) {
    if (!mon || g_harderByPct <= 0.0f) return;
    long L = current_loop_number();                             // 1 = first playthrough, 2 = first loop, ...
    if (L < (long)g_scalingStartsAt) return;                    // NG+ only (or normal), no extra scaling yet
    int tiers = (int)L - g_scalingStartsAt + 1;                 // 1 at the start loop, +1 each loop beyond
    float f = 1.0f + (g_harderByPct / 100.0f) * (float)tiers;
    if (f > 100.0f) f = 100.0f;                                 // sanity cap
    uint8_t* m = reinterpret_cast<uint8_t*>(mon);
    *reinterpret_cast<float*>(m + OFF_MON_HEALTHSCALE) *= f;
    *reinterpret_cast<float*>(m + OFF_MON_DAMAGESCALE) *= f;
}
__declspec(naked) static void det_setLevel() {
    __asm {
        pushad
        push esi                 // Monster* (this; esi holds it across setLevel)
        call scale_monster_loop
        add  esp, 4
        popad
        jmp  dword ptr [g_setLevelTramp]   // tramp runs the stolen 9 bytes, continues at 0x2b392e
    }
}

// ---- MORE ENEMIES: literally duplicate each spawned enemy N times -----------
// Every entity that enters a level passes through Level::addEntity (rva 0x24afc0,
// __thiscall(Level* ecx, shared_ptr<Entity>& [ebp+8]); ret 4). It copies the shared_ptr into the
// level (lock inc on the control block @0x24b061). Both enemy spawners (LevelGenerator::addRandomEnemies
// AND Level::respawnMonsters) funnel their monsters through it. We hook it: for a regular enemy
// (Entity+0x50 type in [TYPE_GOBLIN=9 .. TYPE_FLOATER=48], i.e. excluding bosses 49+), we let the
// original add happen, then construct N-1 more of the SAME type at the SAME position
// (Entity+0xac/0xb0 = x,y) via Entity::spawnEntity (0x1a5220) and add each. The game's physics push
// the stack apart. N = 1 + escalated% / 100 (capped). Host-side; the spawns are networked to clients.
// spawnEntity ABI (from its call site): ecx = &result(shared_ptr RVO), edx = type, [esp+4] = &pos,
// ret 0 (caller cleans the one stack arg). addEntity increfs, so we release our temp shared_ptr after.
typedef void (__thiscall *addEntity_t)(void* level, void* spRef);
typedef void (__thiscall *setpos_t)(void* entity, void* posPtr);
static addEntity_t g_addEntityTramp = nullptr;
static setpos_t    g_setTetheredPos = nullptr;     // = g_base + 0x1b01d0 (entity, &Vector2{x,y})
static uintptr_t   g_spawnEntityAddr = 0;          // = g_base + 0x1a5220
static volatile bool g_inEnemyDup = false;         // reentrancy guard
struct DupVec2 { float x, y; };
// spawnEntity's 3rd arg is a std::vector<spawn-data> (begin,end,cap), NOT a position. We pass an EMPTY
// one so it takes the default-construct path, then place the copy with setTetheredPosition.
static int g_emptySpawnVec[3] = { 0, 0, 0 };
static int dup_count_for_type(int type) {
    if (g_spawnIncreasePct <= 0) return 1;
    if (type < 9 || type > 48) return 1;                        // regular enemies only (bosses 49+ excluded)
    long L = current_loop_number();
    if (L < (long)g_spawnIncreaseStartsAt) return 1;
    int tiers = (int)L - g_spawnIncreaseStartsAt + 1;
    long effPct = (long)g_spawnIncreasePct * tiers;
    if (g_spawnIncreaseMaxPct > 0 && effPct > (long)g_spawnIncreaseMaxPct) effPct = g_spawnIncreaseMaxPct;
    // Whole 100s = guaranteed extra copies; the leftover % is a per-enemy CHANCE of one more, so
    // sub-100% values (10%, 50%, 250%...) average out correctly over a level full of enemies.
    long n   = 1 + effPct / 100;
    int  frac = (int)(effPct % 100);
    if (frac > 0) {
        static bool seeded = false;
        if (!seeded) { srand(GetTickCount()); seeded = true; }
        if ((rand() % 100) < frac) n += 1;
    }
    if (n < 1)  n = 1;
    if (n > 64) n = 64;                                         // hard safety ceiling on copies per enemy
    return (int)n;
}
// Standard MSVC shared_ptr control-block release on our temp copy (ctrl = sp[1]).
static void release_shared_block(void* ctrl) {
    if (!ctrl) return;
    void** vt = *reinterpret_cast<void***>(ctrl);
    typedef void (__thiscall *ctrlfn)(void*);
    if (InterlockedDecrement(reinterpret_cast<long*>((uint8_t*)ctrl + 4)) == 0) {
        ((ctrlfn)vt[0])(ctrl);                                  // destroy the managed object
        if (InterlockedDecrement(reinterpret_cast<long*>((uint8_t*)ctrl + 8)) == 0)
            ((ctrlfn)vt[1])(ctrl);                              // free the control block
    }
}
// caller-clean thunk around spawnEntity: spawn_copy(retptr, type, posptr)
__declspec(naked) static void spawn_copy_thunk() {
    __asm {
        push ebp
        mov  ebp, esp
        mov  ecx, [ebp+8]                   // retptr (RVO out)
        mov  edx, [ebp+12]                  // type
        push dword ptr [ebp+16]             // &pos (stack arg)
        call dword ptr [g_spawnEntityAddr]
        add  esp, 4                         // caller-clean (spawnEntity is ret 0)
        mov  esp, ebp
        pop  ebp
        ret
    }
}
typedef void (__cdecl *spawn_copy_t)(void* retptr, int type, void* posptr);
// Deferred copy queue: spawning copies REENTRANTLY (inside addEntity, while the level-gen spawn loop
// is mid-iteration) corrupts the spawner and crashes. So we only RECORD copies during add, then spawn
// them from the frame step (flush_enemy_dups), when the level is live and no spawn loop is running.
struct DupReq { void* level; int type; float x, y; };
static const int DUP_CAP = 8192;
static DupReq g_dupQueue[DUP_CAP];
static int g_dupHead = 0, g_dupTail = 0;                     // single-threaded sim: plain indices
// ---- Party: what the shadow makes ----
// Everything added to the level while the shadow acts (its swing step, its let-go, its replayed stomp or holy
// light; g_shadowActing) is the shadow's: arrows, rod and wand shots, spells. They're real (they hit and
// heal) at half strength, like the shadow's own weapon hits. Held by a weak reference, so a freed entity
// whose address comes back is never mistaken for one (an entry whose object is gone is dropped).
struct ShadowEnt { void* ptr; void* ctrl; };
static ShadowEnt g_shadowEnts[64];
static int g_shadowEntNext = 0;
static int g_shadowActing = 0;
static void weak_release(void* ctrl) {
    if (!ctrl) return;
    if (InterlockedDecrement(reinterpret_cast<long*>(static_cast<uint8_t*>(ctrl) + 8)) == 0) {
        typedef void (__thiscall *ctrlfn)(void*);
        reinterpret_cast<ctrlfn>((*reinterpret_cast<void***>(ctrl))[1])(ctrl);   // free the control block
    }
}
static void shadow_ent_add(void* ptr, void* ctrl) {
    if (!ptr || !ctrl) return;
    for (auto& e : g_shadowEnts) if (e.ptr == ptr && e.ctrl == ctrl) return;
    ShadowEnt& e = g_shadowEnts[g_shadowEntNext];
    g_shadowEntNext = (g_shadowEntNext + 1) % 64;
    weak_release(e.ctrl);
    InterlockedIncrement(reinterpret_cast<long*>(static_cast<uint8_t*>(ctrl) + 8));
    e = ShadowEnt{ ptr, ctrl };
}
static bool shadow_ent_is(const void* ptr) {
    if (!ptr) return false;
    for (auto& e : g_shadowEnts) {
        if (e.ptr != ptr) continue;
        if (*reinterpret_cast<long*>(static_cast<uint8_t*>(e.ctrl) + 4) > 0) return true;   // still alive
        weak_release(e.ctrl);
        e = ShadowEnt{};
    }
    return false;
}
// hook body: __fastcall(level=ecx, edx ignored, spRef=stack) -> ret 4, matching addEntity's thiscall.
static void __fastcall do_addEntity(void* level, void* /*edx*/, void* spRef) {
    void* entity = spRef ? *reinterpret_cast<void**>(spRef) : nullptr;
    if (entity && g_shadowActing) {
        void* ctrl = reinterpret_cast<void**>(spRef)[1];
        shadow_ent_add(entity, ctrl);
        const float* ef = reinterpret_cast<const float*>(static_cast<uint8_t*>(entity) + 0xac);
        if (*reinterpret_cast<uint8_t*>((uint8_t*)entity + 0x50) != 109)          // not every dust particle
        logmsg("[brutalist] party: shadow made entity type %d (%p) at %.0f,%.0f size %.0fx%.0f", *reinterpret_cast<uint8_t*>((uint8_t*)entity + 0x50),
               entity, ef[0], ef[1], ef[5], ef[6]);
    }
    if (entity && !g_inEnemyDup) {
        int type = *reinterpret_cast<uint8_t*>((uint8_t*)entity + 0x50);
        int n = dup_count_for_type(type);
        if (n > 1) {
            float x = *reinterpret_cast<float*>((uint8_t*)entity + 0xac);
            float y = *reinterpret_cast<float*>((uint8_t*)entity + 0xb0);
            for (int i = 1; i < n; i++) {
                int nt = (g_dupTail + 1) % DUP_CAP;
                if (nt == g_dupHead) break;                   // queue full: drop the rest
                g_dupQueue[g_dupTail].level = level;
                g_dupQueue[g_dupTail].type  = type;
                g_dupQueue[g_dupTail].x     = x;
                g_dupQueue[g_dupTail].y     = y;
                g_dupTail = nt;
            }
        }
    }
    g_addEntityTramp(level, spRef);                            // original add (unchanged)
}
__declspec(naked) static void det_addEntity() {
    __asm { jmp do_addEntity }   // ecx=level, [esp+4]=spRef -> __fastcall(level, edx, spRef)
}
// Flush from the frame step: spawn queued copies into the CURRENT live level only (drop stale ones).
static void flush_enemy_dups(void* ge) {
    if (g_dupHead == g_dupTail || !ge) return;
    void* curLevel = *reinterpret_cast<void**>((uint8_t*)ge + OFF_GE_LEVEL);
    if (!curLevel) return;
    int budget = 512;                                         // spread big batches over frames
    g_inEnemyDup = true;
    while (g_dupHead != g_dupTail && budget-- > 0) {
        DupReq r = g_dupQueue[g_dupHead];
        g_dupHead = (g_dupHead + 1) % DUP_CAP;
        if (r.level != curLevel) continue;                    // level changed since queued: drop
        DupVec2 pos = { r.x, r.y };
        void* sp2[2] = { nullptr, nullptr };
        g_emptySpawnVec[0] = g_emptySpawnVec[1] = g_emptySpawnVec[2] = 0;  // empty vector arg
        ((spawn_copy_t)spawn_copy_thunk)(&sp2, r.type, &g_emptySpawnVec);  // default-construct the copy
        if (sp2[0]) {
            g_setTetheredPos(sp2[0], &pos);                   // move it to the original's spot
            g_addEntityTramp(curLevel, &sp2);                 // add the copy (level increfs)
        }
        release_shared_block(sp2[1]);                         // drop our temp ref
    }
    g_inEnemyDup = false;
}

// ---- SPREAD: cheaper spawn weight -> more DISTINCT spawns across the level ----
// Companion to duplication: Monster::getSpawnWeight (rva 0x2b3470) returns a type's weight @0x2b34cd
// (movss xmm0,[eax+0x14]). In Level::respawnMonsters that weight is the per-tile budget COST, so
// scaling it down lets more monsters fit at DIFFERENT spawn points (spread out, not stacked). Can't
// zero spawns (selection is relative). Saturates once weights are tiny (bounded by spawn points), so
// it complements duplication. Escalates per loop from g_spawnSpreadStartsAt, gated at runtime.
static const uintptr_t RVA_SPAWNWEIGHT_RET = 0x002b34cd;   // movss xmm0,[eax+0x14] (5 bytes)
static uintptr_t g_spawnWeightResume = 0;                  // = g_base + 0x002b34d2
static volatile float g_spawnWeightFactor = 1.0f;
static void __cdecl update_spawn_weight_factor() {
    float f = 1.0f;
    if (g_spawnSpreadPct > 0) {
        long L = current_loop_number();
        if (L >= (long)g_spawnSpreadStartsAt) {
            int tiers = (int)L - g_spawnSpreadStartsAt + 1;
            long effPct = (long)g_spawnSpreadPct * tiers;
            if (g_spawnSpreadMaxPct > 0 && effPct > (long)g_spawnSpreadMaxPct) effPct = g_spawnSpreadMaxPct;
            float mult = 1.0f + (float)effPct / 100.0f;
            if (mult < 1.0f) mult = 1.0f;
            f = 1.0f / mult;                               // cheaper weight -> more monsters fit the budget
        }
    }
    g_spawnWeightFactor = f;
}
__declspec(naked) static void det_spawnWeight() {
    __asm {
        pushad
        call update_spawn_weight_factor     // refresh factor (clobbers xmm; do it before loading xmm0)
        popad
        movss xmm0, dword ptr [eax+0x14]    // stolen load (eax preserved across pushad/popad)
        mulss xmm0, dword ptr [g_spawnWeightFactor]
        jmp   dword ptr [g_spawnWeightResume]
    }
}

// ---- STARTING LOADOUT: give each player a permanently-cursed bomb --------------------------------
// PlayerCustomizer::setStartingItems(shared_ptr<Player>) (rva 0x30ac30, __cdecl static; per-player
// loadout builder, host-authoritative in MP) runs once when a character is built. We hook it, let the
// normal loadout run, then create a Bomb (ITEMTYPE_BOMB=100), flag it cursed + permanentCursed, and
// give it to the player exactly the way that function gives its own starting items: make_shared<Item>
// -> Level::addEntity -> Inventory::pickUp(shared_ptr<Item>,1,0,-1,-1) on Player+0x4a0 (the Inventory).
// The Level* comes from Player+0x1a0 (setStartingItems sources it the same way @0x70ac8e).
// (g_cursedBomb is declared up with the other config globals.)
static uintptr_t g_makeItemVA  = 0;               // make_shared<Item,Level*&,float&,float&,ItemType,int>
static uintptr_t g_invPickUpVA = 0;               // Inventory::pickUp(shared_ptr<Item>,int,int,int,int)
static uintptr_t g_invEquipVA  = 0;               // Inventory::equipItem(shared_ptr<Item>)
static uintptr_t g_setStartWeaponVA = 0;          // Inventory::setStartingWeapon(shared_ptr<Item>,int,int)
static const int    ITEMTYPE_BOMB        = 100;
static const size_t OFF_ITEM_CURSED      = 0x528; // Item::cursed (byte)
static const size_t OFF_ITEM_PERMCURSE   = 0x529; // Item::permanentCursed (byte)
static const size_t OFF_PLAYER_INVENTORY = 0x4a0; // Player::inventory (embedded Inventory)
static const size_t OFF_ENT_LEVEL2       = 0x1a0; // Entity::level (Player+0x1a0)
static const size_t OFF_ENT_X2 = 0xac, OFF_ENT_Y2 = 0xb0;   // Entity x/y (floats)

// make_shared<Item>(level,x,y,type,count) -> *result = {Item*, ctrl}. ABI from the call site @0x70b2e2:
// ecx=&result, edx=&Level*, pushed right->left &count,&type,&y,&x; cdecl-clean (add esp,0x10).
// NAKED + explicit ebp frame so params are read at fixed offsets regardless of my own pushes.
__declspec(naked) static void make_item_shared(void* result, void** plevel, float* px, float* py, int* ptype, int* pcount) {
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp+0x1c]      // pcount   (&count)
        push dword ptr [ebp+0x18]      // ptype    (&type)
        push dword ptr [ebp+0x14]      // py       (&y)
        push dword ptr [ebp+0x10]      // px       (&x)
        mov  edx, [ebp+0x0c]           // plevel   (&Level*)
        mov  ecx, [ebp+0x08]           // result   (&shared_ptr<Item>)
        mov  eax, g_makeItemVA
        call eax
        add  esp, 0x10                 // cdecl: clean the 4 pushed args
        mov  esp, ebp
        pop  ebp
        ret
    }
}
// Inventory::pickUp(this=&inv, shared_ptr<Item>{ptr,ctrl}, 1,0,-1,-1). __thiscall (self-cleans args).
__declspec(naked) static void inv_pickup_item(void* inv, void* itemPtr, void* itemCtrl) {
    __asm {
        push ebp
        mov  ebp, esp
        push -1
        push -1
        push 0
        push 1
        push dword ptr [ebp+0x10]      // itemCtrl
        push dword ptr [ebp+0x0c]      // itemPtr
        mov  ecx, [ebp+0x08]           // inv (this)
        mov  eax, g_invPickUpVA
        call eax                       // thiscall: callee cleans the 6 dword args
        mov  esp, ebp
        pop  ebp
        ret
    }
}
// Inventory::equipItem(this=&inv, shared_ptr<Item>{ptr,ctrl}). __thiscall (self-cleans the 8-byte arg).
__declspec(naked) static void inv_equip_item(void* inv, void* itemPtr, void* itemCtrl) {
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp+0x10]      // itemCtrl
        push dword ptr [ebp+0x0c]      // itemPtr
        mov  ecx, [ebp+0x08]           // inv (this)
        mov  eax, g_invEquipVA
        call eax                       // thiscall: callee cleans the 8-byte shared_ptr arg
        mov  esp, ebp
        pop  ebp
        ret
    }
}
// Inventory::setStartingWeapon(this=&inv, shared_ptr<Item>{ptr,ctrl}, 0, 1) -- routes the weapon into the
// melee or ranged slot by item type (so a bow lands in the ranged slot). __thiscall (callee cleans).
__declspec(naked) static void inv_set_starting_weapon(void* inv, void* itemPtr, void* itemCtrl) {
    __asm {
        push ebp
        mov  ebp, esp
        push 1
        push 0
        push dword ptr [ebp+0x10]      // itemCtrl
        push dword ptr [ebp+0x0c]      // itemPtr
        mov  ecx, [ebp+0x08]           // inv (this)
        mov  eax, g_setStartWeaponVA
        call eax                       // thiscall: callee cleans the 16-byte args
        mov  esp, ebp
        pop  ebp
        ret
    }
}
static void give_cursed_bomb(void* player) {
    if (!player || !g_makeItemVA || !g_invPickUpVA || !g_addEntityTramp) return;
    void* level = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_LEVEL2);
    if (!level) { logmsg("[bomb] player %p has no level", player); return; }
    float x = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_X2);
    float y = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_Y2);
    int type = ITEMTYPE_BOMB, count = 1;
    int pclass = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(player) + 0x34c);
    void* gelvl = g_ge ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_ge) + OFF_GE_LEVEL) : nullptr;
    logmsg("[bomb] give: player=%p class=%d level(0x1a0)=%p ge.level=%p x=%.1f y=%.1f", player, pclass, level, gelvl, x, y);
    void* item_sp[2] = {nullptr, nullptr};         // shared_ptr<Item> {ptr, ctrl}
    make_item_shared(&item_sp, &level, &x, &y, &type, &count);
    void* item = item_sp[0];
    if (!item) { logmsg("[bomb] make_shared<Item> null (ptr=%p ctrl=%p level=%p)", item_sp[0], item_sp[1], level); return; }
    logmsg("[bomb] created item=%p ctrl=%p type=%d", item_sp[0], item_sp[1], *(int*)((uint8_t*)item + 0x20c));
    *(reinterpret_cast<uint8_t*>(item) + OFF_ITEM_CURSED)    = 1;   // cursed
    *(reinterpret_cast<uint8_t*>(item) + OFF_ITEM_PERMCURSE) = 1;   // permanentCursed (can't be uncursed)
    void* inv = reinterpret_cast<uint8_t*>(player) + OFF_PLAYER_INVENTORY;
    g_addEntityTramp(level, &item_sp);             // add to the world (mirrors setStartingItems); increfs
    if (item_sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)item_sp[1] + 4)); // +1 for by-value pickUp arg
    inv_pickup_item(inv, item_sp[0], item_sp[1]);  // add to the inventory
    if (item_sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)item_sp[1] + 4)); // +1 for by-value equip arg
    inv_equip_item(inv, item_sp[0], item_sp[1]);   // equip it (active slot)
    release_shared_block(item_sp[1]);              // release our original ref (inventory holds it now)
    logmsg("[bomb] gave + equipped permanently-cursed bomb to player=%p (level=%p)", player, level);
}
// Does the player have a skill tree of this AffinityType?
static bool has_affinity(void* player, int type) {
    void** begin = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(player) + 0x870);
    void** end   = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(player) + 0x874);
    if (!begin || !end || end <= begin) return false;
    for (void** p = begin; p < end; ++p) {
        void* a = *p;
        if (a && *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(a) + 4) == type) return true;
    }
    return false;
}
static bool has_archery_affinity(void* player) { return has_affinity(player, 2); }   // ARCHERY
// Does the player already have a ranged weapon? rangedWeapons = vector<shared_ptr<Item>> at Inventory+0x324.
// (The rogue, for instance, already starts with a bow + arrows -- don't hand it a second set.)
static bool has_ranged_weapon(void* player) {
    uint8_t* inv = reinterpret_cast<uint8_t*>(player) + OFF_PLAYER_INVENTORY;
    void** begin = *reinterpret_cast<void***>(inv + 0x324);   // rangedWeapons.begin
    void** end   = *reinterpret_cast<void***>(inv + 0x328);   // rangedWeapons.end
    return begin && end && end > begin;
}
// Archery skill tree -> additionally start with a bow (slotted as the ranged weapon) and 30 arrows.
// This is independent of the primary weapon match, so the player keeps their melee weapon too.
static void give_bow_and_arrows(void* player) {
    if (!player || !g_makeItemVA || !g_invPickUpVA || !g_setStartWeaponVA || !g_addEntityTramp) return;
    if (has_ranged_weapon(player)) {   // already has a bow (e.g. rogue) -- don't double up
        logmsg("[randwpn] archery: player=%p already has a ranged weapon, skipping bow grant", player);
        return;
    }
    void* level = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_LEVEL2);
    if (!level) return;
    float x = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_X2);
    float y = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_Y2);
    void* inv = reinterpret_cast<uint8_t*>(player) + OFF_PLAYER_INVENTORY;
    // Bow (ITEMTYPE_BOW=93) -> setStartingWeapon routes it to the ranged slot.
    {
        int type = 93, count = 1;
        void* sp[2] = {nullptr, nullptr};
        make_item_shared(&sp, &level, &x, &y, &type, &count);
        if (sp[0]) {
            g_addEntityTramp(level, &sp);
            if (sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)sp[1] + 4));
            inv_set_starting_weapon(inv, sp[0], sp[1]);
            release_shared_block(sp[1]);
        }
    }
    // 30 arrows (ITEMTYPE_ARROW=3) -> into the inventory as ammo.
    {
        int type = 3, count = 30;
        void* sp[2] = {nullptr, nullptr};
        make_item_shared(&sp, &level, &x, &y, &type, &count);
        if (sp[0]) {
            g_addEntityTramp(level, &sp);
            if (sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)sp[1] + 4));
            inv_pickup_item(inv, sp[0], sp[1]);
            release_shared_block(sp[1]);
        }
    }
    logmsg("[randwpn] archery: gave bow + 30 arrows to player=%p", player);
}
// Woodsman: setStartingItems has no case for class 7; it falls to the default case, which makes a Wooden
// Club (item 0x37) the starting weapon. Classes 0-6 all have cases, so only the Woodsman reaches it:
// change the item there to the brown axe (`mov dword [ebp-0x6c], 0x37` at 0x30c571).
static void install_woodsman_axe_kit() {
    const uint8_t was[] = {0xC7,0x45,0x94,0x37,0x00,0x00,0x00};
    uint8_t now[sizeof(was)];
    memcpy(now, was, sizeof(was));
    *reinterpret_cast<int32_t*>(now + 3) = ITEMTYPE_AXE_BROWN;
    bool ok = patch_replace("woodsman kit: axe not club", 0x0030c571, was, now, sizeof(was));
    logmsg("[woodsman] starting weapon: brown axe instead of the club %s", ok ? "patched" : "FAILED");
}
// ---- Small bomb (the Woodsman's bomb) -------------------------------------------------------------
// A weaker, yellow bomb: 15 damage (30 with the explosion gloves), doesn't break walls, can't be made into
// bomb arrows, worth little to The Deranged, and stacks apart from regular bombs. It's a regular bomb
// (ITEMTYPE_BOMB) flagged by Item+0x20b, a padding byte between Item::droppable and Item::itemType that
// nothing in the game reads or writes. The game's item-type numbers come in fixed ranges with tables
// behind them, so a flag is far safer than a new type. Everything that has to carry the flag:
//   constructors  Item(level,x,y,type,count) clears it, Item(const Item&) copies it (stack splits)
//   look          setNameAndDescription renames it; its sprites draw from a yellow recolour of their
//                 sheet (see yellow_texture_for), on the ground, in the inventory and when thrown (a
//                 thrown bomb is a Projectile whose storedItem, +0x284, is the item).
//   explosion     Projectile::kill's bomb case: damage 15 and no terrain break (the bomb arrow's settings)
//   save          Item::save is JSON with fixed keys, so the flag rides on "count" as bit 30
//   network       Item::getEntityState packs flags at Item+0x70; bit 21 is unused, the flag goes there
// Everyone in a game needs the mod (as for the whole Woodsman class).
static const size_t OFF_ITEM_SMALL = 0x20b;
static const size_t OFF_PLAYER_CURRWEAPON_SB = 0x76b8;   // Player::currWeapon (raw ptr)
static const int    SMALL_BOMB_DAMAGE = 15;
static const int    SMALL_SAVE_BIT  = 0x40000000;   // on Item::count while saving
static const uint32_t SMALL_NET_BIT = 0x00200000;   // in the item's net state flags (Item+0x70)
static bool is_small_bomb(const void* item) {
    const uint8_t* it = static_cast<const uint8_t*>(item);
    return it && *reinterpret_cast<const int*>(it + 0x20c) == ITEMTYPE_BOMB && it[OFF_ITEM_SMALL] == 1;
}
// A thrown bomb in the other games is a network Projectile whose storedItem doesn't come across (the item
// isn't an entity there), so a small one is marked in its net state too: bit 8 of EntityState::flags3,
// which Projectile::getEntityState leaves unused (bits 0-7 are flags, 18+ damage type and damage).
// setEntityState puts it in Projectile+0x2a2 (padding; the ctor hook clears it).
static const size_t   OFF_PROJ_SMALL_NET = 0x2a2;
static const uint32_t SMALL_PROJ_NET_BIT = 0x100;
static bool is_small_bomb_proj(const uint8_t* p) {
    return p && p[0x1f8] == 14 && (is_small_bomb(*reinterpret_cast<void* const*>(p + 0x284)) || p[OFF_PROJ_SMALL_NET] == 1);
}
typedef void* (__fastcall *sbStrAssign_t)(void* self, void* edx, const char* p, size_t n);
// Name, description, yellow tint and a low value for a small bomb (no-op on anything else).
static void small_bomb_dress(uint8_t* item) {
    if (!is_small_bomb(item)) return;
    sbStrAssign_t assign = reinterpret_cast<sbStrAssign_t>(g_base + 0x0000b2b0);   // std::string::assign
    static const char kName[] = "Small Bomb", kDesc[] = "Packs a little punch.";
    assign(item + 0x480, nullptr, kName, sizeof(kName) - 1);
    assign(item + 0x498, nullptr, kDesc, sizeof(kDesc) - 1);
}
// Yellow recolour. A tint can't turn the blue-grey bomb yellow (it only multiplies), so the small bomb's
// sprites draw from a recoloured copy of their own sheet: the first time one draws, its sf::Texture is
// read back (copyToImage), blue pixels become yellow (red, white, grey and dark pixels stay as they
// are, so the red warning blink still reads), and the copy is uploaded as a new texture, cached per
// sheet. Each item and projectile owns its Sprite objects, so swapping their texture (Sprite+0xfc)
// changes only that small bomb.
typedef void* (__fastcall *texCopyToImage_t)(const void* tex, void* edx, void* outImage);
typedef void  (__fastcall *imageDtor_t)(void* img, void* edx);
typedef void  (__fastcall *sbTexCtor_t)(void* self, void* edx);
typedef bool  (__fastcall *sbTexCreate_t)(void* self, void* edx, unsigned w, unsigned h);
typedef void  (__fastcall *sbTexUpdate_t)(void* self, void* edx, const uint8_t* pixels);
static const int YELLOW_TEX_MAX = 8;
static const void* g_yellowSrc[YELLOW_TEX_MAX] = {};
static __declspec(align(8)) uint8_t g_yellowTex[YELLOW_TEX_MAX][0x40];   // sf::Texture is 0x28 bytes
static int g_yellowCount = 0;
static void recolor_blue_to_yellow(uint8_t* px, size_t n) {
    for (size_t i = 0; i < n; ++i, px += 4) {
        int r = px[0], g = px[1], b = px[2];
        if (!px[3] || b <= r + 6 || b < g) continue;               // only the blue-grey body
        int l = (r + g + 2 * b) / 4;                                // keep the shading
        int nr = l * 3 / 2 + 40, ng = l * 4 / 3 + 26, nb = l / 3;
        px[0] = (uint8_t)(nr > 255 ? 255 : nr);
        px[1] = (uint8_t)(ng > 255 ? 255 : ng);
        px[2] = (uint8_t)nb;
    }
}
static void* yellow_texture_for(const void* src) {
    for (int i = 0; i < g_yellowCount; ++i) {
        if (g_yellowSrc[i] == src) return g_yellowTex[i];
        if (g_yellowTex[i] == src) return nullptr;                // already ours
    }
    if (g_yellowCount >= YELLOW_TEX_MAX) return nullptr;
    __declspec(align(8)) uint8_t img[0x20] = {};                    // sf::Image: Vector2u size, vector<Uint8>
    reinterpret_cast<texCopyToImage_t>(g_base + 0x004169e0)(src, nullptr, img);
    const unsigned w = *reinterpret_cast<unsigned*>(img), h = *reinterpret_cast<unsigned*>(img + 4);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(img + 8);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(img + 12);
    void* out = nullptr;
    if (w && h && pb && (size_t)(pe - pb) >= (size_t)w * h * 4) {
        recolor_blue_to_yellow(pb, (size_t)w * h);
        void* t = g_yellowTex[g_yellowCount];
        reinterpret_cast<sbTexCtor_t>(g_base + 0x00416730)(t, nullptr);
        if (reinterpret_cast<sbTexCreate_t>(g_base + 0x00416cd0)(t, nullptr, w, h)) {
            reinterpret_cast<sbTexUpdate_t>(g_base + 0x004179a0)(t, nullptr, pb);
            g_yellowSrc[g_yellowCount++] = src;
            out = t;
        }
    }
    reinterpret_cast<imageDtor_t>(g_base + 0x00417ec0)(img, nullptr);
    logmsg("[smallbomb] yellow copy of texture %p (%ux%u) %s", src, w, h, out ? "ready" : "FAILED");
    return out;
}
static void sprite_to_yellow(uint8_t* spr) {
    if (!spr) return;
    void*& tex = *reinterpret_cast<void**>(spr + 0xfc);         // Sprite::pTexture (null until lazy-loaded)
    if (!tex) return;
    if (void* y = yellow_texture_for(tex)) tex = y;
}
static void __cdecl small_bomb_item_sprites(uint8_t* item) {
    if (!is_small_bomb(item)) return;
    sprite_to_yellow(*reinterpret_cast<uint8_t**>(item + 0x290));   // Item::sprite
    sprite_to_yellow(*reinterpret_cast<uint8_t**>(item + 0x294));   // Item::droppedSprite
}
typedef void* (__fastcall *itemCtor_t)(void* self, void* edx, void* level, float x, float y, int type, int count);
typedef void* (__fastcall *itemCopyCtor_t)(void* self, void* edx, const void* src);
typedef bool  (__fastcall *setNameDesc_t)(void* self, void* edx, int b);
typedef void  (__fastcall *thisOnly_t)(void* self, void* edx);
typedef void  (__fastcall *thisPtr_t)(void* self, void* edx, void* p);
typedef void* (__fastcall *getState_t)(void* self, void* edx);
static itemCtor_t     g_itemCtorTramp = nullptr;
static itemCopyCtor_t g_itemCopyTramp = nullptr;
static setNameDesc_t  g_setNameDescTramp = nullptr;
static thisOnly_t     g_setCostTramp = nullptr;
static thisPtr_t      g_itemSaveTramp = nullptr, g_itemLoadTramp = nullptr, g_itemSetStateTramp = nullptr;
static thisPtr_t      g_projDrawTramp = nullptr;
static getState_t     g_itemGetStateTramp = nullptr;
// The two constructors only touch the flag and fall straight into the original (no wrapper frame, so the
// original's own exception frame and argument cleanup are untouched).
__declspec(naked) static void det_itemCtor() {      // ecx = this: fresh memory, clear the flag first
    __asm {
        mov  byte ptr [ecx+0x20b], 0
        jmp  dword ptr [g_itemCtorTramp]
    }
}
__declspec(naked) static void det_itemCopy() {      // ecx = this, [esp+4] = const Item& src
    __asm {
        push eax
        mov  eax, dword ptr [esp+8]
        test eax, eax
        jz   nosrc
        mov  al, byte ptr [eax+0x20b]
        jmp  set
    nosrc:
        xor  eax, eax
    set:
        mov  byte ptr [ecx+0x20b], al
        pop  eax
        jmp  dword ptr [g_itemCopyTramp]
    }
}
static bool __fastcall det_setNameDesc(void* self, void* edx, int b) {
    bool r = g_setNameDescTramp(self, edx, b);
    small_bomb_dress(static_cast<uint8_t*>(self));
    return r;
}
static void __fastcall det_setCost(void* self, void* edx) {
    g_setCostTramp(self, edx);
    if (is_small_bomb(self)) *reinterpret_cast<float*>(static_cast<uint8_t*>(self) + 0x24c) *= 0.2f;   // Item::cost
}
static void __fastcall det_itemSave(void* self, void* edx, void* loc) {
    int& count = *reinterpret_cast<int*>(static_cast<uint8_t*>(self) + 0x268);
    const bool isSmall = is_small_bomb(self);
    if (isSmall) count |= SMALL_SAVE_BIT;
    g_itemSaveTramp(self, edx, loc);
    if (isSmall) count &= ~SMALL_SAVE_BIT;
}
static void __fastcall det_itemLoad(void* self, void* edx, void* loc) {
    g_itemLoadTramp(self, edx, loc);
    uint8_t* it = static_cast<uint8_t*>(self);
    int& count = *reinterpret_cast<int*>(it + 0x268);
    if (count & SMALL_SAVE_BIT) {
        count &= ~SMALL_SAVE_BIT;
        it[OFF_ITEM_SMALL] = 1;
        det_setCost(self, nullptr);
        small_bomb_dress(it);
    }
}
static void* __fastcall det_itemGetState(void* self, void* edx) {
    void* st = g_itemGetStateTramp(self, edx);
    if (is_small_bomb(self)) *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self) + 0x70) |= SMALL_NET_BIT;
    return st;
}
static void __fastcall det_itemSetState(void* self, void* edx, void* st) {
    uint8_t* it = static_cast<uint8_t*>(self);
    const uint8_t was = it[OFF_ITEM_SMALL];
    if (st) it[OFF_ITEM_SMALL] = (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(st) + 0x1c) & SMALL_NET_BIT) ? 1 : 0;
    g_itemSetStateTramp(self, edx, st);
    if (it[OFF_ITEM_SMALL] != was && is_small_bomb(it)) { det_setCost(it, nullptr); small_bomb_dress(it); }
}
static bool clone_small_bomb_weapon(const uint8_t* w);   // (Party, below)
static bool shadow_small_proj(const uint8_t* proj);
struct ShadowBomb;
static ShadowBomb* shadow_bomb_entry(const uint8_t* proj);
static void shadow_bomb_explosion(const uint8_t* proj, uint8_t* spell);   // (Party, below)
// Projectile::draw: a thrown small bomb (PROJECTILE_BOMB = 14) draws yellow too (its item, or in the other
// games the net flag, see is_small_bomb_proj).
static getState_t g_projGetStateTramp = nullptr;
static thisPtr_t  g_projSetStateTramp = nullptr;
static void* __fastcall det_projGetState(void* self, void* edx) {
    void* st = g_projGetStateTramp(self, edx);
    uint8_t* p = static_cast<uint8_t*>(self);
    if (is_small_bomb_proj(p) || shadow_small_proj(p))
        *reinterpret_cast<uint32_t*>(p + 0x74) |= SMALL_PROJ_NET_BIT;     // state (+0x54) flags3 (+0x20)
    return st;
}
static void __fastcall det_projSetState(void* self, void* edx, void* st) {
    g_projSetStateTramp(self, edx, st);
    uint8_t* p = static_cast<uint8_t*>(self);
    if (!st || p[0x1f8] != 14) return;
    const uint8_t isSmall = (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(st) + 0x20) & SMALL_PROJ_NET_BIT) ? 1 : 0;
    static const void* lastLogged = nullptr;
    if (isSmall && lastLogged != p) { lastLogged = p; logmsg("[smallbomb] net bomb %p is small (item %p)", p, *reinterpret_cast<void**>(p + 0x284)); }
    p[OFF_PROJ_SMALL_NET] = isSmall;
}
static void __fastcall det_projDraw(void* self, void* edx, void* rt) {
    uint8_t* p = static_cast<uint8_t*>(self);
    if (p[0x1f8] == 14 && (is_small_bomb_proj(p) || shadow_small_proj(p)))
        sprite_to_yellow(*reinterpret_cast<uint8_t**>(p + 0x2c8));
    // The shadow's projectiles are real ones (Projectile::shadow would make them die on first touch and skip
    // their effects), so the shadow look (drawn grey, see-through) is put on only while drawing.
    const bool shade = !p[0x22c] && (shadow_ent_is(p) || shadow_bomb_entry(p));
    if (shade) p[0x22c] = 1;
    g_projDrawTramp(self, edx, rt);
    if (shade) p[0x22c] = 0;
}
// Item::draw (ground) and Item::drawAtPosition (inventory): swap a small bomb's sprites to the yellow
// sheet first. Naked so the originals' own stack alignment code runs untouched.
static void* g_itemDrawTramp = nullptr;
static void* g_itemDrawAtTramp = nullptr;
__declspec(naked) static void det_itemDraw() {
    __asm {
        pushad
        push ecx
        call small_bomb_item_sprites
        add  esp, 4
        popad
        jmp  dword ptr [g_itemDrawTramp]
    }
}
__declspec(naked) static void det_itemDrawAt() {
    __asm {
        pushad
        push ecx
        call small_bomb_item_sprites
        add  esp, 4
        popad
        jmp  dword ptr [g_itemDrawAtTramp]
    }
}
// Weapon::draw: the bomb in a player's hand while winding up a throw is drawn by the throw Weapon, from
// a projectile that has no item yet (so det_projDraw can't tell it's small). When that weapon is the player's current one and the item it
// came from (Player::currWeaponItem, +0x76c0) is a small bomb, draw it yellow; otherwise put back the
// stock texture, in case the same weapon is later used for a regular bomb.
static bool is_player_entity(const void* ent);   // (Bomber tree, below)
static void sprite_from_yellow(uint8_t* spr) {
    if (!spr) return;
    void*& tex = *reinterpret_cast<void**>(spr + 0xfc);
    for (int i = 0; tex && i < g_yellowCount; ++i)
        if (g_yellowTex[i] == tex) { tex = const_cast<void*>(g_yellowSrc[i]); return; }
}
static void __cdecl small_bomb_weapon_sprite(uint8_t* w) {
    uint8_t* p = *reinterpret_cast<uint8_t**>(w + 0x248);              // Weapon::parent
    // A player first (a pointer compare): a weapon can outlive its wielder (a lobby character re-made by
    // the class picker), and reading through a freed parent crashed the draw.
    if (!is_player_entity(p)) return;
    const bool shadow = w[0x29c] != 0;     // a Party shadow's copy: small if the bomb you wound up was
    if (!shadow && *reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON_SB) != w) return;
    // The bomb in hand is the weapon's waiting projectile (Weapon::currProjectile, +0x258; a bomb, type 14,
    // with no item attached until it's thrown), drawn by the weapon. Its sprite is Projectile+0x2c8.
    uint8_t* proj = *reinterpret_cast<uint8_t**>(w + 0x258);
    if (!proj || proj[0x1f8] != 14) return;
    uint8_t* spr = *reinterpret_cast<uint8_t**>(proj + 0x2c8);
    if (shadow ? clone_small_bomb_weapon(w) : is_small_bomb(*reinterpret_cast<void**>(p + 0x76c0))) sprite_to_yellow(spr);
    else sprite_from_yellow(spr);
}
static void* g_weaponDrawTramp = nullptr;
__declspec(naked) static void det_weaponDraw() {
    __asm {
        pushad
        push ecx
        call small_bomb_weapon_sprite
        add  esp, 4
        popad
        jmp  dword ptr [g_weaponDrawTramp]
    }
}
// Projectile::kill, bomb case (0x32eb28): ebx = projectile, ecx = the explosion SpellEntity.
static void bomber_on_explosion(const uint8_t* proj, uint8_t* spell);   // Bomber tree (below)
static int bomber_level_of(const void* ent);                            // Bomber tree (below)
static void small_bomb_set_damage(const uint8_t* proj, uint8_t* spell);
static void __cdecl small_bomb_explosion(const uint8_t* proj, uint8_t* spell) {
    if (spell) bomber_on_explosion(proj, spell);
    small_bomb_set_damage(proj, spell);
    shadow_bomb_explosion(proj, spell);                  // last: halves what the above set, drops its entry
}
static void small_bomb_set_damage(const uint8_t* proj, uint8_t* spell) {
    if (!spell || !(is_small_bomb_proj(proj) || shadow_small_proj(proj))) return;
    // SpellEntity damage is min + rand % (max - min + 1). A regular bomb sets only min (99) and keeps the
    // ctor's max (2), which rolls 99-194. Small: exactly 15, or 30 with the explosion gloves
    // (Projectile::extraExplosive, +0x2a1), which double min right after this.
    // Bomber level 4 (Contact Fuse) doubles it too: 30, or 60 with the gloves.
    int dmg = SMALL_BOMB_DAMAGE;
    if (bomber_level_of(*reinterpret_cast<void* const*>(proj + 0x2a4)) >= 4) dmg *= 2;
    *reinterpret_cast<int*>(spell + 0x1fc) = dmg;                                     // minDamage
    *reinterpret_cast<int*>(spell + 0x200) = dmg * (proj[0x2a1] ? 2 : 1);             // maxDamage
    spell[0x469] = 0;                                          // destructiveExplosion off (as bomb arrows)
    logmsg("[smallbomb] explosion: damage %d, no terrain", dmg);
}
static uintptr_t g_killBombResume = 0;
__declspec(naked) static void det_killBomb() {
    __asm {
        pushad
        push ecx
        push ebx
        call small_bomb_explosion
        add  esp, 8
        popad
        movss xmm1, dword ptr [ebp-0x18]          // replayed
        jmp  dword ptr [g_killBombResume]
    }
}
// Inventory::doStackItem: `mov eax,[ecx+0x20c]; cmp eax,[edi+0x20c]` (then je = same stack). Same type
// AND same small flag, so small and regular bombs stack apart. Leaves ZF as the answer.
__declspec(naked) static void stub_stackSameType() {
    __asm {
        mov  eax, dword ptr [ecx+0x20c]
        cmp  eax, dword ptr [edi+0x20c]
        jne  done
        push edx
        mov  dl, byte ptr [ecx+0x20b]
        cmp  dl, byte ptr [edi+0x20b]
        pop  edx
    done:
        ret
    }
}
// Item::use: `cmp eax,0x64; jne next` is "using a bomb on something" (bomb + arrows = bomb arrows).
// A small bomb takes the not-a-bomb branch. eax = this item's type, esi = this.
static uintptr_t g_useBombCase = 0, g_useNotBomb = 0;
__declspec(naked) static void det_useBomb() {
    __asm {
        cmp  eax, 0x64
        jne  notbomb
        cmp  byte ptr [esi+0x20b], 1
        je   notbomb
        jmp  dword ptr [g_useBombCase]
    notbomb:
        jmp  dword ptr [g_useNotBomb]
    }
}
static void install_small_bomb() {
    const uint8_t pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};       // push ebp; mov ebp,esp; push -1
    const uint8_t costPro[5] = {0x55, 0x8B, 0xEC, 0x51, 0x53};   // push ebp; mov ebp,esp; push ecx; push ebx
    g_itemCtorTramp     = reinterpret_cast<itemCtor_t>(install_detour(0x00216000, (void*)&det_itemCtor, 5, pro, 5));
    g_itemCopyTramp     = reinterpret_cast<itemCopyCtor_t>(install_detour(0x002164f0, (void*)&det_itemCopy, 5, pro, 5));
    g_setNameDescTramp  = reinterpret_cast<setNameDesc_t>(install_detour(0x00217290, (void*)&det_setNameDesc, 5, pro, 5));
    g_setCostTramp      = reinterpret_cast<thisOnly_t>(install_detour(0x00229170, (void*)&det_setCost, 5, costPro, 5));
    g_itemSaveTramp     = reinterpret_cast<thisPtr_t>(install_detour(0x0022cc90, (void*)&det_itemSave, 5, pro, 5));
    g_itemLoadTramp     = reinterpret_cast<thisPtr_t>(install_detour(0x0022d2d0, (void*)&det_itemLoad, 5, pro, 5));
    g_itemGetStateTramp = reinterpret_cast<getState_t>(install_detour(0x00229ca0, (void*)&det_itemGetState, 5, pro, 5));
    g_itemSetStateTramp = reinterpret_cast<thisPtr_t>(install_detour(0x00229f20, (void*)&det_itemSetState, 5, pro, 5));
    g_projDrawTramp     = reinterpret_cast<thisPtr_t>(install_detour(0x00322e60, (void*)&det_projDraw, 5, pro, 5));
    {
        const uint8_t getPro[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};   // push ebp; mov ebp,esp; and esp,-8
        g_projGetStateTramp = reinterpret_cast<getState_t>(install_detour(0x0032a900, (void*)&det_projGetState, 6, getPro, 6));
        g_projSetStateTramp = reinterpret_cast<thisPtr_t>(install_detour(0x0032add0, (void*)&det_projSetState, 5, pro, 5));
        logmsg("[smallbomb] thrown bomb net state %d/%d", !!g_projGetStateTramp, !!g_projSetStateTramp);
    }
    const uint8_t drawPro[6]   = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};   // push ebp; mov ebp,esp; and esp,-16
    const uint8_t drawAtPro[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};   // push ebx; mov ebx,esp; sub esp,8
    g_itemDrawTramp   = install_detour(0x0022c310, (void*)&det_itemDraw, 6, drawPro, 6);
    {
        const uint8_t wpro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
        g_weaponDrawTramp = install_detour(0x003f1110, (void*)&det_weaponDraw, 5, wpro, 5);
        logmsg("[smallbomb] weapon draw hook %s", g_weaponDrawTramp ? "installed" : "FAILED");
    }
    g_itemDrawAtTramp = install_detour(0x0022ee10, (void*)&det_itemDrawAt, 6, drawAtPro, 6);
    logmsg("[smallbomb] item draw %d drawAtPosition %d", !!g_itemDrawTramp, !!g_itemDrawAtTramp);
    logmsg("[smallbomb] ctor %d copy %d name %d cost %d save %d load %d getState %d setState %d draw %d",
           !!g_itemCtorTramp, !!g_itemCopyTramp, !!g_setNameDescTramp, !!g_setCostTramp, !!g_itemSaveTramp,
           !!g_itemLoadTramp, !!g_itemGetStateTramp, !!g_itemSetStateTramp, !!g_projDrawTramp);

    auto jmpPatch = [](const char* name, uintptr_t rva, const uint8_t* expect, int len, uint8_t op, void* dest) {
        uint8_t buf[16]; memset(buf, 0x90, sizeof(buf));
        buf[0] = op;
        *reinterpret_cast<int32_t*>(buf + 1) =
            static_cast<int32_t>(reinterpret_cast<uint8_t*>(dest) - reinterpret_cast<uint8_t*>(g_base + rva + 5));
        bool ok = patch_replace(name, rva, expect, buf, len);
        logmsg("[smallbomb] %s %s", name, ok ? "patched" : "FAILED");
    };
    g_killBombResume = g_base + 0x0032eb2d;
    const uint8_t k[] = {0xF3,0x0F,0x10,0x4D,0xE8};                                   // movss xmm1,[ebp-0x18]
    jmpPatch("kill: bomb explosion", 0x0032eb28, k, sizeof(k), 0xE9, (void*)&det_killBomb);
    const uint8_t s[] = {0x8B,0x81,0x0C,0x02,0x00,0x00,0x3B,0x87,0x0C,0x02,0x00,0x00};  // mov eax,[ecx+20c]; cmp eax,[edi+20c]
    jmpPatch("doStackItem: same type", 0x0020973b, s, sizeof(s), 0xE8, (void*)&stub_stackSameType);
    g_useBombCase = g_base + 0x00225778;
    g_useNotBomb  = g_base + 0x0022591d;
    const uint8_t u[] = {0x83,0xF8,0x64,0x0F,0x85,0xA5,0x01,0x00,0x00};                // cmp eax,0x64; jne +0x1a5
    jmpPatch("use: no small bomb arrows", 0x0022576f, u, sizeof(u), 0xE9, (void*)&det_useBomb);
}
// The Woodsman starts with 5 small bombs, and so does a Random character that rolled the Bomber tree
// (with random_match_weapon on). Equipped as the ranged weapon unless there already is one (a bow).
static void give_small_bombs(void* player) {
    if (!player || !g_makeItemVA || !g_invPickUpVA || !g_addEntityTramp) return;
    const bool equip = !has_ranged_weapon(player);
    void* level = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_LEVEL2);
    if (!level) return;
    float x = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_X2);
    float y = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_Y2);
    int type = ITEMTYPE_BOMB, count = 5;
    void* sp[2] = {nullptr, nullptr};
    make_item_shared(&sp, &level, &x, &y, &type, &count);
    if (!sp[0]) return;
    uint8_t* item = static_cast<uint8_t*>(sp[0]);
    item[OFF_ITEM_SMALL] = 1;
    det_setCost(item, nullptr);
    small_bomb_dress(item);
    g_addEntityTramp(level, &sp);
    if (sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)sp[1] + 4));
    void* inv = reinterpret_cast<uint8_t*>(player) + OFF_PLAYER_INVENTORY;
    inv_pickup_item(inv, sp[0], sp[1]);
    if (g_invEquipVA && equip) {                   // no ranged weapon otherwise: equip them so they throw
        if (sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)sp[1] + 4));
        inv_equip_item(inv, sp[0], sp[1]);
    }
    release_shared_block(sp[1]);
    logmsg("[woodsman] gave 5 small bombs to player=%p%s", player, equip ? " (equipped)" : "");
}
// Debug start kit (start_regens / start_all_spellbooks / start_weapons_*). Each item is made at the
// player's feet and added to the level, then picked up into the bag; one that doesn't fit stays on
// the ground there. skill >= 0 makes it a spellbook of that spell (Item::setSkill).
static const char* item_name(const void* item) {     // Item::name, std::string at +0x480 (MSVC SSO)
    const uint8_t* str = static_cast<const uint8_t*>(item) + 0x480;
    return *reinterpret_cast<const uint32_t*>(str + 0x14) >= 16 ? *reinterpret_cast<const char* const*>(str)
                                                              : reinterpret_cast<const char*>(str);
}
static void give_bag_item(void* player, int type, int count, int skill = -1) {
    void* level = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_LEVEL2);
    if (!level) return;
    float x = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_X2);
    float y = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(player) + OFF_ENT_Y2);
    void* sp[2] = {nullptr, nullptr};
    make_item_shared(&sp, &level, &x, &y, &type, &count);
    if (!sp[0]) return;
    if (skill >= 0 && g_realSetSkill) reinterpret_cast<void (__fastcall*)(void*, void*, int)>(g_realSetSkill)(sp[0], nullptr, skill);
    logmsg("[debug] start kit: %d x%d \"%s\"", type, count, item_name(sp[0]));
    g_addEntityTramp(level, &sp);
    if (sp[1]) InterlockedIncrement(reinterpret_cast<long*>((uint8_t*)sp[1] + 4));
    inv_pickup_item(reinterpret_cast<uint8_t*>(player) + OFF_PLAYER_INVENTORY, sp[0], sp[1]);
    release_shared_block(sp[1]);
}
static void give_debug_start_kit(void* player) {
    if (!player || !g_makeItemVA || !g_invPickUpVA || !g_addEntityTramp) return;
    if (g_startRegens) {
        give_bag_item(player, 6, 999);                       // ITEMTYPE_POTION_HEAL, "Potion of Regeneration"
        logmsg("[debug] start kit: 999 regens for player=%p", player);
    }
    if (g_startAllBooks) {
        int n = 0;
        for (int s = 0; s < 16; s++, n++) give_bag_item(player, ITEM_TYPE_SPELLBOOK, 1, s);
        if (g_wideningApplied)                               // Portal / Telekinesis / Psychic Push, if on
            for (int i = 0; i < N_SPELLS; i++)
                if (g_enabled[i] && g_spells[i].id >= 16) { give_bag_item(player, ITEM_TYPE_SPELLBOOK, 1, g_spells[i].id); n++; }
        logmsg("[debug] start kit: %d spellbooks for player=%p", n, player);
    }
    for (int c = 0; c < N_WEAPON_KIT; c++) {
        if (!(g_startWeaponCats & (1u << c))) continue;
        int n = 0;
        for (int k = 0; k < 10 && WEAPON_KIT[c].types[k]; k++, n++) give_bag_item(player, WEAPON_KIT[c].types[k], 1);
        if (!strcmp(WEAPON_KIT[c].key, "start_weapons_bows")) give_bag_item(player, 3, 99);   // ITEMTYPE_ARROW
        logmsg("[debug] start kit: %d %s for player=%p", n, WEAPON_KIT[c].name, player);
    }
}
typedef void (__cdecl *setStartItems_t)(void* spPtr, void* spCtrl);
static setStartItems_t g_setStartItemsTramp = nullptr;

// PlayerCustomizer::populateAffinities(shared_ptr<Player>, vector<AffinityType>) -- __cdecl, both args
// BY VALUE: 5 dwords {player, ctrl, begin, end, cap}, and the callee releases the shared_ptr and frees
// the vector (the caller does `add esp, 0x14`). It adds one affinity per list entry, then appends the
// fixed baseline affinities. This runs ONLY for characters with a non-empty custom affinity list (i.e.
// Random characters); fixed classes take setStartingAffinities instead and never reach here.
//   We drop every AFFINITY_NO_TREES marker from the list (compacting in place; the vector is ours to
// edit and is freed by begin/cap, which we don't touch). A 0-tree roll arrives as exactly one marker,
// so zero rolled trees land and the character keeps only the baselines. This keys off the list, not
// the local ini, so host and client build the same character. Lists without a marker pass untouched.
//   All 5 dwords are forwarded. An earlier 4-arg version left `cap` as whatever was on our stack, and
// the stock code frees the vector using that value.
typedef void (__cdecl *populateAff_t)(void* player, void* ctrl, int* begin, int* end, int* cap);
static populateAff_t g_populateAffTramp = nullptr;
static void __cdecl det_populateAffinities(void* player, void* ctrl, int* begin, int* end, int* cap) {
    if (begin && end > begin) {
        int* out = begin;
        for (int* it = begin; it < end; ++it)
            if (*it != AFFINITY_NO_TREES) *out++ = *it;
        end = out;
    }
    g_populateAffTramp(player, ctrl, begin, end, cap);
}
// ---- Axe skill tree (a new AffinityType 26) -----------------------------------------------------
// Axes are the one weapon family with no tree. This adds one built from the Club tree, but only for axes:
//   1 Fatiguing Blows  harder hits that stun more   (Club's level 1)
//   2 Axe Throw        up + attack throws the axe; it comes back like a thrown dagger (Dagger's level 2).
//   3 Charged Attack   hold attack to charge, swings and throws (Club's level 2)
//                      It charges like a swing, and aims up/down by what you hold as it leaves your hand
//   4 Faster Charge    charges faster               (Club's level 4)
// The game's own types are 0..25 (CLUB = 25), so 26 is free. Affinity::getNewAffinity only knows 0..25;
// we answer 26 ourselves with an Affinity built exactly like the Club one (same ctor, size and icon),
// pointing at our own copy of AffinityClub's vtable with two slots replaced: initSkillStrings (runs
// Club's, then renames the strings) and levelUp (only bumps the level). The perks don't use flags in
// AffinityAttributes; each check below reads the Axe tree's level straight off the player instead, so
// whatever sets the level (level-up, save load, network) is automatically right.
//   "Is this an axe" = the weapon's WeaponProperties name is "axe" or "axe2". Axes have no typeFlags bit
// (clubs are 0x80, daggers 0x02), and isHeavy also covers the spear, black sword and Thunder God hammer.
//   Charging needs a charge keyframe. Clubs swing with "player-swing", which has one; axes use
// "player-heavy-swing", which has none, so we set Weapon::chargeAttackFrame to its wind-up keyframe (5)
// whenever an axe charge starts. Only the Weapon ctors set that field, so this never leaks to others.
//   A machine without the mod can't build type 26 (its factory has no case for it), so a co-op player
// with the Axe tree needs everyone on the mod, and a save holding the tree needs the mod to load.
static const int AFFINITY_CLUB = 25;
static const int AFFINITY_AXE  = 26;
static const int AXE_MAX_LEVEL = 4;
static const int AXE_ICON_FRAME = 0x18;                       // Club's icon frame (no axe icon exists yet)
static const int AXE_CHARGE_KEYFRAME = 5;                     // player-heavy-swing's wind-up keyframe (drawn back)
static const int AXE_THROW_CHARGE_KEYFRAME = 3;               // a throw charges here instead: axe raised over the head
static const uintptr_t RVA_GET_NEW_AFFINITY  = 0x000ecb70;   // Affinity::getNewAffinity(type, int) (ecx, edx)
static const uintptr_t RVA_AFFINITY_CTOR     = 0x000ec060;   // Affinity::Affinity(AffinityType, unsigned maxLevel)
static const uintptr_t RVA_SPRITE_SET_FRAME  = 0x003b9c20;   // Sprite::setFrame(int)
static const uintptr_t RVA_STRING_ASSIGN     = 0x0000b2b0;   // std::string::assign(const char*, size_t)
static const uintptr_t RVA_CLUB_VTABLE       = 0x007165b4;   // AffinityClub::`vftable'
static const uintptr_t RVA_SET_STARTING_AFFS = 0x00309f40;   // PlayerCustomizer::setStartingAffinities (cdecl)
static const size_t OFF_PLAYER_AFFS          = 0x870;        // Player::affinities vector<Affinity*>
static const size_t OFF_PLAYER_CURRWEAPON    = 0x76b8;       // Player::currWeapon (shared_ptr<Weapon>, raw ptr)
static const size_t OFF_WEAPON_ATTACKSCRIPT  = 0x318;        // Weapon::attackScript
static const size_t OFF_WEAPON_PROPS         = 0x31c;        // Weapon::weaponProperties

typedef void  (__fastcall *affCtor_t)(void* self, void* edx, int type, int maxLevel);
typedef void  (__fastcall *spriteSetFrame_t)(void* self, void* edx, int frame);
typedef void* (__fastcall *strAssign_t)(void* self, void* edx, const char* p, size_t n);
typedef void  (__fastcall *affInit_t)(void* self, void* edx);
typedef void* (__fastcall *affDtor_t)(void* self, void* edx, unsigned flags);
typedef void* (__fastcall *getNewAff_t)(int type, int arg);
typedef void  (__cdecl *setStartAffs_t)(void* player, void* ctrl);

// MSVC std::string: inline buffer when capacity (+0x14) < 16, else a heap pointer; size at +0x10.
static const char* game_str(const uint8_t* s, size_t* len) {
    if (len) *len = *reinterpret_cast<const uint32_t*>(s + 0x10);
    uint32_t cap = *reinterpret_cast<const uint32_t*>(s + 0x14);
    return cap >= 16 ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
}
static bool game_str_is(const uint8_t* s, const char* want) {
    size_t n; const char* p = game_str(s, &n);
    return n == strlen(want) && memcmp(p, want, n) == 0;
}
static bool is_axe_props(const uint8_t* props) {
    return props && (game_str_is(props, "axe") || game_str_is(props, "axe2"));
}
// The Spearheaded Baton counts as an axe too. It shares the "spear" WeaponProperties with the real Spear
// (both items are built from it); the baton swings ("player-swing"), the Spear stabs ("player-stab"),
// so the weapon's attack script (Weapon+0x318, name string first) tells them apart.
static bool is_baton(const uint8_t* weapon, const uint8_t* props) {
    if (!weapon || !props || !game_str_is(props, "spear")) return false;
    const uint8_t* script = *reinterpret_cast<const uint8_t* const*>(weapon + 0x318);
    return script && game_str_is(script, "player-swing");
}
static bool is_axe_weapon(const uint8_t* weapon, const uint8_t* props) {
    return is_axe_props(props) || is_baton(weapon, props);
}
static bool is_axe_weapon(const uint8_t* weapon) {
    return weapon && is_axe_weapon(weapon, *reinterpret_cast<const uint8_t* const*>(weapon + 0x31c));
}
// Level of the player's Axe tree (0 if they don't have it).
static int axe_tree_level(const uint8_t* player) {
    if (!player) return 0;
    uint8_t* const* b = *reinterpret_cast<uint8_t* const* const*>(player + OFF_PLAYER_AFFS);
    uint8_t* const* e = *reinterpret_cast<uint8_t* const* const*>(player + OFF_PLAYER_AFFS + 4);
    if (!b || e <= b) return 0;
    for (uint8_t* const* it = b; it < e; ++it)
        if (*it && *reinterpret_cast<int*>(*it + 4) == AFFINITY_AXE) return *reinterpret_cast<int*>(*it + 0x24);
    return 0;
}

// Our vtable: [0] = AffinityClub's RTTI locator, [1..4] = the 4 virtuals (dtor, initSkillStrings, levelUp, sync).
static void* g_axeVtbl[5];
static affInit_t g_clubInitStrings = nullptr;
static const char* const kAxeText[][2] = {
    { "Club", "Axe" },
    { "Club attacks deal more damage and stun enemies more.", "Axe attacks deal more damage and stun enemies more." },
    { "Shockwave", "Axe Throw" },
    { "Fully charged attacks unleash a powerful shockwave.",
      "Hold up and attack to throw your axe. Thrown axes return to their owner." },
    { "Hold the attack button to charge up your swing.",
      "Hold the attack button to charge up your swing or throw. A full charge doubles the damage." },
};
static void axe_rename(uint8_t* s) {
    for (const auto& t : kAxeText)
        if (game_str_is(s, t[0])) {
            reinterpret_cast<strAssign_t>(g_base + RVA_STRING_ASSIGN)(s, nullptr, t[1], strlen(t[1]));
            return;
        }
}
static void __fastcall axe_initSkillStrings(uint8_t* self, void*) {
    g_clubInitStrings(self, nullptr);                 // fills name + 4 skill names/descriptions (game allocator)
    axe_rename(self + 0xc);                           // Affinity::name
    static const size_t kVecs[2] = { 0x34, 0x40 };   // skillNames, skillDescriptions (vector<string>)
    for (size_t vo : kVecs) {
        uint8_t* b = *reinterpret_cast<uint8_t**>(self + vo);
        uint8_t* e = *reinterpret_cast<uint8_t**>(self + vo + 4);
        for (uint8_t* s = b; b && s + 0x18 <= e; s += 0x18) axe_rename(s);
        // Axe order: level 2 = Axe Throw (Club's Shockwave slot, renamed), level 3 = Charged Attack. MSVC's
        // release std::string holds no pointer to itself, so swapping the raw 0x18 bytes is a plain swap.
        if (b && b + 3 * 0x18 <= e) {
            uint8_t tmp[0x18];
            memcpy(tmp, b + 0x18, 0x18); memcpy(b + 0x18, b + 0x30, 0x18); memcpy(b + 0x30, tmp, 0x18);
        }
    }
    static bool logged = false;   // once per session: what the menu will show
    if (!logged) {
        logged = true;
        logmsg("[axe] tree name: %s", game_str(self + 0xc, nullptr));
        for (size_t vo : kVecs) {
            uint8_t* b = *reinterpret_cast<uint8_t**>(self + vo);
            uint8_t* e = *reinterpret_cast<uint8_t**>(self + vo + 4);
            for (uint8_t* s = b; b && s + 0x18 <= e; s += 0x18) logmsg("[axe]   %s", game_str(s, nullptr));
        }
    }
}
static void __fastcall axe_levelUp(uint8_t* self, void*, void* /*attributes*/) {
    ++*reinterpret_cast<int*>(self + 0x24);           // perks read this level directly (see above)
}
// ---- The mod's own UI art (Woodsman picker icon + title, Axe tree icon) ----
// Drawn by hand (_local/art/woodsman/), baked into woodsman_art.h as raw RGBA by
// _local/scripts/make_woodsman_art_h.py. Each becomes its own sf::Texture (the game's statically linked
// SFML: Texture(), create(w,h), update(pixels)), built the first time it's needed, on the game thread.
// A sprite shows one by pointing its texture at it and taking frame 0, so the game's sheets stay untouched.
#include "woodsman_art.h"
typedef void (__fastcall *texCtor_t)(void* self, void* edx);
typedef bool (__fastcall *texCreate_t)(void* self, void* edx, unsigned w, unsigned h);
typedef void (__fastcall *texUpdate_t)(void* self, void* edx, const uint8_t* pixels);
static const uintptr_t RVA_TEXTURE_CTOR   = 0x00416730;   // sf::Texture::Texture()
static const uintptr_t RVA_TEXTURE_CREATE = 0x00416cd0;   // sf::Texture::create(unsigned, unsigned)
static const uintptr_t RVA_TEXTURE_UPDATE = 0x004179a0;   // sf::Texture::update(const Uint8*)
static const size_t OFF_SPRITE_TEXTURE = 0xfc;            // Sprite (an sf::Sprite): m_texture
static const size_t OFF_SPRITE_FRAME   = 0x118;           // Sprite::frame (setFrame skips an unchanged frame)
enum { MODTEX_CLASS_ICON, MODTEX_CLASS_TITLE, MODTEX_AXE_ICON, MODTEX_COUNT };
static __declspec(align(8)) uint8_t g_modTexMem[MODTEX_COUNT][0x40];   // sf::Texture is 0x28 bytes
static void* g_modTex[MODTEX_COUNT];
static bool  g_modTexTried[MODTEX_COUNT];
static void* mod_texture(int i) {
    if (g_modTexTried[i]) return g_modTex[i];
    g_modTexTried[i] = true;
    static const uint8_t* const px[MODTEX_COUNT] = {WOODSMAN_CLASS_ICON_RGBA, WOODSMAN_CLASS_TITLE_RGBA, AXE_TREE_ICON_RGBA};
    static const unsigned w[MODTEX_COUNT] = {WOODSMAN_CLASS_ICON_W, WOODSMAN_CLASS_TITLE_W, AXE_TREE_ICON_W};
    static const unsigned h[MODTEX_COUNT] = {WOODSMAN_CLASS_ICON_H, WOODSMAN_CLASS_TITLE_H, AXE_TREE_ICON_H};
    void* t = g_modTexMem[i];
    reinterpret_cast<texCtor_t>(g_base + RVA_TEXTURE_CTOR)(t, nullptr);
    if (!reinterpret_cast<texCreate_t>(g_base + RVA_TEXTURE_CREATE)(t, nullptr, w[i], h[i])) {
        logmsg("[art] texture %d (%ux%u) could not be created; using the stand-in", i, w[i], h[i]);
        return nullptr;
    }
    reinterpret_cast<texUpdate_t>(g_base + RVA_TEXTURE_UPDATE)(t, nullptr, px[i]);
    g_modTex[i] = t;
    logmsg("[art] texture %d (%ux%u) ready", i, w[i], h[i]);
    return t;
}
// Point a game Sprite at `tex` and show `frame` of it (re-applied even if the frame number is unchanged).
static void sprite_show(uint8_t* spr, void* tex, int frame) {
    *reinterpret_cast<void**>(spr + OFF_SPRITE_TEXTURE) = tex;
    *reinterpret_cast<int*>(spr + OFF_SPRITE_FRAME) = -1;
    reinterpret_cast<spriteSetFrame_t>(g_base + RVA_SPRITE_SET_FRAME)(spr, nullptr, frame);
}

// Build an uninitialized Axe Affinity, the way getNewAffinity builds the Club one. Callers run
// initSkillStrings (vtable slot 1) afterwards, as they do for every factory result.
static uint8_t* make_axe_affinity() {
    if (!g_axeVtbl[1] || !g_opnew) return nullptr;
    uint8_t* a = static_cast<uint8_t*>(g_opnew(0x1e8));
    if (!a) return nullptr;
    reinterpret_cast<affCtor_t>(g_base + RVA_AFFINITY_CTOR)(a, nullptr, AFFINITY_AXE, AXE_MAX_LEVEL);
    *reinterpret_cast<void***>(a) = &g_axeVtbl[1];
    if (void* tex = mod_texture(MODTEX_AXE_ICON)) sprite_show(a + 0x4c, tex, 0);   // the Axe tree's own icon
    else reinterpret_cast<spriteSetFrame_t>(g_base + RVA_SPRITE_SET_FRAME)(a + 0x4c, nullptr, AXE_ICON_FRAME);
    return a;
}
static getNewAff_t g_getNewAffTramp = nullptr;
static uint8_t* make_bomber_affinity();
static uint8_t* make_woods_affinity(int type);
static void* __fastcall det_getNewAffinity(int type, int arg) {
    if (type == AFFINITY_AXE) return make_axe_affinity();
    if (type == 27) return make_bomber_affinity();             // AFFINITY_BOMBER
    if (type == 28 || type == 29) return make_woods_affinity(type);   // AFFINITY_BRUTALIST / SCAVENGER
    return g_getNewAffTramp(type, arg);
}
// setStartingAffinities builds each class's trees inline (not through getNewAffinity) and has no Woodsman
// case, so after it runs we add the Woodsman's Axe tree. Random characters are built by populateAffinities.
typedef void (__fastcall *vecEmplace_t)(void* vec, void* edx, void* where, const void* val);
static const uintptr_t RVA_VEC4_EMPLACE = 0x00078480;   // vector<Affinity*>::_Emplace_reallocate(where, &val)
static setStartAffs_t g_setStartAffsTramp = nullptr;
static void __cdecl det_setStartingAffinities(void* player, void* ctrl) {
    g_setStartAffsTramp(player, ctrl);
    uint8_t* p = static_cast<uint8_t*>(player);
    if (!p || *reinterpret_cast<int*>(p + 0x34c) != CLASS_WOODSMAN) return;
    uint8_t* axe = make_axe_affinity();
    if (!axe) return;
    axe_initSkillStrings(axe, nullptr);
    *reinterpret_cast<int*>(axe + 0x24) = g_woodsmanAxeLevel;      // 0 normally; the test option unlocks levels
    // First in the list, the way the other classes' own trees come before the stat trees (str, dex, ...).
    void** vec = reinterpret_cast<void**>(p + OFF_PLAYER_AFFS);    // begin, end, cap
    void** beg = static_cast<void**>(vec[0]);
    void** end = static_cast<void**>(vec[1]);
    if (beg && end != vec[2]) { memmove(beg + 1, beg, (end - beg) * sizeof(void*)); *beg = axe; vec[1] = end + 1; }
    else reinterpret_cast<vecEmplace_t>(g_base + RVA_VEC4_EMPLACE)(vec, nullptr, beg ? beg : end, &axe);
    logmsg("[woodsman] Axe tree added (player=%p, level %d)", p, g_woodsmanAxeLevel);
    // Then Bomber, Brutalist and Scavenger, in that order, right after the Axe tree.
    struct { int type; int level; } extra[3] = {
        { 27, g_woodsmanBomberLevel }, { 28, g_woodsmanBrutalistLevel }, { 29, g_woodsmanScavengerLevel } };
    for (int i = 0; i < 3; ++i) {
        uint8_t* t = extra[i].type == 27 ? make_bomber_affinity() : make_woods_affinity(extra[i].type);
        if (!t) continue;
        reinterpret_cast<affInit_t>((*reinterpret_cast<void***>(t))[1])(t, nullptr);
        *reinterpret_cast<int*>(t + 0x24) = extra[i].level;
        beg = static_cast<void**>(vec[0]); end = static_cast<void**>(vec[1]);
        void** where = (beg && end - beg > i) ? beg + 1 + i : end;
        if (beg && end != vec[2]) { memmove(where + 1, where, (end - where) * sizeof(void*)); *where = t; vec[1] = end + 1; }
        else reinterpret_cast<vecEmplace_t>(g_base + RVA_VEC4_EMPLACE)(vec, nullptr, where, &t);
        logmsg("[woodsman] tree %d added (player=%p, level %d)", extra[i].type, p, extra[i].level);
    }
}

// ---- Bomber skill tree (AffinityType 27), the Woodsman's second tree ---------------------------------
//   1 Scavenged Fuses  enemies you kill have a 10% chance to drop a small bomb
//   2 Chain Reaction   enemies your bombs kill drop a small bomb (95%) or a regular bomb (5%)
//   3 Incendiary       your bombs blast 30% wider, set enemies on fire and don't hurt players
//   4 Contact Fuse     your bombs fly twice as far and go off when they touch an enemy; small bombs do x2
// Built like the Axe tree: an Affinity with AffinityClub's layout and a copy of its vtable, with
// initSkillStrings replaced (Club's runs, then every string is overwritten) and levelUp only bumping the
// level. The perks read the level straight off the player. Icon: the Club tree's frame as a stand-in.
// "Your bombs" = the bomb projectile's instigator (Projectile+0x2a4) is that player. Kills are seen in
// Unit::hurt (hp goes from > 0 to <= 0); a kill is a bomb kill when the damage source is the explosion
// SpellEntity a bomb made (remembered in a small ring as each bomb goes off), the bomb Projectile itself (a
// direct hit), or the enemy burns to death within 10 s of a hit by your bomb. Drops happen on the host.
static const int AFFINITY_BOMBER = 27;
static const int BOMBER_MAX_LEVEL = 4;
static void* g_bomberVtbl[5];
static const char* const kBomberNames[4] = { "Scavenged Fuses", "Chain Reaction", "Incendiary", "Contact Fuse" };
static const char* const kBomberDescs[4] = {
    "Enemies you kill have a small chance to drop a small bomb.",
    "Enemies killed by your bombs drop a small bomb, or rarely a regular bomb.",
    "Your bombs have a 30% larger blast, set enemies on fire and don't hurt you or your allies.",
    "Your bombs are thrown twice as far and explode when they touch an enemy. Small bombs deal double damage.",
};
static void __fastcall bomber_initSkillStrings(uint8_t* self, void*) {
    g_clubInitStrings(self, nullptr);                 // allocates the name + 4 names/descriptions
    strAssign_t assign = reinterpret_cast<strAssign_t>(g_base + RVA_STRING_ASSIGN);
    assign(self + 0xc, nullptr, "Bomber", 6);
    for (int v = 0; v < 2; ++v) {
        const char* const* txt = v ? kBomberDescs : kBomberNames;
        uint8_t* b = *reinterpret_cast<uint8_t**>(self + (v ? 0x40 : 0x34));
        uint8_t* e = *reinterpret_cast<uint8_t**>(self + (v ? 0x44 : 0x38));
        for (int i = 0; b && i < 4 && b + (i + 1) * 0x18 <= e; ++i) assign(b + i * 0x18, nullptr, txt[i], strlen(txt[i]));
    }
}
static uint8_t* make_bomber_affinity() {
    if (!g_bomberVtbl[1] || !g_opnew) return nullptr;
    uint8_t* a = static_cast<uint8_t*>(g_opnew(0x1e8));
    if (!a) return nullptr;
    reinterpret_cast<affCtor_t>(g_base + RVA_AFFINITY_CTOR)(a, nullptr, AFFINITY_BOMBER, BOMBER_MAX_LEVEL);
    *reinterpret_cast<void***>(a) = &g_bomberVtbl[1];
    reinterpret_cast<spriteSetFrame_t>(g_base + RVA_SPRITE_SET_FRAME)(a + 0x4c, nullptr, AXE_ICON_FRAME);
    return a;
}
static int tree_level(const uint8_t* player, int type) {
    if (!player) return 0;
    uint8_t* const* b = *reinterpret_cast<uint8_t* const* const*>(player + OFF_PLAYER_AFFS);
    uint8_t* const* e = *reinterpret_cast<uint8_t* const* const*>(player + OFF_PLAYER_AFFS + 4);
    for (uint8_t* const* it = b; b && it < e; ++it)
        if (*it && *reinterpret_cast<int*>(*it + 4) == type) return *reinterpret_cast<int*>(*it + 0x24);
    return 0;
}
static bool is_player_entity(const void* ent) {
    if (!ent || !g_ge) return false;
    uint8_t* g = reinterpret_cast<uint8_t*>(g_ge);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN + 4);
    for (uint8_t* e = pb; pb && pe && e < pe; e += PLAYER_VEC_STRIDE)
        if (*reinterpret_cast<void**>(e) == ent) return true;
    return false;
}
// A player in either GameEngine: g_ge (the only one in single player) or, on an MP host, the server's,
// which is where co-op damage is decided.
static bool is_player_any(const void* ent) {
    if (is_player_entity(ent)) return true;
    void* srv = g_lastServer;
    if (!ent || !srv || GetTickCount() - g_lastServerStep > 500) return false;
    uint8_t* g = static_cast<uint8_t*>(srv) + OFF_SERVER_GE;
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN + 4);
    for (uint8_t* e = pb; pb && pe && e < pe; e += PLAYER_VEC_STRIDE)
        if (*reinterpret_cast<void**>(e) == ent) return true;
    return false;
}
// In either GameEngine: on an MP host bombs go off in the server's, whose players aren't in g_ge.
static int bomber_level_of(const void* ent) {
    return is_player_any(ent) ? tree_level(static_cast<const uint8_t*>(ent), AFFINITY_BOMBER) : 0;
}
// Recent bomb explosions (SpellEntity -> who threw the bomb), for telling bomb kills apart. `spares`: a
// Bomber 3 bomb, whose blast passes players. In co-op each game hurts its own player, and the other games'
// copy of a blast is a network SpellEntity that never went through Projectile::kill there, so the host marks
// it in the explosion's net state (bit 2 of EntityState::flags2; an explosion, SpellType 3, sends only bits
// 0-1 there) and det_spellSetState files the copy here, its caster (the thrower) as the thrower.
static const uint32_t BOMBER_SPARES_NET_BIT = 0x4;
static struct BombBlast { const void* spell; const void* thrower; bool spares, fromNet; } g_bombBlasts[16];
static int g_bombBlastNext = 0;
static BombBlast* bomb_blast_entry(const void* spell) {
    for (auto& b : g_bombBlasts) if (b.spell && b.spell == spell) return &b;
    return nullptr;
}
static void bomb_blast_record(const void* spell, const void* thrower, bool spares, bool fromNet) {
    BombBlast* b = bomb_blast_entry(spell);
    if (!b) { b = &g_bombBlasts[g_bombBlastNext]; g_bombBlastNext = (g_bombBlastNext + 1) % 16; }
    *b = { spell, thrower, spares, fromNet };
}
static void bomber_on_explosion(const uint8_t* proj, uint8_t* spell) {
    const void* thrower = *reinterpret_cast<void* const*>(proj + 0x2a4);   // Projectile::instigator
    const bool incendiary = bomber_level_of(thrower) >= 3;
    bomb_blast_record(spell, thrower, incendiary, false);
    if (!incendiary) return;
    // Level 3: 30% wider blast (the explosion's box, Entity w/h at +0xc0/+0xc4, grown around its centre)
    // and fire (SpellEntity::elementalDmgAccumulationFire; each hit adds 2.5x this, 1.0 ignites).
    float* x = reinterpret_cast<float*>(spell + 0xac); float* y = reinterpret_cast<float*>(spell + 0xb0);
    float* w = reinterpret_cast<float*>(spell + 0xc0); float* h = reinterpret_cast<float*>(spell + 0xc4);
    *x -= *w * 0.15f; *y -= *h * 0.15f; *w *= 1.3f; *h *= 1.3f;
    *reinterpret_cast<float*>(spell + 0x20c) = 1.0f;
    logmsg("[bomber] incendiary blast %.0fx%.0f", *w, *h);
}
static const void* bomb_blast_thrower(const void* source) {
    const BombBlast* b = bomb_blast_entry(source);
    return b ? b->thrower : nullptr;
}
static bool bomb_blast_spares(const void* spell) {
    const BombBlast* b = bomb_blast_entry(spell);
    return b && b->spares;
}
typedef void* (__fastcall *spellGetState_t)(void* self, void* edx);
typedef void  (__fastcall *spellSetState_t)(void* self, void* edx, void* st);
static spellGetState_t g_spellGetStateTramp = nullptr;
static spellSetState_t g_spellSetStateTramp = nullptr;
static void* __fastcall det_spellGetState(void* self, void* edx) {
    void* st = g_spellGetStateTramp(self, edx);
    uint8_t* s = static_cast<uint8_t*>(self);
    if (*reinterpret_cast<int*>(s + 0x224) == 3 && bomb_blast_spares(s))
        *reinterpret_cast<uint32_t*>(s + 0x70) |= BOMBER_SPARES_NET_BIT;   // state (+0x54) flags2 (+0x1c)
    return st;
}
static void __fastcall det_spellSetState(void* self, void* edx, void* st) {
    g_spellSetStateTramp(self, edx, st);
    uint8_t* s = static_cast<uint8_t*>(self);
    if (!st || *reinterpret_cast<int*>(s + 0x224) != 3) return;
    const bool mark = (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(st) + 0x1c) & BOMBER_SPARES_NET_BIT) != 0;
    BombBlast* b = bomb_blast_entry(s);
    if (mark) {
        const void* caster = *reinterpret_cast<void* const*>(s + 0x218);   // SpellEntity::caster
        if (!b || !b->spares) logmsg("[bomber] net blast %p from %p: spares players", s, caster);
        bomb_blast_record(s, caster, true, true);
    } else if (b && b->fromNet) {
        b->spell = nullptr;                                                // a reused address
    }
}
// A bomb hitting an enemy directly: the damage source is the bomb Projectile itself (PROJECTILE_BOMB = 14).
static const uintptr_t RVA_PROJECTILE_VTABLE = 0x0071e970;   // Projectile::`vftable'
static const void* bomb_projectile_thrower(const void* source) {
    const uint8_t* p = static_cast<const uint8_t*>(source);
    if (!p || *reinterpret_cast<const uintptr_t*>(p) != g_base + RVA_PROJECTILE_VTABLE || p[0x1f8] != 14) return nullptr;
    return *reinterpret_cast<void* const*>(p + 0x2a4);                     // Projectile::instigator
}
// Level 3 (Incendiary): your bombs don't hurt players, you or your allies. The blast skips players in
// SpellEntity::doCollision (det_spellCollision, so no knockback either); this catches any other hit by
// your bomb (a direct hit by the bomb itself) at the top of Player::hurt.
static bool bomber_spares_player(const uint8_t* player, const uint8_t* meta) {
    if (!player || !meta) return false;
    const void* source = *reinterpret_cast<void* const*>(meta + 0xc);      // DamageEventMeta::source
    const void* thrower = bomb_blast_thrower(source);
    if (!thrower) thrower = bomb_projectile_thrower(source);
    if (!bomb_blast_spares(source) &&
        (!thrower || !is_player_any(thrower) || tree_level(static_cast<const uint8_t*>(thrower), AFFINITY_BOMBER) < 3))
        return false;
    logmsg("[bomber] no friendly fire: %s bomb hit on player %p skipped", thrower == player ? "own" : "ally's", player);
    return true;
}
// Enemies your bombs hit recently (blast or direct), so a death by the fire they started still counts as a
// bomb kill: the burn's damage carries no bomb, so a burning enemy hit by your bomb within the last 10 s is
// put down to that bomb.
static struct { const void* unit; const void* thrower; DWORD tick; } g_bombHits[32];
static int g_bombHitNext = 0;
static const DWORD BOMB_FIRE_CREDIT_MS = 10000;
static void bomb_hit_remember(const void* unit, const void* thrower) {
    for (auto& h : g_bombHits) if (h.unit == unit) { h.thrower = thrower; h.tick = GetTickCount(); return; }
    g_bombHits[g_bombHitNext] = { unit, thrower, GetTickCount() };
    g_bombHitNext = (g_bombHitNext + 1) % 32;
}
static const void* bomb_fire_thrower(const uint8_t* unit) {
    if (!unit[0x213]) return nullptr;                                      // Unit::burning
    for (const auto& h : g_bombHits)
        if (h.unit == unit && GetTickCount() - h.tick <= BOMB_FIRE_CREDIT_MS) return h.thrower;
    return nullptr;
}
// Drop a bomb (small or regular) at a spot on the level, as a loose item.
static void drop_bomb_at(void* level, float x, float y, bool smallBomb) {
    if (!level || !g_makeItemVA || !g_addEntityTramp) return;
    int type = ITEMTYPE_BOMB, count = 1;
    void* sp[2] = {nullptr, nullptr};
    make_item_shared(&sp, &level, &x, &y, &type, &count);
    if (!sp[0]) return;
    if (smallBomb) {
        uint8_t* item = static_cast<uint8_t*>(sp[0]);
        item[OFF_ITEM_SMALL] = 1;
        det_setCost(item, nullptr);
        small_bomb_dress(item);
    }
    g_addEntityTramp(level, &sp);
    release_shared_block(sp[1]);
}
typedef int (__fastcall *unitHurt_t)(void* self, void* edx, const void* meta);
static unitHurt_t g_unitHurtTramp = nullptr;
static void stomp_note_hit(const uint8_t* meta);   // (Party, below)
static const void* brutalist_pre_hurt(uint8_t* u, const uint8_t* meta, uint8_t* copy);   // Brutalist (below)
static void brutalist_post_hurt(uint8_t* u, const uint8_t* meta, float hp0);
static int __fastcall det_unitHurt(void* self, void* edx, const void* meta) {
    uint8_t* u = static_cast<uint8_t*>(self);
    const float hp0 = *reinterpret_cast<float*>(u + 0x10c);
    const bool target = u[0x160] && u[0x226];       // a real enemy, not a breakable doodad (as the contact fuse)
    // Read the damage's bomb (if any) before the hurt runs: the source may be the bomb itself.
    const uint8_t* m = static_cast<const uint8_t*>(meta);
    const void* source = m ? *reinterpret_cast<void* const*>(m + 0xc) : nullptr;     // DamageEventMeta::source
    const void* bombThrower = bomb_blast_thrower(source);                          // its blast
    if (!bombThrower) bombThrower = bomb_projectile_thrower(source);               // a direct hit
    alignas(4) uint8_t metaCopy[0x20];
    const bool enemy = target && m && !is_player_entity(u);
    const void* useMeta = enemy ? brutalist_pre_hurt(u, m, metaCopy) : meta;   // Executioner may raise it
    alignas(4) uint8_t halfCopy[0x20];
    if (m && shadow_ent_is(source)) {          // Party: the shadow's arrows, shots and spells hit for half
        memcpy(halfCopy, useMeta, 0x1c);
        int* d = reinterpret_cast<int*>(halfCopy);
        const int was = *d;
        if (*d > 0) *d = (*d + 1) / 2;
        useMeta = halfCopy;
        logmsg("[brutalist] party: shadow's %d hit %p for %d (was %d)", *reinterpret_cast<const uint8_t*>(
               static_cast<const uint8_t*>(source) + 0x50), u, *d, was);
    }
    if (enemy) stomp_note_hit(static_cast<const uint8_t*>(useMeta));     // Party: your stomp's landing hit
    int r = g_unitHurtTramp(self, edx, useMeta);
    if (enemy) brutalist_post_hurt(u, static_cast<const uint8_t*>(useMeta), hp0);   // Momentum, Sunder
    if (!target || u[0xa2] || !meta || is_player_entity(u)) return r;
    if (bombThrower && hp0 > 0.0f) bomb_hit_remember(u, bombThrower);
    if (hp0 <= 0.0f || *reinterpret_cast<float*>(u + 0x10c) > 0.0f) return r;
    const void* killer = *reinterpret_cast<void* const*>(m + 0x14);       // DamageEventMeta::instigator
    if (!bombThrower) bombThrower = bomb_fire_thrower(u);                   // burned to death after a bomb
    if (bombThrower) killer = bombThrower;
    const int lvl = bomber_level_of(killer);
    if (lvl <= 0) return r;
    bool drop = false, smallBomb = true;
    if (bombThrower && lvl >= 2) { drop = true; smallBomb = (rand() % 100) >= 5; }   // 95% small, 5% regular
    else drop = (rand() % 100) < 10;                                                 // level 1: 10%
    if (!drop) return r;
    void* level = *reinterpret_cast<void**>(u + OFF_ENT_LEVEL2);
    float cx = *reinterpret_cast<float*>(u + 0xac) + *reinterpret_cast<float*>(u + 0xc0) * 0.5f;
    float cy = *reinterpret_cast<float*>(u + 0xb0) + *reinterpret_cast<float*>(u + 0xc4) * 0.5f;
    drop_bomb_at(level, cx, cy, smallBomb);
    logmsg("[bomber] %s kill of %p (vtbl rva 0x%x) dropped a %s bomb (level %d)", bombThrower ? "bomb" : "plain", u,
           (unsigned)(*reinterpret_cast<uintptr_t*>(u) - g_base), smallBomb ? "small" : "regular", lvl);
    return r;
}
// Projectile::stepBomb, level 4: on the bomb's first step, double its throw velocity (Entity vx/vy at
// +0xb4/+0xb8); every step, if it overlaps a live enemy, zero its fuse (+0x244) so this step explodes it.
// Projectile+0x2a3 is padding; the ctor hook clears it and it marks "already boosted".
static void* g_projCtorTramp = nullptr;
__declspec(naked) static void det_projCtor() {
    __asm {
        mov  word ptr [ecx+0x2a2], 0     // +0x2a2 small over the net, +0x2a3 contact-fuse boosted
        jmp  dword ptr [g_projCtorTramp]
    }
}
static void* g_stepBombTramp = nullptr;
static uint8_t* volatile g_lastBombProj = nullptr;   // the bomb that stepped last, and when (test harness: goto_bomb)
static volatile DWORD g_lastBombTick = 0;
static void __cdecl bomber_step_bomb(uint8_t* proj) {
    g_lastBombProj = proj; g_lastBombTick = GetTickCount();
    if (proj[0xa2]) return;                                       // the host steers bombs
    if (bomber_level_of(*reinterpret_cast<void**>(proj + 0x2a4)) < 4) return;
    if (proj[0x2a3] != 0xB7) {
        proj[0x2a3] = 0xB7;
        *reinterpret_cast<float*>(proj + 0xb4) *= 2.0f;
        *reinterpret_cast<float*>(proj + 0xb8) *= 2.0f;
        logmsg("[bomber] contact-fuse bomb thrown x2");
    }
    if (*reinterpret_cast<int*>(proj + 0x244) == 0) return;
    uint8_t* level = *reinterpret_cast<uint8_t**>(proj + OFF_ENT_LEVEL2);
    uint8_t** mons = level ? *reinterpret_cast<uint8_t***>(level + 0xfd0) : nullptr;   // vector<shared_ptr<Entity>>*
    if (!mons) return;
    // The bomb's box over this whole step (where it is and where its speed takes it, plus a pixel): stepBomb
    // moves it and its collision bounces it off an enemy (the impact hit) before the next check, so testing
    // only where it is now caught the enemy after it had already bounced away.
    const float vx = *reinterpret_cast<float*>(proj + 0xb4), vy = *reinterpret_cast<float*>(proj + 0xb8);
    const float bx = *reinterpret_cast<float*>(proj + 0xac) + (vx < 0 ? vx : 0) - 1.0f;
    const float by = *reinterpret_cast<float*>(proj + 0xb0) + (vy < 0 ? vy : 0) - 1.0f;
    const float bw = *reinterpret_cast<float*>(proj + 0xc0) + (vx < 0 ? -vx : vx) + 2.0f;
    const float bh = *reinterpret_cast<float*>(proj + 0xc4) + (vy < 0 ? -vy : vy) + 2.0f;
    const int faction = *reinterpret_cast<int*>(proj + 0x158);
    for (uint8_t* it = mons[0]; it && it < mons[1]; it += 8) {
        uint8_t* e = *reinterpret_cast<uint8_t**>(it);
        // Same test as Level::getMonsterClosestTo: a live, targetable unit (+0x160, +0x226; doodads in this
        // list fail it), not dead (+0x148), of another faction (+0x158).
        if (!e || !e[0x160] || !e[0x226] || e[0x148] || *reinterpret_cast<int*>(e + 0x158) == faction) continue;
        const float ex = *reinterpret_cast<float*>(e + 0xac), ey = *reinterpret_cast<float*>(e + 0xb0);
        const float ew = *reinterpret_cast<float*>(e + 0xc0), eh = *reinterpret_cast<float*>(e + 0xc4);
        if (bx < ex + ew && ex < bx + bw && by < ey + eh && ey < by + bh) {
            *reinterpret_cast<int*>(proj + 0x244) = 0;           // fuse out: stepBomb explodes it now
            logmsg("[bomber] contact fuse: enemy %p (vtbl rva 0x%x, faction %d) at %.0f,%.0f %.0fx%.0f; bomb %.0f,%.0f faction %d",
                   e, (unsigned)(*reinterpret_cast<uintptr_t*>(e) - g_base), *reinterpret_cast<int*>(e + 0x158), ex, ey, ew, eh, bx, by, faction);
            return;
        }
    }
}
__declspec(naked) static void det_stepBomb() {
    __asm {
        pushad
        push ecx
        call bomber_step_bomb
        add  esp, 4
        popad
        jmp  dword ptr [g_stepBombTramp]
    }
}
static void install_bomber_tree() {
    void** club = reinterpret_cast<void**>(g_base + RVA_CLUB_VTABLE);
    for (int i = 0; i < 5; i++) g_bomberVtbl[i] = club[i - 1];
    g_bomberVtbl[2] = reinterpret_cast<void*>(&bomber_initSkillStrings);
    g_bomberVtbl[3] = reinterpret_cast<void*>(&axe_levelUp);
    const uint8_t hurtPro[6] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08};       // push ebx; mov ebx,esp; sub esp,8
    const uint8_t pro[5]     = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    const uint8_t stepPro[6] = {0x8A, 0x91, 0x39, 0x01, 0x00, 0x00};       // mov dl,[ecx+0x139]
    g_unitHurtTramp = reinterpret_cast<unitHurt_t>(install_detour(0x003e6aa0, (void*)&det_unitHurt, 6, hurtPro, 6));
    g_projCtorTramp = install_detour(0x0031ff10, (void*)&det_projCtor, 5, pro, 5);
    g_stepBombTramp = install_detour(0x00332180, (void*)&det_stepBomb, 6, stepPro, 6);
    const uint8_t getPro[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};       // push ebp; mov ebp,esp; and esp,-8
    g_spellGetStateTramp = reinterpret_cast<spellGetState_t>(install_detour(0x003a1190, (void*)&det_spellGetState, 6, getPro, 6));
    g_spellSetStateTramp = reinterpret_cast<spellSetState_t>(install_detour(0x003a18e0, (void*)&det_spellSetState, 5, pro, 5));
    logmsg("[bomber] hurt %d projCtor %d stepBomb %d blast net state %d/%d", !!g_unitHurtTramp, !!g_projCtorTramp,
           !!g_stepBombTramp, !!g_spellGetStateTramp, !!g_spellSetStateTramp);
}

// ---- Brutalist (AffinityType 28) and Scavenger (29), the Woodsman's third and fourth trees ----------
// Built like the Bomber tree (AffinityClub's layout, a copy of its vtable with initSkillStrings and levelUp
// replaced); the perks read the level straight off the player.
//   Brutalist (4 levels)
//     1 Momentum     each attack that hits makes the next one faster (+MOMENTUM_IAS attack speed a stack,
//                    up to 3). One miss is forgiven, a second in a row ends the streak, and it fades
//                    after 5 s without a hit. Rides on Player::increasedAttackSpeed (+0x940), which
//                    Player::attack copies to the weapon when a swing starts (frame times x (1 - ias)).
//     2 Sunder       each hit lowers the enemy's defense (Unit+0x21c, an int; below 0 it adds damage) by
//                    10% of the damage dealt, for 5 s. Stacks without limit; a hit never refreshes an older
//                    stack. Host side (where enemies take damage).
//     3 Executioner  +20% damage against enemies under 30% health.
//     4 Party        the game's own switched-off afterimage (Player+0xb80, never set by the game): a shadow
//                    copy trails you and repeats your attacks.
//   Scavenger (3 levels), each level +5 max health
//     1 Hearthside   resting at a bonfire heals 5 more (the moment Player+0xc94 "rested here" is set)
//     2 Lockpick     locked doors open without a key: Player::haveBossKey answers yes, and
//                    Player::removeBossKey keeps the key you hold. Chests check the key elsewhere.
//     3 Recycler     a weapon or armor the anvil (GOD_EQUIP) or The Deranged (GOD_CHAOS) accepts pays the
//                    nearest Scavenger 10 gold. Host side, like the shrine itself.
static const int AFFINITY_BRUTALIST = 28;
static const int AFFINITY_SCAVENGER = 29;
static const float MOMENTUM_IAS = 0.08f;
static const int   MOMENTUM_MAX = 3;
static const DWORD MOMENTUM_FADE_MS = 5000;
static const DWORD SUNDER_MS = 5000;
static const int   SCAVENGER_HP_PER_LEVEL = 5;
static const int   SCAVENGER_BONFIRE_HEAL = 5;
static const int   RECYCLE_GOLD_MIN = 1, RECYCLE_GOLD_MAX = 4;   // a gold pickup of 1-4, dropped at the shrine
static void* g_brutalistVtbl[5];
static void* g_scavengerVtbl[5];
static const char* const kBrutalistNames[4] = { "Momentum", "Sunder", "Executioner", "Party" };
static const char* const kBrutalistDescs[4] = {
    "Each hit in a row makes your next attack faster, up to 3 times. One miss is forgiven. Fades after 5 seconds.",
    "Your hits lower the enemy's defense by a tenth of the damage dealt for 5 seconds. Stacks without limit.",
    "Deal 20% more damage to enemies below 30% health.",
    "A shadow of yourself follows you and copies your attacks.",
};
static const char* const kScavengerNames[4] = { "Hearthside", "Lockpick", "Recycler", "" };
static const char* const kScavengerDescs[4] = {
    "Bonfires heal you 5 more. +5 max health.",
    "Open locked doors without a key, and keep any key you hold. +5 max health.",
    "Weapons and armor given to the anvil or The Deranged give you a bit of gold. +5 max health.",
    "",
};
static void __fastcall woods_initSkillStrings(uint8_t* self, void*) {
    g_clubInitStrings(self, nullptr);                 // allocates the name + 4 names/descriptions
    const bool brut = *reinterpret_cast<int*>(self + 4) == AFFINITY_BRUTALIST;
    // Scavenger has 3 levels. Affinity::maxLevel (+0x2c) is set to 3 only now: Club's string setup
    // assumes 4 levels and corrupts the heap when the tree is built with 3.
    if (!brut) *reinterpret_cast<int*>(self + 0x2c) = 3;
    strAssign_t assign = reinterpret_cast<strAssign_t>(g_base + RVA_STRING_ASSIGN);
    const char* name = brut ? "Brutalist" : "Scavenger";
    assign(self + 0xc, nullptr, name, strlen(name));
    for (int v = 0; v < 2; ++v) {
        const char* const* txt = v ? (brut ? kBrutalistDescs : kScavengerDescs) : (brut ? kBrutalistNames : kScavengerNames);
        uint8_t* b = *reinterpret_cast<uint8_t**>(self + (v ? 0x40 : 0x34));
        uint8_t* e = *reinterpret_cast<uint8_t**>(self + (v ? 0x44 : 0x38));
        for (int i = 0; b && i < 4 && b + (i + 1) * 0x18 <= e; ++i) assign(b + i * 0x18, nullptr, txt[i], strlen(txt[i]));
    }
}
static uint8_t* make_woods_affinity(int type) {
    void** vt = type == AFFINITY_BRUTALIST ? g_brutalistVtbl : g_scavengerVtbl;
    if (!vt[1] || !g_opnew) return nullptr;
    uint8_t* a = static_cast<uint8_t*>(g_opnew(0x1e8));
    if (!a) return nullptr;
    reinterpret_cast<affCtor_t>(g_base + RVA_AFFINITY_CTOR)(a, nullptr, type, 4);   // 4 even for Scavenger: see woods_initSkillStrings
    *reinterpret_cast<void***>(a) = &vt[1];
    // Stand-in icons: Berserk's for Brutalist, Alchemy's for Scavenger.
    reinterpret_cast<spriteSetFrame_t>(g_base + RVA_SPRITE_SET_FRAME)(a + 0x4c, nullptr, type == AFFINITY_BRUTALIST ? 0x12 : 0x08);
    return a;
}

// -- Momentum: per-player streak state --
struct MomentumState { void* player; int stacks; int misses; bool hit; bool prevSwing; DWORD lastHit; float applied; };
static MomentumState g_momentum[8];
static MomentumState* momentum_for(void* player) {
    for (auto& m : g_momentum) if (m.player == player) return &m;
    for (auto& m : g_momentum) if (!m.player) { m = {}; m.player = player; return &m; }
    MomentumState* m = &g_momentum[(reinterpret_cast<uintptr_t>(player) >> 4) & 7];
    *m = {}; m->player = player;
    return m;
}
// Runs right after Player::updateStats, which rebuilds max HP and +0x940 (attack speed) from scratch. It
// only runs when stats change, not every frame, so the per-frame part is woodsman_tick below.
static void woodsman_stats(uint8_t* p) {
    const int scav = tree_level(p, AFFINITY_SCAVENGER);
    if (scav > 0) *reinterpret_cast<float*>(p + 0x110) += (float)(SCAVENGER_HP_PER_LEVEL * scav);
    // A new Scavenger level also fills the 5 it added, the way a Vitality level does (Unit::hp +0x10c).
    static struct { const uint8_t* player; int level; } seen[8];
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; ++i) if (seen[i].player == p) slot = i;
    if (slot < 0) for (int i = 0; i < 8 && slot < 0; ++i) if (!seen[i].player || !is_player_entity(seen[i].player)) {
        slot = i; seen[i].player = p; seen[i].level = 0;
    }
    if (slot >= 0 && scav != seen[slot].level) {
        if (scav > seen[slot].level) {
            float* hp = reinterpret_cast<float*>(p + 0x10c);
            const float mx = *reinterpret_cast<float*>(p + 0x110);
            *hp += (float)(SCAVENGER_HP_PER_LEVEL * (scav - seen[slot].level));
            if (*hp > mx) *hp = mx;
        }
        seen[slot].level = scav;
    }
    for (auto& m : g_momentum) if (m.player == p) {                // put the Momentum bonus back on
        m.applied = MOMENTUM_IAS * m.stacks;
        *reinterpret_cast<float*>(p + 0x940) += m.applied;
    }
}
// Every frame, for every player: the afterimage switch, the Momentum streak (an attack ends when the
// weapon's swinging flag drops), and its attack-speed bonus applied as a change to +0x940.
// -- Party: a shadow copy that replays you, CLONE_DELAY frames late --
// The game's cut prototype (Player::afterimages, +0xb80) draws the shadow from Player::afterImageFrames
// (+0x7860, vector<Sprite>, a sprite snapshot pushed by pushShadowCloneSprite, trimmed to afterimages*40,
// index 0 the oldest). Stock only pushes on frames you move, so the shadow trailed you like a pet; now it
// pushes every frame, so it walks your exact path 40 frames behind. Its weapons were copies of yours that
// swung the moment you did and were wiped when your swing ended; both of its clone loops (Player::attack,
// Player::step) are skipped and the copies are made here instead. At your startSwing (Player::attack's
// call, 0x2d3ae2) the weapon is copied with its swing arguments, and CLONE_DELAY frames later the copy is
// added to the level at the shadow's spot and swung the same way. The copy constructor can't put the copy
// in your weapon list (see the setParent patch below), and only listed weapons get their hit shapes built
// each frame (Unit::updateWeaponChildren -> Weapon::resetScriptOrigin), which is why the shadow's swings
// never hit: Weapon::setParent is called again once the copy is owned. Its swing is stepped here each frame
// (Weapon::stepSwing, as Player::step does for its own weapons). Shadow hits do half damage (stock).
// Being parented to you, its weapon is laid out from your position, so for those calls your position is
// swapped for the shadow's (clone_origin_begin): the swing comes off its body and a thrown axe flies home to
// it. Your button presses during the swing (holding and letting go of a charge) and your throw's aim are
// recorded and replayed on its copy at the same point of its swing, and its axe's catch only ends its own
// swing (stock it cancelled your attack and took your weapon, so your axe never came back). The axe charge
// itself (set just after startSwing) is copied over when yours starts, and letting go of a bow or a bomb
// (Weapon::stopSwing) is replayed too, so the shadow fires and throws its own; its small bombs stay small.
static const int CLONE_DELAY = 40;
static const uintptr_t RVA_WEAPON_START_SWING = 0x003f25f0;   // Weapon::startSwing(a, b, faction) + xmm2, ret 0xc
static const uintptr_t RVA_WEAPON_COPY_SHARED = 0x00305d00;   // make_shared<Weapon>(Weapon&): ecx = &result, edx = src
static const uintptr_t RVA_WEAPON_SET_CLONE   = 0x003f58c0;   // Weapon::setClone() (shadow = 1)
static const uintptr_t RVA_WEAPON_SET_PARENT  = 0x003f3a50;   // Weapon::setParent(shared_ptr<Unit>) by value, ret 8
static const uintptr_t RVA_WEAPON_SET_POS     = 0x003f3d30;   // Weapon::setPosition(const Vector2f&), ret 4
static const uintptr_t RVA_SPRITE_GET_POS     = 0x003b9de0;   // Sprite::getPosition(Vector2f* out), ret 4
static const uintptr_t RVA_WEAPON_STEP_SWING  = 0x003ef970;   // Weapon::stepSwing(bool facingLeft, bool), ret 8
typedef void (__thiscall *weaponStepSwing_t)(void* self, int facingLeft, int b);
typedef void (__fastcall *weaponCopy_t)(void* result, void* src);
typedef void (__thiscall *weaponNoArg_t)(void* self);
typedef void (__thiscall *weaponSetParent_t)(void* self, void* ptr, void* ctrl);
typedef void (__thiscall *weaponSetPos_t)(void* self, const float* v);
typedef float* (__thiscall *spriteGetPos_t)(void* self, float* out);
struct CloneSwing { uint8_t* player; void* sp[2]; int due; int a, b, c; float f; bool started; int age; bool swung; float x, y; bool thrown;
    // Your swing as it plays out, to replay on the shadow's copy: your charge is held and let go by your
    // button, which only reaches your own weapon (Weapon::canActivateWand +0x224 and chargingAttack +0x28c,
    // per frame from your startSwing), and a throw's aim byte (+0x2a7, see axe_latch_aim).
    uint8_t* orig; int rec, hn, aim; uint8_t h224[256], h28c[256];
    // An axe's charge is switched on by Player::attack just after startSwing (axe_charge_apply), so the copy
    // made at startSwing never had it: the charge flag, its hold keyframe (+0x290) and head start (+0x294)
    // are taken from your weapon the first frame it charges and put on the copy at the same point.
    bool capSet, capApplied; int cap290, cap294;
    // Bows and bombs fire when you let go (Player::stopAttack -> Weapon::stopSwing): the frame of your
    // swing that happened on, replayed on the copy. smallBomb: the bomb you wound up was a small one.
    int stopAt; bool stopped, smallBomb; uint8_t prevCharge;
    // Holy light (Divine Weapon): the frame of your swing it came out on and your weapon speed then.
    int holyAt; bool holyDone; float holySpeed; };
static CloneSwing g_clones[16];
static int g_cloneFrame = 0;
static int g_shadowThrows = 0;   // shadow throws seen in this game (the test harness reads it)
static uintptr_t g_startSwingVA = 0;
__declspec(naked) static void call_start_swing(void* w, int a, int b, int c, float f) {
    __asm {
        push ebp
        mov  ebp, esp
        movss xmm2, dword ptr [ebp+0x18]
        push dword ptr [ebp+0x14]
        push dword ptr [ebp+0x10]
        push dword ptr [ebp+0x0c]
        mov  ecx, dword ptr [ebp+0x08]
        call dword ptr [g_startSwingVA]
        pop  ebp
        ret
    }
}
// The shadow's spot: the oldest snapshot's position plus half your size (as Player::step placed its weapons).
static bool clone_spot(const uint8_t* p, float* pos, uint8_t* flip) {
    uint8_t* fb = *reinterpret_cast<uint8_t* const*>(p + 0x7860);
    uint8_t* fe = *reinterpret_cast<uint8_t* const*>(p + 0x7864);
    if (!fb || fe <= fb) return false;
    reinterpret_cast<spriteGetPos_t>(g_base + RVA_SPRITE_GET_POS)(fb, pos);
    pos[0] += *reinterpret_cast<const float*>(p + 0xc0) * 0.5f;
    pos[1] += *reinterpret_cast<const float*>(p + 0xc4) * 0.5f;
    if (flip) *flip = fb[0xa8];
    return true;
}
// Where the shadow stands, in your own position terms (Entity +0xac/+0xb0). Your position is kept every frame
// (woodsman_tick), and the shadow is where you were as many frames ago as the game has snapshots to draw it
// from (index 0 of afterImageFrames; a new level clears them). Sprite positions can't be used for this: a
// flipped sprite's position is shifted by its width, so turning around moved the shadow's spot.
struct CloneTrail { const uint8_t* p; float pos[64][2]; int head, n; };
static CloneTrail g_trails[8];
static void clone_trail_push(const uint8_t* p) {
    CloneTrail* t = nullptr;
    for (auto& tr : g_trails) if (tr.p == p) { t = &tr; break; }
    if (!t) for (auto& tr : g_trails) if (!tr.p || !is_player_entity(const_cast<uint8_t*>(tr.p))) { tr = CloneTrail{}; tr.p = p; t = &tr; break; }
    if (!t) return;
    t->pos[t->head][0] = *reinterpret_cast<const float*>(p + 0xac);
    t->pos[t->head][1] = *reinterpret_cast<const float*>(p + 0xb0);
    t->head = (t->head + 1) % 64;
    if (t->n < 64) ++t->n;
}
static bool clone_body(const uint8_t* p, float* pos) {
    uint8_t* fb = *reinterpret_cast<uint8_t* const*>(p + 0x7860);
    uint8_t* fe = *reinterpret_cast<uint8_t* const*>(p + 0x7864);
    const int frames = (fb && fe > fb) ? (int)((fe - fb) / 0x194) : 0;
    for (auto& t : g_trails) {
        if (t.p != p || !t.n || !frames) continue;
        const int k = t.n < frames ? t.n : frames;            // k-1 frames before the newest
        const int i = ((t.head - k) % 64 + 64) % 64;
        pos[0] = t.pos[i][0]; pos[1] = t.pos[i][1];
        return true;
    }
    return false;
}
static CloneSwing* clone_of(const uint8_t* w) {
    if (!w || !w[0x29c]) return nullptr;                                     // Weapon::shadow
    for (auto& c : g_clones) if (c.player && c.sp[0] == w) return &c;
    return nullptr;
}
// Weapon::resetScriptOrigin lays a weapon's box and sprite out from its parent's position, and a thrown one
// homes back to it. The shadow's weapons are parented to you (so they're in your weapon list and hit), so for
// the length of that call your position is swapped for the shadow's: its swings come off its body and its
// thrown axe flies back to it.
struct CloneOriginSwap { uint8_t* p; float x, y; };
static CloneOriginSwap g_cloneSwapNow = { nullptr, 0, 0 };   // the swap in effect (for the catch log)
static CloneOriginSwap clone_origin_begin(const uint8_t* w) {
    CloneOriginSwap s{ nullptr, 0, 0 };
    if (g_cloneSwapNow.p) return s;              // already swapped (resetScriptOrigin inside stepSwing)
    CloneSwing* c = clone_of(w);
    float body[2];
    if (!c || *reinterpret_cast<uint8_t* const*>(w + 0x248) != c->player || !clone_body(c->player, body)) return s;
    s.p = c->player;
    s.x = *reinterpret_cast<float*>(s.p + 0xac); s.y = *reinterpret_cast<float*>(s.p + 0xb0);
    *reinterpret_cast<float*>(s.p + 0xac) = body[0]; *reinterpret_cast<float*>(s.p + 0xb0) = body[1];
    g_cloneSwapNow = s;
    return s;
}
static void clone_origin_end(const CloneOriginSwap& s) {
    if (!s.p) return;
    *reinterpret_cast<float*>(s.p + 0xac) = s.x; *reinterpret_cast<float*>(s.p + 0xb0) = s.y;
    g_cloneSwapNow.p = nullptr;
}
// Weapon::endDaggerThrow (the catch) cancels the parent's attack and hands it the weapon back
// (Player::cancelAttack, Player::nullifyWeapon). For the shadow's axe that was you, which broke your own
// thrown axe's return. A shadow weapon's catch ends its flight and its swing (as cancelAttack does yours;
// a swing still under way with the throw flag would throw again), and clone_tick drops it.
// Weapon::doCollision also catches a returning weapon that touches its parent, which for the shadow's axe is
// you, wherever the shadow is: that catch is ignored, and the shadow's axe is caught only when it reaches the
// shadow (the proximity catch in Weapon::resetScriptOrigin, run with the shadow's position swapped in).
static int g_inResetOrigin = 0;
typedef void (__fastcall *endDaggerThrow_t)(void* self, void* edx);
static endDaggerThrow_t g_endThrowTramp = nullptr;
static void __fastcall det_endDaggerThrow(void* self, void* edx) {
    uint8_t* w = static_cast<uint8_t*>(self);
    if (w[0x29c]) {
        if (!g_inResetOrigin) return;                            // touched you, not the shadow: keep flying
        uint8_t* p = *reinterpret_cast<uint8_t**>(w + 0x248);
        float bd[2] = { 0, 0 };
        if (is_player_entity(p)) clone_body(p, bd);
        logmsg("[brutalist] party: frame %d shadow caught its thrown weapon at %.0f,%.0f (shadow at %.0f,%.0f, you at %.0f,%.0f)", g_cloneFrame,
               *reinterpret_cast<float*>(w + 0x344), *reinterpret_cast<float*>(w + 0x348), bd[0], bd[1],
               g_cloneSwapNow.p == p ? g_cloneSwapNow.x : 0.f, g_cloneSwapNow.p == p ? g_cloneSwapNow.y : 0.f);
        w[0x2a6] = 0; w[0x2a5] = 0; w[0x269] = 0;                // thrown, doDaggerThrow, swinging
        return;
    }
    g_endThrowTramp(self, edx);
}
static void clone_free(CloneSwing& c) {
    if (c.sp[0] && c.started) static_cast<uint8_t*>(c.sp[0])[0x148] = 1;   // Entity::die: the level drops it
    release_shared_block(c.sp[1]);
    c = CloneSwing{};
}
static void __cdecl clone_record(uint8_t* w, int a, int b, int c, float f) {
    uint8_t* p = *reinterpret_cast<uint8_t**>(w + 0x248);                   // Weapon::parent
    if (!is_player_entity(p) || *reinterpret_cast<int*>(p + 0xb80) <= 0) return;
    CloneSwing* slot = nullptr;
    for (auto& cs : g_clones) if (!cs.player) { slot = &cs; break; }
    if (!slot) return;
    void* sp[2] = {nullptr, nullptr};
    reinterpret_cast<weaponCopy_t>(g_base + RVA_WEAPON_COPY_SHARED)(sp, w);
    if (!sp[0]) return;
    float x = *reinterpret_cast<float*>(p + 0xac), y = *reinterpret_cast<float*>(p + 0xb0);   // for the log
    *slot = CloneSwing{ p, { sp[0], sp[1] }, g_cloneFrame + CLONE_DELAY, a, b, c, f, false, 0, false, x, y };
    slot->orig = w; slot->rec = g_cloneFrame; slot->hn = 0; slot->aim = -1; slot->stopAt = -1; slot->holyAt = -1;
    slot->smallBomb = is_small_bomb(*reinterpret_cast<void**>(p + 0x76c0));    // Player::currWeaponItem
}
// Player::stopAttack lets go of your current weapon (a bow fires, a bomb is thrown) with Weapon::stopSwing,
// and then did the same for the shadow's weapons in its clone list, which is no longer used. Note when it
// happened to a weapon a shadow is copying, to do it to the copy that many frames into its own swing.
typedef void (__fastcall *weaponStopSwing_t)(void* self, void* edx);
static weaponStopSwing_t g_stopSwingTramp = nullptr;
static void __fastcall det_stopSwing(void* self, void* edx) {
    for (auto& c : g_clones)
        if (c.player && c.orig == self && c.stopAt < 0) c.stopAt = g_cloneFrame - c.rec;
    g_stopSwingTramp(self, edx);
}
// Holy light: Player::onWeaponJustStartedLastDmgFrame (0x303c50; thiscall, xmm1 = weapon speed, plain ret),
// run by Player::step for your current weapon only. With a Divine Weapon charge (status effect 0x1c) it uses
// one and fires a sword-laser projectile (type 0x12) from the weapon. When yours fires, the frame is noted, and
// the shadow does it the same number of frames into its swing: the call is repeated with its weapon as your
// current one, facing its way, from its spot, and as the server's copy of a remote player (serverSide +0xa3
// with serverPreparedDivineWeapon +0x7960), which fires without using a charge. All of it is put back after.
static void* g_lastDmgTramp = nullptr;
static float g_ldXmm1 = 0;
static int g_holyReplay = 0;
typedef void* (__thiscall *getStatus_t)(void* unit, int type);
static void __cdecl holy_note(uint8_t* p, float speed) {
    if (g_holyReplay || !is_player_entity(p) || *reinterpret_cast<int*>(p + 0xb80) <= 0) return;
    uint8_t* w = *reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON);
    if (!w || p[0x808] || !*reinterpret_cast<int*>(p + 0xa08)) return;     // isSkeleton, no Divine Weapon
    const uint8_t* script = *reinterpret_cast<uint8_t**>(w + 0x318);
    if (!script || script[0x95]) return;                                   // bows don't
    const bool charged = reinterpret_cast<getStatus_t>(g_base + 0x003e8460)(p, 0x1c) != nullptr;
    if (!charged && !(p[0xa3] && p[0x7960])) return;
    for (auto& c : g_clones)
        if (c.player == p && c.orig == w && c.holyAt < 0) {
            c.holyAt = g_cloneFrame - c.rec; c.holySpeed = speed;
            logmsg("[brutalist] party: holy light from your swing %d frames in", c.holyAt);
        }
}
__declspec(naked) static void det_lastDmgFrame() {
    __asm {
        movss dword ptr [g_ldXmm1], xmm1
        pushad
        push dword ptr [g_ldXmm1]
        push ecx
        call holy_note
        add  esp, 8
        popad
        movss xmm1, dword ptr [g_ldXmm1]
        jmp  dword ptr [g_lastDmgTramp]
    }
}
__declspec(naked) static void call_last_dmg(uint8_t* p, float speed) {
    __asm {
        push ebp
        mov  ebp, esp
        movss xmm1, dword ptr [ebp+0xc]
        mov  ecx, dword ptr [ebp+8]
        call dword ptr [g_lastDmgTramp]
        pop  ebp
        ret
    }
}
static void holy_replay(CloneSwing& c, uint8_t* w) {
    uint8_t* p = c.player;
    uint8_t** cw = reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON);
    uint8_t* const was = *cw;
    const uint8_t a3 = p[0xa3], prep = p[0x7960], face = p[0x225];
    *cw = w; p[0xa3] = 1; p[0x7960] = 1; p[0x225] = w[0x268];
    CloneOriginSwap s = clone_origin_begin(w);
    ++g_shadowActing; ++g_holyReplay;
    call_last_dmg(p, c.holySpeed);
    --g_holyReplay; --g_shadowActing;
    clone_origin_end(s);
    *cw = was; p[0xa3] = a3; p[0x7960] = prep; p[0x225] = face;
    logmsg("[brutalist] party: frame %d shadow's holy light (%d frames in)", g_cloneFrame, c.age);
}
// Bear's Stomp: Player::stopBearStomp(bool landed) (0x301220, thiscall, ret 4) makes the landing shockwave
// (SpellEntity SPELL_BEAR_STOMP, 80x80 at your feet), dust and a sound, and clears the stomp (+0x990) and the
// set of enemies its fall already hit (+0x78fc). A landing is noted and replayed from the shadow's spot 40
// frames later: the stomp flag, the net effect toggle (+0xc99) and that set are kept out of it (an empty
// stand-in set is swapped in), so nothing of yours changes.
typedef void (__fastcall *bearStomp_t)(void* p, void* edx, int landed);
static bearStomp_t g_stompTramp = nullptr;
struct StompReplay { uint8_t* p; int due; };
static StompReplay g_stomps[8];
static int g_stompReplay = 0;
struct TreeHead { void* l; void* par; void* r; uint8_t color, isnil, pad[2]; void* val[2]; };
static TreeHead g_emptyHead;
static void __fastcall det_stopBearStomp(void* self, void* edx, int landed) {
    uint8_t* p = static_cast<uint8_t*>(self);
    if (!g_stompReplay && (landed & 0xff) && p[0x990] && is_player_entity(p) && *reinterpret_cast<int*>(p + 0xb80) > 0)
        for (auto& st : g_stomps)
            if (!st.p) { st = StompReplay{ p, g_cloneFrame + CLONE_DELAY }; logmsg("[brutalist] party: your stomp landed"); break; }
    g_stompTramp(self, edx, landed);
}
// The stomp's damage is your body landing on an enemy (Player::doCollision while stomping, through Unit::hurt
// with you as source and attacker); the shockwave only pushes. The shadow has no body, so its landing hurts
// whatever enemy is under its feet with your stomp's hit (DamageEventMeta, 0x1c bytes) at half damage.
// Player::doCollision hurts an enemy you stomp on (vtable +0x60) with you as the source and no attacker, while
// the stomp (+0x990) is still on.
struct StompHit { const uint8_t* p; uint8_t meta[0x1c]; };
static StompHit g_stompHits[4];
static void stomp_note_hit(const uint8_t* meta) {
    const uint8_t* inst = *reinterpret_cast<const uint8_t* const*>(meta + 0xc);     // the source: you
    if (!inst || g_stompReplay || !is_player_entity(inst) || !inst[0x990] || *reinterpret_cast<const int*>(inst + 0xb80) <= 0) return;
    StompHit* h = nullptr;
    for (auto& x : g_stompHits) if (x.p == inst) { h = &x; break; }
    if (!h) for (auto& x : g_stompHits) if (!x.p || !is_player_entity(x.p)) { h = &x; break; }
    if (!h) return;
    h->p = inst;
    memcpy(h->meta, meta, 0x1c);
    logmsg("[brutalist] party: your stomp hit for %d (source %p, you %p)", *reinterpret_cast<const int*>(meta),
           *reinterpret_cast<void* const*>(meta + 0xc), inst);
}
static void stomp_shadow_hits(uint8_t* p, const float* body) {
    const StompHit* h = nullptr;
    for (auto& x : g_stompHits) if (x.p == p) { h = &x; break; }
    uint8_t* level = *reinterpret_cast<uint8_t**>(p + OFF_ENT_LEVEL2);
    uint8_t** mons = level ? *reinterpret_cast<uint8_t***>(level + 0xfd0) : nullptr;
    if (!h || !mons) return;
    // The shadow's feet: your box at its spot, from its middle down to a little below its feet.
    const float w = *reinterpret_cast<float*>(p + 0xc0), ht = *reinterpret_cast<float*>(p + 0xc4);
    const float bx = body[0] - 2.0f, by = body[1] + ht * 0.5f, bw = w + 4.0f, bh = ht * 0.5f + 6.0f;
    const int faction = *reinterpret_cast<int*>(p + 0x158);
    uint8_t meta[0x1c];
    memcpy(meta, h->meta, 0x1c);
    int* dmg = reinterpret_cast<int*>(meta);
    if (*dmg > 0) *dmg = (*dmg + 1) / 2;
    uint8_t* hit[16]; int n = 0;
    for (uint8_t* it = mons[0]; it && it < mons[1] && n < 16; it += 8) {
        uint8_t* e = *reinterpret_cast<uint8_t**>(it);
        if (!e || !e[0x160] || !e[0x226] || e[0x148] || *reinterpret_cast<float*>(e + 0x10c) <= 0.0f) continue;
        if (*reinterpret_cast<int*>(e + 0x158) == faction) continue;
        const float ex = *reinterpret_cast<float*>(e + 0xac), ey = *reinterpret_cast<float*>(e + 0xb0);
        const float ew = *reinterpret_cast<float*>(e + 0xc0), eh = *reinterpret_cast<float*>(e + 0xc4);
        if (ex < bx + bw && ex + ew > bx && ey < by + bh && ey + eh > by) hit[n++] = e;
    }
    for (int i = 0; i < n; i++) {        // collected first: a death can change the monster list
        reinterpret_cast<unitHurt_t>(g_base + 0x003e6aa0)(hit[i], nullptr, meta);
        logmsg("[brutalist] party: shadow stomp hit %p for %d", hit[i], *dmg);
    }
}
static void stomp_tick() {
    for (auto& st : g_stomps) {
        if (!st.p || g_cloneFrame < st.due) continue;
        uint8_t* p = st.p;
        st = StompReplay{};
        float body[2];
        if (!is_player_entity(p) || *reinterpret_cast<int*>(p + 0xb80) <= 0 || !clone_body(p, body)) continue;
        const float x = *reinterpret_cast<float*>(p + 0xac), y = *reinterpret_cast<float*>(p + 0xb0);
        const uint8_t stomping = p[0x990], net = p[0xc99];
        void** head = reinterpret_cast<void**>(p + 0x78fc);
        void* const realHead = *head;
        const int realSize = *reinterpret_cast<int*>(p + 0x7900);
        g_emptyHead.l = g_emptyHead.par = g_emptyHead.r = &g_emptyHead;
        g_emptyHead.color = 1; g_emptyHead.isnil = 1;
        *head = &g_emptyHead; *reinterpret_cast<int*>(p + 0x7900) = 0;
        *reinterpret_cast<float*>(p + 0xac) = body[0]; *reinterpret_cast<float*>(p + 0xb0) = body[1];
        p[0x990] = 1;
        ++g_shadowActing; ++g_stompReplay;
        g_stompTramp(p, nullptr, 1);
        stomp_shadow_hits(p, body);
        --g_stompReplay; --g_shadowActing;
        *reinterpret_cast<float*>(p + 0xac) = x; *reinterpret_cast<float*>(p + 0xb0) = y;
        p[0x990] = stomping; p[0xc99] = net;
        *head = realHead; *reinterpret_cast<int*>(p + 0x7900) = realSize;
        logmsg("[brutalist] party: frame %d shadow stomp landed at %.0f,%.0f", g_cloneFrame, body[0], body[1]);
    }
}
// Healing: SPELL_HEAL (the Wand of Healing) heals in SpellEntity::doCollision and SPELL_HEAL_CIRCLE in
// SpellEntity::stepHealCircle, each through the healed unit's virtual heal (vtable +0x6c: Player::heal or
// Unit::heal, thiscall, int amount, ret 4). Which spell is running is noted on the way into those two, and a
// heal called from exactly those two places by one of the shadow's spells is halved.
static const uint8_t* g_curSpell = nullptr;
static void* g_spellCollTramp = nullptr;
static void* g_healCircleTramp = nullptr;
// Bomber level 3: a blast from your bomb skips players (you and allies) altogether. The explosion case of
// doCollision pushes the target away before it hurts it, so the knockback (and the damage of hitting a
// wall from it) is only avoided by skipping the collision. `other` is doCollision's const shared_ptr<Entity>&.
static int __cdecl bomber_blast_skips(const uint8_t* spell, void* const* other) {
    if (!other || !*other) return 0;
    if (!bomb_blast_spares(spell) || !is_player_any(*other)) return 0;
    const void* thrower = bomb_blast_thrower(spell);
    static const void* lastSpell = nullptr; static const void* lastPlayer = nullptr;
    if (lastSpell != spell || lastPlayer != *other) {
        lastSpell = spell; lastPlayer = *other;
        logmsg("[bomber] no friendly fire: %s blast passes player %p (no damage, no knockback)",
               thrower == *other ? "own" : "ally's", *other);
    }
    return 1;
}
__declspec(naked) static void det_spellCollision() {
    __asm {
        mov  dword ptr [g_curSpell], ecx
        pushad
        push dword ptr [esp+0x24] // other (past the 8 saved registers and our return address)
        push ecx                 // this (SpellEntity)
        call bomber_blast_skips
        add  esp, 8
        test eax, eax
        jnz  skip
        popad
        jmp  dword ptr [g_spellCollTramp]
    skip:
        popad
        ret  4                   // doCollision is void (thiscall, one argument)
    }
}
__declspec(naked) static void det_healCircle() {
    __asm {
        mov  dword ptr [g_curSpell], ecx
        jmp  dword ptr [g_healCircleTramp]
    }
}
typedef void (__fastcall *healFn_t)(void* self, void* edx, int amount);
static healFn_t g_playerHealTramp = nullptr, g_unitHealTramp = nullptr;
static int shadow_heal(int amount, uintptr_t ret) {
    const uintptr_t r = ret - g_base;
    if ((r != 0x0039daf8 && r != 0x003ae718) || amount <= 0 || !shadow_ent_is(g_curSpell)) return amount;
    const int half = (amount + 1) / 2;
    logmsg("[brutalist] party: shadow's healing %d (was %d)", half, amount);
    return half;
}
static void __fastcall det_playerHeal(void* self, void* edx, int amount) {
    g_playerHealTramp(self, edx, shadow_heal(amount, reinterpret_cast<uintptr_t>(_ReturnAddress())));
}
static void __fastcall det_unitHeal(void* self, void* edx, int amount) {
    g_unitHealTramp(self, edx, shadow_heal(amount, reinterpret_cast<uintptr_t>(_ReturnAddress())));
}
// The shadow's small bombs, in hand and thrown (the in-hand bomb is drawn yellow only for your current
// weapon, and a thrown one is told apart by the item it carries, which the copy's may not).
static bool clone_small_bomb_weapon(const uint8_t* w) {
    if (!w || !w[0x29c]) return false;
    for (auto& c : g_clones) if (c.player && c.sp[0] == w) return c.smallBomb;
    return false;
}
// The shadow's bombs are real bombs: stock Projectile::shadow (+0x22c) makes a projectile die on its first
// touch (Projectile::doCollision) and skip all of Projectile::kill, explosion included, with a puff instead
// (deathEffect), so a flagged bomb vanished on landing. They're told apart by this list instead (an entry
// is dropped when its bomb explodes), for the small-bomb look and half damage.
struct ShadowBomb { const uint8_t* proj; bool isSmall; };
static ShadowBomb g_shadowBombs[16];
static int g_shadowBombNext = 0;
static ShadowBomb* shadow_bomb_entry(const uint8_t* proj) {
    if (!proj || proj[0x1f8] != 14) return nullptr;                         // PROJECTILE_BOMB
    for (auto& q : g_shadowBombs) if (q.proj == proj) return &q;
    return nullptr;
}
static bool shadow_small_proj(const uint8_t* proj) {
    const ShadowBomb* q = shadow_bomb_entry(proj);
    return q && q->isSmall;
}
// Projectile::kill's bomb case, after the small-bomb damage: the shadow's hits are half damage (stock
// Weapon::doCollision rule), so its blast is too. SpellEntity damage = min + rand % (max - min + 1); halving
// both keeps the regular bomb's 99-194 roll at about 49-95 (the gloves double min right after this).
static void shadow_bomb_explosion(const uint8_t* proj, uint8_t* spell) {
    ShadowBomb* q = shadow_bomb_entry(proj);
    if (!q) return;
    q->proj = nullptr;
    if (!spell) return;
    int* mn = reinterpret_cast<int*>(spell + 0x1fc);
    int* mx = reinterpret_cast<int*>(spell + 0x200);
    const int was = *mn;
    *mn = (*mn + 1) / 2;
    *mx = (*mx + 1) / 2;
    logmsg("[brutalist] party: shadow bomb exploded (%s): damage %d -> %d", q->isSmall ? "small" : "bomb", was, *mn);
}
static float g_ssXmm[4];
__declspec(naked) static void det_playerStartSwing() {
    __asm {
        movss dword ptr [g_ssXmm], xmm0
        movss dword ptr [g_ssXmm+4], xmm1
        movss dword ptr [g_ssXmm+8], xmm2
        movss dword ptr [g_ssXmm+12], xmm3
        pushad
        push dword ptr [g_ssXmm+8]          // f
        push dword ptr [esp+4+32+0xc]       // c
        push dword ptr [esp+8+32+8]         // b
        push dword ptr [esp+0xc+32+4]       // a
        push ecx                            // the weapon
        call clone_record
        add  esp, 0x14
        popad
        movss xmm0, dword ptr [g_ssXmm]
        movss xmm1, dword ptr [g_ssXmm+4]
        movss xmm2, dword ptr [g_ssXmm+8]
        movss xmm3, dword ptr [g_ssXmm+12]
        jmp  dword ptr [g_startSwingVA]
    }
}
static void clone_tick() {
    ++g_cloneFrame;
    stomp_tick();
    for (auto& c : g_clones) {
        if (!c.player) continue;
        uint8_t* p = c.player;
        uint8_t* w = static_cast<uint8_t*>(c.sp[0]);
        if (!is_player_entity(p) || *reinterpret_cast<int*>(p + 0xb80) <= 0) { clone_free(c); continue; }
        if (c.orig) {   // follow your swing while that weapon is still your current one
            if (*reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON) != c.orig) c.orig = nullptr;
            else {
                const int r = g_cloneFrame - c.rec;
                if (r >= 0 && r < 256) { c.h224[r] = c.orig[0x224]; c.h28c[r] = c.orig[0x28c]; c.hn = r + 1; }
                if (!c.capSet && c.orig[0x28c]) {
                    c.capSet = true;
                    c.cap290 = *reinterpret_cast<int*>(c.orig + 0x290);
                    c.cap294 = *reinterpret_cast<int*>(c.orig + 0x294);
                }
                if (c.orig[0x2a6] && c.aim < 0) c.aim = c.orig[0x2a7];
            }
        }
        float pos[2]; uint8_t flip = 0;
        const bool spot = clone_spot(p, pos, &flip);
        if (!c.started) {
            if (g_cloneFrame < c.due) continue;
            void* level = *reinterpret_cast<void**>(p + OFF_ENT_LEVEL2);
            if (!level || !spot || !g_addEntityTramp) { clone_free(c); continue; }
            g_addEntityTramp(level, c.sp);
            w[0x268] = flip;                                                     // Weapon::facingLeft
            reinterpret_cast<weaponNoArg_t>(g_base + RVA_WEAPON_SET_CLONE)(w);
            void* pctrl = *reinterpret_cast<void**>(p + 0xc);                    // the player's weak_this
            if (pctrl) InterlockedIncrement(reinterpret_cast<long*>(static_cast<uint8_t*>(pctrl) + 4));
            reinterpret_cast<weaponSetParent_t>(g_base + RVA_WEAPON_SET_PARENT)(w, p, pctrl);   // into your weapon list
            ++g_shadowActing;                       // what its swing makes (a bow's arrow) is the shadow's
            call_start_swing(w, c.a, c.b, c.c, c.f);
            --g_shadowActing;
            uint8_t* proj = *reinterpret_cast<uint8_t**>(w + 0x258);             // Weapon::currProjectile
            if (proj) shadow_ent_add(proj, *reinterpret_cast<void**>(w + 0x25c));  // its arrow / bomb in hand
            // A wand keeps the charges your wand had when you swung: the copy's own, so yours aren't spent.
            // Weapon::totalDamageFactor is only ever lowered for a sword pogo (Player::step, on your current
            // weapon); the copy took whatever it was mid-step, sometimes 0, which floored its hits at 1.
            *reinterpret_cast<float*>(w + 0x360) = 1.0f;
            c.started = true;
            float body[2] = { 0, 0 };
            clone_body(p, body);
            uint8_t* box = w;   // the weapon's laid-out spot (+0x344/+0x348) vs its body, to show it's on the shadow
            const uint8_t* props = *reinterpret_cast<uint8_t**>(w + 0x31c);
            logmsg("[brutalist] party: shadow swing at %.0f,%.0f, weapon at %.0f,%.0f, you at %.0f,%.0f "
                   "(you swung at %.0f,%.0f, %d frames ago; wand type %d)", body[0], body[1],
                   *reinterpret_cast<float*>(box + 0x344), *reinterpret_cast<float*>(box + 0x348),
                   *reinterpret_cast<float*>(p + 0xac), *reinterpret_cast<float*>(p + 0xb0),
                   c.x, c.y, g_cloneFrame - c.due + CLONE_DELAY, props && ((props[0x5c] >> 2) & 1) ? props[0x45] : -1);
            continue;
        }
        // A wand fires at its effect cue if Weapon::canActivateWand (+0x224) is set. Yours is cleared by your
        // own cast in the frame it fires, so replaying it frame by frame could miss: the shadow's wand keeps it.
        const uint8_t* wprops = *reinterpret_cast<uint8_t**>(w + 0x31c);
        const bool wand = wprops && ((wprops[0x5c] >> 2) & 1);
        if (wand) w[0x224] = 1;
        if (c.age < c.hn) {                       // your button, the same number of frames into the swing
            if (!wand) w[0x224] = c.h224[c.age];
            if (!c.h28c[c.age]) w[0x28c] = 0;
            else if (c.capSet && !c.capApplied) {   // your charge started here
                c.capApplied = true;
                w[0x28c] = 1;
                *reinterpret_cast<int*>(w + 0x290) = c.cap290;
                *reinterpret_cast<int*>(w + 0x294) = c.cap294;
            }
        } else if (!c.orig) w[0x28c] = 0;         // your swing is over: so is any charge
        if (c.stopAt >= 0 && c.age >= c.stopAt && !c.stopped) {   // you let go here: fire / throw
            c.stopped = true;
            uint8_t* proj = *reinterpret_cast<uint8_t**>(w + 0x258);             // Weapon::currProjectile
            if (proj) {                                                          // as Player::stopAttack did
                const uint8_t t = proj[0x1f8];
                if (t == 0 || t == 0x1e) *reinterpret_cast<int*>(proj + 0x208) = *reinterpret_cast<int*>(p + 0x9b4);
                if (t == 14) {                                                   // a bomb: real, listed (see above)
                    proj[0x22c] = 0;
                    g_shadowBombs[g_shadowBombNext] = ShadowBomb{ proj, c.smallBomb };
                    g_shadowBombNext = (g_shadowBombNext + 1) % 16;
                }
            }
            CloneOriginSwap s = clone_origin_begin(w);   // from the shadow's hands
            ++g_shadowActing;
            g_stopSwingTramp(w, nullptr);
            --g_shadowActing;
            clone_origin_end(s);
            if (proj) logmsg("[brutalist] party: frame %d shadow let go (%s, %d frames in)", g_cloneFrame,
                             proj[0x1f8] == 14 ? (c.smallBomb ? "small bomb" : "bomb") : "arrow/shot", c.age);
        }
        if (c.holyAt >= 0 && c.age >= c.holyAt && !c.holyDone) { c.holyDone = true; holy_replay(c, w); }
        // A swing is advanced by its wielder (Player::step does it for your weapon and its old clone list).
        {
            CloneOriginSwap s = clone_origin_begin(w);
            ++g_shadowActing;                       // wand and rod shots fire from its swing's effect cues
            reinterpret_cast<weaponStepSwing_t>(g_base + RVA_WEAPON_STEP_SWING)(w, w[0x268], 0);
            --g_shadowActing;
            clone_origin_end(s);
        }
        if (w[0x269] || w[0x2a6]) c.swung = true;
        if (w[0x28c] != c.prevCharge) {
            c.prevCharge = w[0x28c];
            logmsg("[brutalist] party: frame %d shadow charge %s (%d frames in, keyframe %d, timer %d)", g_cloneFrame,
                   w[0x28c] ? "held" : "let go", c.age, w[0x27c], *reinterpret_cast<int*>(w + 0x294));
        }
        if (w[0x2a6] && !c.thrown) {
            c.thrown = true;
            g_shadowThrows++;
            logmsg("[brutalist] party: frame %d shadow threw (weapon %p, player %p)", g_cloneFrame, w, c.player);
        }
        // Over once the swing has ended and a thrown weapon is back (Weapon::isDaggerThrown +0x2a6).
        if ((c.swung && !w[0x269] && !w[0x2a6]) || ++c.age > 240) {
            if (c.thrown) logmsg("[brutalist] party: shadow throw back after %d frames", c.age);
            clone_free(c);
        }
    }
}
// jarvisMarch(const vector<Vector2f>& in, vector<Vector2f>& out) (fastcall: ecx = in, edx = out) walks the
// hull until it gets back to its first point, compared with ==. A NaN point never compares equal, so it
// looped forever, logging "this probably should never happen" past 100 points, until the game's watchdog
// killed it (a hang, 2026-11-15 dump: a Party shadow's weapon copy laid out with NaN x, inside
// make_shared<Weapon> -> setParent -> resetScriptOrigin). A hull from non-finite points becomes a tiny
// triangle at the finite part of them (or 0), handed in as a separate vector so the caller's points are
// untouched; resetScriptOrigin lays the shape out again next frame.
typedef void (__fastcall *jarvis_t)(const void* in, void* out);
static jarvis_t g_jarvisTramp = nullptr;
static void __fastcall det_jarvisMarch(const void* in, void* out) {
    float* const* v = static_cast<float* const*>(in);
    float* b = v[0]; float* e = v[1];
    bool bad = false; double sx = 0, sy = 0; int nx = 0, ny = 0;
    if (b && e > b + 4)                                              // 3+ points: the march runs
        for (float* q = b; q + 1 < e; q += 2) {
            if (std::isfinite(q[0])) { sx += q[0]; nx++; } else bad = true;
            if (std::isfinite(q[1])) { sy += q[1]; ny++; } else bad = true;
        }
    if (!bad) { g_jarvisTramp(in, out); return; }
    static int s_logged = 0;
    if (s_logged < 20) {
        ++s_logged;
        logmsg("[crashfix] jarvisMarch given %d points with NaN/inf (e.g. %.1f,%.1f) from +0x%x: using a stand-in triangle",
               (int)((e - b) / 2), b[0], b[1], (unsigned)(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base));
    }
    const float cx = nx ? (float)(sx / nx) : 0.f, cy = ny ? (float)(sy / ny) : 0.f;
    float tri[6] = { cx, cy, cx + 1.f, cy, cx, cy + 1.f };
    float* fake[3] = { tri, tri + 6, tri + 6 };                     // std::vector: begin, end, capacity
    g_jarvisTramp(fake, out);
}
// The shadow is drawn from Player::afterImageFrames (+0x7860), which Player::step fills with
// Player::pushShadowCloneSprite (0x2fe040) and trims to afterimages (+0xb80) * 40 frames. A remote player in co-op
// never gets there on this machine (it's moved by Player::interpolate), so it had no shadow here and its shadow's
// swings had nowhere to come from. The game's own pushes are noted; a Party player that didn't get one this frame
// gets it from woodsman_tick instead, the same way.
struct ShadowPushMark { const uint8_t* p; int frame; };
static ShadowPushMark g_shadowPushed[8];
typedef void (__fastcall *playerVoid2_t)(void* self, void* edx);
static void __fastcall det_pushShadowClone(uint8_t* p, void*) {
    ShadowPushMark* m = nullptr;
    for (auto& s : g_shadowPushed) if (s.p == p) { m = &s; break; }
    if (!m) for (auto& s : g_shadowPushed) if (!s.p || !is_player_entity(const_cast<uint8_t*>(s.p))) { m = &s; break; }
    if (m) { m->p = p; m->frame = g_cloneFrame; }
    reinterpret_cast<playerVoid2_t>(g_base + 0x002fe040)(p, nullptr);
}
static bool shadow_pushed_lately(const uint8_t* p) {
    for (auto& s : g_shadowPushed) if (s.p == p) return g_cloneFrame - s.frame <= 2;
    return false;
}
static void shadow_push_remote(uint8_t* p) {
    typedef unsigned (__fastcall *dqSize_t)(void* self, void* edx);
    typedef void** (__fastcall *dqBegin_t)(void* self, void* edx, void** out);
    typedef void* (__fastcall *dqErase_t)(void* self, void* edx, void** out, void* where);
    uint8_t* vec = p + 0x7860;
    reinterpret_cast<playerVoid2_t>(g_base + 0x002fe040)(p, nullptr);           // Player::pushShadowCloneSprite
    const unsigned keep = (unsigned)*reinterpret_cast<int*>(p + 0xb80) * 40;
    for (int guard = 0; guard < 64 && reinterpret_cast<dqSize_t>(g_base + 0x00045930)(vec, nullptr) > keep; guard++) {
        void* it = nullptr; void* out = nullptr;
        reinterpret_cast<dqBegin_t>(g_base + 0x00045950)(vec, nullptr, &it);
        reinterpret_cast<dqErase_t>(g_base + 0x00045960)(vec, nullptr, &out, it);
    }
}
static void install_clone() {
    g_startSwingVA = g_base + RVA_WEAPON_START_SWING;
    bool r = redirect_call(0x002d3ae2, g_startSwingVA, (void*)&det_playerStartSwing);
    // Another player's swing in co-op reaches this game through Player::interpolate's own startSwing call (a = b = 0):
    // record that too, so everyone's shadow copies everyone's swings, not just your own.
    bool ri = redirect_call(0x002f0b78, g_startSwingVA, (void*)&det_playerStartSwing);
    bool rp = redirect_call(0x002e9eb5, g_base + 0x002fe040, (void*)&det_pushShadowClone);   // Player::step's afterimage push
    const uint8_t atkOld[6] = {0x0F, 0x86, 0xC3, 0x01, 0x00, 0x00}, atkNew[6] = {0xE9, 0xC4, 0x01, 0x00, 0x00, 0x90};
    const uint8_t stpOld[6] = {0x0F, 0x86, 0xB1, 0x01, 0x00, 0x00}, stpNew[6] = {0xE9, 0xB2, 0x01, 0x00, 0x00, 0x90};
    const uint8_t recOld[2] = {0x76, 0x07}, recNew[2] = {0x90, 0x90};
    bool a = patch_replace("party: no instant clone swing", 0x002d3857, atkOld, atkNew, 6);   // jbe -> jmp past the loop
    bool b = patch_replace("party: no step clone weapons", 0x002e7bca, stpOld, stpNew, 6);
    bool c = patch_replace("party: snapshot every frame", 0x002e9eb1, recOld, recNew, 2);    // not only when moving
    const uint8_t pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    g_endThrowTramp = reinterpret_cast<endDaggerThrow_t>(install_detour(0x003f9900, (void*)&det_endDaggerThrow, 5, pro, 5));
    g_stopSwingTramp = reinterpret_cast<weaponStopSwing_t>(install_detour(0x003f34c0, (void*)&det_stopSwing, 5, pro, 5));
    g_lastDmgTramp = install_detour(0x00303c50, (void*)&det_lastDmgFrame, 5, pro, 5);
    g_stompTramp = reinterpret_cast<bearStomp_t>(install_detour(0x00301220, (void*)&det_stopBearStomp, 5, pro, 5));
    g_spellCollTramp   = install_detour(0x0039c730, (void*)&det_spellCollision, 5, pro, 5);
    g_healCircleTramp  = install_detour(0x003ae480, (void*)&det_healCircle, 5, pro, 5);
    g_playerHealTramp  = reinterpret_cast<healFn_t>(install_detour(0x002f8e50, (void*)&det_playerHeal, 5, pro, 5));
    g_unitHealTramp    = reinterpret_cast<healFn_t>(install_detour(0x003e7cd0, (void*)&det_unitHeal, 5, pro, 5));
    logmsg("[brutalist] party: holy light hook %s, stomp hook %s, half-healing hooks %d%d%d%d", g_lastDmgTramp ? "installed" : "FAILED",
           g_stompTramp ? "installed" : "FAILED", g_spellCollTramp != nullptr, g_healCircleTramp != nullptr,
           g_playerHealTramp != nullptr, g_unitHealTramp != nullptr);
    logmsg("[woodsman] party clone: startSwing hook %d (net copies %d, remote afterimages %d), attack loop %d, step loop %d, every-frame snapshots %d, catch hook %d, "
           "let-go hook %d", r, ri, rp, a, b, c, g_endThrowTramp != nullptr, g_stopSwingTramp != nullptr);
    g_jarvisTramp = reinterpret_cast<jarvis_t>(install_detour(0x000eb270, (void*)&det_jarvisMarch, 5, pro, 5));
    logmsg("[crashfix] jarvisMarch NaN guard %s", g_jarvisTramp ? "installed" : "FAILED");
}

// A thrown axe that never comes back: its flight (the throw timer, +0x2a8) is advanced and the catch is
// checked only while the game lays the weapon out each frame (Weapon::resetScriptOrigin, for the weapons
// in its wielder's list). One stuck in the air left you holding nothing until something else (winding up
// a bomb) replaced it. A throw home takes about 60 frames; if yours is still out after AXE_STUCK_FRAMES,
// or its timer stops moving for AXE_FROZEN_FRAMES, or the weapon has been killed, it's caught for you the
// way the game catches it (Weapon::endDaggerThrow), with a log line saying which.
static const int AXE_STUCK_FRAMES = 360, AXE_FROZEN_FRAMES = 45;
static struct { const uint8_t* player; const uint8_t* weapon; int frames, lastTimer, still; } g_axeOut[8];
static void axe_unstick(uint8_t* p) {
    uint8_t* w = *reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON);
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; ++i) if (g_axeOut[i].player == p) slot = i;
    if (!w || !w[0x2a6] || w[0x29c] || !is_axe_weapon(w)) { if (slot >= 0) g_axeOut[slot].player = nullptr; return; }
    if (slot < 0) for (int i = 0; i < 8 && slot < 0; ++i) if (!g_axeOut[i].player) slot = i;
    if (slot < 0) return;
    auto& s = g_axeOut[slot];
    const int timer = *reinterpret_cast<int*>(w + 0x2a8);
    if (s.player != p || s.weapon != w) { s.player = p; s.weapon = w; s.frames = 0; s.lastTimer = timer; s.still = 0; }
    ++s.frames;
    if (timer == s.lastTimer) ++s.still; else { s.still = 0; s.lastTimer = timer; }
    const char* why = w[0x148] ? "the weapon was killed" : s.frames > AXE_STUCK_FRAMES ? "out too long"
                    : s.still > AXE_FROZEN_FRAMES ? "its flight stopped" : nullptr;
    if (!why) return;
    logmsg("[axe] stuck throw caught for %p (%s): %d frames out, timer %d (still %d), axe at %.0f,%.0f, you at %.0f,%.0f, "
           "returning %d, dead %d, parent %p", p, why, s.frames, timer, s.still,
           *reinterpret_cast<float*>(w + 0x344), *reinterpret_cast<float*>(w + 0x348),
           *reinterpret_cast<float*>(p + 0xac), *reinterpret_cast<float*>(p + 0xb0), w[0x370], w[0x148],
           *reinterpret_cast<void**>(w + 0x248));
    s.player = nullptr;
    if (g_endThrowTramp) g_endThrowTramp(w, nullptr);
    else { w[0x2a6] = 0; w[0x269] = 0; }
}
static void woodsman_tick() {
    clone_tick();
    if (!g_ge) return;
    uint8_t* g = reinterpret_cast<uint8_t*>(g_ge);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN + 4);
    const DWORD now = GetTickCount();
    for (uint8_t* e = pb; pb && pe && e < pe; e += PLAYER_VEC_STRIDE) {
        uint8_t* p = *reinterpret_cast<uint8_t**>(e);
        if (!p) continue;
        const int brut = tree_level(p, AFFINITY_BRUTALIST);
        if (brut >= 4) *reinterpret_cast<int*>(p + 0xb80) = 1;    // afterimages: one shadow copy
        if (*reinterpret_cast<int*>(p + 0xb80) > 0) {
            if (p[0xa2] && !shadow_pushed_lately(p)) shadow_push_remote(p);   // co-op: another game's player
            clone_trail_push(p);                                  // where the shadow will stand
        }
        if (!p[0xa2]) axe_unstick(p);                             // (your own player; a remote one's owner does it)
        if (brut < 1) continue;
        MomentumState* m = momentum_for(p);
        const uint8_t* w = *reinterpret_cast<uint8_t* const*>(p + OFF_PLAYER_CURRWEAPON);
        const bool swing = w && w[0x269];
        if (m->prevSwing && !swing) {                               // an attack just ended
            if (m->hit) { if (m->stacks < MOMENTUM_MAX) ++m->stacks; m->misses = 0; }
            else if (m->stacks && ++m->misses >= 2) { m->stacks = 0; m->misses = 0; }
            logmsg("[brutalist] momentum %d (%s, misses %d)", m->stacks, m->hit ? "hit" : "miss", m->misses);
            m->hit = false;
        }
        m->prevSwing = swing;
        if (m->stacks && now - m->lastHit > MOMENTUM_FADE_MS) { m->stacks = 0; m->misses = 0; logmsg("[brutalist] momentum faded"); }
        const float want = MOMENTUM_IAS * m->stacks;
        *reinterpret_cast<float*>(p + 0x940) += want - m->applied;
        m->applied = want;
    }
}
static void momentum_hit(uint8_t* player) {
    if (tree_level(player, AFFINITY_BRUTALIST) < 1) return;
    MomentumState* m = momentum_for(player);
    m->hit = true;
    m->lastHit = GetTickCount();
}

// -- Sunder: defense taken off enemies, put back as each stack runs out --
struct SunderStack { float amount; DWORD expires; };
struct SunderUnit { uint8_t* unit; void* level; int applied; int n; SunderStack st[24]; };
static SunderUnit g_sunder[32];
static void* current_level() {
    return g_ge ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_ge) + OFF_GE_LEVEL) : nullptr;
}
static bool unit_in_level(const uint8_t* unit, const uint8_t* level) {
    uint8_t** mons = level ? *reinterpret_cast<uint8_t** const*>(level + 0xfd0) : nullptr;
    for (uint8_t* it = mons ? mons[0] : nullptr; it && it < mons[1]; it += 8)
        if (*reinterpret_cast<uint8_t**>(it) == unit) return true;
    return false;
}
static void sunder_apply(SunderUnit& s) {
    float total = 0.0f;
    for (int i = 0; i < s.n; ++i) total += s.st[i].amount;
    const int want = (int)(total + 0.5f);
    *reinterpret_cast<int*>(s.unit + 0x21c) -= want - s.applied;    // Unit::defense
    s.applied = want;
}
static void sunder_add(uint8_t* unit, float amount) {
    void* level = *reinterpret_cast<void**>(unit + OFF_ENT_LEVEL2);
    SunderUnit* s = nullptr;
    for (auto& e : g_sunder) if (e.unit == unit && e.level == level) { s = &e; break; }
    if (!s) for (auto& e : g_sunder) if (!e.unit) { s = &e; *s = {}; s->unit = unit; s->level = level; break; }
    if (!s) return;                                                 // 32 enemies already sundered
    if (s->n == 24) { memmove(s->st, s->st + 1, sizeof(SunderStack) * 23); s->n = 23; }   // oldest out
    s->st[s->n++] = { amount, GetTickCount() + SUNDER_MS };
    sunder_apply(*s);
    logmsg("[brutalist] sunder %p: %d stacks, defense now %d", unit, s->n, *reinterpret_cast<int*>(unit + 0x21c));
}
// Every frame: drop expired stacks. An entry whose level is gone, or whose enemy left the level (died and
// was removed), is forgotten without touching it.
static void sunder_tick() {
    void* level = current_level();
    const DWORD now = GetTickCount();
    for (auto& s : g_sunder) {
        if (!s.unit) continue;
        if (s.level != level || !unit_in_level(s.unit, static_cast<uint8_t*>(level))) { s.unit = nullptr; continue; }
        int k = 0;
        for (int i = 0; i < s.n; ++i) if ((int)(s.st[i].expires - now) > 0) s.st[k++] = s.st[i];
        if (k == s.n) continue;
        s.n = k;
        sunder_apply(s);
        if (!s.n) s.unit = nullptr;
    }
}

// -- Unit::hurt pieces (called from det_unitHurt) --
// Executioner: a copy of the damage meta with the amount raised 20% (the original may be shared by several
// targets, e.g. one explosion). Returns the meta to use.
static const void* brutalist_pre_hurt(uint8_t* u, const uint8_t* meta, uint8_t* copy) {
    if (!meta || u[0xa2]) return meta;
    const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(meta + 0x14);
    if (!is_player_entity(inst) || tree_level(inst, AFFINITY_BRUTALIST) < 3) return meta;
    const float hp = *reinterpret_cast<float*>(u + 0x10c), maxHp = *reinterpret_cast<float*>(u + 0x110);
    const int dmg = *reinterpret_cast<const int*>(meta);
    if (hp <= 0.0f || maxHp <= 0.0f || hp >= maxHp * 0.3f || dmg <= 0) return meta;
    memcpy(copy, meta, 0x1c);
    int up = (int)(dmg * 1.2f + 0.5f);
    if (up <= dmg) up = dmg + 1;
    *reinterpret_cast<int*>(copy) = up;
    logmsg("[brutalist] executioner %d -> %d (hp %.0f/%.0f)", dmg, up, hp, maxHp);
    return copy;
}
static void brutalist_post_hurt(uint8_t* u, const uint8_t* meta, float hp0) {
    const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(meta + 0x14);
    if (!is_player_entity(inst)) return;
    const int lvl = tree_level(inst, AFFINITY_BRUTALIST);
    if (lvl < 1) return;
    const void* source = *reinterpret_cast<void* const*>(meta + 0xc);
    const void* weapon = *reinterpret_cast<void* const*>(inst + OFF_PLAYER_CURRWEAPON);
    if (source && source == weapon && *reinterpret_cast<const int*>(meta) > 0) momentum_hit(const_cast<uint8_t*>(inst));
    const float dealt = hp0 - *reinterpret_cast<float*>(u + 0x10c);
    const uint8_t* src = static_cast<const uint8_t*>(source);
    if (src && src[0x50] == 90 && src[0x29c])                      // Entity::type WEAPON, Weapon::shadow
        logmsg("[brutalist] party: shadow hit %p for %.0f", u, dealt);
    if (lvl < 2 || u[0xa2]) return;
    if (dealt > 0.0f && *reinterpret_cast<float*>(u + 0x10c) > 0.0f) sunder_add(u, dealt * 0.1f);
}

// -- Scavenger --
typedef void (__fastcall *playerHeal_t)(void* self, void* edx, int amount);
static void __cdecl scavenger_bonfire_rest(uint8_t* p) {
    if (!p || p[0xa2] || tree_level(p, AFFINITY_SCAVENGER) < 1) return;
    void** vt = *reinterpret_cast<void***>(p);
    reinterpret_cast<playerHeal_t>(vt[0x6c / 4])(p, nullptr, SCAVENGER_BONFIRE_HEAL);   // Player::heal
    logmsg("[scavenger] bonfire rest: +%d healing", SCAVENGER_BONFIRE_HEAL);
}
static void* g_bonfireRestTramp = nullptr;
__declspec(naked) static void det_bonfireRest() {   // at `mov ecx,esi; mov byte [esi+0xc94],1` (esi = player)
    __asm {
        pushad
        push esi
        call scavenger_bonfire_rest
        add  esp, 4
        popad
        jmp  dword ptr [g_bonfireRestTramp]
    }
}
typedef bool (__fastcall *haveKey_t)(void* self, void* edx);
typedef void (__fastcall *removeKey_t)(void* self, void* edx);
static haveKey_t g_haveKeyTramp = nullptr;
static removeKey_t g_removeKeyTramp = nullptr;
static bool __fastcall det_haveBossKey(uint8_t* p, void* edx) {
    if (tree_level(p, AFFINITY_SCAVENGER) >= 2) return true;
    return g_haveKeyTramp(p, edx);
}
static void __fastcall det_removeBossKey(uint8_t* p, void* edx) {
    if (tree_level(p, AFFINITY_SCAVENGER) >= 2) { logmsg("[scavenger] locked door opened, key kept"); return; }
    g_removeKeyTramp(p, edx);
}
// Shrine::useItem(shared_ptr<Item>) -- thiscall, the shared_ptr by value (callee releases it), ret 8;
// a result < 0 is a rejection. The item may be gone afterwards, so it is judged before the call.
static void* g_useItemTramp = nullptr;
static uintptr_t g_useItemRet = 0;
static uint8_t* g_recycleShrine = nullptr;
static void __cdecl recycle_pre(uint8_t* shrine, const uint8_t* item) {
    g_recycleShrine = nullptr;
    if (!shrine || !item || shrine[0xa2]) return;
    const int god = *reinterpret_cast<int*>(shrine + 0x318);       // Shrine::godType
    if (god != 0 && god != 3) return;                              // GOD_CHAOS (The Deranged), GOD_EQUIP (anvil)
    const int type = *reinterpret_cast<const int*>(item + 0x20c);  // Item::itemType
    const int slot = *reinterpret_cast<const int*>(item + 0x204);  // Item::equipSlot
    const bool weapon = type > 43 && type < 98;                    // ITEMTYPE_WEAPON_START .. _END
    const bool armor = slot == 0 || slot == 2 || slot == 3 || slot == 6;   // head, chest, hands, feet
    if (weapon || armor) g_recycleShrine = shrine;
}
static void __cdecl recycle_post(int result) {
    uint8_t* shrine = g_recycleShrine;
    g_recycleShrine = nullptr;
    if (!shrine || result < 0 || !g_ge) return;
    const float sx = *reinterpret_cast<float*>(shrine + 0xac) + *reinterpret_cast<float*>(shrine + 0xc0) * 0.5f;
    const float sy = *reinterpret_cast<float*>(shrine + 0xb0) + *reinterpret_cast<float*>(shrine + 0xc4) * 0.5f;
    uint8_t* g = reinterpret_cast<uint8_t*>(g_ge);
    uint8_t* pb = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN);
    uint8_t* pe = *reinterpret_cast<uint8_t**>(g + OFF_GE_PLAYERS_BEGIN + 4);
    uint8_t* best = nullptr; float bestD = 96.0f * 96.0f;
    for (uint8_t* e = pb; pb && pe && e < pe; e += PLAYER_VEC_STRIDE) {
        uint8_t* p = *reinterpret_cast<uint8_t**>(e);
        if (!p || tree_level(p, AFFINITY_SCAVENGER) < 3) continue;
        const float dx = *reinterpret_cast<float*>(p + 0xac) - sx, dy = *reinterpret_cast<float*>(p + 0xb0) - sy;
        if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = p; }
    }
    if (!best) return;
    // A loose gold pickup (ITEMTYPE_GOLD 0; Item::count is its value), dropped where the item went in.
    void* level = *reinterpret_cast<void**>(shrine + OFF_ENT_LEVEL2);
    if (!level || !g_makeItemVA || !g_addEntityTramp) return;
    int type = 0, count = RECYCLE_GOLD_MIN + rand() % (RECYCLE_GOLD_MAX - RECYCLE_GOLD_MIN + 1);
    float x = sx, y = sy;
    void* sp[2] = {nullptr, nullptr};
    make_item_shared(&sp, &level, &x, &y, &type, &count);
    if (!sp[0]) return;
    g_addEntityTramp(level, &sp);
    release_shared_block(sp[1]);
    logmsg("[scavenger] recycled at shrine (god %d, result %d): dropped %d gold", *reinterpret_cast<int*>(shrine + 0x318), result, count);
}
__declspec(naked) static void det_shrineUseItem() {
    __asm {
        push ecx
        push dword ptr [esp+8]          // the Item (shared_ptr's raw pointer)
        push ecx
        call recycle_pre
        add  esp, 8
        pop  ecx
        pop  dword ptr [g_useItemRet]   // our caller's return address; the shared_ptr is now on top
        call dword ptr [g_useItemTramp] // Shrine::useItem pops the shared_ptr (ret 8)
        push eax
        push eax
        call recycle_post
        add  esp, 4
        pop  eax
        jmp  dword ptr [g_useItemRet]
    }
}

// -- Random characters: the Woodsman's trees in the roll pool --
// Re-rolls the list randomizeAffinities just made (same length) from the stock pool plus 26..29.
static uint32_t g_woodsRollSeed = 0;
static void reroll_with_woodsman_trees(void* menu) {
    if (!g_randWoodsmanTrees || !menu) return;
    int** vec = reinterpret_cast<int**>(reinterpret_cast<uint8_t*>(menu) + OFF_MENU_ROLLED_AFFS);
    int* b = vec[0]; int* e = vec[1];
    if (!b || e <= b || (e == b + 1 && *b == AFFINITY_NO_TREES)) return;
    int pool[32]; int n = 0;
    static const int kStock[16] = { 0, 1, 2, 3, 5, 11, 12, 13, 14, 15, 16, 18, 19, 20, 21, 25 };
    for (int t : kStock) pool[n++] = t;
    if (!g_woodsRollSeed) g_woodsRollSeed = GetTickCount() ^ 0x2545f491u;
    auto next = [](uint32_t& s) { s = s * 1103515245u + 12345u; return (s >> 16) & 0x7fff; };
    if (next(g_woodsRollSeed) % 6 == 0) { pool[n++] = 22; pool[n++] = 23; pool[n++] = 24; }   // as stock: 1 in 6
    for (int t = AFFINITY_AXE; t <= AFFINITY_SCAVENGER; ++t) pool[n++] = t;
    for (int i = n - 1; i > 0; --i) { int j = next(g_woodsRollSeed) % (i + 1); int t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
    const int take = (int)(e - b) < n ? (int)(e - b) : n;
    for (int i = 0; i < take; ++i) b[i] = pool[i];
    logmsg("[random] rolled with Woodsman trees: %d %d %d %d", take > 0 ? b[0] : -1, take > 1 ? b[1] : -1, take > 2 ? b[2] : -1, take > 3 ? b[3] : -1);
}

static void install_woods_trees() {
    void** club = reinterpret_cast<void**>(g_base + RVA_CLUB_VTABLE);
    for (int i = 0; i < 5; i++) g_brutalistVtbl[i] = g_scavengerVtbl[i] = club[i - 1];
    g_brutalistVtbl[2] = g_scavengerVtbl[2] = reinterpret_cast<void*>(&woods_initSkillStrings);
    g_brutalistVtbl[3] = g_scavengerVtbl[3] = reinterpret_cast<void*>(&axe_levelUp);
    const uint8_t rest[9]   = {0x8B, 0xCE, 0xC6, 0x86, 0x94, 0x0C, 0x00, 0x00, 0x01};   // mov ecx,esi; mov byte [esi+0xc94],1
    const uint8_t haveK[8]  = {0x6A, 0x01, 0x81, 0xC1, 0xA0, 0x04, 0x00, 0x00};         // push 1; add ecx,0x4a0
    const uint8_t remK[8]   = {0x53, 0x56, 0x8D, 0x99, 0xA0, 0x04, 0x00, 0x00};         // push ebx; push esi; lea ebx,[ecx+0x4a0]
    const uint8_t pro[5]    = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    g_bonfireRestTramp = install_detour(0x00100025, (void*)&det_bonfireRest, 9, rest, 9);
    g_haveKeyTramp   = reinterpret_cast<haveKey_t>(install_detour(0x003006e0, (void*)&det_haveBossKey, 8, haveK, 8));
    g_removeKeyTramp = reinterpret_cast<removeKey_t>(install_detour(0x003006f0, (void*)&det_removeBossKey, 8, remK, 8));
    g_useItemTramp   = install_detour(0x00380570, (void*)&det_shrineUseItem, 5, pro, 5);
    // Party: the game's afterimage attack builds its shadow weapon as a copy of yours (Weapon's copy ctor),
    // and the copy ctor calls Weapon::setParent before any shared_ptr owns the new weapon. setParent locks
    // the weapon's own weak_ptr to add it to the parent's weapon list, which throws bad_weak_ptr there and
    // ends the game (the prototype was switched off). When the lock fails we skip the list and still set
    // the parent: `je <throw>` -> `je <set parent>`. A weapon that is owned (every normal one) locks fine.
    const uint8_t spOld[6] = {0x0F, 0x84, 0x70, 0x02, 0x00, 0x00};   // je 0x7f3d24 (_Throw_bad_weak_ptr)
    const uint8_t spNew[6] = {0x0F, 0x84, 0xDA, 0x01, 0x00, 0x00};   // je 0x7f3c8e (mov ecx,this; set parent)
    bool sp = patch_replace("party: clone weapon parent", 0x003f3aae, spOld, spNew, 6);
    logmsg("[woodsman] afterimage weapon fix %s", sp ? "applied" : "FAILED");
    logmsg("[woodsman] brutalist/scavenger hooks: bonfire %d key %d/%d shrine %d", !!g_bonfireRestTramp,
           !!g_haveKeyTramp, !!g_removeKeyTramp, !!g_useItemTramp);
}

// Player::attack, level 1 (the stun bonus). Stock: `if (isClub) { if (clubBonusStun) <apply> } else <next
// weapon check>`. We route the same three ways, adding "axe with Axe tree >= 1" to <apply>.
static uintptr_t g_axeStunApply = 0, g_axeStunSkip = 0, g_axeStunNotClub = 0, g_axeStunJmp = 0;
static uintptr_t __cdecl axe_stun_route(const uint8_t* player, const uint8_t* props) {
    if (props[0x5c] & 0x80) return player[0x9e8] ? g_axeStunApply : g_axeStunSkip;   // club, clubBonusStun
    if (is_axe_weapon(*reinterpret_cast<uint8_t* const*>(player + OFF_PLAYER_CURRWEAPON), props) &&
        axe_tree_level(player) >= 1) return g_axeStunApply;
    return g_axeStunNotClub;
}
__declspec(naked) static void det_axeStun() {   // at Player::attack 0x2d3388: edx = props, edi = player
    __asm {
        pushad
        push edx
        push edi
        call axe_stun_route
        add  esp, 8
        mov  g_axeStunJmp, eax
        popad
        jmp  dword ptr [g_axeStunJmp]
    }
}
// Player::attack, levels 2-4. Replaces the whole stock club block (charge + fast charge) with the same
// logic plus the axe version. Level 2 sets Weapon::doDaggerThrow from "up held" exactly as the Dagger
// tree does. A throw charges like a swing (the charge keyframe comes before the release keyframe).
static uintptr_t g_axeChargeResume = 0;
static void __cdecl axe_charge_apply(uint8_t* player, const uint8_t* props, uint8_t* weapon) {
    if (props[0x5c] & 0x80) {                                      // club: stock behaviour, unchanged
        if (player[0x9e9]) {                                       // clubCanCharge
            weapon[0x28c] = 1;                                     // chargingAttack
            if (player[0x9eb]) {                                   // clubFastCharge
                uint8_t* cw = *reinterpret_cast<uint8_t**>(player + OFF_PLAYER_CURRWEAPON);
                *reinterpret_cast<int*>(cw + 0x294) = 0x28;        // chargeAttackTimer head start
            }
        }
        return;
    }
    if (!is_axe_weapon(weapon, props)) return;
    const bool baton = !is_axe_props(props);   // its club swing already has a charge keyframe; keep the stock one
    int lvl = axe_tree_level(player);
    if (lvl <= 0) return;
    bool throwing = false;
    if (lvl >= 2) { weapon[0x2a5] = player[0x49c]; throwing = player[0x49c] != 0; }   // doDaggerThrow = up held
    if (lvl >= 3) {                                                  // throws charge too: release to throw
        weapon[0x28c] = 1;                                          // chargingAttack
        // Where the swing holds while charging (chargeAttackFrame). A throw holds with the axe raised over
        // the head, a swing with it drawn back, so you can tell which one you're charging.
        if (!baton) *reinterpret_cast<int*>(weapon + 0x290) = throwing ? AXE_THROW_CHARGE_KEYFRAME : AXE_CHARGE_KEYFRAME;
        if (lvl >= 4) *reinterpret_cast<int*>(weapon + 0x294) = 0x28;
    }
}
__declspec(naked) static void det_axeCharge() {   // at Player::attack 0x2d3612: ecx = props, edx = weapon, edi = player
    __asm {
        pushad
        push edx
        push ecx
        push edi
        call axe_charge_apply
        add  esp, 12
        popad
        jmp  dword ptr [g_axeChargeResume]
    }
}
// Player::cancelAttack (stock: pressing swap, cycle-ranged or cycle-skill mid-swing cancels it, and so do a
// few Player::step cases). It only clears Weapon+0x269 (swinging), not chargingAttack +0x28c, so a charged
// swing stayed frozen on its charge frame and letting go did nothing (Player::stopAttack needs +0x269).
// While a charge is held, a button press no longer cancels it: you keep charging and letting go swings.
// Any other cancel also lets go of the charge, so the swing finishes instead of freezing.
typedef void (__fastcall *cancelAttack_t)(void* self, void* edx);
static cancelAttack_t g_cancelAttackTramp = nullptr;
static void __fastcall det_cancelAttack(void* self, void* edx) {
    uint8_t* p = static_cast<uint8_t*>(self);
    uint8_t* w = *reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON);
    if (w && w[0x269] && w[0x28c]) {
        uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
        if (ret == 0x002d5468 || ret == 0x002d589d || ret == 0x002d5cc1) {   // cycleWeapon / cycleRange / cycleSkill
            logmsg("[axe] button press kept the charge (held %d frames)", *reinterpret_cast<int*>(w + 0x294));
            return;
        }
        w[0x28c] = 0;
        logmsg("[axe] charge let go by a cancel (from +0x%x)", static_cast<unsigned>(ret));
    }
    g_cancelAttackTramp(self, edx);
}
// GameEngine::handlePlayerInput(Input*, playerIndex, controllerIndex). While swap (action 8) is held, the
// melee/ranged/skill buttons cycle your items instead, and stock skips their release checks altogether, so
// letting go of a held charge while swap was down never reached Player::stopMeleeAttack / stopAttack: the
// swing froze until you pressed attack again. Run the skipped release ourselves.
typedef void (__fastcall *handlePlayerInput_t)(void* self, void* edx, void* in, int playerIdx, int ctrl);
typedef bool (__fastcall *inputAct_t)(void* self, void* edx, int action, unsigned ctrl);
typedef void (__fastcall *playerVoid_t)(void* self, void* edx);
static handlePlayerInput_t g_handleInputTramp = nullptr;
static void __fastcall det_handlePlayerInput(void* self, void* edx, void* in, int playerIdx, int ctrl) {
    g_handleInputTramp(self, edx, in, playerIdx, ctrl);
    if (!self || !in || playerIdx < 0) return;
    uint8_t** pb = *reinterpret_cast<uint8_t***>(static_cast<uint8_t*>(self) + 0x874);
    uint8_t** pe = *reinterpret_cast<uint8_t***>(static_cast<uint8_t*>(self) + 0x878);
    if (!pb || pb + playerIdx * 2 >= pe) return;
    uint8_t* p = pb[playerIdx * 2];                                  // vector<shared_ptr<Player>>
    if (!p) return;
    auto isHeld     = reinterpret_cast<inputAct_t>(g_base + 0x001fb3f0);
    auto isReleased = reinterpret_cast<inputAct_t>(g_base + 0x001fb490);
    if (!isHeld(in, nullptr, 8, ctrl)) return;                       // swap not held: stock handled it
    uint8_t* w = *reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON);
    if (p[0x78f8] && isReleased(in, nullptr, 5, ctrl)) {             // melee was held (meleeAttack) and let go
        logmsg("[axe] melee let go while swap held: releasing (charge %d)", w ? w[0x28c] : -1);
        reinterpret_cast<playerVoid_t>(g_base + 0x002d4b60)(p, nullptr);   // Player::stopMeleeAttack
    }
    if (w && w[0x269] && (isReleased(in, nullptr, 6, ctrl) || isReleased(in, nullptr, 7, ctrl))) {
        logmsg("[axe] ranged/skill let go while swap held: releasing");
        reinterpret_cast<playerVoid_t>(g_base + 0x002d3d20)(p, nullptr);   // Player::stopAttack
    }
}
// In-place replacements for `mov al,[reg+0x5c]; shr al,N; and al,1` type checks: al = the answer, ZF
// set from it (the stock je follows), every other register (and eax's upper bytes) kept.
// The weapon comes along so the Spearheaded Baton (see is_baton) is recognised too.
static int __cdecl club_or_axe(const uint8_t* props, const uint8_t* weapon) {
    return (props[0x5c] & 0x80) || is_axe_weapon(weapon, props);
}
static int __cdecl dagger_or_axe(const uint8_t* props, const uint8_t* weapon) {
    return (props[0x5c] & 0x02) || is_axe_weapon(weapon, props);
}
// Network copy of a swing (Player::interpolate): an axe counts as a dagger only when the swing is a throw,
// so a normal axe swing still reaches the checks after the dagger branch.
static int __cdecl dagger_or_thrown_axe(const uint8_t* props, const uint8_t* net, const uint8_t* weapon) {
    return (props[0x5c] & 0x02) || (net && net[7] && is_axe_weapon(weapon, props));
}
// Each stub: props in eax (ecx for the last), the weapon in the register named.
#define AXE_CHECK_STUB(name, fn, weaponReg)          \
__declspec(naked) static void name() {               \
    __asm { push ecx }                               \
    __asm { push edx }                               \
    __asm { push eax }                               \
    __asm { push weaponReg }                         \
    __asm { push eax }                               \
    __asm { call fn }                                \
    __asm { add  esp, 8 }                            \
    __asm { mov  ecx, eax }                          \
    __asm { pop  eax }                               \
    __asm { mov  al, cl }                            \
    __asm { pop  edx }                               \
    __asm { pop  ecx }                               \
    __asm { and  al, 1 }                             \
    __asm { ret }                                    \
}
AXE_CHECK_STUB(stub_clubOrAxe_eax,       club_or_axe,   esi)   // Weapon::stepSwing: the "fully charged" ding (esi = weapon)
AXE_CHECK_STUB(stub_daggerOrAxe_eax,     dagger_or_axe, esi)   // Weapon::stepSwing throw (esi = weapon)
AXE_CHECK_STUB(stub_daggerOrAxe_eax_ecx, dagger_or_axe, ecx)   // Player::attack throw damage (ecx = currWeapon)
// Player::getEntityState (what the server sends about a player): `mov cl,[edx+0x5c]; shr cl,1; and cl,1`, the
// dagger test in front of "this swing is a throw" (state bit 0x1000000 from Weapon::doDaggerThrow +0x2a5). An axe
// failed it, so another game saw your axe throw as a plain swing. Props in edx, weapon in esi; cl and ZF out,
// every other register kept (eax holds the state bits being built).
__declspec(naked) static void stub_daggerOrAxe_cl_state() {
    __asm {
        push eax
        push ecx
        push edx
        push esi                // the weapon
        push edx                // its props
        call dagger_or_axe
        add  esp, 8
        pop  edx
        pop  ecx
        mov  cl, al
        pop  eax
        and  cl, 1
        ret
    }
}
#undef AXE_CHECK_STUB
__declspec(naked) static void stub_daggerOrThrownAxe_ecx() {   // Player::interpolate: props in ecx, weapon in edx, net state at [ebp-0x14]
    __asm {
        push ecx
        push edx
        push eax
        push edx
        push dword ptr [ebp-0x14]
        push ecx
        call dagger_or_thrown_axe
        add  esp, 12
        mov  edx, eax
        pop  eax
        mov  al, dl
        pop  edx
        pop  ecx
        and  al, 1
        ret
    }
}
// ---- Axe throw aim (up / straight / down) ----
// When the axe leaves the hand (Weapon::stepSwing sets isDaggerThrown), record the aim from what the
// thrower holds: up = 1, down = 2, neither = 0. It goes in Weapon+0x2a7, padding between isDaggerThrown
// (+0x2a6) and daggerThrowTimer (+0x2a8). The outbound flight (Weapon::resetScriptOrigin) sets the
// velocity to (+-4, 0) every frame for 15 frames; for an aimed axe we tilt that 45 degrees up or down.
// The return trip homes back to the thrower as before. Daggers always get aim 0 (stock flight).
// Bit 0x80 of the byte marks an axe: a fully charged axe throw flies at 1.5x speed (about 50% farther);
// anything short of full is 1x. The charge is recorded at the throw (bits 2-6: 31 full, 0 not), because
// the game zeroes the timer when the swing animation ends, a few frames into the flight.
static const size_t OFF_WEAPON_PARENT = 0x248;   // Weapon::parent (the wielder)
static const size_t OFF_WEAPON_AIM    = 0x2a7;   // our byte: bits 0-1 aim (0 straight, 1 up, 2 down, 3 straight up),
                                                 // bits 2-6 charge 0..31, 0x80 = axe
// The flying axe keeps whatever swing keyframe (+0x27c) is current, and the heavy swing can still step on
// past the release pose after it leaves the hand. Its later keyframes (11) hold the axe low, so depending
// on timing it flew at ankle height (measured: box centred 5.5 below the thrower's centre, under the floor
// line) instead of chest height (release keyframe 9). So the keyframe at release is remembered here per
// weapon and held for the whole flight (axe_aim_tilt, every Weapon::step).
struct AxeFlightPose { const uint8_t* weapon; uint8_t keyframe; };
static AxeFlightPose g_axeFlightPose[16] = {};
static void axe_remember_pose(const uint8_t* weapon, uint8_t kf) {
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (g_axeFlightPose[i].weapon == weapon) { slot = i; break; }
        if (!g_axeFlightPose[i].weapon) slot = i;
    }
    g_axeFlightPose[slot] = { weapon, kf };
}
static int axe_flight_pose(const uint8_t* weapon) {
    for (int i = 0; i < 16; i++) if (g_axeFlightPose[i].weapon == weapon) return g_axeFlightPose[i].keyframe;
    return -1;
}
// Weapon::resetScriptOrigin lays out the weapon's box from the current keyframe's shapes, after the swing may
// have stepped on this frame, so the pose is pinned here too (else the hitbox flies low while the sprite
// doesn't).
typedef void (__fastcall *resetScriptOrigin_t)(void* self, void* edx);
static resetScriptOrigin_t g_resetOriginTramp = nullptr;
static void __fastcall det_resetScriptOrigin(void* self, void* edx) {
    uint8_t* w = static_cast<uint8_t*>(self);
    if (w[0x2a6]) {
        const int kf = axe_flight_pose(w);
        if (kf >= 0) w[0x27c] = (uint8_t)kf;
    } else {
        for (int i = 0; i < 16; i++)                         // back in hand: forget it, so a later weapon
            if (g_axeFlightPose[i].weapon == w) g_axeFlightPose[i].weapon = nullptr;   // at this address isn't pinned
    }
    CloneOriginSwap s = clone_origin_begin(w);   // a Party shadow weapon is laid out from the shadow's body
    ++g_inResetOrigin;
    g_resetOriginTramp(self, edx);
    --g_inResetOrigin;
    clone_origin_end(s);
}
// The throw direction your held keys give right now: holdingUp alone = straight up (3); up with left/right =
// 45 degrees up (1); holdingDown = 45 down (2); else forward (0).
static uint8_t axe_aim_from_input(const uint8_t* p) {
    return p[0x49c] ? ((p[0x49d] || p[0x49e]) ? 1 : 3) : (p[0x49b] ? 2 : 0);
}
static void __cdecl axe_latch_aim(uint8_t* weapon) {
    uint8_t aim = 0;
    if (CloneSwing* c = clone_of(weapon)) {     // a Party shadow throws the way you did, not the way you aim now
        if (c->aim >= 0) {
            weapon[OFF_WEAPON_AIM] = (uint8_t)c->aim;
            if (is_axe_weapon(weapon)) axe_remember_pose(weapon, weapon[0x27c]);
            return;
        }
    }
    if (is_axe_weapon(weapon)) {
        const uint8_t* p = *reinterpret_cast<uint8_t**>(weapon + OFF_WEAPON_PARENT);
        // holdingUp alone = straight up (3); up with left/right = 45 degrees up (1); holdingDown = 45 down (2)
        if (p) aim = axe_aim_from_input(p);
        // Only a full charge gets the range bonus; anything less throws at the normal distance.
        int t = *reinterpret_cast<int*>(weapon + 0x294), mx = *reinterpret_cast<int*>(weapon + 0x2ec);
        int q = (mx > 0 && t >= mx) ? 31 : 0;
        aim |= (uint8_t)(0x80 | (q << 2));
        axe_remember_pose(weapon, weapon[0x27c]);
    }
    weapon[OFF_WEAPON_AIM] = aim;
}
// Aim arrow while an axe throw is wound up. Player::draw shows the aim cursor (the sprite at Player+0x7060,
// aim_cursor.png) only for a thrown item with a projectile in hand (a bomb): origin (3,16), at the projectile
// offset by (+-0x718c, -0x7190), rotated by facing * the throw angle, flipped with you, drawn with the
// sprite's own texture (+0xfc) and quad (+0xac). Same sprite, same steps, for the axe: from your middle,
// pointing the way the throw will go if you let go now.
typedef void (__fastcall *playerDraw_t)(void* self, void* edx, void* target);
static playerDraw_t g_playerDrawTramp = nullptr;
typedef void (__thiscall *tf2f_t)(void* self, float a, float b);
typedef void (__thiscall *tf1f_t)(void* self, float a);
typedef const void* (__thiscall *tfGet_t)(const void* self);
typedef void (__thiscall *spriteFlip_t)(void* self, bool f);
typedef void (__thiscall *spriteNoArg_t)(void* self);
typedef void (__thiscall *rtDrawVerts_t)(void* target, const void* verts, size_t n, int type, const void* states);
typedef void (__cdecl *tfMul_t)(void* t, const void* other);
static bool axe_throw_wound_up(const uint8_t* p, const uint8_t* w) {
    return w && !w[0x29c] && is_axe_weapon(w) && w[0x269] && w[0x2a5] && !w[0x2a6] && w[0x28c];
}
static void __fastcall det_playerDraw(void* self, void* edx, void* target) {
    g_playerDrawTramp(self, edx, target);
    uint8_t* p = static_cast<uint8_t*>(self);
    if (!target || p[0x78c0] || p[0x274]) return;                       // not drawn (as stock skips)
    uint8_t* w = *reinterpret_cast<uint8_t**>(p + OFF_PLAYER_CURRWEAPON);
    if (!axe_throw_wound_up(p, w)) return;
    uint8_t* spr = p + 0x7060;
    static const float kDeg[4] = { 0.f, 45.f, -45.f, 90.f };             // forward, 45 up, 45 down, straight up
    const bool left = p[0x225] != 0;
    const float ang = (left ? 1.f : -1.f) * kDeg[axe_aim_from_input(p) & 3];
    const float off = *reinterpret_cast<float*>(p + 0x718c);
    float x = *reinterpret_cast<float*>(p + 0xac) + *reinterpret_cast<float*>(p + 0xc0) * 0.5f;
    float y = *reinterpret_cast<float*>(p + 0xb0) + *reinterpret_cast<float*>(p + 0xc4) * 0.5f - *reinterpret_cast<float*>(p + 0x7190);
    reinterpret_cast<tf2f_t>(g_base + 0x00412b10)(spr, 3.f, 16.f);       // setOrigin
    reinterpret_cast<spriteFlip_t>(g_base + 0x003b9ac0)(spr, left);      // Sprite::setFlipped
    x += spr[0xa8] ? off : -off;                                         // as stock, from the flip state
    reinterpret_cast<tf2f_t>(g_base + 0x00412b70)(spr, x, y);            // setPosition
    reinterpret_cast<tf1f_t>(g_base + 0x00412ba0)(spr, ang);             // setRotation
    reinterpret_cast<spriteNoArg_t>(g_base + 0x003b9f00)(spr);           // Sprite::lazyLoadTexture
    void* tex = *reinterpret_cast<void**>(spr + 0xfc);
    if (!tex) return;
    alignas(16) uint8_t states[0x60];
    memcpy(states, reinterpret_cast<const void*>(g_base + 0x0078d648), sizeof(states));   // RenderStates::Default
    *reinterpret_cast<void**>(states + 0x58) = tex;
    reinterpret_cast<tfMul_t>(g_base + 0x00411c30)(states + 0x18,
        reinterpret_cast<tfGet_t>(g_base + 0x00412960)(spr));            // states.transform *= getTransform()
    reinterpret_cast<rtDrawVerts_t>(g_base + 0x00413d80)(target, spr + 0xac, 4, 4, states);
}
static uintptr_t g_throwStartResume = 0, g_throwFlightResume = 0;
__declspec(naked) static void det_throwStart() {   // replaces `mov byte [esi+0x2a6],1`; a je follows, so keep flags
    __asm {
        pushfd
        pushad
        push esi
        call axe_latch_aim
        add  esp, 4
        popad
        popfd
        mov  byte ptr [esi+0x2a6], 1
        jmp  dword ptr [g_throwStartResume]
    }
}
static const float kAimCos = 0.70710678f;   // 45 degrees
static const float kAimUpX  = 0.001f;        // straight up: keep a sliver of vx so the facing (its sign) holds
static const float kOne = 1.0f;
// Replaces the outbound `vx = xmm0 (+-4); vy = 0` (sets [edi+0xb4]/[edi+0xb8] and xmm5/xmm2 for the code
// after it). eax, xmm1 and xmm6 are free here (the code after reloads them before reading).
static const float kChargeStep = 0.5f / 31.0f;   // charge 31 (full) = +50%, 0 = none
__declspec(naked) static void det_throwFlight() {
    __asm {
        movzx eax, byte ptr [edi+0x2a7]
        test  eax, 0x80                          // axe? scale speed by the recorded charge
        jz    aim
        shr   eax, 2
        and   eax, 31
        cvtsi2ss xmm1, eax
        mulss xmm1, dword ptr [kChargeStep]
        addss xmm1, dword ptr [kOne]
        mulss xmm0, xmm1                         // vx *= 1 + 0.5 * charge
        movzx eax, byte ptr [edi+0x2a7]
        and   eax, 3
    aim:
        test  eax, eax
        jz    straight
        cmp   eax, 3
        jne   diag
        movaps xmm1, xmm0                        // straight up: vy = -|vx|, vx ~ 0 (same sign)
        mulss xmm1, xmm1
        sqrtss xmm2, xmm1
        xorps xmm1, xmm1
        subss xmm1, xmm2
        movaps xmm2, xmm1
        mulss xmm0, dword ptr [kAimUpX]
        jmp   setv
    diag:
        mulss xmm0, dword ptr [kAimCos]          // 45 degrees: vx *= cos, |vy| = |vx|
        movaps xmm2, xmm0
        movaps xmm1, xmm0
        mulss xmm1, xmm1
        sqrtss xmm2, xmm1                        // xmm2 = |vx| (down: +y)
        cmp   eax, 1
        jne   setv
        xorps xmm1, xmm1
        subss xmm1, xmm2
        movaps xmm2, xmm1                        // up: -y
    setv:
        movss dword ptr [edi+0xb4], xmm0
        movaps xmm5, xmm0
        movss dword ptr [edi+0xb8], xmm2
        jmp   dword ptr [g_throwFlightResume]
    straight:
        movss dword ptr [edi+0xb4], xmm0
        movaps xmm5, xmm0
        mov   dword ptr [edi+0xb8], 0
        xorps xmm2, xmm2
        jmp   dword ptr [g_throwFlightResume]
    }
}
// ---- Axe return speed ----
// A thrown dagger/axe flies out at a constant speed for 15 frames (that alone sets how far it goes), stops,
// and then flies home at (timer-15) * 0.1 a frame toward the owner (Weapon::resetScriptOrigin), capped at
// 4.0. An axe (0x80 in the aim byte) keeps exactly that pull for its first AXE_RETURN_HANG frames, so it
// hangs at the far end like a dagger, then adds AXE_RETURN_EXTRA per further frame, capped at
// AXE_RETURN_MAX: home in about 29 frames instead of 43. The catch radius is 10, so 6.5 a frame can't
// overshoot it.
static const float AXE_RETURN_HANG  = 6.0f;
static const float AXE_RETURN_EXTRA = 0.2f;
static const float AXE_RETURN_MAX   = 6.5f;
static const float kZeroF = 0.0f;
static float g_stockReturnAccel = 0.1f, g_stockReturnMax = 4.0f;
static uintptr_t g_returnAccelResume = 0, g_returnMaxResume = 0;
__declspec(naked) static void det_returnAccel() {   // replaces `mulss xmm0,[0.1]` (xmm0 = timer-15; edi = weapon; xmm1 free)
    __asm {
        test  byte ptr [edi+0x2a7], 0x80
        jz    stock
        movaps xmm1, xmm0
        subss xmm1, dword ptr [AXE_RETURN_HANG]
        maxss xmm1, dword ptr [kZeroF]
        mulss xmm1, dword ptr [AXE_RETURN_EXTRA]
        mulss xmm0, dword ptr [g_stockReturnAccel]
        addss xmm0, xmm1
        jmp   dword ptr [g_returnAccelResume]
    stock:
        mulss xmm0, dword ptr [g_stockReturnAccel]
        jmp   dword ptr [g_returnAccelResume]
    }
}
__declspec(naked) static void det_returnMax() {     // replaces `movss xmm1,[4.0]` (edi = weapon)
    __asm {
        test  byte ptr [edi+0x2a7], 0x80
        jz    stock
        movss xmm1, dword ptr [AXE_RETURN_MAX]
        jmp   dword ptr [g_returnMaxResume]
    stock:
        movss xmm1, dword ptr [g_stockReturnMax]
        jmp   dword ptr [g_returnMaxResume]
    }
}
// ---- Axe charge damage ----
// Weapon::doCollision adds (chargeTimer / chargeMax) * 5 to the rolled damage, for anything that charges
// (stock: clubs). A full charge takes 1.5 s, so +5 hardly pays for it on an axe. For an axe the bonus is the
// rolled damage instead (at least 5): a full charge doubles the hit, a half charge adds half. Thrown axes keep
// their charge in flight, so a charged throw gets it too. Replaces `mulss xmm1,[5.0]` (xmm1 = charge ratio,
// xmm2 = rolled damage, edi = weapon); the code after needs xmm6, so every xmm is kept.
static float g_chargeRoll = 0.0f, g_chargeMul = 5.0f, g_stockChargeMul = 5.0f;
static void __cdecl axe_charge_mul(const uint8_t* weapon) {
    g_chargeMul = g_stockChargeMul;
    if (is_axe_weapon(weapon) && g_chargeRoll > g_chargeMul) g_chargeMul = g_chargeRoll;
}
static uintptr_t g_chargeBonusResume = 0;
__declspec(naked) static void det_chargeBonus() {
    __asm {
        movss  dword ptr [g_chargeRoll], xmm2
        pushad
        sub    esp, 128
        movups [esp], xmm0
        movups [esp+16], xmm1
        movups [esp+32], xmm2
        movups [esp+48], xmm3
        movups [esp+64], xmm4
        movups [esp+80], xmm5
        movups [esp+96], xmm6
        movups [esp+112], xmm7
        push   edi
        call   axe_charge_mul
        add    esp, 4
        movups xmm0, [esp]
        movups xmm1, [esp+16]
        movups xmm2, [esp+32]
        movups xmm3, [esp+48]
        movups xmm4, [esp+64]
        movups xmm5, [esp+80]
        movups xmm6, [esp+96]
        movups xmm7, [esp+112]
        add    esp, 128
        popad
        mulss  xmm1, dword ptr [g_chargeMul]
        jmp    dword ptr [g_chargeBonusResume]
    }
}
// ---- Axe throw aim tilt ----
// While charging an axe throw, tilt the axe toward where it will go: up held alone = upright, up + left/right = 45 up, down held =
// 45 down, neither = level. It uses the bow's Weapon::aimAngle (+0x2d0, degrees, positive = up), which
// Weapon::updateSprite already applies to the whole held pose. The angle eases toward the target, stays
// frozen once the axe leaves the hand (so it flies tilted the way it's going), and is 0 for anything else
// an axe does, so normal swings and their hitboxes are untouched. Daggers and other weapons: no change.
// Hooked in Weapon::step right after the stock aim update and before its clamp (-45..74 for melee).
static const size_t OFF_WEAPON_AIMANGLE = 0x2d0;
static const float  AXE_AIM_TILT_DEG    = 45.0f;
static const float  AXE_AIM_UP_DEG      = 80.0f;   // straight up (up held alone)
static void __cdecl axe_aim_tilt(uint8_t* weapon) {
    if (!is_axe_weapon(weapon)) return;
    float& ang = *reinterpret_cast<float*>(weapon + OFF_WEAPON_AIMANGLE);
    if (!weapon[0x2a5]) { ang = 0.0f; return; }              // not a throw (doDaggerThrow)
    if (weapon[0x2a6]) {                                     // in flight: keep the tilt it left with,
        const int kf = axe_flight_pose(weapon);              // and the release pose (see AxeFlightPose)
        if (kf >= 0) weapon[0x27c] = (uint8_t)kf;
        return;
    }
    if (!weapon[0x269]) { ang = 0.0f; return; }              // not swinging
    const uint8_t* p = *reinterpret_cast<uint8_t**>(weapon + OFF_WEAPON_PARENT);
    float target = 0.0f;
    if (p) target = p[0x49c] ? ((p[0x49d] || p[0x49e]) ? AXE_AIM_TILT_DEG : AXE_AIM_UP_DEG)
                             : (p[0x49b] ? -AXE_AIM_TILT_DEG : 0.0f);
    ang += (target - ang) * 0.4f;
    if (ang - target < 0.5f && target - ang < 0.5f) ang = target;
}
static uintptr_t g_aimTiltResume = 0, g_aimClampMaxPtr = 0;
__declspec(naked) static void det_aimTilt() {   // at Weapon::step 0x3f0c61, esi = weapon
    __asm {
        pushad
        sub    esp, 128
        movups [esp], xmm0
        movups [esp+16], xmm1
        movups [esp+32], xmm2
        movups [esp+48], xmm3
        movups [esp+64], xmm4
        movups [esp+80], xmm5
        movups [esp+96], xmm6
        movups [esp+112], xmm7
        push   esi
        call   axe_aim_tilt
        add    esp, 4
        movups xmm0, [esp]
        movups xmm1, [esp+16]
        movups xmm2, [esp+32]
        movups xmm3, [esp+48]
        movups xmm4, [esp+64]
        movups xmm5, [esp+80]
        movups xmm6, [esp+96]
        movups xmm7, [esp+112]
        add    esp, 128
        popad
        mov    eax, dword ptr [g_aimClampMaxPtr]          // replaced: movss xmm0,[90.0]; mov eax,[esi+0x31c]
        movss  xmm0, dword ptr [eax]
        mov    eax, dword ptr [esi+0x31c]
        jmp    dword ptr [g_aimTiltResume]
    }
}

// The game only applies aimAngle to a pose when the attack script is marked aimable ([script+0x95], the bow
// scripts), and the heavy swing isn't, so the angle alone changes nothing on screen. Weapon::updateSprite's
// `sprite.setRotation(keyframe degrees)` (0x3f92b7) is redirected here to add the tilt to the axe sprite
// itself: purely visual, hitboxes don't use it. Facing left the sprite is mirrored, so the sign flips.
// While charging: up stands the axe upright, level holds it back over the head, down drops the head low.
static uintptr_t g_setRotation = 0;
static void __cdecl axe_sprite_tilt(const uint8_t* weapon, float* deg) {
    if (!weapon[0x2a5] || !is_axe_weapon(weapon)) return;
    float a = *reinterpret_cast<const float*>(weapon + OFF_WEAPON_AIMANGLE);
    *deg += weapon[0x268] ? -a : a;   // facingLeft (checked in screenshots: aiming up raises the axe head)
}
__declspec(naked) static void stub_axeSpriteRotation() {   // ecx = sprite, [esp+4] = degrees, esi = weapon
    __asm {
        pushad
        lea  eax, [esp+36]
        push eax
        push esi
        call axe_sprite_tilt
        add  esp, 8
        popad
        jmp  dword ptr [g_setRotation]
    }
}

// Write `jmp dest` (or `call dest`) over len expected bytes at rva, NOP-padding the rest. `prefix` bytes
// of the original are kept in front of the call (for P5, whose check has an unrelated mov in the middle).
static bool axe_patch(const char* name, uintptr_t rva, const uint8_t* expect, int len, uint8_t op, void* dest,
                      const uint8_t* prefix = nullptr, int prefixLen = 0) {
    uint8_t buf[64];
    if (len > (int)sizeof(buf) || prefixLen + 5 > len) return false;
    memset(buf, 0x90, len);
    if (prefixLen) memcpy(buf, prefix, prefixLen);
    uint8_t* at = reinterpret_cast<uint8_t*>(g_base + rva) + prefixLen;
    buf[prefixLen] = op;
    *reinterpret_cast<int32_t*>(buf + prefixLen + 1) =
        static_cast<int32_t>(reinterpret_cast<uint8_t*>(dest) - (at + 5));
    bool ok = patch_replace(name, rva, expect, buf, len);
    logmsg("[axe] %s %s", name, ok ? "patched" : "FAILED");
    return ok;
}
static void install_axe_tree() {
    // vtable copy (RTTI locator + 4 slots), with our initSkillStrings and levelUp.
    void** club = reinterpret_cast<void**>(g_base + RVA_CLUB_VTABLE);
    for (int i = 0; i < 5; i++) g_axeVtbl[i] = club[i - 1];
    g_clubInitStrings = reinterpret_cast<affInit_t>(club[1]);
    g_axeVtbl[2] = reinterpret_cast<void*>(&axe_initSkillStrings);
    g_axeVtbl[3] = reinterpret_cast<void*>(&axe_levelUp);

    const uint8_t pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};   // push ebp; mov ebp,esp; push -1
    g_getNewAffTramp = reinterpret_cast<getNewAff_t>(install_detour(RVA_GET_NEW_AFFINITY, (void*)&det_getNewAffinity, 5, pro, 5));
    g_setStartAffsTramp = reinterpret_cast<setStartAffs_t>(install_detour(RVA_SET_STARTING_AFFS, (void*)&det_setStartingAffinities, 5, pro, 5));
    logmsg("[axe] factory hook %s, starting-trees hook %s",
           g_getNewAffTramp ? "installed" : "FAILED", g_setStartAffsTramp ? "installed" : "FAILED");
    if (!g_getNewAffTramp) return;   // without the factory the tree can't exist; leave the weapons stock

    g_axeStunApply   = g_base + 0x002d339b;
    g_axeStunSkip    = g_base + 0x002d340e;
    g_axeStunNotClub = g_base + 0x002d33c7;
    g_axeChargeResume = g_base + 0x002d3645;
    {
        const uint8_t cpro[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};   // push ebp; mov ebp,esp; and esp,-8
        g_cancelAttackTramp = reinterpret_cast<cancelAttack_t>(install_detour(0x002d46a0, (void*)&det_cancelAttack, 6, cpro, 6));
        g_handleInputTramp = reinterpret_cast<handlePlayerInput_t>(install_detour(0x001c5040, (void*)&det_handlePlayerInput, 5, pro, 5));
        g_playerDrawTramp = reinterpret_cast<playerDraw_t>(install_detour(0x002f21b0, (void*)&det_playerDraw, 5, pro, 5));
        logmsg("[axe] throw aim arrow hook %s", g_playerDrawTramp ? "installed" : "FAILED");
        logmsg("[axe] cancel-keeps-charge hook %s, release-while-swap hook %s",
               g_cancelAttackTramp ? "installed" : "FAILED", g_handleInputTramp ? "installed" : "FAILED");
    }
    const uint8_t p1[] = {0x8A,0x42,0x5C,0xC0,0xE8,0x07,0x24,0x01,0x74,0x35,0x80,0xBF,0xE8,0x09,0x00,0x00,0x00,0x74,0x73};
    const uint8_t p2[] = {0x8A,0x41,0x5C,0xC0,0xE8,0x07,0x24,0x01,0x74,0x29,0x80,0xBF,0xE9,0x09,0x00,0x00,0x00,0x74,0x20,
                          0xC6,0x82,0x8C,0x02,0x00,0x00,0x01,0x80,0xBF,0xEB,0x09,0x00,0x00,0x00,0x74,0x10,0x8B,0x87,0xB8,
                          0x76,0x00,0x00,0xC7,0x80,0x94,0x02,0x00,0x00,0x28,0x00,0x00,0x00};
    const uint8_t p4[] = {0x8A,0x40,0x5C,0xC0,0xE8,0x07,0x24,0x01};
    const uint8_t p5[] = {0x8A,0x40,0x5C,0x8B,0x8E,0x18,0x03,0x00,0x00,0xD0,0xE8,0x24,0x01};
    const uint8_t p6[] = {0x8A,0x41,0x5C,0xD0,0xE8,0x24,0x01};
    const uint8_t p7[] = {0x8A,0x40,0x5C,0xD0,0xE8,0x24,0x01};
    axe_patch("attack: stun (lv1)",          0x002d3388, p1, sizeof(p1), 0xE9, (void*)&det_axeStun);
    axe_patch("attack: charge/throw (lv2-4)", 0x002d3612, p2, sizeof(p2), 0xE9, (void*)&det_axeCharge);
    axe_patch("stepSwing: charged ding",     0x003f033a, p4, sizeof(p4), 0xE8, (void*)&stub_clubOrAxe_eax);
    axe_patch("stepSwing: throw",            0x003f076e, p5, sizeof(p5), 0xE8, (void*)&stub_daggerOrAxe_eax, p5 + 3, 6);
    axe_patch("interpolate: net throw",      0x002f09e8, p6, sizeof(p6), 0xE8, (void*)&stub_daggerOrThrownAxe_ecx);
    const uint8_t pState[] = {0x8A,0x4A,0x5C,0xD0,0xE9,0x80,0xE1,0x01};
    axe_patch("entity state: net throw sent", 0x002ee4f7, pState, sizeof(pState), 0xE8, (void*)&stub_daggerOrAxe_cl_state);
    // (No "attack: throw damage" patch: that check picks the thrown-dagger damage stat, +0x910, over the
    // melee one, +0x90c. A thrown axe stays on the melee stat (strength), the same as its swings.)
    (void)p7;
    const uint8_t p8[] = {0xC6,0x86,0xA6,0x02,0x00,0x00,0x01};   // mov byte [esi+0x2a6],1
    const uint8_t p9[] = {0xF3,0x0F,0x11,0x87,0xB4,0x00,0x00,0x00,0x0F,0x28,0xE8,0xC7,0x87,0xB8,0x00,0x00,0x00,
                          0x00,0x00,0x00,0x00,0x0F,0x57,0xD2,0xEB,0x81};   // vx = xmm0; xmm5 = xmm0; vy = 0; xmm2 = 0; jmp
    g_throwStartResume  = g_base + 0x003f07de;
    g_throwFlightResume = g_base + 0x003f6656;
    axe_patch("stepSwing: throw aim",        0x003f07d7, p8, sizeof(p8), 0xE9, (void*)&det_throwStart);
    axe_patch("flight: aimed outbound",      0x003f66bb, p9, sizeof(p9), 0xE9, (void*)&det_throwFlight);
    // mov eax,[esi+0x31c]; movss xmm0,[0xb20604] (the 90-degree clamp; absolute, so it's relocated)
    uint8_t p10[] = {0x8B,0x86,0x1C,0x03,0x00,0x00,0xF3,0x0F,0x10,0x05,0x00,0x00,0x00,0x00};
    *reinterpret_cast<uint32_t*>(p10 + 10) = static_cast<uint32_t>(g_base + 0x00720604);
    g_aimClampMaxPtr = g_base + 0x00720604;
    g_aimTiltResume  = g_base + 0x003f0c6f;
    axe_patch("step: throw aim tilt",        0x003f0c61, p10, sizeof(p10), 0xE9, (void*)&det_aimTilt);
    {   // the return trip's pull and speed cap (absolute operands, so relocated)
        uint8_t pa[8] = {0xF3,0x0F,0x59,0x05,0,0,0,0}, pm[8] = {0xF3,0x0F,0x10,0x0D,0,0,0,0};
        *reinterpret_cast<uint32_t*>(pa + 4) = static_cast<uint32_t>(g_base + 0x0071f8c4);
        *reinterpret_cast<uint32_t*>(pm + 4) = static_cast<uint32_t>(g_base + 0x007203b4);
        g_stockReturnAccel = *reinterpret_cast<float*>(g_base + 0x0071f8c4);
        g_stockReturnMax   = *reinterpret_cast<float*>(g_base + 0x007203b4);
        g_returnAccelResume = g_base + 0x003f65a7;
        g_returnMaxResume   = g_base + 0x003f662a;
        axe_patch("flight: return pull",  0x003f659f, pa, sizeof(pa), 0xE9, (void*)&det_returnAccel);
        axe_patch("flight: return speed", 0x003f6622, pm, sizeof(pm), 0xE9, (void*)&det_returnMax);
    }
    {   // charge bonus in Weapon::doCollision (absolute operand, so relocated)
        uint8_t pc[8] = {0xF3,0x0F,0x59,0x0D,0,0,0,0};
        *reinterpret_cast<uint32_t*>(pc + 4) = static_cast<uint32_t>(g_base + 0x0071ef50);
        g_stockChargeMul = *reinterpret_cast<float*>(g_base + 0x0071ef50);
        g_chargeBonusResume = g_base + 0x003fa7f3;
        axe_patch("doCollision: axe charge bonus", 0x003fa7eb, pc, sizeof(pc), 0xE9, (void*)&det_chargeBonus);
    }
    g_resetOriginTramp = reinterpret_cast<resetScriptOrigin_t>(install_detour(0x003f6250, (void*)&det_resetScriptOrigin, 5, pro, 5));
    logmsg("[axe] resetScriptOrigin: flight pose hook %s", g_resetOriginTramp ? "installed" : "FAILED");
    g_setRotation = g_base + 0x00412ba0;   // sf::Transformable::setRotation(float)
    bool rot = redirect_call(0x003f92b7, g_setRotation, (void*)&stub_axeSpriteRotation);
    logmsg("[axe] updateSprite: tilt rotation %s", rot ? "patched" : "FAILED");
}

// ---- Woodsman class (Player::PlayerClass 7) -------------------------------------------------------
// A real seventh class, picked in the character select right after the Houndmaster. It borrows the
// Houndmaster's look everywhere as a stand-in (picker title + icon frames, body sprites from
// classes/ranger/, portrait, palettes) and its starting stats, but nothing tied to the dog: the dog,
// the whistle and the pet trees all check for class == 4, so a Woodsman never gets them. It starts with
// the brown axe (the patched default case of setStartingItems) and the Axe tree (det_setStartingAffinities).
//   Why class 7 is safe: the class is saved as a byte and sent in 4 bits of the net state; the per-class
// win counters are keyed by class name ("Woodsman_victory_count"); every class switch in the game has a
// default; the game-over screen skips classes above 4; backgrounds just filter by class number.
// A game without the mod can't show or build class 7, so co-op needs the mod on every machine.
static const uintptr_t RVA_MENU_RESET         = 0x0031ca90;   // PlayerMenu::reset(3 args), builds availableClasses
static const uintptr_t RVA_CLASS_NAME         = 0x002c7ee0;   // Player::getClassName(ret, class) (ecx, edx)
static const uintptr_t RVA_CLASS_GFX_PREFIX   = 0x002c7f70;   // Player::getClassGfxPathPrefix(ret, class, int) (ecx, edx, stack)
static const uintptr_t RVA_PORTRAIT_SPRITE    = 0x002c6990;   // Player::getNewInstanceOfPortraitSprite(class, int) (ecx, edx)
static const uintptr_t RVA_LOAD_CLASS_SPRITES = 0x002c8070;   // Player::loadClassSprites(class, int) thiscall
static const uintptr_t RVA_RANDOM_PALETTE     = 0x00309510;   // PlayerCustomizeMenu::randomPalette(idx, class) thiscall
static const uintptr_t RVA_SPRITE_SET_FRAME_VA_CALL1 = 0x00313152;  // drawCustomizeMenu: classIconsSprite.setFrame(class)
static const uintptr_t RVA_SPRITE_SET_FRAME_VA_CALL2 = 0x0031316c;  // drawCustomizeMenu: classTextSprite.setFrame(class)
static const size_t OFF_MENU_CLASSES   = 0x3398;              // PlayerMenu::availableClasses vector<PlayerClass>
static const size_t OFF_MENU_CLASS_SEL = 0x33c8;              // PlayerMenu selection counter (class = list[sel % n])
static const uintptr_t RVA_VEC_ENUM_EMPLACE = 0x00045fd0;     // vector<enum>::_Emplace_reallocate(where, &val)

// PlayerMenu::reset: after the stock list is built, insert the Woodsman right after the Houndmaster (or
// before Random if the Houndmaster is locked), keeping whatever class was selected selected.
typedef void (__fastcall *menuReset_t)(void* self, void* edx, int a, int b, int c);
static menuReset_t g_menuResetTramp = nullptr;
static void __fastcall det_menuReset(uint8_t* self, void*, int a, int b, int c) {
    g_menuResetTramp(self, nullptr, a, b, c);
    int** vec = reinterpret_cast<int**>(self + OFF_MENU_CLASSES);   // begin, end, cap
    int* b0 = vec[0]; int* e0 = vec[1];
    if (!b0 || e0 <= b0) return;
    int n = (int)(e0 - b0);
    for (int i = 0; i < n; i++) if (b0[i] == CLASS_WOODSMAN) return;   // already there
    int pos = n;
    for (int i = 0; i < n; i++) if (b0[i] == CLASS_BEASTMASTER) { pos = i + 1; break; }
    if (pos == n) for (int i = 0; i < n; i++) if (b0[i] == 6) { pos = i; break; }   // before Random
    unsigned& sel = *reinterpret_cast<unsigned*>(self + OFF_MENU_CLASS_SEL);
    int selClass = b0[sel % (unsigned)n];
    if (vec[1] != vec[2]) {
        memmove(b0 + pos + 1, b0 + pos, (n - pos) * sizeof(int));
        b0[pos] = CLASS_WOODSMAN;
        vec[1] = e0 + 1;
    } else {
        int val = CLASS_WOODSMAN;
        reinterpret_cast<vecEmplace_t>(g_base + RVA_VEC_ENUM_EMPLACE)(vec, nullptr, b0 + pos, &val);
    }
    for (int i = 0; i <= n; i++) if (vec[0][i] == selClass) { sel = (unsigned)i; break; }
}
// Picker icon + title. The game's sheets have one frame per stock class, so for the Woodsman the sprite is
// pointed at the mod's own texture (frame 0), and back at the game's sheet for any other class. If the
// mod's texture can't be made, the Houndmaster's frame stands in.
static uintptr_t g_spriteSetFrame = 0;
static void* g_classSheet[2];   // the game's class_icons / class_text texture, remembered when swapped out
static void __cdecl class_frame(uint8_t* spr, int which, int frame) {
    void* tex = (frame == CLASS_WOODSMAN) ? mod_texture(which ? MODTEX_CLASS_TITLE : MODTEX_CLASS_ICON)
                                          : g_modTex[which ? MODTEX_CLASS_TITLE : MODTEX_CLASS_ICON];
    void*& cur = *reinterpret_cast<void**>(spr + OFF_SPRITE_TEXTURE);
    if (frame == CLASS_WOODSMAN && tex) {
        if (cur != tex) { g_classSheet[which] = cur; sprite_show(spr, tex, 0); }
        return;
    }
    if (tex && cur == tex && g_classSheet[which]) { sprite_show(spr, g_classSheet[which], frame == CLASS_WOODSMAN ? 4 : frame); return; }
    reinterpret_cast<spriteSetFrame_t>(g_spriteSetFrame)(spr, nullptr, frame == CLASS_WOODSMAN ? 4 : frame);
}
__declspec(naked) static void stub_classIconFrame() {   // replaces `call Sprite::setFrame` (ecx = sprite, [esp+4] = frame)
    __asm {
        pushad
        push dword ptr [esp+36]
        push 0
        push ecx
        call class_frame
        add  esp, 12
        popad
        ret  4
    }
}
__declspec(naked) static void stub_classTextFrame() {
    __asm {
        pushad
        push dword ptr [esp+36]
        push 1
        push ecx
        call class_frame
        add  esp, 12
        popad
        ret  4
    }
}
// Player::getClassName: "Woodsman" for class 7 (the result string is built in place, like the stock one).
typedef void* (__fastcall *className_t)(void* ret, int cls);
static className_t g_classNameTramp = nullptr;
static void* __fastcall det_className(uint8_t* ret, int cls) {
    if (cls != CLASS_WOODSMAN) return g_classNameTramp(ret, cls);
    *reinterpret_cast<uint32_t*>(ret + 0x10) = 0;      // empty std::string: size 0, capacity 15
    *reinterpret_cast<uint32_t*>(ret + 0x14) = 15;
    ret[0] = 0;
    reinterpret_cast<strAssign_t>(g_base + RVA_STRING_ASSIGN)(ret, nullptr, "Woodsman", 8);
    return ret;
}
// The class number in edx / ecx / a stack slot -> the Houndmaster's, for functions that pick sprites.
static uintptr_t g_gfxPrefixTramp = 0, g_portraitTramp = 0, g_loadSpritesTramp = 0, g_randPaletteTramp = 0;
__declspec(naked) static void det_gfxPrefix() {      // edx = class
    __asm {
        cmp  edx, 7
        jne  go
        mov  edx, 4
    go: jmp  dword ptr [g_gfxPrefixTramp]
    }
}
__declspec(naked) static void det_portrait() {       // ecx = class
    __asm {
        cmp  ecx, 7
        jne  go
        mov  ecx, 4
    go: jmp  dword ptr [g_portraitTramp]
    }
}
__declspec(naked) static void det_loadSprites() {    // [esp+4] = class
    __asm {
        cmp  dword ptr [esp+4], 7
        jne  go
        mov  dword ptr [esp+4], 4
    go: jmp  dword ptr [g_loadSpritesTramp]
    }
}
__declspec(naked) static void det_randPalette() {    // [esp+8] = class
    __asm {
        cmp  dword ptr [esp+8], 7
        jne  go
        mov  dword ptr [esp+8], 4
    go: jmp  dword ptr [g_randPaletteTramp]
    }
}
// setStartingAttributes: `mov eax,[ecx+0x34c]; cmp eax,6; ja default` -> a Woodsman takes the
// Houndmaster's starting stats (that case only sets numbers; nothing pet-related).
static uintptr_t g_attrsResume = 0;
__declspec(naked) static void det_startAttrsClass() {
    __asm {
        mov  eax, dword ptr [ecx+0x34c]
        cmp  eax, 7
        jne  go
        mov  eax, 4
    go: cmp  eax, 6
        jmp  dword ptr [g_attrsResume]      // lands on the stock `ja default` (flags kept)
    }
}
// After the class switch (0x30ca2c, where every case lands; ecx = the player), a Woodsman's Vitality goes
// from the Houndmaster's 2 to 4: max HP is Vitality * 10 + 50, so he starts at 90 HP and each Vitality
// level still adds 10. The background tweaks run after this, as they do for every class. The same spot
// zeroes Player::affinityPoints; for testing, start_skill_points goes there instead (every class).
static const int WOODSMAN_BASE_VITALITY = 4;
static void* g_attrsJoinTramp = nullptr;
static uintptr_t g_attrsJoinResume = 0;   // 0x30ca39, past the 13 stolen bytes
__declspec(naked) static void det_startAttrsJoin() {
    __asm {
        cmp  dword ptr [ecx+0x34c], 7
        jne  go
        mov  eax, dword ptr [ecx+0x7704]          // the base stats (str, dex, vit, ...)
        mov  dword ptr [eax+8], 4               // WOODSMAN_BASE_VITALITY
    go: mov  eax, dword ptr [ebp+8]           // the stolen `mov eax,[ebp+8]; mov [eax+0x7fc],0`, but with
        push edx                               // start_skill_points instead of 0 (Player::affinityPoints)
        mov  edx, dword ptr [g_startSkillPoints]
        mov  dword ptr [eax+0x7fc], edx
        pop  edx
        jmp  dword ptr [g_attrsJoinResume]
    }
}
static void install_woodsman() {
    const uint8_t pro5[5]  = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    const uint8_t pro6[6]  = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};   // push ebp; mov ebp,esp; and esp,-8
    const uint8_t name5[5] = {0x55, 0x8B, 0xEC, 0x51, 0x56};         // push ebp; mov ebp,esp; push ecx; push esi
    g_menuResetTramp   = reinterpret_cast<menuReset_t>(install_detour(RVA_MENU_RESET, (void*)&det_menuReset, 6, pro6, 6));
    g_classNameTramp   = reinterpret_cast<className_t>(install_detour(RVA_CLASS_NAME, (void*)&det_className, 5, name5, 5));
    g_gfxPrefixTramp   = (uintptr_t)install_detour(RVA_CLASS_GFX_PREFIX,   (void*)&det_gfxPrefix,   5, pro5, 5);
    g_portraitTramp    = (uintptr_t)install_detour(RVA_PORTRAIT_SPRITE,    (void*)&det_portrait,    5, pro5, 5);
    g_loadSpritesTramp = (uintptr_t)install_detour(RVA_LOAD_CLASS_SPRITES, (void*)&det_loadSprites, 5, pro5, 5);
    g_randPaletteTramp = (uintptr_t)install_detour(RVA_RANDOM_PALETTE,     (void*)&det_randPalette, 5, pro5, 5);
    g_spriteSetFrame = g_base + RVA_SPRITE_SET_FRAME;
    bool f1 = redirect_call(RVA_SPRITE_SET_FRAME_VA_CALL1, g_spriteSetFrame, (void*)&stub_classIconFrame);
    bool f2 = redirect_call(RVA_SPRITE_SET_FRAME_VA_CALL2, g_spriteSetFrame, (void*)&stub_classTextFrame);
    const uint8_t attrs[9] = {0x8B, 0x81, 0x4C, 0x03, 0x00, 0x00, 0x83, 0xF8, 0x06};   // mov eax,[ecx+0x34c]; cmp eax,6
    g_attrsResume = g_base + 0x0030c807;
    void* at = install_detour(0x0030c7fe, (void*)&det_startAttrsClass, 9, attrs, 9);
    const uint8_t join[13] = {0x8B, 0x45, 0x08, 0xC7, 0x80, 0xFC, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    g_attrsJoinResume = g_base + 0x0030ca39;
    g_attrsJoinTramp = install_detour(0x0030ca2c, (void*)&det_startAttrsJoin, 13, join, 13);
    logmsg("[woodsman] base vitality / start skill points (%d) hook %s", g_startSkillPoints, g_attrsJoinTramp ? "installed" : "FAILED");
    logmsg("[woodsman] hooks: picker=%d name=%d gfx=%d portrait=%d sprites=%d palette=%d frames=%d/%d stats=%d",
           g_menuResetTramp != 0, g_classNameTramp != 0, g_gfxPrefixTramp != 0, g_portraitTramp != 0,
           g_loadSpritesTramp != 0, g_randPaletteTramp != 0, f1, f2, at != 0);
}

// Random-class wands can be created by ANY class branch (knight/rogue/... whose weapon we swapped to a
// wand), not just the mage branch, so the mage byte-patch that zeroes charges doesn't reach them. When
// g_wandZeroCharges is on, zero the equipped weapon's charges here if a Random char ended up holding a
// wand. currWeaponItem is a shared_ptr<Item> at Player+0x76c0; wandCharges is Item+0x52c (max at +0x52d).
static void zero_random_wand_charges(void* player) {
    if (!g_wandZeroCharges || !player) return;
    if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(player) + 0x350) == 0) return; // not a Random char
    // Inventory at Player+0x4a0; meleeWeapons = vector<shared_ptr<Item>> at Inventory+0x318 (8-byte elems).
    uint8_t* inv = reinterpret_cast<uint8_t*>(player) + 0x4a0;
    void** begin = *reinterpret_cast<void***>(inv + 0x318);   // meleeWeapons.begin
    void** end   = *reinterpret_cast<void***>(inv + 0x31c);   // meleeWeapons.end
    if (!begin || !end || end <= begin) return;
    int zeroed = 0;
    for (void** p = begin; p < end; p += 2) {                 // each shared_ptr<Item> = {ptr, ctrl}
        void* item = *p;
        if (!item) continue;
        int t = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(item) + 0x20c);   // itemType
        if (t > 67 && t < 77) {                                                      // WAND_BEGIN..WAND_END
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(item) + 0x52c) = 0;  // wandCharges = 0
            zeroed++;
        }
    }
    if (zeroed) logmsg("[randwpn] zeroed %d random-class wand(s) for player=%p", zeroed, player);
}
static void __cdecl det_setStartingItems(void* spPtr, void* spCtrl) {
    g_setStartItemsTramp(spPtr, spCtrl);           // run the normal class loadout first
    // (the Woodsman's axe comes from the stock default case, patched from the club in install_woodsman_axe_kit)
    const bool randomChar = spPtr && *(reinterpret_cast<uint8_t*>(spPtr) + 0x350) != 0;
    if (g_cursedBomb && spPtr) give_cursed_bomb(spPtr);
    // Random char with the Archery tree: add a bow + 30 arrows (in addition to the matched weapon).
    if (g_randArcheryBow && randomChar && has_archery_affinity(spPtr))
        give_bow_and_arrows(spPtr);
    // 5 small bombs: the Woodsman, or a Random char whose Bomber tree matches its items (after the bow,
    // so a bow keeps the ranged slot).
    if (spPtr && (*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(spPtr) + 0x34c) == CLASS_WOODSMAN
                  || (randomChar && g_randWeaponMatch && has_affinity(spPtr, 27))))
        give_small_bombs(spPtr);
    zero_random_wand_charges(spPtr);
    give_debug_start_kit(spPtr);
}

// ---- LOOP: wrap floorIndex past the final floor instead of winning ------
// Replaces the `inc [edi+0x3c0]` (floorIndex++) in loadNextLevel. We do the increment
// ourselves; if the loop toggle is on and we just advanced past the final floor, reset
// to floor 0 and engage New Game Plus, so the game loads level 1 again with the same
// party (player objects persist across floor transitions) -- a true same-character loop.
static uintptr_t g_floorResume = 0;
static void __cdecl floor_inc_handler(void* ge) {
    if (!ge) return;
    int* fi = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ge) + OFF_FLOORINDEX);
    *fi += 1;                                  // the original `inc [edi+0x3c0]`
    reload_if_ini_changed();
    if (g_loop && *fi > FINAL_FLOOR) {
        *fi = 0;
        *(reinterpret_cast<uint8_t*>(ge) + OFF_GE_NEWGAMEPLUS) = 1;
        logmsg("[vultra] LOOP: floorIndex passed %d -> wrapped to 0, New Game Plus engaged", FINAL_FLOOR);
    }
}
__declspec(naked) static void det_floorInc() {
    __asm {
        pushad
        pushfd
        push edi                 // edi = GameEngine (this) at this site
        call floor_inc_handler
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_floorResume]
    }
}
// Win-decision redirect: at the win, take the game's own "init a run" branch instead of
// the "end" branch, with New Game Plus set, so the engine restarts the run (same players).
static uintptr_t g_winInitPath = 0;
static uintptr_t g_winEndPath = 0;
__declspec(naked) static void det_winDecision() {
    __asm {
        mov  al, [edi+0x4c5]          // the original flag (edi = Client)
        test al, al
        jne  go_init                  // flag != 0 -> normal init branch
        cmp  byte ptr g_loop, 0
        je   go_end                   // loop off -> normal end (real win)
        mov  byte ptr [edi+0x318], 1   // Client+0x24(GameEngine)+0x2f4 newGamePlus: engage NG+
        jmp  go_init
    go_init:
        jmp  dword ptr [g_winInitPath]
    go_end:
        jmp  dword ptr [g_winEndPath]
    }
}
static bool install_win_redirect() {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + RVA_WIN_DECISION_JE);
    static const uint8_t expect[6] = {0x0F, 0x84, 0x99, 0x00, 0x00, 0x00};
    if (memcmp(p, expect, 6) != 0) { logmsg("[vultra] loop win-redirect: byte mismatch, skipped"); return false; }
    g_winInitPath = g_base + RVA_WIN_INIT_PATH;
    g_winEndPath  = g_base + RVA_WIN_END_PATH;
    DWORD oldp;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&det_winDecision) - (p + 5));
    p[5] = 0x90;
    VirtualProtect(p, 6, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}
static bool install_floor_wrap() {
    uint8_t* p = reinterpret_cast<uint8_t*>(g_base + RVA_FLOOR_INC);
    static const uint8_t expect[6] = {0xFF, 0x87, 0xC0, 0x03, 0x00, 0x00};
    if (memcmp(p, expect, 6) != 0) { logmsg("[vultra] loop floor-wrap: byte mismatch, skipped"); return false; }
    g_floorResume = g_base + RVA_FLOOR_INC_RESUME;
    DWORD oldp;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldp)) return false;
    p[0] = 0xE9;
    *reinterpret_cast<int32_t*>(p + 1) = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&det_floorInc) - (p + 5));
    p[5] = 0x90;                 // nop the leftover 6th byte
    VirtualProtect(p, 6, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return true;
}

// ---- win-path tracer (logging only; set DIAG_WIN 0 to remove) -----------
#if DIAG_WIN
static void* g_csTramp = nullptr;   // createSaveState
static void* g_ngTramp = nullptr;   // GameEngine::newGame
static void* g_geTramp = nullptr;   // Credits::stepGoodEnding
static void* g_crTramp = nullptr;   // Credits::step
static void* g_tlTramp = nullptr;   // GameEngine::transitionLevel
static void* g_siTramp = nullptr;   // GameEngine::stepIntermission
static void* g_lnTramp = nullptr;   // GameEngine::loadNextLevel
static bool  g_endingLogged = false;
static bool  g_creditsLogged = false;
static int   g_lastFloor = -999;

static void __cdecl wlog_createSave(void* gs, void* ge) {
    uint8_t ngp = ge ? *(reinterpret_cast<uint8_t*>(ge) + OFF_GE_NEWGAMEPLUS) : 0xFF;
    logmsg("[win] createSaveState gs=%p ge=%p ge.newGamePlus=%d", gs, ge, ngp);
}
static void __cdecl wlog_newGame(void* ge, void* ret) {
    g_fairyBlessCount = 0;   // fresh run: reset the stacked-fairy accumulator (replaces the old
                             // reset-on-empty-bonfire heuristic, which broke fairy stacking in MP)
    logmsg("[win] GameEngine::newGame ge=%p caller_rva=0x%x", ge, (unsigned)((uintptr_t)ret - g_base));
}
static void __cdecl wlog_goodEnding(void* ret) {
    if (g_endingLogged) return;            // once per win is enough
    g_endingLogged = true;
    logmsg("[win] *** GOOD ENDING reached *** caller_rva=0x%x loop=%d", (unsigned)((uintptr_t)ret - g_base), g_loop);
}
static void __cdecl wlog_credits(void* ret) {
    if (g_creditsLogged) return;
    g_creditsLogged = true;
    logmsg("[win] *** CREDITS::step reached *** caller_rva=0x%x loop=%d", (unsigned)((uintptr_t)ret - g_base), g_loop);
}
static void __cdecl wlog_transition(void* self, void* ret) {
    logmsg("[win] transitionLevel ge=%p caller_rva=0x%x", self, (unsigned)((uintptr_t)ret - g_base));
}
static void __cdecl wlog_intermission(void* ge) {
    if (!ge) return;
    int f = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ge) + OFF_FLOORINDEX);
    if (f == g_lastFloor) return;                 // only log when the floor changes
    g_lastFloor = f;
    uint8_t ngp = *(reinterpret_cast<uint8_t*>(ge) + OFF_GE_NEWGAMEPLUS);
    logmsg("[win] stepIntermission floorIndex=%d newGamePlus=%d", f, ngp);
}

__declspec(naked) static void det_log_createSave() {
    __asm {
        mov  eax, [esp+4]            // GameEngine* arg
        pushad
        push eax                     // ge
        push ecx                     // this (GameSave)
        call wlog_createSave
        add  esp, 8
        popad
        jmp  dword ptr [g_csTramp]
    }
}
__declspec(naked) static void det_log_newGame() {
    __asm {
        mov  eax, [esp]              // return addr
        pushad
        push eax                     // ret
        push ecx                     // this (GameEngine)
        call wlog_newGame
        add  esp, 8
        popad
        jmp  dword ptr [g_ngTramp]
    }
}
__declspec(naked) static void det_log_goodEnding() {
    __asm {
        mov  eax, [esp]              // return addr
        pushad
        push eax
        call wlog_goodEnding
        add  esp, 4
        popad
        jmp  dword ptr [g_geTramp]
    }
}
__declspec(naked) static void det_log_credits() {
    __asm {
        mov  eax, [esp]
        pushad
        push eax
        call wlog_credits
        add  esp, 4
        popad
        jmp  dword ptr [g_crTramp]
    }
}
__declspec(naked) static void det_log_transition() {
    __asm {
        mov  eax, [esp]
        pushad
        push eax                     // ret
        push ecx                     // this (GameEngine)
        call wlog_transition
        add  esp, 8
        popad
        jmp  dword ptr [g_tlTramp]
    }
}
__declspec(naked) static void det_log_intermission() {
    __asm {
        pushad
        push ecx                     // this (GameEngine)
        call wlog_intermission
        add  esp, 4
        popad
        jmp  dword ptr [g_siTramp]
    }
}
static void __cdecl wlog_loadNext(void* ge, void* level, void* ret) {
    int f = ge ? *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ge) + OFF_FLOORINDEX) : -1;
    logmsg("[win] loadNextLevel ge=%p floorIndex(before inc)=%d level=%p caller_rva=0x%x",
           ge, f, level, (unsigned)((uintptr_t)ret - g_base));
}
__declspec(naked) static void det_log_loadNext() {
    __asm {
        mov  eax, [esp]              // ret
        mov  edx, [esp+4]            // Level* arg
        pushad
        push eax                     // ret
        push edx                     // level
        push ecx                     // this (GameEngine)
        call wlog_loadNext
        add  esp, 0xc
        popad
        jmp  dword ptr [g_lnTramp]
    }
}
#endif

// ---- installer ----------------------------------------------------------
// Scripted test driver for development. It lives in the private dev repo (cloned into the
// gitignored _local/), so public builds compile these no-op stubs instead. `build2.bat release`
// defines VULTRA_NO_TEST_HARNESS to leave it out even where the file exists.
#if !defined(VULTRA_NO_TEST_HARNESS) && __has_include("../_local/harness/testharness.h")
#include "../_local/harness/testharness.h"
#else
static void test_frame(void*) {}
static void test_harness_install() {}
#endif

static bool install_hook() {
    char exePath[MAX_PATH]; GetModuleFileNameA(GetModuleHandleW(nullptr), exePath, MAX_PATH);
    const char* exeName = strrchr(exePath, '\\'); exeName = exeName ? exeName + 1 : exePath;
    if (_stricmp(exeName, "vagante.exe") != 0) { logmsg("[vultra] host '%s' != vagante.exe - inert", exeName); return false; }
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) return false;
    load_config();
    // Redirect the spellbook-generation roll to our pool picker. We intentionally do NOT
    // hook setSkill's entry, so load/pickup re-apply a book's stored skill unchanged.
    g_realSetSkill = g_base + RVA_SET_SKILL;
    if (!redirect_call(RVA_GEN_SETSKILL_CALL, g_realSetSkill, reinterpret_cast<void*>(&thunk_genSetSkill))) {
        logmsg("[vultra] generation call-site redirect FAILED - abort"); return false;
    }
    // Sanitize the per-refresh skill re-derivation so widening can't misread non-widened books.
    if (!redirect_call(RVA_NAMEDESC_SETSKILL_CALL, g_realSetSkill, reinterpret_cast<void*>(&thunk_nameDescSetSkill)))
        logmsg("[vultra] setNameAndDescription sanitize redirect FAILED (non-fatal)");
    // Creation-only pool filter for shop/chest/boss spellbooks (they skip the floor roll loop).
    {
        const uint8_t gen_pro[] = {0x8B,0xC6,0x8B,0x4D,0xF4};   // mov eax,esi ; mov ecx,[ebp-0xc]
        g_genRandItemResume = g_base + 0x00243c50;
        void* g = install_detour(0x00243c4b, (void*)&det_genRandItem, 5, gen_pro, 5);
        logmsg("[vultra] shop/chest/boss spellbook filter %s", g ? "installed" : "FAILED");
    }
    apply_widening_if_needed();
#if DIAG
    {
        uint8_t* t = reinterpret_cast<uint8_t*>(g_base + RVA_SET_SKILL);
        static const uint8_t pro[STEAL] = {0x55,0x8B,0xEC,0x6A,0xFF};
        if (memcmp(t, pro, STEAL) == 0) {
            uint8_t* tr = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, STEAL+5, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            memcpy(tr, t, STEAL); tr[STEAL] = 0xE9;
            *reinterpret_cast<int32_t*>(tr+STEAL+1) = static_cast<int32_t>((t+STEAL)-(tr+STEAL+5));
            g_ssTramp = reinterpret_cast<setSkill_t>(tr);
            patch_jmp(t, reinterpret_cast<void*>(&det_log_setSkill));
            logmsg("[vultra] DIAG setSkill logger installed");
        }
        const uint8_t drop_pro[] = {0x55,0x8B,0xEC,0x6A,0xFF};
        g_dropTramp = install_detour(0x00228c60, (void*)&det_log_drop, 5, drop_pro, 5);
        logmsg("[vultra] DIAG Item::drop logger %s", g_dropTramp ? "installed" : "FAILED");
    }
#endif
    // install_overlay();  // DISABLED: game's ImGui is LTCG-optimized (custom ABI) -> calling Begin crashes.

    // Wisp Curse background re-enable
    g_opnew = reinterpret_cast<opnew_t>(g_base + RVA_OP_NEW);
    g_opdel = reinterpret_cast<opdel_t>(g_base + RVA_OP_DELETE);
    const uint8_t bg_pro[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    g_origGetBg = install_detour(RVA_GET_AVAIL_BG, (void*)&det_getBg, 5, bg_pro, 5);
    logmsg("[vultra] wisp curse hook %s", g_origGetBg ? "installed" : "FAILED");

    // Wisp Curse force-for-everyone: replace the per-floor flag-init store in loadNextLevel.
    {
        const uint8_t wisp_pro[] = {0xC6, 0x85, 0x73, 0xFF, 0xFF, 0xFF, 0x00};  // mov byte[ebp-0x8d],0
        g_wispResume = g_base + 0x001c3ae2;   // resume at the instruction AFTER the init store
        void* w = install_detour(RVA_WISP_INIT_FLAG, (void*)&det_wispForce, 7, wisp_pro, 7);
        logmsg("[vultra] wisp-force hook %s", w ? "installed" : "FAILED");
    }

    // Mage Rod<->Wand options (all live-toggleable). The weapon detour is installed unconditionally and
    // reads its flag live (writes ITEMTYPE_ROD when off, so it's vanilla); the skill-tree and zero-charge
    // byte patches are applied/reverted to match the current ini by sync_mage_patches(), which also runs
    // on every live reload. So changing them in the manager takes effect on the next mage, no restart.
    {
        const uint8_t mw_pro[] = {0xC7,0x45,0x94,0x4F,0x00,0x00,0x00};  // mov dword[ebp-0x6c], ITEMTYPE_ROD
        g_mageWandResume = g_base + 0x0030b3b8;                          // resume after the 7-byte mov
        void* m = install_detour(0x0030b3b1, (void*)&det_mageWand, 7, mw_pro, 7);  // trampoline unused; we skip the orig
        logmsg("[vultra] mage weapon-type hook %s", m ? "installed" : "FAILED");
        sync_mage_patches();   // apply skill-tree + zero-charge patches to match the startup ini values
        logmsg("[vultra] mage options synced (wand_tree=%d random_wand=%d zero_charges=%d)",
               g_wandSkillTree, g_randomWandWeapon, g_wandZeroCharges);
    }

    // Random-class weapon match: detour each concrete class's starting-weapon-type write (mage shares the
    // det_mageWand hook above). Installed unconditionally; pick_random_class_weapon returns the vanilla
    // weapon unless the player is a Random char AND g_randWeaponMatch is on, so this is transparent off.
    {
        const uint8_t kn_pro[] = {0xC7,0x85,0x7C,0xFF,0xFF,0xFF,0x31,0x00,0x00,0x00}; // mov [ebp-0x84],0x31
        const uint8_t rg_pro[] = {0xC7,0x85,0x74,0xFF,0xFF,0xFF,0x2D,0x00,0x00,0x00}; // mov [ebp-0x8c],0x2d
        const uint8_t dp_pro[] = {0xC7,0x45,0x94,0x55,0x00,0x00,0x00};                 // mov [ebp-0x6c],0x55
        const uint8_t bm_pro[] = {0xC7,0x45,0x94,0x37,0x00,0x00,0x00};                 // mov [ebp-0x6c],0x37
        g_knightWpnResume   = g_base + 0x0030acba;   // site 0x30acb0 + 10
        g_rogueWpnResume    = g_base + 0x0030afb7;   // site 0x30afad + 10
        g_deprivedWpnResume = g_base + 0x0030c14f;   // site 0x30c148 + 7
        g_beastWpnResume    = g_base + 0x0030c4c5;   // site 0x30c4be + 7
        void* k = install_detour(0x0030acb0, (void*)&det_knightWeapon,   10, kn_pro, 10);
        void* r = install_detour(0x0030afad, (void*)&det_rogueWeapon,    10, rg_pro, 10);
        void* d = install_detour(0x0030c148, (void*)&det_deprivedWeapon,  7, dp_pro, 7);
        void* b = install_detour(0x0030c4be, (void*)&det_beastWeapon,     7, bm_pro, 7);
        logmsg("[vultra] random-class weapon hooks: knight=%d rogue=%d deprived=%d beast=%d (match=%d)",
               k != 0, r != 0, d != 0, b != 0, (int)g_randWeaponMatch);
        // Set randomizedClass(+0x350) BEFORE the loadout is built so the weapon detours see it.
        g_initPlayerReal = g_base + 0x00309ac0;   // PlayerCustomizer::initializePlayer
        bool ir = redirect_call(0x00308a3f, g_base + 0x00309ac0, (void*)&det_initRandomized);
        logmsg("[vultra] random-flag pre-init hook %s", ir ? "installed" : "FAILED");
        // Deprived: force the (background-gated) weapon to be built for randomized chars.
        const uint8_t dg_pro[] = {0x0F,0x85,0x22,0xF5,0xFF,0xFF};   // jne 0x70b667
        g_deprivedGateResume = g_base + 0x0030c145;
        g_deprivedGateSkip   = g_base + 0x0030b667;
        void* dg = install_detour(0x0030c13f, (void*)&det_deprivedGate, 6, dg_pro, 6);
        logmsg("[vultra] deprived weapon-gate hook %s", dg ? "installed" : "FAILED");
        const uint8_t de_pro[] = {0x68,0xD0,0x00,0x00,0x00};        // push 0xd0
        g_depEnchKeep = g_base + 0x0030c1b7;
        g_depEnchSkip = g_base + 0x0030c236;
        void* de = install_detour(0x0030c1b2, (void*)&det_deprivedEnchants, 5, de_pro, 5);
        logmsg("[vultra] deprived enchant gate hook %s", de ? "installed" : "FAILED");
        // Randomized max HP: wrap Player::updateStats and pin maxHp to a per-player random 60..100 afterwards.
        const uint8_t us_pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};   // push ebp; mov ebp,esp; push -1
        g_updateStatsTramp = reinterpret_cast<updateStats_t>(
            install_detour(0x002cb380, (void*)&det_updateStats, 5, us_pro, 5));
        logmsg("[vultra] random-health hook %s", g_updateStatsTramp ? "installed" : "FAILED");
    }

    // Multiplayer: keep the host accepting new joiners (as spectators) after the run has started.
    if (g_midgameSpectators) {
        g_specPathA1 = reinterpret_cast<void*>(g_base + 0x0036b3dd);   // overload1 assign-free-slot
        g_specPathA2 = reinterpret_cast<void*>(g_base + 0x0036bc52);   // overload2 assign-free-slot
        const uint8_t rej_pro[5] = {0x0F, 0x28, 0x05, 0x50, 0x0E};     // movaps xmm0,[0xb20e50] (both reject builders)
        uint8_t* r1 = reinterpret_cast<uint8_t*>(g_base + 0x0036b28a); // overload1 "That game has already started." builder
        uint8_t* r2 = reinterpret_cast<uint8_t*>(g_base + 0x0036b8e1); // overload2 same
        bool ok1 = (memcmp(r1, rej_pro, 5) == 0) && patch_jmp(r1, (void*)&stub_spec_ov1);
        bool ok2 = (memcmp(r2, rej_pro, 5) == 0) && patch_jmp(r2, (void*)&stub_spec_ov2);
        logmsg("[vultra] midgame-spectator join hook overload1=%s overload2=%s",
               ok1 ? "installed" : "FAILED/mismatch", ok2 ? "installed" : "FAILED/mismatch");
    }

    // Multiplayer: keep the lobby advertised as joinable so non-modded clients keep seeing JOIN mid-run.
    if (g_keepLobbyJoinable) {
        const uint8_t je1[2] = {0x74, 0x1F};   // je 0x76abc2 (gate on [Server+0x10e8])
        const uint8_t je2[2] = {0x74, 0x16};   // je 0x76abc2 (gate on [Server+0x10e9])
        const uint8_t nop2[2] = {0x90, 0x90};
        bool j1 = patch_replace("joinable-gate-1", 0x0036aba1, je1, nop2, 2);
        bool j2 = patch_replace("joinable-gate-2", 0x0036abaa, je2, nop2, 2);
        logmsg("[vultra] keep-lobby-joinable patch gate1=%s gate2=%s",
               j1 ? "ok" : "FAIL", j2 ? "ok" : "FAIL");
    }

    // Caged-fairy mode (Default/None/All): floor-gate hook + dedup-bypass hook in generateTreasure.
    {
        const uint8_t fairy_pro[] = {0x8B, 0x83, 0xA4, 0x00, 0x00, 0x00};  // mov eax,[ebx+0xa4]
        g_fairyResume = g_base + 0x002941f2;   // resume at cmp eax,1
        void* f = install_detour(RVA_FAIRY_FLOOR_LOAD, (void*)&det_fairyMode, 6, fairy_pro, 6);
        logmsg("[vultra] fairy-mode gate hook %s", f ? "installed" : "FAILED");

        const uint8_t dedup_pro[] = {0x0F, 0x85, 0x38, 0x03, 0x00, 0x00};  // jne 0x69457d
        g_fairyDedupFall = g_base + 0x00294245;   // continue -> spawn
        g_fairyDedupSkip = g_base + 0x0029457d;   // skip / no spawn
        void* d = install_detour(RVA_FAIRY_DEDUP_JNE, (void*)&det_fairyDedup, 6, dedup_pro, 6);
        logmsg("[vultra] fairy-dedup hook %s", d ? "installed" : "FAILED");

        // Stacking blessings: count every blessed fairy (mark) + drive the campfire bonus off it (bonfire).
        const uint8_t fbmark_pro[]  = {0xC6, 0x87, 0x3C, 0x03, 0x00, 0x00, 0x01};  // mov byte[edi+0x33c],1
        g_fairyBlessResume = g_base + 0x001b7836;
        void* fbm = install_detour(RVA_FAIRY_BLESS_MARK, (void*)&det_fairyBlessMark, 7, fbmark_pro, 7);
        const uint8_t bbload_pro[]  = {0x8B, 0x87, 0x3C, 0x05, 0x00, 0x00};        // mov eax,[edi+0x53c]
        g_bonfireBlessResume = g_base + 0x00100635;
        void* bbl = install_detour(RVA_BONFIRE_BLESS_LOAD, (void*)&det_bonfireBless, 6, bbload_pro, 6);
        // Configurable per-fairy heal amount (no-op at the default of 5).
        const uint8_t bheal_pro[]   = {0x80, 0xBE, 0x08, 0x08, 0x00, 0x00, 0x00};  // cmp byte[esi+0x808],0
        g_bonfireHealResume = g_base + 0x00100664;
        void* bhl = install_detour(RVA_BONFIRE_HEAL, (void*)&det_bonfireHeal, 7, bheal_pro, 7);
        logmsg("[vultra] fairy-bless-stack hooks mark=%s bonfire=%s heal=%s (stack=%d heal=%d)",
               fbm ? "ok" : "FAIL", bbl ? "ok" : "FAIL", bhl ? "ok" : "FAIL", g_fairyStack, g_fairyHeal);
    }

    // New Game Plus / looping: detour initGameState entry to force the flag when enabled.
    {
        const uint8_t igs_pro[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
        g_igsTramp = reinterpret_cast<initGameState_t>(
            install_detour(RVA_INIT_GAMESTATE, (void*)&det_initGameState, 5, igs_pro, 5));
        logmsg("[vultra] new-game-plus hook %s (loop=%d)", g_igsTramp ? "installed" : "FAILED", g_loop);
        // Start-at-NG+ has NO dedicated hook: it's armed self-driven inside step_frame (see the
        // levelgen+0x50 check). Hooking newGame for it crashed, so that approach was dropped.
        logmsg("[vultra] difficulty (self-driven in step) start_difficulty=%d scaling_starts_at=%d harderBy=%.1f%%",
               g_startDifficulty, g_scalingStartsAt, g_harderByPct);
    }
    // Co-op portals: hook SpellEntity::enterPortal so a placed portal teleports whoever enters it,
    // not just the caster (rebuilds the un-synced entrance<->exit link from the replicated entities).
    // Installed unconditionally; gated live by g_portalCoop (ini [multiplayer] portal_coop).
    {
        const uint8_t ep_pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp; mov ebp,esp; push -1
        g_enterPortalTramp = reinterpret_cast<enterPortal_t>(
            install_detour(RVA_ENTER_PORTAL, (void*)&det_enterPortal, 5, ep_pro, 5));
        logmsg("[vultra] coop-portal hook %s (portal_coop=%d)",
               g_enterPortalTramp ? "installed" : "FAILED", g_portalCoop);
        // Two-way portal: make both ends interactable (behaviour gated live by g_portalTwoWay).
        logmsg("[vultra] portal two-way hook %s (portal_two_way=%d)",
               install_portal_twoway() ? "installed" : "FAILED", g_portalTwoWay);
    }
    // Axe skill tree (AffinityType 26) and the axe-only versions of the club/dagger perks.
    install_axe_tree();
    install_woodsman();   // the Woodsman class (class 7): picker entry, Houndmaster look, axe + Axe tree
    install_small_bomb(); // the Woodsman's small bomb (a flagged regular bomb)
    install_woodsman_axe_kit();
    install_bomber_tree(); // the Woodsman's Bomber tree (AffinityType 27); after install_axe_tree
    install_woods_trees(); // Brutalist (28) and Scavenger (29)
    install_clone();       // Party: the delayed shadow copy
    // Random character skill-tree count: rewrite the hardcoded 4 in randomizeAffinities.
    apply_random_tree_count();
    // Companion hooks so random_tree_count == 0 yields truly zero trees (not the class defaults):
    // mark the 0-tree placeholder after the roll, and strip markers when the character is built.
    // Lists without a marker pass through untouched.
    {
        const uint8_t pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp; mov ebp,esp; push -1
        g_randomizeAffTramp = reinterpret_cast<randomizeAff_t>(
            install_detour(RVA_RANDOMIZE_AFFINITIES, (void*)&det_randomizeAffinities, 5, pro, 5));
        logmsg("[vultra] randomize-affinities hook %s", g_randomizeAffTramp ? "installed" : "FAILED");
        g_populateAffTramp = reinterpret_cast<populateAff_t>(
            install_detour(RVA_POPULATE_AFFINITIES, (void*)&det_populateAffinities, 5, pro, 5));
        logmsg("[vultra] populate-affinities hook %s", g_populateAffTramp ? "installed" : "FAILED");
    }
    // Same-character loop: hook Client::step to capture the live GameEngine and (when the test
    // toggle is on) warp-restart on F11. The warp uses the devs' own debug primitive
    // (loadNextLevel regenerates the floor on the persistent party), so it keeps the character.
    g_clientLoadNextLevelVA = g_base + RVA_CLIENT_LOADNEXTLEVEL;
    g_setNewGamePlusVA      = g_base + RVA_SET_NEWGAMEPLUS;
    g_serverLoadNextLevelVA = g_base + RVA_SERVER_LOADNEXTLEVEL;
    {
        const uint8_t step_pro[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
        g_stepTramp = reinterpret_cast<clientStep_t>(
            install_detour(RVA_CLIENT_STEP, (void*)&det_step, 5, step_pro, 5));
        logmsg("[vultra] loop step-hook %s (test_warp=%d, start_floor=%d)",
               g_stepTramp ? "installed" : "FAILED", g_loopTestWarp, g_loopStartFloor);
        if (g_stepTramp) test_harness_install();   // needs the step hook to drive the script
        // MP host: hook Server::step entry (liveness + F11 request).
        g_serverStepTramp = reinterpret_cast<serverStep_t>(
            install_detour(RVA_SERVER_STEP, (void*)&det_server_step, 5, step_pro, 5));
        logmsg("[vultra] MP server step-hook %s", g_serverStepTramp ? "installed" : "FAILED");
        // Host settings sync (event-driven): lobby join/leave on the client, lobby ready on the host,
        // plus a Steam listener for lobby-data changes.
        g_lobbyJoinedTramp = reinterpret_cast<thisArg2_t>(
            install_detour(RVA_CLIENT_LOBBY_JOINED, (void*)&det_lobbyJoined, 5, step_pro, 5));
        g_clientDisconnectTramp = reinterpret_cast<thisArg0_t>(
            install_detour(RVA_CLIENT_DISCONNECT, (void*)&det_clientDisconnect, 5, step_pro, 5));
        g_lobbyCreatedTramp = reinterpret_cast<thisArg2_t>(
            install_detour(RVA_SERVER_LOBBY_CREATED, (void*)&det_lobbyCreated, 5, step_pro, 5));
        g_updateLobbyTramp = reinterpret_cast<thisArg0_t>(
            install_detour(RVA_SERVER_UPDATE_LOBBY, (void*)&det_updateLobby, 5, step_pro, 5));
        logmsg("[sync] hooks: joined=%s disconnect=%s created=%s updateLobby=%s listener=%s",
               g_lobbyJoinedTramp ? "ok" : "FAILED", g_clientDisconnectTramp ? "ok" : "FAILED",
               g_lobbyCreatedTramp ? "ok" : "FAILED", g_updateLobbyTramp ? "ok" : "FAILED",
               register_lobby_listener() ? "ok" : "FAILED");
        // Watch the game folder so an ini save (the manager's button) triggers the reload.
        HANDLE wt = CreateThread(nullptr, 0, ini_watch_thread, nullptr, 0, nullptr);
        if (wt) CloseHandle(wt);
        // MP host: catch the boss-win pre-broadcast (after GE::step, before sendGameState) so clients
        // never see gameComplete -> never roll the credits. Steal the 7-byte `cmp byte[ebx+0x10ef],0`.
        const uint8_t postge_pro[] = {0x80, 0xBB, 0xEF, 0x10, 0x00, 0x00, 0x00};
        g_serverPostTramp = install_detour(RVA_SERVER_POST_GESTEP, (void*)&det_server_postgestep, 7, postge_pro, 7);
        logmsg("[vultra] MP server pre-broadcast win-hook %s", g_serverPostTramp ? "installed" : "FAILED");
        // Starting loadout: hook PlayerCustomizer::setStartingItems to add a permanently-cursed bomb.
        g_makeItemVA  = g_base + 0x001a3760;   // make_shared<Item,Level*&,float&,float&,ItemType,int>
        g_invPickUpVA = g_base + 0x00209cc0;   // Inventory::pickUp(shared_ptr<Item>,int,int,int,int)
        g_invEquipVA  = g_base + 0x0020a9a0;   // Inventory::equipItem(shared_ptr<Item>)
        g_setStartWeaponVA = g_base + 0x0020a600;  // Inventory::setStartingWeapon(shared_ptr<Item>,int,int)
        const uint8_t ssi_pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp; mov ebp,esp; push -1
        g_setStartItemsTramp = reinterpret_cast<setStartItems_t>(
            install_detour(0x0030ac30, (void*)&det_setStartingItems, 5, ssi_pro, 5));
        logmsg("[vultra] starting-loadout hook %s (cursed_bomb=%d)",
               g_setStartItemsTramp ? "installed" : "FAILED", g_cursedBomb);
        // Note: the cursed bomb is intentionally un-consumable (= unlimited bombs). Throwing it logs a
        // per-throw "could not dropDecrement rangeItem!" ERROR (the failed consume); that is acceptable
        // (it's per-throw, not per-frame), so we deliberately do NOT mute it.
        // Boss never wins: intercept Cutscene::trigger so the final-boss-defeat ending cutscene never
        // plays (it's the sole writer of endOfCampaign -> credits). Skips it + requests a loop instead.
        const uint8_t cut_pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp; mov ebp,esp; push -1
        g_cutsceneTrigTramp = install_detour(RVA_CUTSCENE_TRIGGER, (void*)&det_cutscene_trigger, 5, cut_pro, 5);
        logmsg("[vultra] boss-defeat cutscene hook %s", g_cutsceneTrigTramp ? "installed" : "FAILED");
        // Escalating loops: hook Monster::setLevel just after it sets health/damage scale factors, so
        // we can multiply them by the per-cycle factor on the host. Gated at runtime by loop_scale_pct
        // + g_loopCount, so installing it unconditionally is harmless on a base (un-looped) run.
        const uint8_t sl_pro[9] = {0x8A, 0x45, 0xDB, 0x88, 0x86, 0x41, 0x03, 0x00, 0x00};  // mov al,[ebp-0x25]; mov [esi+0x341],al
        g_setLevelTramp = install_detour(RVA_MONSTER_SETLEVEL_TAIL, (void*)&det_setLevel, 9, sl_pro, 9);
        logmsg("[vultra] monster scaling hook %s (harderBy=%.1f%%, scalingFrom=%d)",
               g_setLevelTramp ? "installed" : "FAILED", g_harderByPct, g_scalingStartsAt);
        // More enemies: hook Level::addEntity and duplicate each regular enemy N times (host-side).
        g_spawnEntityAddr = g_base + 0x001a5220;
        g_setTetheredPos  = reinterpret_cast<setpos_t>(g_base + 0x001b01d0);
        const uint8_t ae_pro[5] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};  // push ebp; mov ebp,esp; push -1
        g_addEntityTramp = reinterpret_cast<addEntity_t>(
            install_detour(0x0024afc0, (void*)&det_addEntity, 5, ae_pro, 5));
        logmsg("[vultra] enemy-dup hook %s (spawn+%d%% from loop %d, cap %d%%)",
               g_addEntityTramp ? "installed" : "FAILED", g_spawnIncreasePct, g_spawnIncreaseStartsAt, g_spawnIncreaseMaxPct);
        // Spread: hook getSpawnWeight's return so monsters cost less spawn budget (more distinct spawns).
        const uint8_t sw_pro[5] = {0xF3, 0x0F, 0x10, 0x40, 0x14};  // movss xmm0,[eax+0x14]
        g_spawnWeightResume = g_base + 0x002b34d2;
        void* sw = install_detour(RVA_SPAWNWEIGHT_RET, (void*)&det_spawnWeight, 5, sw_pro, 5);
        logmsg("[vultra] enemy-spread hook %s (spread+%d%% from loop %d, cap %d%%)",
               sw ? "installed" : "FAILED", g_spawnSpreadPct, g_spawnSpreadStartsAt, g_spawnSpreadMaxPct);
    }
    // Player::hurt: logs the damage-skip guards (diagnostic) and drops Bomber 3 bomb hits on players.
    {
        const uint8_t hurt_pro[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
        g_hurtTramp = install_detour(RVA_PLAYER_HURT, (void*)&det_hurt, 5, hurt_pro, 5);
        logmsg("[vultra] hurt diag hook %s", g_hurtTramp ? "installed" : "FAILED");
    }
    // The boss-win loop is handled inside the step hook (win_maybe_loop): it watches the ending
    // flag and warp-restarts instead of rolling credits. No call-site redirect needed.
    // Old auto-restart experiments (kept for reference, not installed): forcing the initGameState
    // branch only re-inits forever. The working path is the warp primitive above.
    (void)&install_floor_wrap; (void)&install_win_redirect;
#if DIAG_WIN
    {
        const uint8_t pro[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
        g_csTramp = install_detour(RVA_CREATE_SAVESTATE, (void*)&det_log_createSave, 5, pro, 5);
        g_ngTramp = install_detour(RVA_NEWGAME,          (void*)&det_log_newGame,    5, pro, 5);
        g_geTramp = install_detour(RVA_STEP_GOOD_ENDING, (void*)&det_log_goodEnding, 5, pro, 5);
        g_crTramp = install_detour(RVA_CREDITS_STEP,     (void*)&det_log_credits,    5, pro, 5);
        g_tlTramp = install_detour(RVA_TRANSITION_LEVEL, (void*)&det_log_transition, 5, pro, 5);
        g_siTramp = install_detour(RVA_STEP_INTERMISSION,(void*)&det_log_intermission,5, pro, 5);
        g_lnTramp = install_detour(RVA_LOADNEXTLEVEL,    (void*)&det_log_loadNext,   5, pro, 5);
        logmsg("[vultra] win-tracer: createSave=%d newGame=%d goodEnding=%d credits=%d transition=%d intermission=%d loadNext=%d",
               g_csTramp != nullptr, g_ngTramp != nullptr, g_geTramp != nullptr, g_crTramp != nullptr, g_tlTramp != nullptr, g_siTramp != nullptr, g_lnTramp != nullptr);
    }
#endif

    {
        char buf[256]; int n = 0;
        for (int i = 0; i < g_poolN && n < 240; i++) n += sprintf(buf + n, "%d ", g_pool[i]);
        logmsg("[vultra] installed; widening=%d pool(%d)=[ %s]; ini=%s", g_wideningApplied, g_poolN, buf, g_iniPath);
    }
    return true;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hMod); install_hook(); }
    return TRUE;
}
