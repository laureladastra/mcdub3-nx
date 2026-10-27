/*
 * apu_query.c -- APU state a project can ask about from guest overrides.
 */
#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"

/* 1 while the front end is trapped or halted: the APU's pipeline (and its
 * clock) stands still until the driver's interrupt handler clears it
 * (apu_core.c, the frame thread). A driver loop that waits for the APU
 * from the thread that would run that handler waits for nothing. */
int mcpx_apu_fe_trapped(void)
{
    uint32_t fectl;
    if (!g_state)
        return 0;
    fectl = g_state->regs[NV_PAPU_FECTL];
    return (fectl & (NV_PAPU_FECTL_FEMETHMODE_TRAPPED | NV_PAPU_FECTL_FEMETHMODE_HALTED)) != 0;
}
