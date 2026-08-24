/*
 * stub_cobuf.c — fixed-pool implementation of the co_buf API surface the
 * firmware uses (co_buf_alloc/release; the accessors are header inlines that
 * read the struct fields directly, so the layout in co_buf.h is binding).
 */

#include "stub.h"
#include "co_buf.h"

struct pool_buf {
    co_buf_t hdr;
    uint8_t payload[STUB_COBUF_PAYLOAD];
};

static struct pool_buf pool[STUB_NB_COBUF];

void stub_cobuf_init(void)
{
    stub_memset(pool, 0, sizeof(pool));
}

uint8_t hstub_co_buf_alloc(co_buf_t **pp_buf, uint16_t head_len,
                           uint16_t data_len, uint16_t tail_len)
{
    uint32_t total = (uint32_t)head_len + data_len + tail_len;

    if (pp_buf == NULL) {
        return CO_BUF_ERR_INVALID_PARAM;
    }
    *pp_buf = NULL;
    if (total > STUB_COBUF_PAYLOAD) {
        return CO_BUF_ERR_INSUFFICIENT_RESOURCE;
    }

    stub_lock();
    for (unsigned i = 0; i < STUB_NB_COBUF; i++) {
        co_buf_t *b = &pool[i].hdr;

        if (b->acq_cnt == 0) {
            b->acq_cnt = 1;
            b->head_len = head_len;
            b->data_len = data_len;
            b->tail_len = tail_len;
            b->pool_id = 0;
            b->metadata_bf = 0;
            stub_unlock();
            *pp_buf = b;
            return CO_BUF_ERR_NO_ERROR;
        }
    }
    stub_unlock();
    return CO_BUF_ERR_INSUFFICIENT_RESOURCE;
}

uint8_t hstub_co_buf_release(co_buf_t *p_buf)
{
    if (p_buf == NULL || p_buf->acq_cnt == 0) {
        return CO_BUF_ERR_INVALID_PARAM;
    }
    stub_lock();
    p_buf->acq_cnt--;
    stub_unlock();
    return CO_BUF_ERR_NO_ERROR;
}
