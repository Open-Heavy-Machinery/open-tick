/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file framelog.c
 * @brief Frame log writer (opentick/framelog.h, docs/frame-log-format.md).
 */

#include <opentick/framelog.h>

#include <stdarg.h>
#include <string.h>

static void fail(OtkFrameLog *log, const char *fmt, ...) {
    if (log->error) {
        return; /* the first error is kept */
    }
    log->error = true;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(log->error_msg, sizeof(log->error_msg), fmt, ap);
    va_end(ap);
}

const char *otk_bus_clock_name(OtkBusClock clock) {
    switch (clock) {
    case OTK_BUS_CLOCK_NONE:
        return "none";
    case OTK_BUS_CLOCK_SIM:
        return "sim";
    case OTK_BUS_CLOCK_KERNEL:
        return "kernel";
    case OTK_BUS_CLOCK_HOST:
        return "host";
    case OTK_BUS_CLOCK_BRIDGE:
        return "bridge";
    default:
        return NULL;
    }
}

const char *otk_frame_fault_name(OtkFrameFault fault) {
    switch (fault) {
    case OTK_FRAME_OK:
        return "";
    case OTK_FRAME_DROP:
        return "drop";
    case OTK_FRAME_DROP_ONE_IN:
        return "drop_one_in";
    case OTK_FRAME_BUS_OFF:
        return "bus_off";
    case OTK_FRAME_OVERFLOW:
        return "overflow";
    case OTK_FRAME_CORRUPT:
        return "corrupt";
    default:
        return NULL;
    }
}

OtkFrameFault otk_frame_fault_from_link(const OtkLinkInfo *info) {
    switch (info->lost) {
    case OTK_LINK_LOST_DROP:
        return OTK_FRAME_DROP;
    case OTK_LINK_LOST_DROP_ONE_IN:
        return OTK_FRAME_DROP_ONE_IN;
    case OTK_LINK_LOST_BUS_OFF:
        return OTK_FRAME_BUS_OFF;
    case OTK_LINK_LOST_OVERFLOW:
        return OTK_FRAME_OVERFLOW;
    case OTK_LINK_LOST_NONE:
    default:
        return info->corrupted ? OTK_FRAME_CORRUPT : OTK_FRAME_OK;
    }
}

static bool is_lost(OtkFrameFault fault) {
    return fault != OTK_FRAME_OK && fault != OTK_FRAME_CORRUPT;
}

/* Seconds with exactly nine decimals, from the integer: never through a double. */
static int print_time(FILE *fp, OtkNs t) {
    return fprintf(fp, "%llu.%09llu", (unsigned long long)(t / OTK_NS_PER_S),
                   (unsigned long long)(t % OTK_NS_PER_S));
}

bool otk_framelog_init(OtkFrameLog *log, FILE *fp, const OtkFrameLogConfig *cfg) {
    memset(log, 0, sizeof(*log));
    log->fp = fp;
    if (cfg->step == 0U) {
        fail(log, "step must be > 0");
        return false;
    }
    const char *clock = otk_bus_clock_name(cfg->bus_clock);
    if (clock == NULL) {
        fail(log, "unknown bus clock %d", (int)cfg->bus_clock);
        return false;
    }
    if (cfg->source != NULL && strpbrk(cfg->source, ";\r\n") != NULL) {
        fail(log, "source must not contain ';' or a line break");
        return false;
    }
    log->step = cfg->step;
    log->bus_clock = cfg->bus_clock;

    int rc = fprintf(fp, "# otk-frame-log %d; step_ns=%llu; bus_clock=%s; bus_clock_err_ns=",
                     OTK_FRAMELOG_FORMAT_VERSION, (unsigned long long)cfg->step, clock);
    if (rc >= 0 && cfg->err_known) {
        rc = fprintf(fp, "%llu", (unsigned long long)cfg->err_ns);
    }
    if (rc >= 0 && cfg->source != NULL && cfg->source[0] != '\0') {
        rc = fprintf(fp, "; source=%s", cfg->source);
    }
    if (rc >= 0) {
        rc = fputs("\nt_s,t_bus_s,dir,link,id,flags,len,data,fault\n", fp);
    }
    if (rc < 0) {
        fail(log, "cannot write the header");
        return false;
    }
    return true;
}

bool otk_framelog_frame(OtkFrameLog *log, const OtkFrameRow *row) {
    if (log->error) {
        return false;
    }
    const OtkCanFrame *f = row->frame;
    if (f == NULL || !otk_can_frame_valid(f)) {
        fail(log, "row %llu: invalid frame", (unsigned long long)(log->rows + 1U));
        return false;
    }
    if (row->t % log->step != 0U) {
        fail(log, "row %llu: t is not a multiple of the step", (unsigned long long)(log->rows + 1U));
        return false;
    }
    if (row->dir != OTK_FRAME_TO_DUT && row->dir != OTK_FRAME_FROM_DUT) {
        fail(log, "row %llu: unknown direction", (unsigned long long)(log->rows + 1U));
        return false;
    }
    const char *fault = otk_frame_fault_name(row->fault);
    if (fault == NULL) {
        fail(log, "row %llu: unknown fault", (unsigned long long)(log->rows + 1U));
        return false;
    }
    if (row->t_bus != OTK_FRAMELOG_NO_TIME && log->bus_clock == OTK_BUS_CLOCK_NONE) {
        fail(log, "row %llu: a bus time with bus_clock=none", (unsigned long long)(log->rows + 1U));
        return false;
    }
    if (row->dir == OTK_FRAME_TO_DUT && is_lost(row->fault) && row->t_bus != OTK_FRAMELOG_NO_TIME) {
        fail(log, "row %llu: a lost to_dut frame was never on the bus", (unsigned long long)(log->rows + 1U));
        return false;
    }
    const char *link = (row->link != NULL) ? row->link : "";
    if (strpbrk(link, ",\" \t\r\n") != NULL) {
        fail(log, "row %llu: link name with a comma, quote, space or line break",
             (unsigned long long)(log->rows + 1U));
        return false;
    }

    FILE *fp = log->fp;
    int rc = print_time(fp, row->t);
    if (rc >= 0) {
        rc = fputc(',', fp);
    }
    if (rc >= 0 && row->t_bus != OTK_FRAMELOG_NO_TIME) {
        rc = print_time(fp, row->t_bus);
    }
    const bool ext = (f->flags & OTK_CAN_EXTENDED) != 0U;
    char flags[4];
    size_t nf = 0U;
    if (ext) {
        flags[nf++] = 'x';
    }
    if ((f->flags & OTK_CAN_FD) != 0U) {
        flags[nf++] = 'f';
    }
    if ((f->flags & OTK_CAN_BRS) != 0U) {
        flags[nf++] = 'b';
    }
    flags[nf] = '\0';
    if (rc >= 0) {
        rc = fprintf(fp, ",%s,%s,%0*lX,%s,%u,", (row->dir == OTK_FRAME_TO_DUT) ? "to_dut" : "from_dut", link,
                     ext ? 8 : 3, (unsigned long)f->id, flags, (unsigned)f->len);
    }
    for (uint8_t i = 0U; rc >= 0 && i < f->len; i++) {
        rc = fprintf(fp, "%02X", (unsigned)f->data[i]);
    }
    if (rc >= 0) {
        rc = fprintf(fp, ",%s\n", fault);
    }
    if (rc < 0) {
        fail(log, "row %llu: cannot write", (unsigned long long)(log->rows + 1U));
        return false;
    }
    log->rows++;
    return true;
}

bool otk_framelog_ok(const OtkFrameLog *log) {
    return !log->error;
}

const char *otk_framelog_error(const OtkFrameLog *log) {
    return log->error ? log->error_msg : "";
}
