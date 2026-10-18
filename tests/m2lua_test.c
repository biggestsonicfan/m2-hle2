/*
 * m2lua_test.c -- the game scripts (core/m2lua.h, Pinboard #958) on a bare
 * memory bus and a made-up ROM set. No ROM, no window.
 *
 *   - M2emulator's AND/OR/XOR/NOT/SHL/SHR: unsigned 32-bit results, floats
 *     rounded, SHR logical; and Lua 5.5's own & | ~ << >> beside them.
 *   - I960_Read/Write* through the bus, Romset_Read/Patch* on the regions,
 *     and the patches coming off on suspend and unload.
 *   - A script file: Options (names, values, defaults), Init, Frame, the
 *     runfuncs of the options that are on, changefunc, PostDraw's list.
 *   - The options as RetroArch cheats (the libretro core): the .cht text and
 *     the codes retro_cheat_set hands over.
 *
 *   - require("model2") with no scripts folder: the built-in stand-ins.
 *
 * A ctest (CMakeLists.txt, m2lua_test). Writes m2lua_test_set.lua and
 * model2.lua in the working directory and deletes them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m2lua.h"

static memory_bus_t bus;
static romset_t     rom;
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } \
                              else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

/* The chunk's results, joined by tabs. */
static const char *eval(const char *code) {
    static char out[512];
    if (!m2lua_eval(code, out, sizeof out)) printf("  (error: %s)\n", out);
    return out;
}

static void test_bits(void) {
    CHECK(strcmp(eval("return NOT(0)"), "4294967295") == 0, "NOT(0) is 0xFFFFFFFF, unsigned");
    CHECK(strcmp(eval("return AND(-1, 0xF0F0)"), "61680") == 0, "AND takes -1 as 0xFFFFFFFF");
    CHECK(strcmp(eval("return OR(0x80000000, 1)"), "2147483649") == 0, "OR keeps bit 31 unsigned");
    CHECK(strcmp(eval("return XOR(0xFF, 0x0F)"), "240") == 0, "XOR");
    CHECK(strcmp(eval("return SHL(1, 31), SHL(1, 32)"), "2147483648\t1") == 0, "SHL masks the count to 5 bits");
    CHECK(strcmp(eval("return SHR(0x80000000, 31)"), "1") == 0, "SHR is logical");
    CHECK(strcmp(eval("return AND(2.6, 3)"), "3") == 0, "a float argument is rounded");
    CHECK(strcmp(eval("return 0xF0 & 0x3C, 0xF0 | 0x0F, 0xFF ~ 0x0F, ~0 & 0xFFFFFFFF, 1 << 4, 0x80 >> 7"),
                 "48\t255\t240\t4294967295\t16\t1") == 0, "Lua 5.5's own bitwise operators");
    CHECK(strcmp(eval("return math.type(AND(1, 1)), 7 // 2"), "integer\t3") == 0,
          "the helpers return integers; floor division is there");
}

static void test_bus(void) {
    eval("I960_WriteDWord(0x00500010, 0x12345678)");
    CHECK(mem_read32(&bus, 0x00500010) == 0x12345678u, "I960_WriteDWord lands on the bus");
    CHECK(strcmp(eval("return I960_ReadByte(0x00500010), I960_ReadWord(0x00500012)"), "120\t4660") == 0,
          "I960_ReadByte/Word read it back little-endian");
    CHECK(strcmp(eval("return I960_GetRamPtr(0x00500000) ~= 0"), "true") == 0, "I960_GetRamPtr: RAM has a host address");
}

static void test_romset(void) {
    CHECK(strcmp(eval("return Romset_ReadDWord(0, 4)"), "3721247658") == 0, "Romset_ReadDWord, area 0 is the program ROM");
    eval("Romset_PatchWord(0, 4, 0x1234)");
    CHECK(rom.maincpu[4] == 0x34 && rom.maincpu[5] == 0x12 && rom.maincpu[6] == 0xCD, "Romset_PatchWord patches two bytes");
    CHECK(strcmp(eval("return Romset_ReadByte(0, 4096), Romset_ReadByte(7, 0)"), "0\t0") == 0,
          "past the end, or an area with nothing, reads 0");
    eval("Romset_PatchByte(10, 1, 0x77)");
    CHECK(rom.tgp_tables[1] == 0x77, "area 10 is the TGP tables");
    m2lua_unpatch();
    CHECK(rom.maincpu[4] == 0xAA && rom.maincpu[5] == 0xBB && rom.tgp_tables[1] == 0, "the patches come off");
}

static const char *k_script =
    "Frames, Runs, Changes, Inits = 0, 0, 0, 0\n"
    "function Init() Inits = Inits + 1; Romset_PatchByte(0, 0, 0x99) end\n"
    "function Frame() Frames = Frames + 1 end\n"
    "function PostDraw() Video_DrawText(10, 20, 'f=' .. Frames, 0xFF0000) end\n"
    "Options = {\n"
    "  cheat = { name = 'Infinite time', values = { 'Off', 'On', 'Max' },\n"
    "            runfunc = function(v) Runs = Runs + v end,\n"
    "            changefunc = function(v) Changes = Changes * 10 + v end },\n"
    "  plain = { },\n"
    "}\n";

static bool write_script(void) {
    FILE *f = fopen("m2lua_test_set.lua", "wb");
    if (!f) return false;
    fputs(k_script, f);
    fclose(f);
    return true;
}

static void test_options(void) {
    CHECK(g_m2lua.noptions == 2, "two options");
    int c = m2lua_option_index("cheat"), p = m2lua_option_index("plain");
    CHECK(c >= 0 && strcmp(g_m2lua.options[c].name, "Infinite time") == 0 && g_m2lua.options[c].nvalues == 3,
          "an option's name and values");
    CHECK(p >= 0 && strcmp(g_m2lua.options[p].name, "plain") == 0 && g_m2lua.options[p].nvalues == 2 &&
          strcmp(g_m2lua.options[p].values[1], "On") == 0, "the defaults: the key, Off/On");
    m2lua_frame();
    CHECK(strcmp(eval("return Frames, Runs"), "1\t0") == 0, "Frame runs, an option that is off does not");
    m2lua_set_option(c, 2);
    m2lua_frame();
    CHECK(strcmp(eval("return Frames, Runs, Changes, Options.cheat.value"), "2\t2\t2\t2") == 0,
          "changefunc(2), then runfunc(2) each frame; the table sees the value");
}

/* The libretro core's cheats: a .cht entry per value other than the default,
 * codes by index or by name, back to the defaults when none is on. */
static void test_cheats(void) {
    static char cht[4096];
    int len = m2lua_cht_text(cht, (int)sizeof cht);
    CHECK(len > 0 && strstr(cht, "cheats = \"3\"") && strstr(cht, "cheat0_desc = \"Infinite time: On\"") &&
          strstr(cht, "cheat0_code = \"cheat=1\"") && strstr(cht, "cheat2_code = \"plain=1\"") &&
          !strstr(cht, "cheat=0"), "the .cht: one cheat per value but the default");
    int c = m2lua_option_index("cheat"), p = m2lua_option_index("plain");
    int want[M2LUA_OPTION_MAX];
    m2lua_cheat_defaults(want);
    CHECK(m2lua_cheat_code("cheat=Max+plain=1", want) == 2 && want[c] == 2 && want[p] == 1,
          "a code: by name and by index, joined by +");
    CHECK(m2lua_cheat_code("nosuch=1+cheat=9+plain", want) == 0, "an unknown key, a value out of range: nothing");
    m2lua_cheat_apply(want);
    CHECK(g_m2lua.options[c].value == 2 && g_m2lua.options[p].value == 1, "applied to the options");
    m2lua_cheat_defaults(want);
    m2lua_cheat_apply(want);
    CHECK(g_m2lua.options[c].value == 0 && g_m2lua.options[p].value == 0, "every cheat off: the defaults");
}

static void test_script(void) {
    CHECK(write_script(), "wrote the script");
    m2lua_set_dir(".");
    CHECK(m2lua_load("m2lua_test_set", &bus, &rom), "the script loads");
    CHECK(strcmp(eval("return Inits"), "1") == 0 && rom.maincpu[0] == 0x99, "Init ran and patched the ROM");
    test_options();
    test_cheats();
    m2lua_set_option(m2lua_option_index("cheat"), 2);   /* as test_options left it */
    m2lua_draw_t list[M2LUA_DRAW_MAX];
    int n = 0;
    m2lua_take_draw(list, &n);
    CHECK(n == 1 && strcmp(list[0].text, "f=2") == 0 && list[0].color == 0xFFFF0000u && list[0].x == 10,
          "PostDraw's text, opaque red, at its place");
    m2lua_frame_hook(false);
    m2lua_take_draw(list, &n);
    CHECK(g_m2lua.suspended && rom.maincpu[0] == 0x11 && n == 0, "a session: patches off, nothing drawn");
    m2lua_frame_hook(true);
    CHECK(!g_m2lua.suspended && strcmp(eval("return Inits, Frames, Runs"), "1\t1\t2") == 0 && rom.maincpu[0] == 0x99,
          "the session over: the script again from its start, the option still on");
    m2lua_unload();
    CHECK(rom.maincpu[0] == 0x11 && !g_m2lua.loaded, "unload takes the patches off");
    remove("m2lua_test_set.lua");
}

/* The web page's scratchpad: the text, not a file; kept, not run, while a
 * session has the board. */
static void test_source(void) {
    CHECK(m2lua_load_source("m2lua_test_set", k_script, &bus, &rom, false) && g_m2lua.suspended &&
          g_m2lua.L == NULL && rom.maincpu[0] == 0x11, "a session has the board: the text waits, nothing patched");
    m2lua_frame_hook(true);
    CHECK(!g_m2lua.suspended && rom.maincpu[0] == 0x99 && g_m2lua.noptions == 2 &&
          strcmp(eval("return Inits, Frames"), "1\t1") == 0, "the session over: it runs");
    eval("Romset_PatchByte(0, 0, 0x42)");
    CHECK(m2lua_reload() && rom.maincpu[0] == 0x99 && strcmp(eval("return Frames"), "0") == 0,
          "reload runs the text again, not a file");
    CHECK(!m2lua_load_source("m2lua_test_set", "x = = 1", &bus, &rom, true) && rom.maincpu[0] == 0x11 &&
          strstr(g_m2lua.error, "m2lua_test_set.lua:1:") != NULL, "a bad text: off, the old patches gone, the error named");
    CHECK(!m2lua_load_source("m2lua_test_set", "", &bus, &rom, true) && !g_m2lua.loaded && !g_m2lua.source,
          "empty text: no script");
}

/* require("model2") with no SCRIPTS folder (the web's case): the built-in
 * stand-ins for M2emulator's model2.lua and common.lua; a file in the folder
 * still wins. */
static void test_builtin_modules(void) {
    static const char k_rally[] = "require(\"model2\")\nfunction Frame() Model2_SetWideScreen(1) end\n";
    m2lua_set_dir("m2lua_test_nowhere");
    CHECK(m2lua_load_source("m2lua_test_set", k_rally, &bus, &rom, true), "require(\"model2\") with no folder");
    CHECK(strcmp(eval("return RAM2BASE == 0x200000, HEX32(0xABCDEF01), HEX8(0x1FF), HEX4(10), JOY2 + JOY_BUTTON3"),
                 "true\tABCDEF01\tFF\tA\t560") == 0, "model2's map, common's HEX helpers and JOY numbers");
    m2lua_frame_hook(true);
    CHECK(g_m2lua.video.widescreen, "its Frame ran: widescreen on");
    FILE *f = fopen("model2.lua", "wb");
    if (f) { fputs("RAM2BASE = 7\n", f); fclose(f); }
    m2lua_set_dir(".");
    CHECK(m2lua_load_source("m2lua_test_set", k_rally, &bus, &rom, true) && strcmp(eval("return RAM2BASE, HEX8"), "7\tnil") == 0,
          "the folder's model2.lua wins");
    remove("model2.lua");
    m2lua_unload();
}

static void test_bad_script(void) {
    FILE *f = fopen("m2lua_test_bad.lua", "wb");
    if (f) { fputs("function Frame( end\n", f); fclose(f); }
    CHECK(!m2lua_load("m2lua_test_bad", &bus, &rom) && g_m2lua.error[0], "a script that does not compile is off, with its error");
    remove("m2lua_test_bad.lua");
}

int main(void) {
    log_init();
    if (!mem_init(&bus, NULL, 0)) { printf("FAIL: mem_init\n"); return 1; }
    rom.maincpu = calloc(1, 4096);    rom.maincpu_size = 4096;
    rom.tgp_tables = calloc(1, 16);   rom.tgp_tables_size = 16;
    rom.maincpu[0] = 0x11;
    rom.maincpu[4] = 0xAA; rom.maincpu[5] = 0xBB; rom.maincpu[6] = 0xCD; rom.maincpu[7] = 0xDD;
    m2lua_init_once();
    g_m2lua.bus = &bus;
    g_m2lua.rom = &rom;
    test_bits();
    test_bus();
    test_romset();
    m2lua_unload();
    test_script();
    test_source();
    test_builtin_modules();
    test_bad_script();
    m2lua_unload();
    printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
