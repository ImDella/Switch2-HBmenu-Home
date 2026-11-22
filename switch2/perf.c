/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 */
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "perf.h"
#include "../common/common.h"

enum { PERF_WINDOW = 60 };

static const char *const phase_names[PerfPhase_Count] = {
    "wait", "draw", "copy", "present",
};

static struct {
    u64 frame_start;
    u64 last_mark;
    u64 previous_frame_start;
    u32 frames;
    u64 phase_sum[PerfPhase_Count];
    u64 phase_max[PerfPhase_Count];
    u64 interval_sum;
    u64 interval_max;
    u32 quads;
    u32 draws;
    u64 frame_number;
    double fps;
    char summary[192];
} perf;

static double ticksToMs(u64 ticks) {
    return (double)armTicksToNs(ticks) / 1000000.0;
}

void perfFrameStart(void) {
    const u64 now = armGetSystemTick();
    if (perf.previous_frame_start) {
        const u64 interval = now - perf.previous_frame_start;
        perf.interval_sum += interval;
        if (interval > perf.interval_max)
            perf.interval_max = interval;
    }
    perf.previous_frame_start = now;
    perf.frame_start = now;
    perf.frame_number++;
    perf.last_mark = now;
}

void perfMark(enum PerfPhase phase) {
    const u64 now = armGetSystemTick();
    const u64 spent = now - perf.last_mark;
    perf.phase_sum[phase] += spent;
    if (spent > perf.phase_max[phase])
        perf.phase_max[phase] = spent;
    perf.last_mark = now;
}

void perfCountDraws(uint32_t quads, uint32_t draws) {
    perf.quads = quads;
    perf.draws = draws;
}

void perfFrameEnd(void) {
    if (++perf.frames < PERF_WINDOW)
        return;
    const double frames = perf.frames;
    const double interval = ticksToMs(perf.interval_sum) / frames;
    perf.fps = interval > 0 ? 1000.0 / interval : 0.0;
    size_t used = (size_t)snprintf(perf.summary, sizeof(perf.summary),
                                   "%.1f fps (max %.1f ms)", perf.fps,
                                   ticksToMs(perf.interval_max));
    for (int i = 0; i < PerfPhase_Count && used < sizeof(perf.summary); i++) {
        used += (size_t)snprintf(perf.summary + used, sizeof(perf.summary) - used,
                                 "  %s %.2f/%.2f", phase_names[i],
                                 ticksToMs(perf.phase_sum[i]) / frames,
                                 ticksToMs(perf.phase_max[i]));
    }
    if (perf.draws && used < sizeof(perf.summary))
        snprintf(perf.summary + used, sizeof(perf.summary) - used, "  %u quads %u draws",
                 perf.quads, perf.draws);
    perf.frames = 0;
    memset(perf.phase_sum, 0, sizeof(perf.phase_sum));
    memset(perf.phase_max, 0, sizeof(perf.phase_max));
    perf.interval_sum = 0;
    perf.interval_max = 0;
}

const char *perfSummary(void) {
    return perf.summary;
}

void perfDrawOverlay(void) {
#ifdef HBMENU_PERF_OVERLAY
    const color_t yellow = MakeColor(255, 255, 0, 255);
    char counter[48];
    snprintf(counter, sizeof(counter), "frame %llu  %.1f fps",
             (unsigned long long)perf.frame_number, perf.fps);
    DrawText(interuiregular14, 8, 14, yellow, counter);
    if (perf.summary[0])
        DrawText(interuiregular14, 200, 22, yellow, perf.summary);
#endif
}
