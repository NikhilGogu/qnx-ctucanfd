/*
 * Copyright 2026 The ctucanfd authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fault injector for the hardware layer, built only with -DCTUCAN_FI (make FI=1).
 *
 * QEMU 6.2 models no error counters, fault confinement or bus timing, so the
 * driver's error, bus-off and slow-TX paths cannot be reached on the emulated
 * board. The injector overlays scripted values on the registers the driver
 * reads (INT_STAT, STATUS.EWL, FAULT_STATE, REC/TEC, TX_STATUS) and watches
 * the commands it writes (COMMAND.ERCRST, TX_COMMAND, INT_STAT acks), so the
 * service loop sees the sequences silicon would produce.
 *
 * The overlay changes only what the driver sees: QEMU still sends a frame as
 * soon as its TXT buffer is set ready.
 */
#include <errno.h>
#include <stdlib.h>
#include "ctucanfd_hw.h"

#define EWL_DEFAULT 96u     /* reset value of the EWL register */

struct ctucan_fi {
    unsigned scenario, step;
    uint64_t now, t_next, recover_at;
    bool     err_valid;         /* error registers are overlaid */
    bool     ewl;               /* STATUS.EWL */
    bool     ercrst;            /* driver wrote COMMAND.ERCRST while bus-off */
    bool     busoff;            /* frames set ready stay "Ready": no transmission while bus-off */
    uint32_t fault;             /* FAULT_STATE bits */
    uint32_t rec_tec;
    uint32_t int_stat;          /* injected INT_STAT bits, cleared by the driver's acks */
    unsigned tx_delay, tx_left; /* hold the next tx_left completions for tx_delay ms */
    uint32_t held;              /* TXT buffers shown as in transmission */
    uint32_t aborted;           /* held buffers the driver aborted */
    uint64_t hold_until[CTUCAN_TXT_BUFFERS];
    uint64_t last_hold;         /* release times never fall in the order buffers were set ready */
    uint32_t ready_seq[CTUCAN_TXT_BUFFERS], ready_count;
};

struct ctucan_fi *ctucan_fi_create(void)
{
    return calloc(1, sizeof(struct ctucan_fi));
}

static void set_err(struct ctucan_fi *fi, uint32_t fault, unsigned tec, unsigned rec)
{
    bool ewl = tec >= EWL_DEFAULT || rec >= EWL_DEFAULT;

    if (ewl != fi->ewl) {
        fi->int_stat |= INT_EWLI;   /* datasheet: set on crossing the limit either way */
    }
    if (fault != fi->fault) {
        fi->int_stat |= INT_FCSI;
    }
    fi->ewl = ewl;
    fi->fault = fault;
    fi->rec_tec = ((uint32_t)tec << 16) | rec;
}

int ctucan_fi_start(struct ctucan_fi *fi, unsigned scenario, unsigned arg, unsigned arg2, uint64_t now_ms)
{
    if (scenario > CTUCAN_FI_TXDELAY) {
        return EINVAL;
    }
    fi->scenario = scenario;
    fi->step = 0;
    fi->now = fi->t_next = now_ms;
    fi->ercrst = false;
    fi->busoff = false;
    switch (scenario) {
    case CTUCAN_FI_OFF:
        fi->err_valid = false;
        fi->tx_delay = fi->tx_left = 0;
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            fi->hold_until[b] = now_ms;     /* release at the next tick */
        }
        break;
    case CTUCAN_FI_TXDELAY:
        fi->tx_delay = arg != 0 ? arg : 20;
        fi->tx_left = arg2 != 0 ? arg2 : 64;
        break;
    default:
        /* start from a quiet error-active node, without raising interrupts */
        fi->err_valid = true;
        fi->fault = FS_ERA;
        fi->ewl = false;
        fi->rec_tec = 0;
        break;
    }
    return EOK;
}

/* Advances the script to now_ms. Returns true while the injector still has
 * something to do (a step pending or a completion held). */
bool ctucan_fi_tick(struct ctucan_fi *fi, uint64_t now_ms)
{
    bool done = false;

    fi->now = now_ms;
    /* Due completions are released oldest first, at most three per tick, so
     * the driver refills several buffers at once at changing positions. */
    for (int n = 0; n < 3 && !fi->busoff; n++) {
        int pick = -1;
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            if ((fi->held & (1u << b)) && now_ms >= fi->hold_until[b] &&
                (pick < 0 || fi->hold_until[b] < fi->hold_until[pick] ||
                 (fi->hold_until[b] == fi->hold_until[pick] && fi->ready_seq[b] < fi->ready_seq[pick]))) {
                pick = (int)b;
            }
        }
        if (pick < 0) {
            break;
        }
        fi->held &= ~(1u << pick);
        fi->int_stat |= INT_TXBHCI;
    }
    if (now_ms >= fi->t_next) {
        switch (fi->scenario) {
        case CTUCAN_FI_WARN:
            if (fi->step == 0) {
                set_err(fi, FS_ERA, EWL_DEFAULT + 1, 0);
                fi->t_next = now_ms + 5;
                fi->step = 1;
            } else if (fi->step == 1) {
                set_err(fi, FS_ERA, 0, 0);
                fi->step = 2;
            } else {
                done = true;
            }
            break;
        case CTUCAN_FI_PASSIVE:
            switch (fi->step) {
            case 0: set_err(fi, FS_ERA, EWL_DEFAULT + 1, 0); fi->t_next = now_ms + 2; break;
            case 1: set_err(fi, FS_ERP, 128, 0); fi->t_next = now_ms + 10; break;
            case 2: set_err(fi, FS_ERA, 100, 0); fi->t_next = now_ms + 2; break;
            case 3: set_err(fi, FS_ERA, 0, 0); break;
            default: done = true; break;
            }
            if (!done) {
                fi->step++;
            }
            break;
        case CTUCAN_FI_BUSOFF:
            switch (fi->step) {
            case 0: set_err(fi, FS_ERA, EWL_DEFAULT + 1, 0); fi->t_next = now_ms + 2; fi->step = 1; break;
            case 1: set_err(fi, FS_ERP, 128, 0); fi->t_next = now_ms + 2; fi->step = 2; break;
            case 2:
                set_err(fi, FS_BOF, 256, 0);
                fi->busoff = true;
                fi->ercrst = false;
                fi->step = 3;
                break;
            case 3:
                if (fi->ercrst) {
                    /* 128 x 11 recessive bits at 500 kbit/s take 2.8 ms */
                    fi->recover_at = now_ms + 3;
                    fi->step = 4;
                }
                break;
            case 4:
                if (now_ms >= fi->recover_at) {
                    set_err(fi, FS_ERA, 0, 0);
                    fi->busoff = false;
                    fi->step = 5;
                }
                break;
            default:
                done = true;
                break;
            }
            break;
        case CTUCAN_FI_TXDELAY:
            done = fi->tx_left == 0 && fi->held == 0;
            break;
        default:
            done = true;
            break;
        }
    }
    return !done || fi->held != 0 || fi->int_stat != 0;
}

unsigned ctucan_fi_state(const struct ctucan_fi *fi)
{
    return (fi->scenario << 4) | (fi->step & 0xfu);
}

uint32_t ctucan_rd(const ctucan_hw_t *hw, uint32_t off)
{
    uint32_t v = ctucan_raw_rd(hw, off);
    const struct ctucan_fi *fi = hw->fi;

    if (fi == NULL) {
        return v;
    }
    switch (off) {
    case CTUCAN_INT_STAT:
        return v | fi->int_stat;
    case CTUCAN_STATUS:
        if (fi->err_valid) {
            v = (v & ~STAT_EWL) | (fi->ewl ? STAT_EWL : 0);
        }
        return v;
    case CTUCAN_EWL_ERP_FAULT:
        return fi->err_valid ? (v & 0xffffu) | fi->fault : v;
    case CTUCAN_REC_TEC:
        return fi->err_valid ? fi->rec_tec : v;
    case CTUCAN_TX_STATUS:
        for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
            uint32_t bit = 1u << b, st;
            if (fi->aborted & bit) {
                st = TXT_ABT;
            } else if (fi->held & bit) {
                st = fi->busoff ? TXT_RDY : TXT_TRAN;
            } else {
                continue;
            }
            v = (v & ~(0xfu << (4u * b))) | (st << (4u * b));
        }
        return v;
    default:
        return v;
    }
}

void ctucan_wr(const ctucan_hw_t *hw, uint32_t off, uint32_t val)
{
    struct ctucan_fi *fi = hw->fi;

    if (fi != NULL) {
        switch (off) {
        case CTUCAN_INT_STAT:
            fi->int_stat &= ~val;
            break;
        case CTUCAN_COMMAND:
            if ((val & CMD_ERCRST) && fi->err_valid && (fi->fault & FS_BOF)) {
                fi->ercrst = true;
            }
            break;
        case CTUCAN_MODE:
            if (val & MODE_RST) {
                fi->held = fi->aborted = 0;
                fi->int_stat = 0;
            }
            break;
        case CTUCAN_TX_COMMAND:
            for (unsigned b = 0; b < CTUCAN_TXT_BUFFERS; b++) {
                uint32_t bit = 1u << b;
                if (!(val & TXC_TXB(b))) {
                    continue;
                }
                if ((val & TXC_TXCR) && (fi->busoff || fi->tx_left > 0)) {
                    uint64_t until = fi->now + fi->tx_delay;
                    if (until < fi->last_hold) {
                        until = fi->last_hold;  /* never before an older buffer */
                    }
                    fi->held |= bit;
                    fi->aborted &= ~bit;
                    fi->hold_until[b] = fi->last_hold = until;
                    fi->ready_seq[b] = fi->ready_count++;
                    if (fi->tx_left > 0 && !fi->busoff) {
                        fi->tx_left--;
                    }
                }
                if ((val & TXC_TXCA) && (fi->held & bit)) {
                    fi->held &= ~bit;
                    fi->aborted |= bit;
                    fi->int_stat |= INT_TXBHCI;
                }
                if (val & TXC_TXCE) {
                    fi->held &= ~bit;
                    fi->aborted &= ~bit;
                }
            }
            break;
        default:
            break;
        }
    }
    ctucan_raw_wr(hw, off, val);
}
