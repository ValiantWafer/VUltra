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
static void logmsg(const char* fmt, ...) {
    char path[MAX_PATH]; host_dir_file("vultra.log", path);
    FILE* f = fopen(path, "a"); if (!f) return;
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
static int affinity_to_weapon_type(int at) {
    switch (at) {
        // ARCHERY(2) is handled separately (bow + arrows, additive) -- see give_bow_and_arrows.
        case 12: return 45;   // DAGGER  -> ITEMTYPE_DAGGER
        case 13: return 49;   // SWORD   -> ITEMTYPE_SWORD
        case 14: return 68;   // WAND    -> ITEMTYPE_WAND
        case 18: return 85;   // CQC     -> ITEMTYPE_CAESTUS (fists)
        case 21: return 79;   // ROD     -> ITEMTYPE_ROD
        case 25: return 55;   // CLUB    -> ITEMTYPE_CLUB
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
static void __fastcall det_updateStats(void* self, void* /*edx*/, int a, int b) {
    g_updateStatsTramp(self, a, b);                  // run the real stat recompute first
    if (!g_randHealth || !self) return;
    if (*(reinterpret_cast<uint8_t*>(self) + 0x350) == 0) return;             // Random chars only
    int cls = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(self) + 0x34c);
    if (cls < 0 || cls > 6) return;                                           // sanity: a real player
    float* maxHp = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x110);
    if (*maxHp < 5.0f) return;                                                // skip uninitialized/doll objects
    HpSlot* sl = hp_slot_for(self);
    *maxHp = sl->target;                                                      // pin max HP every frame
    float* hp = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x10c);
    if (sl->fillFrames > 0) { *hp = sl->target; sl->fillFrames--; }           // start full (re-fill window)
    else if (*hp > sl->target) *hp = sl->target;                             // otherwise just clamp to max
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
static void load_config() {
    host_dir_file("vultramod.ini", g_iniPath);
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath);  // flush Win ini cache -> read fresh
    for (int i = 0; i < N_SPELLS; i++)
        g_enabled[i] = GetPrivateProfileIntA("spellbook_pool", g_spells[i].key, g_spells[i].def, g_iniPath) != 0;
    g_wispCurse = GetPrivateProfileIntA("backgrounds", "wisp_curse", 0, g_iniPath) != 0;  // opt-in
    g_replaceBg = (int)GetPrivateProfileIntA("backgrounds", "replace_bg", 5, g_iniPath);
    g_forceWispCurse = GetPrivateProfileIntA("backgrounds", "force_wisp_curse", 0, g_iniPath) != 0;  // opt-in (host)
    g_fairyMode = (int)GetPrivateProfileIntA("npc", "fairy_mode", 0, g_iniPath);             // 0=Default, 1=None, 2=All levels
    g_fairyStack = GetPrivateProfileIntA("npc", "fairy_stack_blessing", 0, g_iniPath) != 0;  // +5 max HP per fairy (host)
    g_fairyHeal = (int)GetPrivateProfileIntA("npc", "fairy_heal", 5, g_iniPath);              // max HP each fairy adds (host)
    if (g_fairyHeal < 0)    g_fairyHeal = 0;
    if (g_fairyHeal > 1000) g_fairyHeal = 1000;
    g_fairyHealDelta = g_fairyHeal - 5;
    g_loop = GetPrivateProfileIntA("loop", "new_game_plus", 0, g_iniPath) != 0;            // opt-in
    g_startDifficulty = (int)GetPrivateProfileIntA("loop", "start_difficulty", 2, g_iniPath);  // run starts at this tier (1..100)
    if (g_startDifficulty < 1)   g_startDifficulty = 1;
    if (g_startDifficulty > 100) g_startDifficulty = 100;
    g_loopTestWarp  = GetPrivateProfileIntA("loop", "test_warp", 0, g_iniPath) != 0;       // dev/test only (F11 warp)
    g_loopStartFloor = (int)GetPrivateProfileIntA("loop", "loop_start_floor", 0, g_iniPath);  // where the loop restarts
    g_warpFloor      = (int)GetPrivateProfileIntA("loop", "warp_floor", 0, g_iniPath);        // F11 destination
    g_campfireOnWrap = GetPrivateProfileIntA("loop", "campfire_on_wrap", 0, g_iniPath) != 0;  // campfire before restart
    g_loopFinalFloor = (int)GetPrivateProfileIntA("loop", "final_floor", 13, g_iniPath);
    {   // escalating loops: "harder by" is a float percent; read as a string and parse
        char buf[32];
        GetPrivateProfileStringA("loop", "loop_scale_pct", "50", buf, sizeof(buf), g_iniPath);
        g_harderByPct = (float)atof(buf);
        if (g_harderByPct < 0.0f)    g_harderByPct = 0.0f;
        if (g_harderByPct > 1000.0f) g_harderByPct = 1000.0f;
    }
    g_scalingStartsAt = (int)GetPrivateProfileIntA("loop", "scaling_starts_at", 3, g_iniPath);  // tier the extra scaling begins
    if (g_scalingStartsAt < 1)   g_scalingStartsAt = 1;
    if (g_scalingStartsAt > 100) g_scalingStartsAt = 100;
    g_spawnIncreasePct = (int)GetPrivateProfileIntA("loop", "spawn_increase_pct", 0, g_iniPath);   // +N% enemies/level/loop
    if (g_spawnIncreasePct < 0)     g_spawnIncreasePct = 0;
    if (g_spawnIncreasePct > 10000) g_spawnIncreasePct = 10000;
    g_spawnIncreaseStartsAt = (int)GetPrivateProfileIntA("loop", "spawn_increase_starts_at", 3, g_iniPath);
    if (g_spawnIncreaseStartsAt < 1)   g_spawnIncreaseStartsAt = 1;
    if (g_spawnIncreaseStartsAt > 100) g_spawnIncreaseStartsAt = 100;
    g_spawnIncreaseMaxPct = (int)GetPrivateProfileIntA("loop", "spawn_increase_max_pct", 0, g_iniPath);  // 0 = no cap
    if (g_spawnIncreaseMaxPct < 0)      g_spawnIncreaseMaxPct = 0;
    if (g_spawnIncreaseMaxPct > 100000) g_spawnIncreaseMaxPct = 100000;
    g_spawnSpreadPct = (int)GetPrivateProfileIntA("loop", "spawn_spread_pct", 0, g_iniPath);  // spread-out density
    if (g_spawnSpreadPct < 0)     g_spawnSpreadPct = 0;
    if (g_spawnSpreadPct > 10000) g_spawnSpreadPct = 10000;
    g_spawnSpreadStartsAt = (int)GetPrivateProfileIntA("loop", "spawn_spread_starts_at", 3, g_iniPath);
    if (g_spawnSpreadStartsAt < 1)   g_spawnSpreadStartsAt = 1;
    if (g_spawnSpreadStartsAt > 100) g_spawnSpreadStartsAt = 100;
    g_spawnSpreadMaxPct = (int)GetPrivateProfileIntA("loop", "spawn_spread_max_pct", 0, g_iniPath);  // 0 = no cap
    if (g_spawnSpreadMaxPct < 0)      g_spawnSpreadMaxPct = 0;
    if (g_spawnSpreadMaxPct > 100000) g_spawnSpreadMaxPct = 100000;
    g_cursedBomb = GetPrivateProfileIntA("player", "god_cursed_bomb", 0, g_iniPath) != 0;   // spawn loadout (host)
    g_randWeaponMatch = GetPrivateProfileIntA("player", "random_match_weapon", 0, g_iniPath) != 0;  // live-toggleable
    g_randNoTreeDefault = GetPrivateProfileIntA("player", "random_no_tree_default", 1, g_iniPath) != 0;  // 1=default weapon, 0=fists
    g_randArcheryBow = GetPrivateProfileIntA("player", "random_archery_bow", 1, g_iniPath) != 0;  // Archery tree -> +bow +30 arrows
    g_randHealth = GetPrivateProfileIntA("player", "random_health", 0, g_iniPath) != 0;  // random max HP 60..100 for Random chars
    g_midgameSpectators = GetPrivateProfileIntA("multiplayer", "allow_midgame_spectators", 0, g_iniPath) != 0;  // applied at startup (host)
    g_keepLobbyJoinable = GetPrivateProfileIntA("multiplayer", "keep_lobby_joinable", 0, g_iniPath) != 0;       // applied at startup (host)
    g_wandSkillTree    = GetPrivateProfileIntA("mage", "wand_skill_tree",    0, g_iniPath) != 0;  // applied at startup
    g_randomWandWeapon = GetPrivateProfileIntA("mage", "random_wand_weapon", 0, g_iniPath) != 0;  // applied at startup
    g_wandZeroCharges  = GetPrivateProfileIntA("mage", "wand_zero_charges",  0, g_iniPath) != 0;  // applied at startup
    rebuild_pool();
    ini_mtime(&g_iniMtime);
}
// Live reload: if vultramod.ini changed on disk (e.g. the manager's "Save changes"),
// re-read it so a running game picks up the new pool on the next spellbook it rolls.
static void reload_if_ini_changed() {
    FILETIME t;
    if (!ini_mtime(&t)) return;
    if (t.dwLowDateTime == g_iniMtime.dwLowDateTime && t.dwHighDateTime == g_iniMtime.dwHighDateTime) return;
    load_config();
    apply_widening_if_needed();
    sync_mage_patches();                 // re-apply/revert the mage Rod<->Wand options to match the ini
    logmsg("[vultra] config reloaded live; pool size %d", g_poolN);
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
static volatile DWORD g_lastServerStep = 0;
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
__declspec(naked) static void det_hurt() {
    __asm {
        pushad
        push ecx                 // this (Player)
        call hurt_log
        add  esp, 4
        popad
        jmp  dword ptr [g_hurtTramp]
    }
}

// Per-frame hook on Client::step: capture the live GameEngine and poll the test hotkey.
typedef void (__fastcall *clientStep_t)(void* self, void* edx);
static clientStep_t g_stepTramp = nullptr;
static void flush_enemy_dups(void* ge);   // defined below (enemy-duplication section)
static void __cdecl step_frame(void* client) {
    reload_if_ini_changed();   // pick up manager "Save" live (e.g. in the lobby) -> re-syncs mage options
    if (client) g_ge = reinterpret_cast<uint8_t*>(client) + OFF_CLIENT_GE;
    flush_enemy_dups(g_ge);    // spawn any queued enemy copies into the live level (deferred, safe here)
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
// hook body: __fastcall(level=ecx, edx ignored, spRef=stack) -> ret 4, matching addEntity's thiscall.
static void __fastcall do_addEntity(void* level, void* /*edx*/, void* spRef) {
    void* entity = spRef ? *reinterpret_cast<void**>(spRef) : nullptr;
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
// Does the player have the Archery skill tree (AffinityType 2)?
static bool has_archery_affinity(void* player) {
    void** begin = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(player) + 0x870);
    void** end   = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(player) + 0x874);
    if (!begin || !end || end <= begin) return false;
    for (void** p = begin; p < end; ++p) {
        void* a = *p;
        if (a && *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(a) + 4) == 2) return true;  // ARCHERY
    }
    return false;
}
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
typedef void (__cdecl *setStartItems_t)(void* spPtr, void* spCtrl);
static setStartItems_t g_setStartItemsTramp = nullptr;
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
    if (g_cursedBomb && spPtr) give_cursed_bomb(spPtr);
    // Random char with the Archery tree: add a bow + 30 arrows (in addition to the matched weapon).
    if (g_randArcheryBow && spPtr && *(reinterpret_cast<uint8_t*>(spPtr) + 0x350) != 0 && has_archery_affinity(spPtr))
        give_bow_and_arrows(spPtr);
    zero_random_wand_charges(spPtr);
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
        // MP host: hook Server::step entry (liveness + F11 request).
        g_serverStepTramp = reinterpret_cast<serverStep_t>(
            install_detour(RVA_SERVER_STEP, (void*)&det_server_step, 5, step_pro, 5));
        logmsg("[vultra] MP server step-hook %s", g_serverStepTramp ? "installed" : "FAILED");
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
    // Diagnostic: hook Player::hurt to log the damage-skip guards when a hit is processed.
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
