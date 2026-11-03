/* Item actions of the dual-screen companion: equip, unequip, eat / drink / use, take out and fuse-to-arrow, by calling the
   game's own pouch functions, the ones its pause menu and D-pad quick menus call (TOTK 1.4.3; static analysis in
   android/APP.md "Dual-screen companion", device checks there too).

   - The companion's poller thread only queues a request (companion_request): one at a time, named by its item's
     identity (pouch key, slot, actor name, partition and the snapshot's pouchGen, which covers durability and fuse).
   - The game's frame-end thread (companion_tick, from hle_nvn.c's frame-end sync wait, where world_survey.c also runs)
     re-checks everything with fresh reads: the game state (title, loading, saving, events, a paused game), the item's
     identity, the game's own enable rules for that action, the guards; then makes exactly one call and decides the
     outcome on later ticks by reading the game state back. Nothing is retried; a request whose outcome is not seen
     within the timeout ends as "failed".
   - Every called game function is identified first: SHA-256 of its first 64 bytes, checked once per mapping of main.
     Any mismatch and nothing is called ("unsupported").
   - Only device-verified actions are offered (VERIFIED below); every other one reports "notAvailableYet". */
#include "companion.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* ---------- SHA-256 (FIPS 180-4), for the short prologues only ---------- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + K256[i] + w[i];
        uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof *s); s[4] += t1; s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}
/* hex digest of n bytes (n < 2^29); declared in companion.h */
void companion_sha256_hex(const void *data, size_t n, char out[65]) {
    const uint8_t *m = data;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t tail[128] = {0};
    size_t full = n / 64 * 64, rest = n - full;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, m + i);
    memcpy(tail, m + full, rest); tail[rest] = 0x80;
    size_t len = rest < 56 ? 64 : 128; uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[len - 1 - i] = (uint8_t)(bits >> 8 * i);
    for (size_t i = 0; i < len; i += 64) sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
}

/* ---------- the game functions (1.4.3 main offsets; paraphrased roles, see APP.md) ---------- */
enum {
    F_EQUIP, F_UNEQUIP, F_IS_EQUIPPED, F_CAN_USE, F_EAT, F_TAKE_OUT, F_CAN_HOLD_MATERIAL, F_CAN_HOLD_ZONAI, F_INFO, F_AIM,
    F_COMPONENT, F_BATTERY_TYPE, F_FLAG_BOOL, F_ENERGY_MAX, F_RECHARGE, F_REFILL,
    F_CALLED,   /* the rest are only reached through the ones above; checked as well so the whole path is the known one */
};
static const struct { uint32_t off; const char *sha64; } FN[] = {
    [F_EQUIP] = {0x20a1128, "e46dbf73f3b9e4c536c6b72036e574da14f1c8888ffb94ce7255f91d215a0248"},             /* bool equip(pouch, cat, slot, part -1) */
    [F_UNEQUIP] = {0x20a9a28, "2ed7e16375b15718721d77e2b8dbd5acafe945931bd9d60125c4bb3a60b39a3e"},           /* bool unequip(pouch, cat, slot, -1, 0, 1) */
    [F_IS_EQUIPPED] = {0x20ac5c4, "79dd52da3439e54925773855f28460616c16928e9919dfca3d5adc20dda3fd3c"},       /* bool (pouch, cat, slot) */
    [F_CAN_USE] = {0x20ac080, "93b1f3a647f16109b299bc5cd8cba16aa885bd653003161dfb9256d67bbad60c"},           /* bool (pouch, cat, slot, n): stock not held */
    [F_EAT] = {0x20af148, "a509a5d211e89ccd5fad0cee84516a3b238c532e17fe18d4e0a7a29fed199ac7"},               /* bool eat(pouch, cat 5|6, slot) */
    [F_TAKE_OUT] = {0x20afab4, "80e06a1ad2d508a20a4469ba0a11bce52a46f812ce4db6ffe0225e1628f96552"},          /* quick menu: (pouch, slot, cat 5|7) */
    [F_CAN_HOLD_MATERIAL] = {0x20affa8, "948366144bc1e3ceea3350f7c2eed2d844a7c71baac88d3bd1140586525a3e4b"}, /* bool (pouch) */
    [F_CAN_HOLD_ZONAI] = {0x20aff18, "6f5df446853a2381c2a8fc5f24bb2487d23acb1ac59ad80be5c77f81c6755c02"},    /* bool (pouch) */
    [F_INFO] = {0x63307c, "21b0d2a38a57f38d6c36d72d282fedb5b0fc381b7c4ab8a44d1b9447ac9d56dc"},              /* info *(table, const char **name) */
    [F_AIM] = {0x711000, "ce834d461ace4959a2f53f2750767b05eacd49e216520edfb2d50cfe747cb9a7"},               /* bool (): take-out fuses to arrows */
    /* the pause menu's energy branch (main+0x2174b48): the player's battery, its full check, recharge / unlimited time */
    [F_COMPONENT] = {0x89f9b0, "04c287171e1caf5b90f9c6eb55718010cd4ce68fb811d695ad41f5721947e22b"},         /* component *(actor, index) */
    [F_BATTERY_TYPE] = {0xc1ee50, "17e7f50f915561ec09f4d4db62d68e3c0376f1512761315c4a2fdaa471736f1b"},      /* the battery's type id */
    [F_FLAG_BOOL] = {0x3d01a0, "d780f7811fc0ad53c7ce457c8f1e47130cd7df2d0ac794433fcc232a536fe2ec"},         /* GameData bool (hash) */
    [F_ENERGY_MAX] = {0x1cce94c, "58f09c881812304c22aa32fe3d3bafd0d15f15d64b58914bbee72bf4fdd9bee2"},       /* float (battery+0x18) */
    [F_RECHARGE] = {0x1fa4e44, "d866d4e1d745f9257318d3d325714aa3241b8c5de7f16ceb7e449dbc068221ac"},         /* (float amount, battery) */
    [F_REFILL] = {0x1fa4fec, "0dcba5ce96fe243c89ec21efaf05a6dcb6440fc3f5e0027cd8d1e77bfa8b5fe9"},           /* (battery, 1): to full */
    {0x20af2b4, "fd96ec7629dfd3b9cff687a1139817db4a0cc2b0ec05bdc287fc4543b6594c9c"},   /* eat food */
    {0x20af1b8, "94485937003241e220585608901e5d592dada08e5e04e5273f70db9f0dace3e5"},   /* eat material */
    {0x20b356c, "df9e085f828c0894378a77a24520b0bdfb66b595b04ae40f99baa59711287a8c"},   /* consume: effects, stock -1 */
    {0x20a837c, "9a0c3c98016cd63156f22b6df356d82417bb40df4b693abd851834cb5623dd09"},   /* equip weapon */
    {0x20a871c, "bb8b71f42955661d92eac90b1fabd8b2f186d4117873d5e900508361b5b5c1db"},   /* equip bow */
    {0x20a8ad0, "1e87a3272499d67440778674d3b64ecce42e89376578b00563bc453b4d655852"},   /* equip shield */
    {0x20a7680, "eb3fd5f7f1afabcca88285a4493012a2d1972eaf4436d92d273780d6cd2ca21b"},   /* take out a Zonai device */
    {0x20a6af8, "26e8b5ae557b285190a78ca9fa86dc2eb80be83b370092f10448fe8df66d8512"},   /* take out a material */
};
enum { FN_COUNT = sizeof FN / sizeof *FN, PROLOGUE = 64 };
enum {
    INFO_SLOT = 0x3a6f678, INFO_GOT = 0x395f900, INFO_TABLE = 0x60,   /* actor info table: *(*(main+INFO_SLOT)+0x60) */
    INFO_USE_TYPE = 0x98, INFO_RECOVER = 0x130, INFO_EDIBLE = 0x15c,  /* PouchUseType string, life recover, edible */
    INFO_ENERGY = 0x128, INFO_ENERGY_TIME = 0x138,                    /* EnergyRecover, NoUseEnergyTime (int) */
    POUCH_REQ_CAT = 0x2f0, POUCH_REQ_SLOT = 0x2f4, POUCH_REQ_POSTED = 0x1fc,   /* the player request take-out fills */
    PLAYER_SLOT = 0x3add238, PLAYER_GOT = 0x395dc70,                  /* the player actor: *(main+PLAYER_SLOT) */
    MAIN_TEXT = 0x2bb9c00,                                            /* main's code segment, from 0 */
    BATTERY_COMPONENT = 0x1f, BATTERY_OWNED = 0x613f16d,              /* component index; GameData bool: Link has the battery */
    BAT_GAUGE = 0x18, BAT_ENERGY = 0x54, BAT_RESERVE = 0x58,          /* max-energy gauge (F_ENERGY_MAX), energy, reserve */
    BAT_TIME_FLAGS = 0xe8, BAT_TIME = 0xec, BAT_TIME_LEFT = 0xf0, BAT_TIME_RATE = 0xf4, BAT_TIME_SHOWN = 0xdc, BAT_TIME_PHASE = 0xd4,
    ENERGY_LIMIT = 0x2d01940, ENERGY_RESERVE_CAP = 0x2cf6528,         /* the floats the full check compares with */
};
enum { BIT_CALC = 0, BIT_WEAPON_CHANGE = 33 };   /* player state +0x658 */

/* ---------- actions ---------- */
enum { A_EQUIP, A_UNEQUIP, A_EAT, A_DRINK, A_USE, A_HOLD, A_TAKE_OUT, A_FUSE, A_COUNT };
static const char *const ACTION[A_COUNT] = {"equip", "unequip", "eat", "drink", "use", "hold", "takeOut", "fuseArrow"};
enum { K_WEAPONS, K_BOWS, K_SHIELDS, K_ARROWS, K_ARMOR, K_MATERIALS, K_FOOD, K_ZONAI, K_KEY_ITEMS };
#define BIT(a) (1u << (a))
/* What the device checks proved, per pouch key: an action is offered in the world (game unpaused) only when its bit
   is set here (android/APP.md "Dual-screen companion" lists the evidence). Not proven: "notAvailableYet". */
/* Thor, 2 Nov 2026 (APP.md "Item actions"): each of these ran in the world through the companion, showed the game's own
   outcome (model, HUD, hearts / extra hearts, stock -1 once, item placed in front of Link), read back, and survived save
   -> reload. Use (A_USE) is offered only for the PouchUseTypes in verified_use.
   Thor, 3 Nov 2026 (APP.md "Item actions"): Use (fairy) at 8.75/14 hearts restored the hearts in the world, stock 5 -> 4
   once, and the saved progress.sav holds 4. Not proven, so not offered: Use (energy materials: the recharge call left
   the battery's energy unchanged in the same tick and the gauge cannot be told apart from the battery's own refill),
   Fuse to arrows for Zonai devices (not tested). */
static unsigned verified_in_world[COMPANION_CATS] = {
    [K_WEAPONS] = BIT(A_EQUIP) | BIT(A_UNEQUIP), [K_BOWS] = BIT(A_EQUIP) | BIT(A_UNEQUIP),
    [K_SHIELDS] = BIT(A_EQUIP) | BIT(A_UNEQUIP), [K_ARMOR] = BIT(A_EQUIP) | BIT(A_UNEQUIP),
    [K_MATERIALS] = BIT(A_EAT) | BIT(A_TAKE_OUT) | BIT(A_FUSE) | BIT(A_USE), [K_FOOD] = BIT(A_EAT) | BIT(A_DRINK),
    [K_ZONAI] = BIT(A_TAKE_OUT),
};
/* PouchUseType (actor info +0x98) -> the pause menu's button */
enum { USE_NONE, USE_EAT, USE_DRINK, USE_FAIRY, USE_ENERGY };
static unsigned verified_use = BIT(USE_FAIRY);   /* not yet: BIT(USE_ENERGY) */
#ifdef COMPANION_ACTIONS_TEST
void companion_actions_test_verify(int key, unsigned mask) { verified_in_world[key] = mask; }
void companion_actions_test_verify_use(unsigned mask) { verified_use = mask; }
#endif

static int use_kind(const char *s) {
    static const char *const EAT[] = {"EatFood", "EatCookingDish", "EatCookingSkewer", "EatBizarre", "EatTooHard"};
    for (unsigned i = 0; i < 5; i++) if (!strcmp(s, EAT[i])) return USE_EAT;
    return !strcmp(s, "Drink") ? USE_DRINK : !strcmp(s, "UseFairy") ? USE_FAIRY : !strcmp(s, "UseEnergyRecoverMaterial") ? USE_ENERGY : USE_NONE;
}
/* Actor facts by name, read with the game's info lookup on the frame-end thread. state 0: wanted, 1: known, -1: none */
typedef struct { char name[COMPANION_NAME]; signed char state; unsigned char use, edible; int32_t recover, energy, energy_time; } Info;
enum { MAX_INFO = 1024, INFO_PER_TICK = 16 };   /* more than one partition's materials and food together: no thrashing */

typedef struct {
    int seq, action, key, slot, part, phase;   /* phase: 0 none, 1 queued, 2 called (reading back) */
    char actor[COMPANION_NAME];
    uintptr_t main;
    uint64_t ident;
    uint64_t t_queued, t_called, frame_called;
    int32_t before_stock; int posted;
    const char *state, *reason;                /* the latest result */
} Request;
enum { PH_NONE, PH_QUEUED, PH_CALLED };
enum { STATE_MS = 100, WATCH_MS = 3000, STALE_MS = 1000, QUEUE_MS = 3000, READBACK_MS = 2000, READBACK_FRAMES = 20, LOST_MS = 6000,
       SETTLE_MS = 1000, SETTLE_FRAMES = 20 };

static CompanionEnv env;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uintptr_t main;
    int guard;                                 /* 0 unchecked, 1 the prologues match, -1 they do not */
    uint64_t at;                               /* when the tick last refreshed this (0 never) */
    const char *reason;                        /* "" when actions may run */
    int pause_menu, aiming, aim_mode, weapon_change, hold_material, hold_zonai, arrows;
    /* the player's battery (0: none found), as the pause menu's energy branch reads it */
    int has_battery; uint64_t battery; float energy, energy_max, energy_reserve, energy_limit, reserve_cap[2];
    /* the world has been "ok" since settle_at / settle_frame with this player actor and pouch partition (settled 0: not ok) */
    int settled; uint64_t settle_at, settle_frame, settle_actor; unsigned settle_part;
} st = {.reason = "stale"};
static Info infos[MAX_INFO]; static unsigned ninfos;
static Request req;
static int last_seq;
static uint64_t watch_until, gen_ident; static long long gen;

static uint64_t now(void) { return env.now_ms ? env.now_ms() : 0; }
static long tid(void) {
#if defined(__linux__)
    return (long)syscall(SYS_gettid);
#else
    return 0;
#endif
}
static void wake(int on) { if (env.set_pending) env.set_pending(on); }
void companion_actions_init(const CompanionEnv *e) { pthread_mutex_lock(&lock); env = *e; pthread_mutex_unlock(&lock); }

static Info *info(const char *name, int want) {
    for (unsigned i = 0; i < ninfos; i++) if (!strcmp(infos[i].name, name)) return &infos[i];
    if (!want) return NULL;
    if (ninfos == MAX_INFO) ninfos = 0;   /* forget everything rather than grow: re-read on demand */
    Info *n = &infos[ninfos++];
    memset(n, 0, sizeof *n); snprintf(n->name, sizeof n->name, "%s", name);
    return n;
}

/* The game state that decides whether anything may run (reads only; see companion_view). */
static const char *state_reason(const CompanionView *v) {
    if (strcmp(v->status, "ready"))
        return !strcmp(v->status, "unsupported") || !strcmp(v->status, "title") || !strcmp(v->status, "noPlayer") ? v->status : "loading";
    if (v->receiver == -1) return "noPlayer";      /* no player pouch actor to receive the request */
    if (v->loading) return "loading";
    if (env.save_busy && env.save_busy()) return "saving";
    if (v->event) return "event";                  /* cutscene, dialogue, any event flow */
    /* Thor 1.4.3: bit 0 is clear while a save loads (and on scene changes), but stays set with the pause menu open */
    if (!(v->player_flags >> BIT_CALC & 1)) return "loading";
    if (v->pause_menu) return "menu";              /* the pause menu (inventory or any other tab) is open */
    return "";
}

static void add(CompanionAction *out, unsigned *n, int a, int key, const char *why) {
    if (!*why && !(verified_in_world[key] & BIT(a))) why = "notAvailableYet";
    else if (!*why && *st.reason) why = st.reason;
    out[(*n)++] = (CompanionAction){ACTION[a], !*why, why};
}
/* The pause menu's "battery full" refusal for an energy material (main+0x2174bb4 / 0x2175f58), with the battery read
   on the last tick: full unless the energy is below the maximum, the item gives unlimited-energy time, or the reserve is
   below its cap. A NaN compares the way the menu's fcmp does. */
static int battery_full(const Info *f) {
    if (!(st.energy >= st.energy_max) || f->energy_time > 0) return 0;
    return !(st.energy_reserve < st.reserve_cap[st.energy_max > st.energy_limit] - st.energy_max);
}
/* The game's own rules (pause menu action window main+0x21782b4, quick menu main+0x21266a0) for one item, with the game
   state of the last tick. Caller holds the lock. */
static unsigned rules(int key, const CompanionView *v, CompanionAction out[3]) {
    unsigned n = 0;
    int stocked = v->stock == INT32_MIN || v->stock >= 1;
    int full = v->life != INT32_MIN && (v->life == v->max_life || v->life == v->max_life - v->break_life);
    switch (key) {
    case K_WEAPONS: case K_BOWS: case K_SHIELDS:   /* quick menu: equip / "none", while weapon changes are allowed */
        add(out, &n, v->equipped ? A_UNEQUIP : A_EQUIP, key, st.weapon_change ? "" : "weaponLocked");
        break;
    case K_ARMOR:
        add(out, &n, v->equipped ? A_UNEQUIP : A_EQUIP, key, "");
        break;
    case K_MATERIALS: case K_FOOD: {
        Info *f = info(v->name, 1);
        if (f->state == 0) { add(out, &n, A_EAT, key, "infoPending"); }
        else if (f->state > 0 && f->use != USE_NONE) {
            int a = f->use == USE_DRINK ? A_DRINK : f->use == USE_EAT ? A_EAT : A_USE;
            const char *why = "";
            if (a == A_USE && !(verified_use & BIT(f->use))) why = "notAvailableYet";
            else if (f->use == USE_ENERGY)   /* the menu's button: stock not held and Link has the battery; then not full */
                why = !stocked ? "noStock" : !st.has_battery ? "noBattery" : !st.battery ? "notNow" : battery_full(f) ? "batteryFull" : "";
            else if (key == K_MATERIALS && (f->recover < 1 || !f->edible)) why = "notEdible";
            else if (!stocked) why = "noStock";
            else if (full && !v->has_effect) why = "fullHearts";
            add(out, &n, a, key, why);
        }
        if (key == K_FOOD) break;
    }   /* fall through: materials are also taken out or fused like Zonai devices */
    case K_ZONAI: {
        int can = key == K_ZONAI ? st.hold_zonai && v->zonai_unlocked : st.hold_material;
        add(out, &n, A_TAKE_OUT, key, st.aim_mode ? "aiming" : !stocked ? "noStock" : can ? "" : "cannotHold");
        add(out, &n, A_FUSE, key, !st.aim_mode ? "notAiming" : !st.arrows ? "noArrows" : !stocked ? "noStock" : "");
        break;
    }
    default: break;   /* arrows (one plain kind), key items: nothing the menus do in the world */
    }
    return n;
}

static void watch(void) {
    watch_until = now() + WATCH_MS;
    wake(1);
}
unsigned companion_item_actions(int key, const CompanionView *v, CompanionAction out[3]) {
    pthread_mutex_lock(&lock);
    watch();
    unsigned n = key >= 0 && key < COMPANION_CATS ? rules(key, v, out) : 0;
    pthread_mutex_unlock(&lock);
    return n;
}
void companion_action_state(int *ok, const char **reason, int *pause_menu, int *aiming) {
    pthread_mutex_lock(&lock);
    watch();
    if (st.at && now() - st.at > STALE_MS) st.reason = "stale";   /* the frame end stopped running */
    *reason = st.guard < 0 ? "unsupported" : st.reason; *ok = !**reason;
    *pause_menu = st.pause_menu; *aiming = st.aiming;
    pthread_mutex_unlock(&lock);
}
long long companion_pouch_gen(uint64_t ident) {
    pthread_mutex_lock(&lock);
    if (ident != gen_ident) { gen_ident = ident; gen++; }
    long long g = gen;
    pthread_mutex_unlock(&lock);
    return g;
}

static void finish(const char *state, const char *reason) {
    req.phase = PH_NONE; req.state = state; req.reason = reason;
    fprintf(stderr, "[companion] request %d %s %s/%d %s: %s%s%s\n", req.seq, ACTION[req.action], companion_category_key(req.key),
            req.slot, req.actor, state, *reason ? " " : "", reason);
}

int companion_request(uintptr_t main_base, const char *action, const char *category, int slot, const char *actor,
                      int partition, long long pouch_gen) {
    int a = -1, key = category ? companion_category(category) : -1;
    for (int i = 0; action && i < A_COUNT; i++) if (!strcmp(action, ACTION[i])) a = i;
    size_t len = actor ? strlen(actor) : 0;
    if (a < 0 || key < 0 || slot < 0 || slot > 254 || !len || len >= COMPANION_NAME || partition < 0 || partition > 1 || pouch_gen <= 0)
        return COMPANION_INVALID;
    for (size_t i = 0; i < len; i++) {
        char ch = actor[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return COMPANION_INVALID;
    }
    if (!env.rd || !env.call) return COMPANION_UNSUPPORTED;
    CompanionView v;
    int ready = companion_view(main_base, env.rd, key, slot, &v);
    pthread_mutex_lock(&lock);
    int r;
    if (st.guard < 0 || !strcmp(v.status, "unsupported")) r = COMPANION_UNSUPPORTED;
    else if (req.phase) r = COMPANION_BUSY;   /* a tap while one runs is dropped, never queued */
    else {
        CompanionAction act[3]; unsigned n = rules(key, &v, act); int offered = 0;
        for (unsigned i = 0; i < n; i++) if (act[i].id == ACTION[a] && act[i].enabled) offered = 1;
        if (!ready || !offered) r = COMPANION_REFUSED;
        else {
            r = req.seq = ++last_seq;
            req.action = a; req.key = key; req.slot = slot; req.part = partition; req.main = main_base;
            snprintf(req.actor, sizeof req.actor, "%s", actor);
            req.ident = gen_ident; req.t_queued = now(); req.phase = PH_QUEUED; req.state = "pending"; req.reason = "";
            /* the item the companion showed must be the one there now: same pouch generation, partition and actor */
            if (pouch_gen != gen || v.ident != gen_ident || (unsigned)partition != v.part || strcmp(v.name, actor)) finish("refused", "itemMoved");
            else wake(1);
        }
    }
    pthread_mutex_unlock(&lock);
    return r;
}

size_t companion_result(int seq, CompanionBuf *out) {
    pthread_mutex_lock(&lock);
    size_t n = 0;
    if (seq > 0 && seq == req.seq) {
        uint64_t t = now();
        /* the frame end stopped running (game closed, stuck in loading): end the request without waiting for it */
        if (req.phase == PH_QUEUED && t - req.t_queued > QUEUE_MS) finish("refused", "stale");
        else if (req.phase == PH_CALLED && t - req.t_called > LOST_MS) finish("failed", "timeout");
        char text[160];
        int len = snprintf(text, sizeof text, "{\"seq\":%d,\"state\":\"%s\",\"reason\":\"%s\"}", req.seq, req.state, req.reason);
        char *d = out->data;
        if (len > 0 && (size_t)len < sizeof text && (out->cap > (size_t)len || (d = realloc(out->data, (size_t)len + 1)))) {
            if (out->cap <= (size_t)len) { out->data = d; out->cap = (size_t)len + 1; }
            memcpy(out->data, text, (size_t)len + 1); out->len = n = (size_t)len;
        }
    }
    pthread_mutex_unlock(&lock);
    return n;
}

/* ---------- the frame-end tick (game thread) ---------- */
static uint64_t game(int f, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    return env.call(st.main + FN[f].off, a0, a1, a2, a3, a4, a5);
}
static int game_bool(int f, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { return (game(f, a0, a1, a2, a3, 0, 0) & 0xff) != 0; }
static uint64_t rd64(uint64_t a) { uint64_t v = 0; return a && env.rd(a, &v, 8) ? v : 0; }

/* The host test lays out its own stand-in prologues (no game code in the repository) and declares their hashes the
   expected ones; the app always compares with FN. */
#ifdef COMPANION_ACTIONS_TEST
static char test_expected[FN_COUNT][65]; static int test_expect;
void companion_actions_test_sha256(const void *m, size_t n, char out[65]) { companion_sha256_hex(m, n, out); }
void companion_actions_test_expect_prologues(uintptr_t main_base) {
    for (unsigned i = 0; i < FN_COUNT; i++) companion_sha256_hex((const uint8_t *)(main_base + FN[i].off), PROLOGUE, test_expected[i]);
    test_expect = 1;
}
static const char *expected(unsigned i) { return test_expect ? test_expected[i] : FN[i].sha64; }
#else
static const char *expected(unsigned i) { return FN[i].sha64; }
#endif
static int check_guard(void) {
    if (rd64(st.main + INFO_GOT) != st.main + INFO_SLOT || rd64(st.main + PLAYER_GOT) != st.main + PLAYER_SLOT) return 0;
    for (unsigned i = 0; i < FN_COUNT; i++) {
        uint8_t code[PROLOGUE]; char hex[65];
        if (!env.rd(st.main + FN[i].off, code, sizeof code)) return 0;
        companion_sha256_hex(code, sizeof code, hex);
        if (strcmp(hex, expected(i))) { fprintf(stderr, "[companion] actions unsupported: main+%#x is not the expected function\n", FN[i].off); return 0; }
    }
    return 1;
}
/* The player's battery, the way the menu finds it: component BATTERY_COMPONENT of the player actor, then that
   component's own typed-interface query (its vtable slot 0, which has to be in main's code) for the battery type. */
static uint64_t battery(void) {
    uint64_t player = rd64(st.main + PLAYER_SLOT);
    uint64_t comp = player ? game(F_COMPONENT, player, BATTERY_COMPONENT, 0, 0, 0, 0) : 0;
    uint64_t query = comp ? rd64(rd64(comp)) : 0;
    if (!query || query < st.main || query >= st.main + MAIN_TEXT) return 0;
    return env.call(query, comp, game(F_BATTERY_TYPE, 0, 0, 0, 0, 0, 0), 0, 0, 0, 0);
}
static void refresh(CompanionView *v, uint64_t frame) {
    companion_view(st.main, env.rd, -1, 0, v);
    st.reason = st.guard < 0 ? "unsupported" : state_reason(v);
    /* After any blocked state (title, loading, an event, a menu, a save) the world must stay ok for SETTLE_MS and
       SETTLE_FRAMES with the same player actor and partition before anything runs. On the Thor a save load passes
       through ticks that look ok (event 0, menu 0) between the load confirmation and the loading screen and again
       right after it; this keeps actions out of that window. Until then the reason is "loading". A gap without ticks
       (nothing was watching) also starts over: the state in between was not seen. */
    if (*st.reason || !st.settled || v->actor != st.settle_actor || v->part != st.settle_part || now() - st.at > STALE_MS) {
        st.settled = !*st.reason; st.settle_at = now(); st.settle_frame = frame; st.settle_actor = v->actor; st.settle_part = v->part;
    }
    if (!*st.reason && (now() - st.settle_at < SETTLE_MS || frame - st.settle_frame < SETTLE_FRAMES)) st.reason = "loading";
    st.pause_menu = !strcmp(v->status, "ready") && v->pause_menu;
    st.weapon_change = !strcmp(v->status, "ready") && (v->player_flags >> BIT_WEAPON_CHANGE & 1);
    if (st.guard > 0 && !*st.reason) {
        st.aim_mode = game_bool(F_AIM, 0, 0, 0, 0);   /* bow drawn: the quick menu's take-out fuses to arrows instead */
        st.hold_material = game_bool(F_CAN_HOLD_MATERIAL, v->pouch, 0, 0, 0);
        st.hold_zonai = game_bool(F_CAN_HOLD_ZONAI, v->pouch, 0, 0, 0);
        st.arrows = game_bool(F_CAN_USE, v->pouch, 2, 0, 1);
        st.has_battery = game_bool(F_FLAG_BOOL, BATTERY_OWNED, 0, 0, 0);
        st.battery = st.has_battery ? battery() : 0;
        if (st.battery && !(env.rd(st.battery + BAT_ENERGY, &st.energy, 4) && env.rd(st.battery + BAT_RESERVE, &st.energy_reserve, 4) &&
                            env.rd(st.main + ENERGY_LIMIT, &st.energy_limit, 4) && env.rd(st.main + ENERGY_RESERVE_CAP, st.reserve_cap, 8)))
            st.battery = 0;
        if (st.battery) st.energy_max = env.call_f(st.main + FN[F_ENERGY_MAX].off, st.battery + BAT_GAUGE, 0);
    } else {
        st.aim_mode = st.hold_material = st.hold_zonai = st.arrows = st.has_battery = 0;
        st.battery = 0;
    }
    st.aiming = st.aim_mode;
    st.at = now();
}
static void resolve_infos(void) {
    uint64_t table = rd64(rd64(st.main + INFO_SLOT) + INFO_TABLE);
    if (!table || st.guard <= 0) return;
    for (unsigned i = 0, done = 0; i < ninfos && done < INFO_PER_TICK; i++) {
        Info *f = &infos[i];
        if (f->state) continue;
        done++;
        const char *name = f->name;
        uint64_t rec = game(F_INFO, table, (uint64_t)(uintptr_t)&name, 0, 0, 0, 0);
        char use[40] = {0}; uint64_t s = rec ? rd64(rec + INFO_USE_TYPE) : 0;
        f->state = -1;
        if (rec && env.rd(rec + INFO_RECOVER, &f->recover, 4) && env.rd(rec + INFO_EDIBLE, &f->edible, 1) &&
            env.rd(rec + INFO_ENERGY, &f->energy, 4) && env.rd(rec + INFO_ENERGY_TIME, &f->energy_time, 4)) {
            if (s) for (unsigned k = 0; k + 1 < sizeof use && env.rd(s + k, &use[k], 1) && use[k]; k++) {}
            use[sizeof use - 1] = 0; f->use = (unsigned char)use_kind(use); f->state = 1;
        }
    }
}
/* The pause menu's energy branch before its eat call (main+0x2174b48): unlimited-energy time for an item that gives it
   (the menu's own field writes, then refill to full), otherwise recharge by the item's EnergyRecover. The menu's gauge
   animation call (on its screen object) has no screen here and is left out. 0: no battery. */
static int recharge(const Info *f) {
    uint64_t bat = battery();
    if (!bat) return 0;
    if (f->energy_time >= 1) {
        float t = (float)f->energy_time, rate = -1.0f; uint8_t flags = 0; uint32_t zero = 0;
        if (!env.rd(bat + BAT_TIME_FLAGS, &flags, 1)) return 0;
        flags |= 8;
        env.wr(bat + BAT_TIME_FLAGS, &flags, 1); env.wr(bat + BAT_TIME_RATE, &rate, 4);
        env.wr(bat + BAT_TIME, &t, 4); env.wr(bat + BAT_TIME_LEFT, &t, 4);
        game(F_REFILL, bat, 1, 0, 0, 0, 0);
        env.wr(bat + BAT_TIME_SHOWN, &t, 4); env.wr(bat + BAT_TIME_PHASE, &zero, 4);
    } else env.call_f(st.main + FN[F_RECHARGE].off, bat, (float)f->energy);
    return 1;
}
/* Run the queued request: everything re-checked with fresh reads on this thread, then exactly one game call. */
static void run(uint64_t frame) {
    CompanionView v;
    companion_view(st.main, env.rd, req.key, req.slot, &v);
    if (*st.reason) { finish("refused", st.reason); return; }
    if (req.main != st.main) { finish("failed", "teardown"); return; }
    if (v.part != (unsigned)req.part || v.ident != req.ident || strcmp(v.name, req.actor)) { finish("refused", "itemMoved"); return; }
    CompanionAction act[3]; unsigned n = rules(req.key, &v, act);
    const char *why = "notOffered";
    for (unsigned i = 0; i < n; i++) if (act[i].id == ACTION[req.action]) why = act[i].reason;
    if (*why) { finish("refused", why); return; }
    int cat = companion_category_game(req.key), a = req.action;
    uint64_t p = v.pouch;
    /* the game's own checks the menus make right before calling */
    if ((a == A_EQUIP || a == A_UNEQUIP) && game_bool(F_IS_EQUIPPED, p, (uint64_t)cat, (uint64_t)req.slot, 0) != (a == A_UNEQUIP)) {
        finish("refused", "itemMoved"); return;
    }
    if ((a == A_EAT || a == A_DRINK || a == A_USE || a == A_TAKE_OUT || a == A_FUSE) && !game_bool(F_CAN_USE, p, (uint64_t)cat, (uint64_t)req.slot, 1)) {
        finish("refused", "noStock"); return;
    }
    if ((a == A_TAKE_OUT || a == A_FUSE) && game_bool(F_AIM, 0, 0, 0, 0) != (a == A_FUSE)) { finish("refused", a == A_FUSE ? "notAiming" : "aiming"); return; }
    req.before_stock = v.stock;
    int r = 0;
    switch (a) {
    case A_EQUIP: r = game_bool(F_EQUIP, p, (uint64_t)cat, (uint64_t)req.slot, (uint32_t)-1); break;
    case A_UNEQUIP: r = (game(F_UNEQUIP, p, (uint64_t)cat, (uint64_t)req.slot, (uint32_t)-1, 0, 1) & 0xff) != 0; break;
    case A_EAT: case A_DRINK: case A_USE: {
        const Info *f = info(req.actor, 0);
        if (a == A_USE && f && f->state > 0 && f->use == USE_ENERGY && !recharge(f)) { finish("refused", "notNow"); return; }
        r = game_bool(F_EAT, p, (uint64_t)cat, (uint64_t)req.slot, 0);   /* the menu eats even if that declines */
        break;
    }
    case A_TAKE_OUT: case A_FUSE: {
        game(F_TAKE_OUT, p, (uint64_t)req.slot, (uint64_t)cat, 0, 0, 0);   /* always returns 1 */
        int32_t rc = -1, rs = -1; uint8_t posted = 0;
        env.rd(p + POUCH_REQ_CAT, &rc, 4); env.rd(p + POUCH_REQ_SLOT, &rs, 4); env.rd(p + POUCH_REQ_POSTED, &posted, 1);
        req.posted = rc == cat && rs == req.slot && posted;
        r = 1;
        break;
    }
    default: break;
    }
    fprintf(stderr, "[companion] request %d %s %s/%d %s: called frame %llu thread %ld returned %d\n", req.seq, ACTION[a],
            companion_category_key(req.key), req.slot, req.actor, (unsigned long long)frame, tid(), r);
    if (!r) { finish("refused", "gameRefused"); return; }   /* the game's own function declined: nothing changed */
    req.phase = PH_CALLED; req.t_called = now(); req.frame_called = frame;
}
/* The outcome, from the game state on a later frame. */
static void read_back(uint64_t frame) {
    if (frame <= req.frame_called) return;
    CompanionView v;
    int ready = companion_view(st.main, env.rd, req.key, req.slot, &v);
    int same = ready && !strcmp(v.name, req.actor), done = 0;
    switch (req.action) {
    case A_EQUIP: done = same && v.equipped; break;
    case A_UNEQUIP: done = ready && !v.equipped; break;
    case A_EAT: case A_DRINK: case A_USE: case A_TAKE_OUT: done = ready && (!same || (v.stock != INT32_MIN && v.stock < req.before_stock)); break;
    case A_FUSE: done = req.posted; break;
    default: break;
    }
    if (done) finish("done", "");
    else if (!ready && !strcmp(v.status, "title")) finish("failed", "teardown");
    else if (now() - req.t_called > READBACK_MS && frame - req.frame_called >= READBACK_FRAMES) finish("failed", "timeout");   /* never retried */
}

void companion_tick(uintptr_t main_base, uint64_t frame) {
    pthread_mutex_lock(&lock);
    if (main_base != st.main) {   /* a new mapping of main: nothing checked, nothing known about it */
        st.main = main_base; st.guard = 0; st.at = 0; st.reason = "stale"; st.settled = 0; ninfos = 0;
        if (req.phase) finish("failed", "teardown");
    }
    if (main_base && !st.guard) {
        CompanionView v;
        if (companion_view(main_base, env.rd, -1, 0, &v) || !strcmp(v.status, "unsupported")) st.guard = check_guard() ? 1 : -1;
    }
    uint64_t t = now();
    if (main_base && (req.phase == PH_QUEUED || !st.at || t - st.at >= STATE_MS)) { CompanionView v; refresh(&v, frame); }
    if (main_base) resolve_infos();
    if (req.phase == PH_QUEUED) run(frame);
    else if (req.phase == PH_CALLED) read_back(frame);
    int wanted = 0;
    for (unsigned i = 0; i < ninfos && !wanted; i++) wanted = !infos[i].state;
    wake(req.phase || t < watch_until || (wanted && st.guard > 0));
    pthread_mutex_unlock(&lock);
}

