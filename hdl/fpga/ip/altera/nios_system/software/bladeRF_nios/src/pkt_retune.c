/* This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (c) 2015 Nuand LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include <stdbool.h>
#include "pkt_handler.h"
#include "pkt_retune.h"
#include "nios_pkt_retune.h"    /* Packet format definition */
#include "devices.h"
#include "band_select.h"
#include "debug.h"

#ifdef BLADERF_NIOS_DEBUG
    volatile uint32_t pkt_retune_error_count = 0;
#   define INCREMENT_ERROR_COUNT() do { pkt_retune_error_count++; } while (0)
#else
#   define INCREMENT_ERROR_COUNT() do {} while (0)
#endif

/* The enqueue/dequeue routines require that this be a power of two */
#define RETUNE_QUEUE_MAX    32
#define QUEUE_FULL          0xff
#define QUEUE_EMPTY         0xfe

/* State of items in the retune queue */
enum entry_state {
    ENTRY_STATE_INVALID = 0,  /* Marks entry invalid and not in use */
    ENTRY_STATE_NEW,          /* We have a new retune request to satisfy */
    ENTRY_STATE_SCHEDULED,    /* We've scheduled the timer interrupt for
                               * this entry and are awaiting the ISR */
    ENTRY_STATE_READY,        /* The timer interrupt has fired - we should
                               * handle this retune */
};

struct queue_entry {
    volatile enum entry_state state;
    bladerf_module module;
    struct lms_freq freq;
};

static struct queue {
    uint8_t count;      /* Total number of items in the queue */
    uint8_t ins_idx;    /* Insertion index */
    uint8_t rem_idx;    /* Removal index */

    struct queue_entry entries[RETUNE_QUEUE_MAX];
} q;

/* Returns queue size after enqueue operation, or QUEUE_FULL if we could
 * not enqueue the requested item */
static inline uint8_t enqueue_retune(const struct lms_freq *f, bladerf_module m)
{
    uint8_t ret;

    if (q.count >= RETUNE_QUEUE_MAX) {
        return QUEUE_FULL;
    }

    memcpy(&q.entries[q.ins_idx].freq, f, sizeof(f[0]));

    q.entries[q.ins_idx].state = ENTRY_STATE_NEW;
    q.entries[q.ins_idx].module = m;

    q.ins_idx = (q.ins_idx + 1) & (RETUNE_QUEUE_MAX - 1);

    q.count++;
    ret = q.count;

    return ret;
}

/* Retune number of items left in the queue after the dequeue operation,
 * or QUEUE_EMPTY if there was nothing to dequeue */
static inline uint8_t dequeue_retune(struct queue_entry *e)
{
    uint8_t ret;

    if (q.count == 0) {
        return QUEUE_EMPTY;
    }

    if (e != NULL) {
        memcpy(&e, &q.entries[q.rem_idx], sizeof(e[0]));
    }

    q.rem_idx = (q.rem_idx + 1) & (RETUNE_QUEUE_MAX - 1);

    q.count--;
    ret = q.count;

    return ret;
}

/* Get the state of the next item in the retune queue */
static inline struct queue_entry * peek_next_retune()
{
    if (q.count == 0) {
        return NULL;
    } else {
        return &q.entries[q.rem_idx];
    }
}

void pkt_retune_init()
{
    memset(&q, 0, sizeof(q));
}

void pkt_retune_work(void)
{
    struct queue_entry *e = peek_next_retune();

    if (e == NULL) {
        return;
    }

    switch (e->state) {
        case ENTRY_STATE_NEW:
            e->state = ENTRY_STATE_SCHEDULED;

            /* TODO: Schedule the timer ISR */
            break;

        case ENTRY_STATE_SCHEDULED:
            /* Nothing to do.
             * We're just waiting for this entry to become */
            break;

        case ENTRY_STATE_READY:

            /* Perform our retune */
            if (lms_set_precalculated_frequency(NULL, e->module, &e->freq)) {
                INCREMENT_ERROR_COUNT();
            } else {
                bool low_band = (e->freq.flags & LMS_FREQ_FLAGS_LOW_BAND) != 0;
                if (band_select(NULL, e->module, low_band)) {
                    INCREMENT_ERROR_COUNT();
                }
            }

            /* Drop the item from the queue */
            dequeue_retune(NULL);
            break;

        default:
            INCREMENT_ERROR_COUNT();
            break;
    }
}

void pkt_retune(struct pkt_buf *b)
{
    int status = -1;
    bladerf_module module;
    uint8_t flags;
    struct lms_freq f;
    uint64_t timestamp;
    uint64_t retune_start;
    uint64_t retune_end;
    uint64_t retune_duration = 0;
    bool low_band;
    bool quick_tune;

    flags = NIOS_PKT_RETUNERESP_FLAG_SUCCESS;

    nios_pkt_retune_unpack(b->req, &module, &timestamp,
                           &f.nint, &f.nfrac, &f.freqsel, &f.vcocap,
                           &low_band, &quick_tune);

    f.vcocap_result = 0xff;

    if (low_band) {
        f.flags = LMS_FREQ_FLAGS_LOW_BAND;
    } else {
        f.flags = 0;
    }

    if (quick_tune) {
        f.flags |= LMS_FREQ_FLAGS_FORCE_VCOCAP;
    }

    if (timestamp == NIOS_PKT_RETUNE_NOW) {
        /* Fire off this retune operation now */
        switch (module) {
            case BLADERF_MODULE_RX:
            case BLADERF_MODULE_TX:
                retune_start = time_tamer_read(module);

                status = lms_set_precalculated_frequency(NULL, module, &f);
                if (status != 0) {
                    goto out;
                }

                flags |= NIOS_PKT_RETUNERESP_FLAG_TSVTUNE_VALID;

                status = band_select(NULL, module, low_band);
                if (status != 0) {
                    goto out;
                }

                retune_end = time_tamer_read(module);
                retune_duration = retune_end - retune_start;
                status = 0;
                break;

            default:
                status = -1;
        }

    } else {
        uint8_t queue_size = enqueue_retune(&f, module);
        retune_duration = 0;

        if (queue_size == QUEUE_FULL) {
            status = -1;
        } else {
            status = 0;
        }
    }

out:
    if (status != 0) {
        flags &= ~(NIOS_PKT_RETUNERESP_FLAG_SUCCESS);
    }

    nios_pkt_retune_resp_pack(b->resp, retune_duration, f.vcocap_result, flags);
}
