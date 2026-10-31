/* Run-time switches for the "xpatch" sites (tools/xpatch.py).
 *
 * An xpatch site is lifted twice, the original code and a patched version
 * behind `if (g_xpatch[XP_<name>])`.  This file owns the switch arrays; a
 * feature turns its patch on with xpatch_set("name", 1) or by writing
 * g_xpatch[XP_<NAME>] directly, and may change it while the game runs.
 * g_xpval[] holds the values of the `wv` sites.  With no sites in
 * tools/xpatch_eu.txt both arrays have one unused entry and the generated
 * code is unchanged. */
#include <stdio.h>
#include <string.h>
#include "xpatch.h"

unsigned char g_xpatch[XP_COUNT > 0 ? XP_COUNT : 1];
unsigned int  g_xpval[XPV_COUNT > 0 ? XPV_COUNT : 1];

/* 1 if the switch exists */
int xpatch_set(const char *name, int on){
    for (int i = 0; i < XP_COUNT; i++)
        if (!strcmp(xp_names[i], name)){ g_xpatch[i] = on ? 1 : 0; return 1; }
    return 0;
}

void xpatch_report(void){
    int n = 0;
    for (int i = 0; i < XP_COUNT; i++) n += g_xpatch[i] != 0;
    fprintf(stderr, "  xpatch           : %d switches, %d on", XP_COUNT, n);
    for (int i = 0; i < XP_COUNT; i++) if (g_xpatch[i]) fprintf(stderr, " %s", xp_names[i]);
    fprintf(stderr, "\n");
}
