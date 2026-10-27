/* Fast travel (v2.3): the behaviour of CrabJournal's NFSU2FastTravel, re-implemented
 * for the recompiled game.  Nothing of the plugin's code is used: its source and release
 * binary were read for what they change, each spot was found and checked by
 * disassembling the European EXE (kit docs/v2.3-research/fasttravel.md).
 *
 * Free Roam, pause menu, world map: pick a zone icon and press Accept.  The game asks
 * "Are you sure you want to engage the GPS system?".  Yes sets the GPS route as before;
 * NO now moves the car to the zone (x, y; height and heading kept, speed 0) and closes
 * the map.  Escape still just closes the dialog.  With a GPS route active the game shows
 * its one-button "cancel GPS" dialog instead (unchanged): cancel the route first.
 *
 *   dialog  hook 0x4b1157   in the Set-GPS dialog builder 0x4b10c0 (esi = map screen),
 *                           before call 0x5587a0: the "No" message [esp+0x14]
 *                           (0xb4623f67, also the Back message [esp+0x18]) becomes
 *                           FT_MSG, so No and Escape can be told apart.
 *   travel  hook 0x4e08e6   GameWorldMapScreen::NotificationMessage (vtable 0x797f28,
 *                           slot 2), eax = [ebp+8] = message: FT_MSG moves the car
 *                           (zone = [[screen+0x4c]+0x14]+0xc, position [zone]+4/+8;
 *                           car = TheWorld [0x890080] +0x1c = first player car, body =
 *                           [[car+0x34]+0x20], position body+0x20/+0x24, velocity
 *                           body+0x70..0x7b), then the message becomes Back 0x911ab364,
 *                           which closes the map.  Zones with data at zone+0xc (photo
 *                           shoots) get the car 8 units off in x and y, so the shoot does
 *                           not start and lock the input.
 *
 *   NFSU2_FASTTRAVEL=0      off (the dialog behaves as in the original game) */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(NFS_EXE_NA)
void fasttravel_install(void){}
int  fasttravel_hook(CPU *c, uint32_t ip){ (void)c; (void)ip; return 0; }
void fasttravel_report(void){}
#else

/* ---- European v1.2 addresses ---- */
#define HOOK_DIALOG   0x004b1157u   /* call 0x5587a0 (Yes/No dialog), esi = screen  */
#define HOOK_MSG      0x004e08e6u   /* cmp eax, 0x9120409e: eax = message, ecx = this */
#define VT_WORLDMAP   0x00797f28u   /* GameWorldMapScreen vtable                     */
#define A_WORLD       0x00890080u   /* TheWorld                                      */
#define MSG_NO        0xb4623f67u
#define MSG_BACK      0x911ab364u
#define FT_MSG        0x46540001u   /* our own "No = travel" message                */

static int g_on = -1;
static uint64_t g_n_dialog, g_n_travel, g_n_fail;

void fasttravel_install(void){
    const char *e = getenv("NFSU2_FASTTRAVEL");
    g_on = !(e && *e == '0');
    fprintf(stderr, "  [fasttravel] %s\n", g_on ? "on: world map, GPS dialog No = travel" : "off");
}

static uint32_t rd(uint32_t a){ return (a && mem_is_committed(a) && mem_is_committed(a + 3u)) ? MR32(a) : 0u; }

/* the zone and the car body, or 0 when something in the chain isn't there */
static int travel(uint32_t screen){
    uint32_t sel = rd(rd(screen + 0x4cu) + 0x14u);
    uint32_t zone = sel ? sel + 0xcu : 0u, obj = rd(zone);
    uint32_t car = rd(rd(A_WORLD) + 0x1cu), body = rd(rd(car + 0x34u) + 0x20u);
    if (!obj || !body || !mem_is_committed(obj + 11u) || !mem_is_committed(body + 0x7bu)){
        if (g_n_fail++ < 10)
            fprintf(stderr, "  [fasttravel] no target (zone 0x%08x, obj 0x%08x, car 0x%08x, body 0x%08x)\n",
                    zone, obj, car, body);
        return 0;
    }
    float x = MRF32(obj + 4u), y = MRF32(obj + 8u);
    int shoot = rd(zone + 0xcu) != 0;
    if (shoot){ x += 8.f; y += 8.f; }
    if (g_n_travel < 50)
        fprintf(stderr, "  [fasttravel] car %.1f, %.1f -> %.1f, %.1f%s\n",
                MRF32(body + 0x20u), MRF32(body + 0x24u), x, y, shoot ? " (photo shoot, offset 8)" : "");
    MRF32(body + 0x20u) = x; MRF32(body + 0x24u) = y;
    MR32(body + 0x70u) = 0; MR32(body + 0x74u) = 0; MR32(body + 0x78u) = 0;
    g_n_travel++;
    return 1;
}

int fasttravel_hook(CPU *c, uint32_t ip){
    if (ip == HOOK_DIALOG){
        if (g_on > 0 && rd(c->esi) == VT_WORLDMAP && MR32(c->esp + 0x14u) == MSG_NO){
            MR32(c->esp + 0x14u) = FT_MSG; g_n_dialog++; }
        return 1;
    }
    if (ip == HOOK_MSG){
        if (c->eax == FT_MSG && rd(c->ecx) == VT_WORLDMAP){
            if (g_on > 0) travel(c->ecx);
            c->eax = MSG_BACK; MR32(c->ebp + 8u) = MSG_BACK;   /* close the map */
        }
        return 1;
    }
    return 0;
}

void fasttravel_report(void){
    if (g_on < 0) return;
    fprintf(stderr, "  [fasttravel] %s: %llu GPS dialogs, %llu travels, %llu without target\n",
            g_on ? "on" : "off", (unsigned long long)g_n_dialog,
            (unsigned long long)g_n_travel, (unsigned long long)g_n_fail);
}
#endif
