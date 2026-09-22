/*
 * com_id.h — the m2-hle2 Communication ID standard, and the YAMP one we browse.
 *
 * RPCN partitions EVERYTHING by ComId. The server list, the world list, the room
 * list, room creation and room search all carry one, and two clients see each
 * other's rooms only if their ComIds match byte for byte. That makes the ComId
 * the one field deciding which lobby space a game plays in — and it means a build
 * that hardcodes a single id drops every Model 2 ROM set into the same list,
 * where a Virtua Cop cabinet advertises next to a Sonic The Fighters one, both
 * look joinable, and the mismatch is discovered by the netcode instead of by the
 * browser. So: one ComId per game, assigned by a rule, not by hand.
 *
 *   M2H <6-char game code> _ <2-digit revision>     12 chars, e.g. "M2HSNCFTR_00"
 *   ^^^                     ^
 *   |                       netcode revision channel (see COMID_REVISION)
 *   the m2-hle2 namespace
 *
 * WHY THAT SHAPE. RPCN reads the ComId as exactly 12 bytes and rejects the
 * request as Malformed unless the first 9 are ASCII uppercase letters or digits;
 * bytes 9-11 it does not police, and PSN's own ids spell them "_NN". Nine
 * characters is the whole usable namespace: three go to the emulator so a lobby
 * space can never collide with a real PSN title's, and six identify the game.
 *
 * It works against a stock RPCN server because of `CreateMissing`:
 * GetServerList/GetWorldList on an unknown ComId REGISTER it, so a new game needs
 * no servers.cfg edit on anybody's part. That is also why the derivation has to
 * be deterministic — both peers compute the same id from the same game key with
 * no central registry to agree on.
 *
 * WHY NOT SHARE YAMP's "YMP" SPACE, given yampnet is the model for all of this.
 * m2-hle2 runs the ARCADE ROM; YAMP hosts the console ports Yakuza ships. Two
 * different binaries simulating the same game cannot lockstep, so a shared room
 * list would advertise rooms that always desync on join. Rooms are therefore made
 * under M2H — but the browser also SEARCHES the YMP space read-only for the games
 * both emulators host, because "nobody is online" and "three people are online in
 * an emulator you cannot play against" are different things to be told, and the
 * second one is worth knowing when you are deciding whether to wait.
 *
 * The YAMP codes below are yampnet's own (source/ComId.cpp), copied for the
 * overlap only. A game YAMP does not host has no meaningful YMP space: yampnet
 * derives unlisted ids by hashing whatever key its HOST passes, which is YAMP's
 * arcadeName and not a string this emulator has, so a hash computed here would
 * name a lobby nobody is ever in. comid_yamp_for_game() reports that honestly
 * instead of inventing one.
 */
#ifndef COM_ID_H
#define COM_ID_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define COMID_LENGTH      12
#define COMID_BUFFER_SIZE (COMID_LENGTH + 1)
#define COMID_CODE_LENGTH 6

#define COMID_PREFIX      "M2H"
#define COMID_PREFIX_YAMP "YMP"

/*
 * The revision channel: the "_NN" tail, which keeps incompatible builds in
 * separate lobby spaces instead of letting them meet and desync.
 *
 * Bump ONLY for a change that breaks peer-to-peer compatibility with the previous
 * build — a packet layout change, a different input encoding, a lockstep rule
 * both sides must agree on, or a board change that alters the simulation from a
 * cold reset. Bumping strands everyone still on the old build in the old space,
 * which is the point, and the reason not to bump it for a change that would have
 * interoperated.
 */
#define COMID_REVISION 0

typedef struct {
    const char *key;    /* already normalised: uppercase, [A-Z0-9] only */
    const char *code;   /* exactly COMID_CODE_LENGTH characters         */
    const char *yamp;   /* YAMP's code for the same arcade game, or NULL */
} comid_entry_t;

/*
 * Hand-assigned codes, so ids are readable in a log and stay put forever. This
 * table is a CONVENIENCE, not the mechanism: an unlisted game still gets a lobby
 * space of its own from the hash below, and adding it here later MOVES it — so
 * add a game before people play it, or leave it derived.
 *
 * Keys are game_profile_t.id first (that is what netplay passes), with the MAME
 * romset names and the obvious spellings beside them, because the key is whatever
 * the caller sends and two spellings would otherwise be two lobby lists.
 *
 * ON ROM REVISIONS. The granularity is the GAME, so a parent and its clones share
 * a space. That is a deliberate limit: a ComId is resolved before login, from a
 * key, and it partitions the room list rather than checking anything. Telling a
 * peer its revision differs is the room attribute word's job (see netplay.h,
 * which publishes the profile and protocol revision there), where the joiner can
 * be shown why. Splitting revisions here would only hide it behind an empty list.
 */
static const comid_entry_t g_comid_registry[] = {
    /* Sonic The Fighters — the reference title, and the first game with netplay. */
    { "SFIGHT",             "SNCFTR", "SNCFTR" },
    { "SCHAMP",             "SNCFTR", "SNCFTR" },
    { "SONICTHEFIGHTERS",   "SNCFTR", "SNCFTR" },
    { "SONICCHAMPIONSHIP",  "SNCFTR", "SNCFTR" },
    { "STF",                "SNCFTR", "SNCFTR" },
    /* Sonic the Fighters - Console: the same ROM with the console release's
     * patches (profiles/sfight_console.h). It simulates differently from the
     * arcade game, so it is a lobby space of its own. YAMP runs that console
     * emulator, so its browse space is YAMP's STF one. */
    { "SFIGHTCONSOLE",      "SNCFTC", "SNCFTR" },

    /* Fighting Vipers — boots on the same board layer. */
    { "FVIPERS",            "FGTVPR", "FGTVPR" },
    { "FIGHTINGVIPERS",     "FGTVPR", "FGTVPR" },
    { "FV",                 "FGTVPR", "FGTVPR" },

    /* The rest of the fighters, listed ahead of support so their ids never move. */
    { "VF2",                "VRTFT2", "VRTFT2" },
    { "VIRTUAFIGHTER2",     "VRTFT2", "VRTFT2" },
    { "LASTBRONX",          "LSTBRX", NULL },
    { "DOA",                "DEDALV", NULL },
    { "DEADORALIVE",        "DEDALV", NULL },

    /* Linked-cabinet games. YAMP hosts Motor Raid and Virtual On too. */
    { "MOTORAID",           "MTRRAD", "MTRRAD" },
    { "MOTORRAID",          "MTRRAD", "MTRRAD" },
    { "VON",                "VRTLON", "VRTLON" },
    { "VIRTUALON",          "VRTLON", "VRTLON" },
    { "CYBERTROOPERS",      "VRTLON", "VRTLON" },

    /* Racers and light-gun games — Model 2 through 2C. */
    { "DAYTONA",            "DYTUSA", NULL },
    { "DAYTONAUSA",         "DYTUSA", NULL },
    { "SRALLYC",            "SGRALY", NULL },
    { "SEGARALLY",          "SGRALY", NULL },
    { "MANXTT",             "MANXTT", NULL },
    { "VCOP",               "VRTCOP", NULL },
    { "VIRTUACOP",          "VRTCOP", NULL },
    { "VCOP2",              "VRTCP2", NULL },
    { "VIRTUACOP2",         "VRTCP2", NULL },
    { "VSTRIKER",           "VRTSTR", NULL },
    { "DYNAMCOP",           "DYNCOP", NULL },
    { "TOPSKATR",           "TOPSKT", NULL },

    /* The homebrew that runs on this board layer. */
    { "M2SNAKE",            "M2SNAK", NULL },
};

static inline bool comid_is_id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/*
 * Case folded, everything outside [A-Z0-9] dropped. "Sonic The Fighters",
 * "sonic_the_fighters" and "SONICTHEFIGHTERS" are one key on purpose: a
 * difference in how a caller spells a title must not silently split its players
 * across two lobby lists.
 */
static inline uint32_t comid_normalise(const char *in, char *out, uint32_t cap) {
    uint32_t n = 0;
    for (const char *p = in; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (comid_is_id_char(c)) out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

/* FNV-1a. Four lines long and identical on every compiler: two peers must derive
 * the same id from the same key, and this runs on both of them. */
static inline uint32_t comid_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (const char *p = s; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

/* 6 characters of base32 = 30 bits, so the top two are folded back in rather than
 * dropped. The alphabet is RFC 4648's, entirely inside [A-Z0-9] — no padding, no
 * lowercase, nothing RPCN rejects. */
static inline void comid_encode_code(uint32_t hash, char *out) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint32_t h = hash ^ (hash >> 30);
    for (uint32_t i = 0; i < COMID_CODE_LENGTH; i++) {
        uint32_t shift = 5 * (COMID_CODE_LENGTH - 1 - i);
        out[i] = kAlphabet[(h >> shift) & 31u];
    }
    out[COMID_CODE_LENGTH] = '\0';
}

static inline bool comid_code_usable(const char *code) {
    if (!code || strlen(code) != COMID_CODE_LENGTH) return false;
    for (uint32_t i = 0; i < COMID_CODE_LENGTH; i++)
        if (!comid_is_id_char(code[i])) return false;
    return true;
}

static inline void comid_compose(const char *prefix, const char *code,
                                 char out[COMID_BUFFER_SIZE]) {
    snprintf(out, COMID_BUFFER_SIZE, "%s%s_%02u", prefix, code, (unsigned)(COMID_REVISION % 100u));
}

/* RPCN's own rule, applied before the request is framed rather than after the
 * server has rejected it: 9 to 12 characters, of which the first 9 are ASCII
 * uppercase or digits. */
static inline bool comid_is_well_formed(const char *id) {
    if (!id) return false;
    size_t len = strlen(id);
    /* Under 9 leaves a NUL inside the region the server validates; over 12 would
     * be silently truncated, which is a different id than the caller asked for
     * and is better refused than quietly honoured. */
    if (len < 9 || len > COMID_LENGTH) return false;
    for (uint32_t i = 0; i < 9; i++) if (!comid_is_id_char(id[i])) return false;
    return true;
}

/*
 * The full canonical shape — 9 of [A-Z0-9], then '_', then two digits.
 *
 * This, not comid_is_well_formed(), is what tells a ComId apart from a game key,
 * and the difference matters: "VIRTUALON" is nine uppercase letters and passes
 * RPCN's rule, so a looser test would send a game's NAME as its ComId and quietly
 * give it a lobby space nobody else computes.
 */
static inline bool comid_looks_like_id(const char *id) {
    if (!comid_is_well_formed(id) || strlen(id) != COMID_LENGTH) return false;
    return id[9] == '_' && id[10] >= '0' && id[10] <= '9' && id[11] >= '0' && id[11] <= '9';
}

static inline bool comid_is_ours(const char *id) {
    return comid_looks_like_id(id) && strncmp(id, COMID_PREFIX, 3) == 0;
}

static inline bool comid_is_yamp(const char *id) {
    return comid_looks_like_id(id) && strncmp(id, COMID_PREFIX_YAMP, 3) == 0;
}

/* A canonical id that is neither ours nor YAMP's: a real PSN title's space, or a
 * build that hardcodes one id for everything. Callers use it to decide to warn. */
static inline bool comid_is_shared_space(const char *id) {
    return comid_looks_like_id(id) && !comid_is_ours(id) && !comid_is_yamp(id);
}

/*
 * The ComId for a game key. Returns false, leaving `out` untouched, only for a
 * null or effectively empty key. `out_listed`, when given, says whether the id
 * came from the table above or from the hash — which is worth telling apart in a
 * log: a hashed id is correct and usable, but it also means nobody ELSE will
 * compute it unless they spell the game exactly the same way, and that is the
 * first thing to check when two peers cannot see each other's rooms.
 */
static inline bool comid_for_game_ex(const char *game_key, char out[COMID_BUFFER_SIZE],
                                     bool *out_listed) {
    if (out_listed) *out_listed = false;
    if (!game_key || !out) return false;

    char key[64];
    if (comid_normalise(game_key, key, sizeof(key)) == 0) return false;  /* no key at all */

    for (size_t i = 0; i < sizeof(g_comid_registry) / sizeof(g_comid_registry[0]); i++) {
        if (strcmp(g_comid_registry[i].key, key) == 0 && comid_code_usable(g_comid_registry[i].code)) {
            comid_compose(COMID_PREFIX, g_comid_registry[i].code, out);
            if (out_listed) *out_listed = true;
            return true;
        }
    }

    char code[COMID_CODE_LENGTH + 1];
    comid_encode_code(comid_hash(key), code);
    comid_compose(COMID_PREFIX, code, out);
    return true;
}

static inline bool comid_for_game(const char *game_key, char out[COMID_BUFFER_SIZE]) {
    return comid_for_game_ex(game_key, out, NULL);
}

/*
 * YAMP's ComId for the same arcade game, for the read-only cross-emulator browse.
 * False when this game is not one YAMP hosts — see the header note on why a hash
 * is not a substitute there.
 */
static inline bool comid_yamp_for_game(const char *game_key, char out[COMID_BUFFER_SIZE]) {
    if (!game_key || !out) return false;
    char key[64];
    if (comid_normalise(game_key, key, sizeof(key)) == 0) return false;
    for (size_t i = 0; i < sizeof(g_comid_registry) / sizeof(g_comid_registry[0]); i++) {
        if (strcmp(g_comid_registry[i].key, key) != 0) continue;
        if (!g_comid_registry[i].yamp || !comid_code_usable(g_comid_registry[i].yamp)) return false;
        comid_compose(COMID_PREFIX_YAMP, g_comid_registry[i].yamp, out);
        return true;
    }
    return false;
}

/*
 * What the caller supplied, turned into the ComId to actually use. `out_note` is
 * a static string, never null, suitable for a log line:
 *
 *   * an id already in the m2-hle2 namespace  -> used verbatim.
 *   * any other canonical ComId               -> used verbatim, and NOTED, because
 *                                                deliberately joining another
 *                                                emulator's or a real title's
 *                                                lobbies is legitimate — and is
 *                                                also exactly what a build that
 *                                                hardcodes one id looks like.
 *   * anything else                           -> read as a game key and derived.
 */
static inline bool comid_resolve(const char *input, char out[COMID_BUFFER_SIZE],
                                 const char **out_note) {
    const char *note = "";
    bool ok = false;

    if (!input || !*input) {
        note = "no communication id or game key was given";
    } else if (comid_is_ours(input)) {
        snprintf(out, COMID_BUFFER_SIZE, "%s", input);
        note = "supplied by the caller, already in the m2-hle2 namespace";
        ok = true;
    } else if (comid_looks_like_id(input)) {
        snprintf(out, COMID_BUFFER_SIZE, "%s", input);
        note = comid_is_yamp(input)
             ? "a YAMP lobby space: rooms there are hosted by a different emulator "
               "running a different build of the game, so a match cannot stay in sync"
             : "a fixed id outside the m2-hle2 namespace: every game using it shares "
               "one lobby list, so pass the game's name instead to get a space of its own";
        ok = true;
    } else {
        bool listed = false;
        if (comid_for_game_ex(input, out, &listed)) {
            note = listed ? "the listed id for this game"
                          : "derived by hashing the game key - no entry in the table, so a peer "
                            "reaches it only by spelling the game the same way";
            ok = true;
        } else {
            note = "not a usable communication id, and no letters or digits to derive one from";
        }
    }

    if (out_note) *out_note = note;
    return ok;
}

#endif /* COM_ID_H */
