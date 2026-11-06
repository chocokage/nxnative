// Remappable controls of the Mac front end (front_mac.m): which keys and which controller buttons give which Switch control.
// The mapping is a text file, one line per assignment (tab or space separated, # starts a comment), that NX Play's Controls settings write:
//   key  <control>  <Mac virtual key code>    the key gives the control; a control listed here loses its default keys, a key listed here its old control
//   pad  <control>  <controller button>       the controller button (named by the Switch button it gives by default: A … Down) gives the control;
//                                             a control listed here loses its default button. Only the 16 buttons (A … Down) go on a controller
// Controls: the 16 Switch buttons A B X Y LStick RStick L R ZL ZR Plus Minus Left Up Right Down (NpadButton bits 0-15, dm_input.h), the keyboard's stick
// directions LStickUp/Down/Left/Right and RStickUp/Down/Left/Right, and Shake. A file with no line for a control keeps that control's default.
// Files: DM_CONTROLS=<path>, else controls.tsv in the game's folder DM_ROOT (the Mac app's ~/Library/Application Support/nxnative/<title ID>: one game), else in
// ~/Library/Application Support/nxnative/<profile's title ID> (a build played outside its Mac app), else
// ~/Library/Application Support/nxnative/controls.tsv (every game). The front end reads it at start and again whenever the game window becomes key, so a change made in NX Play applies on returning to the game.
// A controller's buttons are remapped after pad_apple.h has turned its face buttons into Switch buttons (by position, a Nintendo pad by its labels), so one mapping
// fits every controller; raw HID pads (pad_hid_mac.h) are remapped the same way.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

enum {
    DMC_A, DMC_B, DMC_X, DMC_Y, DMC_LSTICK, DMC_RSTICK, DMC_L, DMC_R, DMC_ZL, DMC_ZR, DMC_PLUS, DMC_MINUS, DMC_LEFT, DMC_UP, DMC_RIGHT, DMC_DOWN,   // NpadButton bits
    DMC_LS_UP, DMC_LS_DOWN, DMC_LS_LEFT, DMC_LS_RIGHT, DMC_RS_UP, DMC_RS_DOWN, DMC_RS_LEFT, DMC_RS_RIGHT, DMC_SHAKE,
    DMC_COUNT, DMC_BUTTONS = DMC_LS_UP, DMC_KEYS = 128   // Mac virtual key codes are below 128
};
static const char *const dmc_names[DMC_COUNT] = {
    "A", "B", "X", "Y", "LStick", "RStick", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
    "LStickUp", "LStickDown", "LStickLeft", "LStickRight", "RStickUp", "RStickDown", "RStickLeft", "RStickRight", "Shake",
};

typedef struct {
    int8_t key[DMC_KEYS];          // virtual key code → control, -1: none
    uint16_t pad[DMC_BUTTONS];     // controller button (by its default Switch button) → the Switch buttons it gives
} dm_controls;

static inline void dmc_default(dm_controls *c) {
    memset(c->key, -1, sizeof c->key);
    static const uint8_t def[][2] = {   // Nintendo layout: J K U I = A B X Y, F G stick presses, Q E Z C = L R ZL ZR, Return Tab = + −, arrows D-pad, WASD left stick, Space shake
        {38, DMC_A}, {40, DMC_B}, {32, DMC_X}, {34, DMC_Y}, {3, DMC_LSTICK}, {5, DMC_RSTICK}, {12, DMC_L}, {14, DMC_R}, {6, DMC_ZL}, {8, DMC_ZR},
        {36, DMC_PLUS}, {48, DMC_MINUS}, {123, DMC_LEFT}, {126, DMC_UP}, {124, DMC_RIGHT}, {125, DMC_DOWN},
        {13, DMC_LS_UP}, {1, DMC_LS_DOWN}, {0, DMC_LS_LEFT}, {2, DMC_LS_RIGHT}, {49, DMC_SHAKE},
    };
    for (size_t i = 0; i < sizeof def / sizeof *def; i++) c->key[def[i][0]] = (int8_t)def[i][1];
    for (int i = 0; i < DMC_BUTTONS; i++) c->pad[i] = (uint16_t)(1u << i);
}
static inline int dmc_control(const char *name) {
    for (int i = 0; i < DMC_COUNT; i++) if (!strcasecmp(name, dmc_names[i])) return i;
    return -1;
}
// Applies a mapping file's text over the defaults. Returns the number of lines it could not use; each is described in err (the first only) when err is given
static inline int dmc_parse(dm_controls *c, const char *text, char *err, size_t errcap) {
    dmc_default(c);
    uint8_t cleared_key[DMC_COUNT] = {0}, cleared_pad[DMC_BUTTONS] = {0};
    int bad = 0, line = 0;
    if (err && errcap) *err = 0;
    for (const char *p = text; p && *p; ) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char buf[256]; line++;
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, p, n); buf[n] = 0; p = e ? e + 1 : NULL;
        char *h = strchr(buf, '#'); if (h) *h = 0;
        char kind[16], ctl[32], val[32];
        int got = sscanf(buf, "%15s %31s %31s", kind, ctl, val);
        if (got <= 0) continue;   // blank or comment
        int k = dmc_control(got >= 2 ? ctl : ""), ok = 0;
        if (got == 3 && k >= 0 && !strcmp(kind, "key")) {
            char *end; long code = strtol(val, &end, 10);
            if (!*end && code >= 0 && code < DMC_KEYS) {
                if (!cleared_key[k]) { cleared_key[k] = 1; for (int i = 0; i < DMC_KEYS; i++) if (c->key[i] == k) c->key[i] = -1; }
                c->key[code] = (int8_t)k; ok = 1;
            }
        } else if (got == 3 && k >= 0 && k < DMC_BUTTONS && !strcmp(kind, "pad")) {
            int b = dmc_control(val);
            if (b >= 0 && b < DMC_BUTTONS) {
                if (!cleared_pad[k]) { cleared_pad[k] = 1; for (int i = 0; i < DMC_BUTTONS; i++) c->pad[i] &= (uint16_t)~(1u << k); }
                c->pad[b] |= (uint16_t)(1u << k); ok = 1;
            }
        }
        if (!ok) { if (!bad && err && errcap) snprintf(err, errcap, "line %d: '%s'", line, buf); bad++; }
    }
    return bad;
}
// A controller's Switch buttons (pad_apple.h's mapping) through the mapping; bits above the 16 buttons (DM_BTN_SHAKE) pass unchanged
static inline uint64_t dmc_pad(const dm_controls *c, uint64_t m) {
    uint64_t out = m & ~(uint64_t)0xFFFF;
    for (int i = 0; i < DMC_BUTTONS; i++) if (m & (1ull << i)) out |= c->pad[i];
    return out;
}
// The controller buttons (by their default Switch button) that give control k: a bit per button
static inline uint16_t dmc_pad_sources(const dm_controls *c, int k) {
    uint16_t s = 0;
    for (int i = 0; i < DMC_BUTTONS; i++) if (c->pad[i] & (1u << k)) s |= (uint16_t)(1u << i);
    return s;
}
// A key's name on a US keyboard, for the Controls panel (NX Play shows the same names)
static inline const char *dmc_key_name(int code) {
    static const char *const n[DMC_KEYS] = {
        [0] = "A", [1] = "S", [2] = "D", [3] = "F", [4] = "H", [5] = "G", [6] = "Z", [7] = "X", [8] = "C", [9] = "V", [11] = "B", [12] = "Q", [13] = "W", [14] = "E",
        [15] = "R", [16] = "Y", [17] = "T", [18] = "1", [19] = "2", [20] = "3", [21] = "4", [22] = "6", [23] = "5", [24] = "=", [25] = "9", [26] = "7", [27] = "-",
        [28] = "8", [29] = "0", [30] = "]", [31] = "O", [32] = "U", [33] = "[", [34] = "I", [35] = "P", [36] = "Return", [37] = "L", [38] = "J", [39] = "'",
        [40] = "K", [41] = ";", [42] = "\\", [43] = ",", [44] = "/", [45] = "N", [46] = "M", [47] = ".", [48] = "Tab", [49] = "Space", [50] = "`", [51] = "Delete",
        [53] = "Esc", [65] = "Keypad .", [67] = "Keypad *", [69] = "Keypad +", [75] = "Keypad /", [76] = "Enter", [78] = "Keypad -", [81] = "Keypad =",
        [82] = "Keypad 0", [83] = "Keypad 1", [84] = "Keypad 2", [85] = "Keypad 3", [86] = "Keypad 4", [87] = "Keypad 5", [88] = "Keypad 6", [89] = "Keypad 7",
        [91] = "Keypad 8", [92] = "Keypad 9", [96] = "F5", [97] = "F6", [98] = "F7", [99] = "F3", [100] = "F8", [101] = "F9", [103] = "F11", [109] = "F10",
        [111] = "F12", [115] = "Home", [116] = "Page Up", [117] = "Forward Delete", [118] = "F4", [119] = "End", [120] = "F2", [121] = "Page Down", [122] = "F1",
        [123] = "←", [124] = "→", [125] = "↓", [126] = "↑",
    };
    return code >= 0 && code < DMC_KEYS && n[code] ? n[code] : NULL;
}
