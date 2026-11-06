// controls.h: the defaults, a mapping file's key and pad lines, the lines it cannot use, and the controller remapping
#include "controls.h"
#include <assert.h>

int main(void) {
    dm_controls c; char err[200];
    assert(dmc_parse(&c, "", err, sizeof err) == 0);
    assert(c.key[38] == DMC_A && c.key[13] == DMC_LS_UP && c.key[49] == DMC_SHAKE && c.key[126] == DMC_UP && c.key[4] == -1);
    assert(dmc_pad(&c, 0xFFFF) == 0xFFFF && dmc_pad(&c, 1ull << 63) == 1ull << 63);

    // A control listed in the file loses its default keys; the key's old control loses it; a second line adds a key
    assert(dmc_parse(&c, "# swap\nkey A 40\nkey A 37\n\nkey RStickUp 34  # I\n", err, sizeof err) == 0);
    assert(c.key[40] == DMC_A && c.key[37] == DMC_A && c.key[38] == -1 && c.key[34] == DMC_RS_UP);
    assert(c.key[32] == DMC_X);   // untouched

    // Pad lines: A and B swapped; the buttons not listed keep theirs
    assert(dmc_parse(&c, "pad A B\npad B A\n", err, sizeof err) == 0);
    assert(dmc_pad(&c, 1u << DMC_B) == 1u << DMC_A && dmc_pad(&c, 1u << DMC_A) == 1u << DMC_B && dmc_pad(&c, 1u << DMC_X) == 1u << DMC_X);
    assert(dmc_pad_sources(&c, DMC_A) == 1u << DMC_B);
    // One button giving two controls; a control with no button left
    assert(dmc_parse(&c, "pad ZR R\n", err, sizeof err) == 0);
    assert(dmc_pad(&c, 1u << DMC_R) == ((1u << DMC_R) | (1u << DMC_ZR)) && dmc_pad(&c, 1u << DMC_ZR) == 0);

    // Lines it cannot use: counted, the first described, the rest of the file still applied
    assert(dmc_parse(&c, "key Nope 3\npad LStickUp A\nkey A 200\nwhat\nkey B 0\n", err, sizeof err) == 4);
    assert(strstr(err, "line 1") && c.key[0] == DMC_B);
    assert(!strcmp(dmc_key_name(36), "Return") && !strcmp(dmc_key_name(126), "↑") && dmc_key_name(10) == NULL);
    puts("t_controls: ok");
    return 0;
}
