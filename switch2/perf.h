/* SPDX-License-Identifier: ISC
 * Copyright 2026 nx-hbmenu Switch 2 port contributors
 *
 * Frame timing for the Switch 2 renderer. Each frame records the time spent
 * waiting for the previous frame, drawing, copying and presenting, and the
 * interval between frames. Every PERF_WINDOW frames the averages and maxima
 * are summarised; a build with HBMENU_PERF_OVERLAY draws that summary on
 * screen.
 */
#ifndef HBMENU_SWITCH2_PERF_H
#define HBMENU_SWITCH2_PERF_H

#include <stdint.h>

enum PerfPhase {
    PerfPhase_Wait,     /* previous frame's GPU work and swapchain image */
    PerfPhase_Draw,     /* menuLoop: building the frame */
    PerfPhase_Copy,     /* CPU frame or draw list to GPU memory */
    PerfPhase_Present,  /* acquire, record, submit, present */
    PerfPhase_Count,
};

/* Marks the start of a frame (graphicsFrameBegin). */
void perfFrameStart(void);
/* Marks the end of a phase that began at the previous mark. */
void perfMark(enum PerfPhase phase);
/* Marks the end of a frame and updates the summary every PERF_WINDOW frames. */
void perfFrameEnd(void);
/* Counts drawn quads and draw calls for the frame (GPU renderer only). */
void perfCountDraws(uint32_t quads, uint32_t draws);

/* The latest summary, or "" before the first window completes. */
const char *perfSummary(void);
/* Draws a frame counter (frame number and fps) at the top left and the
 * summary at the top centre (HBMENU_PERF_OVERLAY builds). */
void perfDrawOverlay(void);

#endif
