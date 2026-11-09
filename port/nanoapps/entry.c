/* NanoApps raw-surface entry glue. Compiled with the SDK's own flags (Thumb, no FPU):
 * the resident enters apps in Thumb state, and nothing may execute a floating-point
 * instruction before the task's FP context is attached. All real work happens in
 * the ARM/VFP library (hal_nanoapps.c and the nanoCDJ core), reached from here.
 *
 * Like NanoApps' own FP ports, the FP context is attached and the app initialised
 * on the frame heartbeat, not inside payload_entry. */
#include "hb_sdk.h"
#include "hb_raw_surface.h"

void nanocdj_device_init(int w, int h);              /* ARM/VFP side */
void nanocdj_device_frame(const hb_spoint_t *touch);
void nanocdj_device_fatal(const char *msg);           /* integer-only drawing */

static int s_w, s_h, s_state;   /* 0 = not started, 1 = running, -1 = failed */

void hb_raw_init(int w, int h)
{
    s_w = w; s_h = h; s_state = 0;
    hb_trace_log("NC_INIT", (uint32_t)w, (uint32_t)h);   /* breadcrumbs: read with tools/trace.py */
    hb_raw_fill(0x000000);
}

void hb_raw_frame(const hb_spoint_t *touch)
{
    hb_fpu_status_t st;
    if (s_state < 0) return;
    if (hb_fpu_attach_current_task(&st) < 0) {
        s_state = -1;
        nanocdj_device_fatal("no FPU context");
        return;
    }
    if (s_state == 0) {
        s_state = 1;
        hb_trace_log("NC_FPU", (uint32_t)st.context, (uint32_t)st.enabled);
        nanocdj_device_init(s_w, s_h);
        hb_trace_log("NC_READY", 0, 0);
    }
    nanocdj_device_frame(touch);
}
