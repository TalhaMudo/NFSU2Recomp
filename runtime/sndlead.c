/* sndlead.c -- keep sound effects from lagging for half a minute after a load.
 *
 * The game's sound thread writes ahead of the DirectSound cursor by 30 ms plus
 * an adaptive margin.  The margin jumps up right away when the thread wakes
 * late, but it comes back down only when a countdown (word [0x8183e4], reset
 * to 3000 ticks of 10 ms, about 30 s) runs out.  In this port the thread waits
 * for the guest lock, and a load holds that lock for a long time, so after
 * every load menu clicks, the engine and crashes played late for up to 30 s.
 *
 * Right after the game reloads the countdown (0x726336, same address in the
 * EU and NA builds), this sets it to 300 ticks (about 3 s).  A late wake-up
 * still raises the margin at once, as before.
 *
 * Idea from th0mk's nfsu2-web. */
#include "cpu.h"

#define WINDOW_TICKS 300

/* Called from guest_hook (debug.c); tools/gen_v2.sh lifts the hook site. */
int sndlead_hook(CPU *c, uint32_t ip){
    (void)c;
    if (ip != GA(0x00726336u)) return 0;
    MR16(GA(0x008183e4u)) = WINDOW_TICKS;
    return 1;
}
