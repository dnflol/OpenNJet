
/*
 * Copyright (C) Nginx, Inc.
 * Copyright (C) 2021-2023  TMLake(Beijing) Technology Co., Ltd.
 */


#include <njt_config.h>
#include <njt_core.h>
#include <njt_event.h>
#include <njt_event_quic_connection.h>


#define NJT_QUIC_MAX_ACK_GAP                 2

/* RFC 9002, 6.1.1. Packet Threshold: kPacketThreshold */
#define NJT_QUIC_PKT_THR                     3 /* packets */
/* RFC 9002, 6.1.2. Time Threshold: kGranularity */
#define NJT_QUIC_TIME_GRANULARITY            1 /* ms */

/* RFC 9002, 7.6.1. Duration: kPersistentCongestionThreshold */
#define NJT_QUIC_PERSISTENT_CONGESTION_THR   3


/* send time of ACK'ed packets */
typedef struct {
    njt_msec_t                               max_pn;
    njt_msec_t                               oldest;
    njt_msec_t                               newest;
} njt_quic_ack_stat_t;


static njt_inline njt_msec_t njt_quic_lost_threshold(njt_quic_connection_t *qc);
static void njt_quic_rtt_sample(njt_connection_t *c, njt_quic_ack_frame_t *ack,
    enum ssl_encryption_level_t level, njt_msec_t send_time);
static njt_int_t njt_quic_handle_ack_frame_range(njt_connection_t *c,
    njt_quic_send_ctx_t *ctx, uint64_t min, uint64_t max,
    njt_quic_ack_stat_t *st);
static void njt_quic_drop_ack_ranges(njt_connection_t *c,
    njt_quic_send_ctx_t *ctx, uint64_t pn);
static njt_int_t njt_quic_detect_lost(njt_connection_t *c,
    njt_quic_ack_stat_t *st);
static njt_msec_t njt_quic_pcg_duration(njt_connection_t *c);
static void njt_quic_persistent_congestion(njt_connection_t *c);
static void njt_quic_congestion_lost(njt_connection_t *c,
    njt_quic_frame_t *frame);
static void njt_quic_lost_handler(njt_event_t *ev);


/* RFC 9002, 6.1.2. Time Threshold: kTimeThreshold, kGranularity */
static njt_inline njt_msec_t
njt_quic_lost_threshold(njt_quic_connection_t *qc)
{
    njt_msec_t  thr;

    thr = njt_max(qc->latest_rtt, qc->avg_rtt);
    thr += thr >> 3;

    return njt_max(thr, NJT_QUIC_TIME_GRANULARITY);
}


njt_int_t
njt_quic_handle_ack_frame(njt_connection_t *c, njt_quic_header_t *pkt,
    njt_quic_frame_t *f)
{
    ssize_t                 n;
    u_char                 *pos, *end;
    uint64_t                min, max, gap, range;
    njt_uint_t              i;
    njt_quic_ack_stat_t     send_time;
    njt_quic_send_ctx_t    *ctx;
    njt_quic_ack_frame_t   *ack;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);

    ctx = njt_quic_get_send_ctx(qc, pkt->level);

    njt_log_debug1(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic njt_quic_handle_ack_frame level:%d", pkt->level);

    ack = &f->u.ack;

    /*
     * RFC 9000, 19.3.1.  ACK Ranges
     *
     *  If any computed packet number is negative, an endpoint MUST
     *  generate a connection error of type FRAME_ENCODING_ERROR.
     */

    if (ack->first_range > ack->largest) {
        qc->error = NJT_QUIC_ERR_FRAME_ENCODING_ERROR;
        njt_log_error(NJT_LOG_INFO, c->log, 0,
                      "quic invalid first range in ack frame");
        return NJT_ERROR;
    }

    min = ack->largest - ack->first_range;
    max = ack->largest;

    send_time.oldest = NJT_TIMER_INFINITE;
    send_time.newest = NJT_TIMER_INFINITE;

    if (njt_quic_handle_ack_frame_range(c, ctx, min, max, &send_time)
        != NJT_OK)
    {
        return NJT_ERROR;
    }

    /* RFC 9000, 13.2.4.  Limiting Ranges by Tracking ACK Frames */
    if (ctx->largest_ack < max || ctx->largest_ack == NJT_QUIC_UNSET_PN) {
        ctx->largest_ack = max;
        njt_log_debug1(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic updated largest received ack:%uL", max);

        /*
         * RFC 9002, 5.1.  Generating RTT Samples
         *
         *  An endpoint generates an RTT sample on receiving an
         *  ACK frame that meets the following two conditions:
         *
         *  - the largest acknowledged packet number is newly acknowledged
         *  - at least one of the newly acknowledged packets was ack-eliciting.
         */

        if (send_time.max_pn != NJT_TIMER_INFINITE) {
            njt_quic_rtt_sample(c, ack, pkt->level, send_time.max_pn);
        }
    }

    if (f->data) {
        pos = f->data->buf->pos;
        end = f->data->buf->last;

    } else {
        pos = NULL;
        end = NULL;
    }

    for (i = 0; i < ack->range_count; i++) {

        n = njt_quic_parse_ack_range(pkt->log, pos, end, &gap, &range);
        if (n == NJT_ERROR) {
            return NJT_ERROR;
        }
        pos += n;

        if (gap + 2 > min) {
            qc->error = NJT_QUIC_ERR_FRAME_ENCODING_ERROR;
            njt_log_error(NJT_LOG_INFO, c->log, 0,
                          "quic invalid range:%ui in ack frame", i);
            return NJT_ERROR;
        }

        max = min - gap - 2;

        if (range > max) {
            qc->error = NJT_QUIC_ERR_FRAME_ENCODING_ERROR;
            njt_log_error(NJT_LOG_INFO, c->log, 0,
                          "quic invalid range:%ui in ack frame", i);
            return NJT_ERROR;
        }

        min = max - range;

        if (njt_quic_handle_ack_frame_range(c, ctx, min, max, &send_time)
            != NJT_OK)
        {
            return NJT_ERROR;
        }
    }

    return njt_quic_detect_lost(c, &send_time);
}


static void
njt_quic_rtt_sample(njt_connection_t *c, njt_quic_ack_frame_t *ack,
    enum ssl_encryption_level_t level, njt_msec_t send_time)
{
    njt_msec_t              latest_rtt, ack_delay, adjusted_rtt, rttvar_sample;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);

    latest_rtt = njt_current_msec - send_time;
    qc->latest_rtt = latest_rtt;

    if (qc->min_rtt == NJT_TIMER_INFINITE) {
        qc->min_rtt = latest_rtt;
        qc->avg_rtt = latest_rtt;
        qc->rttvar = latest_rtt / 2;
        qc->first_rtt = njt_current_msec;

    } else {
        qc->min_rtt = njt_min(qc->min_rtt, latest_rtt);

        ack_delay = (ack->delay << qc->ctp.ack_delay_exponent) / 1000;

        if (c->ssl->handshaked) {
            ack_delay = njt_min(ack_delay, qc->ctp.max_ack_delay);
        }

        adjusted_rtt = latest_rtt;

        if (qc->min_rtt + ack_delay < latest_rtt) {
            adjusted_rtt -= ack_delay;
        }

        rttvar_sample = njt_abs((njt_msec_int_t) (qc->avg_rtt - adjusted_rtt));
        qc->rttvar += (rttvar_sample >> 2) - (qc->rttvar >> 2);
        qc->avg_rtt += (adjusted_rtt >> 3) - (qc->avg_rtt >> 3);
    }

    njt_log_debug4(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic rtt sample latest:%M min:%M avg:%M var:%M",
                   latest_rtt, qc->min_rtt, qc->avg_rtt, qc->rttvar);
}


static njt_int_t
njt_quic_handle_ack_frame_range(njt_connection_t *c, njt_quic_send_ctx_t *ctx,
    uint64_t min, uint64_t max, njt_quic_ack_stat_t *st)
{
    njt_uint_t              found;
    njt_queue_t            *q;
    njt_quic_frame_t       *f;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);

    if (ctx->level == ssl_encryption_application) {
        if (njt_quic_handle_path_mtu(c, qc->path, min, max) != NJT_OK) {
            return NJT_ERROR;
        }
    }

    st->max_pn = NJT_TIMER_INFINITE;
    found = 0;

    q = njt_queue_head(&ctx->sent);

    while (q != njt_queue_sentinel(&ctx->sent)) {

        f = njt_queue_data(q, njt_quic_frame_t, queue);
        q = njt_queue_next(q);

        if (f->pnum > max) {
            break;
        }

        if (f->pnum >= min) {
            njt_quic_congestion_ack(c, f);

            switch (f->type) {
            case NJT_QUIC_FT_ACK:
            case NJT_QUIC_FT_ACK_ECN:
                njt_quic_drop_ack_ranges(c, ctx, f->u.ack.largest);
                break;

            case NJT_QUIC_FT_STREAM:
            case NJT_QUIC_FT_RESET_STREAM:
                njt_quic_handle_stream_ack(c, f);
                break;
            }

            if (f->pnum == max) {
                st->max_pn = f->send_time;
            }

            /* save earliest and latest send times of frames ack'ed */
            if (st->oldest == NJT_TIMER_INFINITE || f->send_time < st->oldest) {
                st->oldest = f->send_time;
            }

            if (st->newest == NJT_TIMER_INFINITE || f->send_time > st->newest) {
                st->newest = f->send_time;
            }

            njt_queue_remove(&f->queue);
            njt_quic_free_frame(c, f);
            found = 1;
        }
    }

    if (!found) {

        if (max < ctx->pnum) {
            /* duplicate ACK or ACK for non-ack-eliciting frame */
            return NJT_OK;
        }

        njt_log_error(NJT_LOG_INFO, c->log, 0,
                      "quic ACK for the packet not sent");

        qc->error = NJT_QUIC_ERR_PROTOCOL_VIOLATION;
        qc->error_ftype = NJT_QUIC_FT_ACK;
        qc->error_reason = "unknown packet number";

        return NJT_ERROR;
    }

    if (!qc->push.timer_set) {
        njt_post_event(&qc->push, &njt_posted_events);
    }

    qc->pto_count = 0;

    return NJT_OK;
}


void
njt_quic_congestion_ack(njt_connection_t *c, njt_quic_frame_t *f)
{
    njt_uint_t              blocked;
    njt_msec_t              timer;
    njt_quic_congestion_t  *cg;
    njt_quic_connection_t  *qc;

    if (f->plen == 0) {
        return;
    }

    qc = njt_quic_get_connection(c);
    cg = &qc->congestion;

    if (f->pnum < qc->rst_pnum) {
        return;
    }

    blocked = (cg->in_flight >= cg->window) ? 1 : 0;

    cg->in_flight -= f->plen;

    timer = f->send_time - cg->recovery_start;

    if ((njt_msec_int_t) timer <= 0) {
        njt_log_debug3(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion ack recovery win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);

        goto done;
    }

    if (cg->window < cg->ssthresh) {
        cg->window += f->plen;

        njt_log_debug3(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion slow start win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);

    } else {
        cg->window += qc->tp.max_udp_payload_size * f->plen / cg->window;

        njt_log_debug3(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion avoidance win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);
    }

    /* prevent recovery_start from wrapping */

    timer = cg->recovery_start - njt_current_msec + qc->tp.max_idle_timeout * 2;

    if ((njt_msec_int_t) timer < 0) {
        cg->recovery_start = njt_current_msec - qc->tp.max_idle_timeout * 2;
    }

done:

    if (blocked && cg->in_flight < cg->window) {
        njt_post_event(&qc->push, &njt_posted_events);
    }
}


static void
njt_quic_drop_ack_ranges(njt_connection_t *c, njt_quic_send_ctx_t *ctx,
    uint64_t pn)
{
    uint64_t               base;
    njt_uint_t             i, smallest, largest;
    njt_quic_ack_range_t  *r;

    njt_log_debug4(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic njt_quic_drop_ack_ranges pn:%uL largest:%uL"
                   " fr:%uL nranges:%ui", pn, ctx->largest_range,
                   ctx->first_range, ctx->nranges);

    base = ctx->largest_range;

    if (base == NJT_QUIC_UNSET_PN) {
        return;
    }

    if (ctx->pending_ack != NJT_QUIC_UNSET_PN && pn >= ctx->pending_ack) {
        ctx->pending_ack = NJT_QUIC_UNSET_PN;
    }

    largest = base;
    smallest = largest - ctx->first_range;

    if (pn >= largest) {
        ctx->largest_range = NJT_QUIC_UNSET_PN;
        ctx->first_range = 0;
        ctx->nranges = 0;
        return;
    }

    if (pn >= smallest) {
        ctx->first_range = largest - pn - 1;
        ctx->nranges = 0;
        return;
    }

    for (i = 0; i < ctx->nranges; i++) {
        r = &ctx->ranges[i];

        largest = smallest - r->gap - 2;
        smallest = largest - r->range;

        if (pn >= largest) {
            ctx->nranges = i;
            return;
        }
        if (pn >= smallest) {
            r->range = largest - pn - 1;
            ctx->nranges = i + 1;
            return;
        }
    }
}


static njt_int_t
njt_quic_detect_lost(njt_connection_t *c, njt_quic_ack_stat_t *st)
{
    njt_uint_t              i, nlost;
    njt_msec_t              now, wait, thr, oldest, newest;
    njt_queue_t            *q;
    njt_quic_frame_t       *start;
    njt_quic_send_ctx_t    *ctx;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);
    now = njt_current_msec;
    thr = njt_quic_lost_threshold(qc);

    /* send time of lost packets across all send contexts */
    oldest = NJT_TIMER_INFINITE;
    newest = NJT_TIMER_INFINITE;

    nlost = 0;

    for (i = 0; i < NJT_QUIC_SEND_CTX_LAST; i++) {

        ctx = &qc->send_ctx[i];

        if (ctx->largest_ack == NJT_QUIC_UNSET_PN) {
            continue;
        }

        while (!njt_queue_empty(&ctx->sent)) {

            q = njt_queue_head(&ctx->sent);
            start = njt_queue_data(q, njt_quic_frame_t, queue);

            if (start->pnum > ctx->largest_ack) {
                break;
            }

            wait = start->send_time + thr - now;

            njt_log_debug4(NJT_LOG_DEBUG_EVENT, c->log, 0,
                           "quic detect_lost pnum:%uL thr:%M wait:%i level:%d",
                           start->pnum, thr, (njt_int_t) wait, start->level);

            if ((njt_msec_int_t) wait > 0
                && ctx->largest_ack - start->pnum < NJT_QUIC_PKT_THR)
            {
                break;
            }

            if (start->send_time > qc->first_rtt) {

                if (oldest == NJT_TIMER_INFINITE || start->send_time < oldest) {
                    oldest = start->send_time;
                }

                if (newest == NJT_TIMER_INFINITE || start->send_time > newest) {
                    newest = start->send_time;
                }

                nlost++;
            }

            njt_quic_resend_frames(c, ctx);
        }
    }


    /* RFC 9002, 7.6.2.  Establishing Persistent Congestion */

    /*
     * Once acknowledged, packets are no longer tracked. Thus no send time
     * information is available for such packets. This limits persistent
     * congestion algorithm to packets mentioned within ACK ranges of the
     * latest ACK frame.
     */

    if (st && nlost >= 2 && (st->newest < oldest || st->oldest > newest)) {

        if (newest - oldest > njt_quic_pcg_duration(c)) {
            njt_quic_persistent_congestion(c);
        }
    }

    njt_quic_set_lost_timer(c);

    return NJT_OK;
}


static njt_msec_t
njt_quic_pcg_duration(njt_connection_t *c)
{
    njt_msec_t              duration;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);

    duration = qc->avg_rtt;
    duration += njt_max(4 * qc->rttvar, NJT_QUIC_TIME_GRANULARITY);
    duration += qc->ctp.max_ack_delay;
    duration *= NJT_QUIC_PERSISTENT_CONGESTION_THR;

    return duration;
}


static void
njt_quic_persistent_congestion(njt_connection_t *c)
{
    njt_quic_congestion_t  *cg;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);
    cg = &qc->congestion;

    cg->recovery_start = njt_current_msec;
    cg->window = qc->tp.max_udp_payload_size * 2;

    njt_log_debug1(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic persistent congestion win:%uz", cg->window);
}


void
njt_quic_resend_frames(njt_connection_t *c, njt_quic_send_ctx_t *ctx)
{
    uint64_t                pnum;
    njt_queue_t            *q;
    njt_quic_frame_t       *f, *start;
    njt_quic_stream_t      *qs;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);
    q = njt_queue_head(&ctx->sent);
    start = njt_queue_data(q, njt_quic_frame_t, queue);
    pnum = start->pnum;

    njt_log_debug1(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic resend packet pnum:%uL", start->pnum);

    njt_quic_congestion_lost(c, start);

    do {
        f = njt_queue_data(q, njt_quic_frame_t, queue);

        if (f->pnum != pnum) {
            break;
        }

        q = njt_queue_next(q);

        njt_queue_remove(&f->queue);

        switch (f->type) {
        case NJT_QUIC_FT_ACK:
        case NJT_QUIC_FT_ACK_ECN:
            if (ctx->level == ssl_encryption_application) {
                /* force generation of most recent acknowledgment */
                ctx->send_ack = NJT_QUIC_MAX_ACK_GAP;
            }

            njt_quic_free_frame(c, f);
            break;

        case NJT_QUIC_FT_PING:
        case NJT_QUIC_FT_PATH_CHALLENGE:
        case NJT_QUIC_FT_PATH_RESPONSE:
        case NJT_QUIC_FT_CONNECTION_CLOSE:
            njt_quic_free_frame(c, f);
            break;

        case NJT_QUIC_FT_MAX_DATA:
            f->u.max_data.max_data = qc->streams.recv_max_data;
            njt_quic_queue_frame(qc, f);
            break;

        case NJT_QUIC_FT_MAX_STREAMS:
        case NJT_QUIC_FT_MAX_STREAMS2:
            f->u.max_streams.limit = f->u.max_streams.bidi
                                     ? qc->streams.client_max_streams_bidi
                                     : qc->streams.client_max_streams_uni;
            njt_quic_queue_frame(qc, f);
            break;

        case NJT_QUIC_FT_MAX_STREAM_DATA:
            qs = njt_quic_find_stream(&qc->streams.tree,
                                      f->u.max_stream_data.id);
            if (qs == NULL) {
                njt_quic_free_frame(c, f);
                break;
            }

            f->u.max_stream_data.limit = qs->recv_max_data;
            njt_quic_queue_frame(qc, f);
            break;

        case NJT_QUIC_FT_STREAM:
            qs = njt_quic_find_stream(&qc->streams.tree, f->u.stream.stream_id);

            if (qs) {
                if (qs->send_state == NJT_QUIC_STREAM_SEND_RESET_SENT
                    || qs->send_state == NJT_QUIC_STREAM_SEND_RESET_RECVD)
                {
                    njt_quic_free_frame(c, f);
                    break;
                }
            }

            /* fall through */

        default:
            njt_queue_insert_tail(&ctx->frames, &f->queue);
        }

    } while (q != njt_queue_sentinel(&ctx->sent));

    if (qc->closing) {
        return;
    }

    njt_post_event(&qc->push, &njt_posted_events);
}


static void
njt_quic_congestion_lost(njt_connection_t *c, njt_quic_frame_t *f)
{
    njt_uint_t              blocked;
    njt_msec_t              timer;
    njt_quic_congestion_t  *cg;
    njt_quic_connection_t  *qc;

    if (f->plen == 0) {
        return;
    }

    qc = njt_quic_get_connection(c);
    cg = &qc->congestion;

    if (f->pnum < qc->rst_pnum) {
        return;
    }

    blocked = (cg->in_flight >= cg->window) ? 1 : 0;

    cg->in_flight -= f->plen;
    f->plen = 0;

    timer = f->send_time - cg->recovery_start;

    if ((njt_msec_int_t) timer <= 0) {
        njt_log_debug3(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic congestion lost recovery win:%uz ss:%z if:%uz",
                       cg->window, cg->ssthresh, cg->in_flight);

        goto done;
    }

    cg->recovery_start = njt_current_msec;
    cg->window /= 2;

    if (cg->window < qc->tp.max_udp_payload_size * 2) {
        cg->window = qc->tp.max_udp_payload_size * 2;
    }

    cg->ssthresh = cg->window;

    njt_log_debug3(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic congestion lost win:%uz ss:%z if:%uz",
                   cg->window, cg->ssthresh, cg->in_flight);

done:

    if (blocked && cg->in_flight < cg->window) {
        njt_post_event(&qc->push, &njt_posted_events);
    }
}


void
njt_quic_set_lost_timer(njt_connection_t *c)
{
    njt_uint_t              i;
    njt_msec_t              now;
    njt_queue_t            *q;
    njt_msec_int_t          lost, pto, w;
    njt_quic_frame_t       *f;
    njt_quic_send_ctx_t    *ctx;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);
    now = njt_current_msec;

    lost = -1;
    pto = -1;

    for (i = 0; i < NJT_QUIC_SEND_CTX_LAST; i++) {
        ctx = &qc->send_ctx[i];

        if (njt_queue_empty(&ctx->sent)) {
            continue;
        }

        if (ctx->largest_ack != NJT_QUIC_UNSET_PN) {
            q = njt_queue_head(&ctx->sent);
            f = njt_queue_data(q, njt_quic_frame_t, queue);
            w = (njt_msec_int_t)
                            (f->send_time + njt_quic_lost_threshold(qc) - now);

            if (f->pnum <= ctx->largest_ack) {
                if (w < 0 || ctx->largest_ack - f->pnum >= NJT_QUIC_PKT_THR) {
                    w = 0;
                }

                if (lost == -1 || w < lost) {
                    lost = w;
                }
            }
        }

        q = njt_queue_last(&ctx->sent);
        f = njt_queue_data(q, njt_quic_frame_t, queue);
        w = (njt_msec_int_t)
                (f->send_time + (njt_quic_pto(c, ctx) << qc->pto_count) - now);

        if (w < 0) {
            w = 0;
        }

        if (pto == -1 || w < pto) {
            pto = w;
        }
    }

    if (qc->pto.timer_set) {
        njt_del_timer(&qc->pto);
    }

    if (lost != -1) {
        njt_log_debug1(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic lost timer lost:%M", lost);

        qc->pto.handler = njt_quic_lost_handler;
        njt_add_timer(&qc->pto, lost);
        return;
    }

    if (pto != -1) {
        njt_log_debug1(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic lost timer pto:%M", pto);

        qc->pto.handler = njt_quic_pto_handler;
        njt_add_timer(&qc->pto, pto);
        return;
    }

    njt_log_debug0(NJT_LOG_DEBUG_EVENT, c->log, 0, "quic lost timer unset");
}


njt_msec_t
njt_quic_pto(njt_connection_t *c, njt_quic_send_ctx_t *ctx)
{
    njt_msec_t              duration;
    njt_quic_connection_t  *qc;

    qc = njt_quic_get_connection(c);

    /* RFC 9002, Appendix A.8.  Setting the Loss Detection Timer */

    duration = qc->avg_rtt;
    duration += njt_max(4 * qc->rttvar, NJT_QUIC_TIME_GRANULARITY);

    if (ctx->level == ssl_encryption_application && c->ssl->handshaked) {
        duration += qc->ctp.max_ack_delay;
    }

    return duration;
}


static
void njt_quic_lost_handler(njt_event_t *ev)
{
    njt_connection_t  *c;

    njt_log_debug0(NJT_LOG_DEBUG_EVENT, ev->log, 0, "quic lost timer");

    c = ev->data;

    if (njt_quic_detect_lost(c, NULL) != NJT_OK) {
        njt_quic_close_connection(c, NJT_ERROR);
        return;
    }

    njt_quic_connstate_dbg(c);
}


void
njt_quic_pto_handler(njt_event_t *ev)
{
    njt_uint_t              i, n;
    njt_msec_t              now;
    njt_queue_t            *q;
    njt_msec_int_t          w;
    njt_connection_t       *c;
    njt_quic_frame_t       *f;
    njt_quic_send_ctx_t    *ctx;
    njt_quic_connection_t  *qc;

    njt_log_debug0(NJT_LOG_DEBUG_EVENT, ev->log, 0, "quic pto timer");

    c = ev->data;
    qc = njt_quic_get_connection(c);
    now = njt_current_msec;

    for (i = 0; i < NJT_QUIC_SEND_CTX_LAST; i++) {

        ctx = &qc->send_ctx[i];

        if (njt_queue_empty(&ctx->sent)) {
            continue;
        }

        q = njt_queue_last(&ctx->sent);
        f = njt_queue_data(q, njt_quic_frame_t, queue);
        w = (njt_msec_int_t)
                (f->send_time + (njt_quic_pto(c, ctx) << qc->pto_count) - now);

        if (f->pnum <= ctx->largest_ack
            && ctx->largest_ack != NJT_QUIC_UNSET_PN)
        {
            continue;
        }

        if (w > 0) {
            continue;
        }

        njt_log_debug2(NJT_LOG_DEBUG_EVENT, c->log, 0,
                       "quic pto %s pto_count:%ui",
                       njt_quic_level_name(ctx->level), qc->pto_count);

        for (n = 0; n < 2; n++) {

            f = njt_quic_alloc_frame(c);
            if (f == NULL) {
                goto failed;
            }

            f->level = ctx->level;
            f->type = NJT_QUIC_FT_PING;
            f->ignore_congestion = 1;

            if (njt_quic_frame_sendto(c, f, 0, qc->path) == NJT_ERROR) {
                goto failed;
            }
        }
    }

    qc->pto_count++;

    njt_quic_set_lost_timer(c);

    njt_quic_connstate_dbg(c);

    return;

failed:

    njt_quic_close_connection(c, NJT_ERROR);
    return;
}


njt_int_t
njt_quic_ack_packet(njt_connection_t *c, njt_quic_header_t *pkt)
{
    uint64_t                base, largest, smallest, gs, ge, gap, range, pn;
    uint64_t                prev_pending;
    njt_uint_t              i, nr;
    njt_quic_send_ctx_t    *ctx;
    njt_quic_ack_range_t   *r;
    njt_quic_connection_t  *qc;

    c->log->action = "preparing ack";

    qc = njt_quic_get_connection(c);

    ctx = njt_quic_get_send_ctx(qc, pkt->level);

    njt_log_debug4(NJT_LOG_DEBUG_EVENT, c->log, 0,
                   "quic njt_quic_ack_packet pn:%uL largest %L fr:%uL"
                   " nranges:%ui", pkt->pn, (int64_t) ctx->largest_range,
                   ctx->first_range, ctx->nranges);

    if (!njt_quic_keys_available(qc->keys, ctx->level, 1)) {
        return NJT_OK;
    }

    prev_pending = ctx->pending_ack;

    if (pkt->need_ack) {

        njt_post_event(&qc->push, &njt_posted_events);

        if (ctx->send_ack == 0) {
            ctx->ack_delay_start = njt_current_msec;
        }

        ctx->send_ack++;

        if (ctx->pending_ack == NJT_QUIC_UNSET_PN
            || ctx->pending_ack < pkt->pn)
        {
            ctx->pending_ack = pkt->pn;
        }
    }

    base = ctx->largest_range;
    pn = pkt->pn;

    if (base == NJT_QUIC_UNSET_PN) {
        ctx->largest_range = pn;
        ctx->largest_received = pkt->received;
        return NJT_OK;
    }

    if (base == pn) {
        return NJT_OK;
    }

    largest = base;
    smallest = largest - ctx->first_range;

    if (pn > base) {

        if (pn - base == 1) {
            ctx->first_range++;
            ctx->largest_range = pn;
            ctx->largest_received = pkt->received;

            return NJT_OK;

        } else {
            /* new gap in front of current largest */

            /* no place for new range, send current range as is */
            if (ctx->nranges == NJT_QUIC_MAX_RANGES) {

                if (prev_pending != NJT_QUIC_UNSET_PN) {
                    if (njt_quic_send_ack(c, ctx) != NJT_OK) {
                        return NJT_ERROR;
                    }
                }

                if (prev_pending == ctx->pending_ack || !pkt->need_ack) {
                    ctx->pending_ack = NJT_QUIC_UNSET_PN;
                }
            }

            gap = pn - base - 2;
            range = ctx->first_range;

            ctx->first_range = 0;
            ctx->largest_range = pn;
            ctx->largest_received = pkt->received;

            /* packet is out of order, force send */
            if (pkt->need_ack) {
                ctx->send_ack = NJT_QUIC_MAX_ACK_GAP;
            }

            i = 0;

            goto insert;
        }
    }

    /*  pn < base, perform lookup in existing ranges */

    /* packet is out of order */
    if (pkt->need_ack) {
        ctx->send_ack = NJT_QUIC_MAX_ACK_GAP;
    }

    if (pn >= smallest && pn <= largest) {
        return NJT_OK;
    }

#if (NJT_SUPPRESS_WARN)
    r = NULL;
#endif

    for (i = 0; i < ctx->nranges; i++) {
        r = &ctx->ranges[i];

        ge = smallest - 1;
        gs = ge - r->gap;

        if (pn >= gs && pn <= ge) {

            if (gs == ge) {
                /* gap size is exactly one packet, now filled */

                /* data moves to previous range, current is removed */

                if (i == 0) {
                    ctx->first_range += r->range + 2;

                } else {
                    ctx->ranges[i - 1].range += r->range + 2;
                }

                nr = ctx->nranges - i - 1;
                if (nr) {
                    njt_memmove(&ctx->ranges[i], &ctx->ranges[i + 1],
                                sizeof(njt_quic_ack_range_t) * nr);
                }

                ctx->nranges--;

            } else if (pn == gs) {
                /* current gap shrinks from tail (current range grows) */
                r->gap--;
                r->range++;

            } else if (pn == ge) {
                /* current gap shrinks from head (previous range grows) */
                r->gap--;

                if (i == 0) {
                    ctx->first_range++;

                } else {
                    ctx->ranges[i - 1].range++;
                }

            } else {
                /* current gap is split into two parts */

                gap = ge - pn - 1;
                range = 0;

                if (ctx->nranges == NJT_QUIC_MAX_RANGES) {
                    if (prev_pending != NJT_QUIC_UNSET_PN) {
                        if (njt_quic_send_ack(c, ctx) != NJT_OK) {
                            return NJT_ERROR;
                        }
                    }

                    if (prev_pending == ctx->pending_ack || !pkt->need_ack) {
                        ctx->pending_ack = NJT_QUIC_UNSET_PN;
                    }
                }

                r->gap = pn - gs - 1;
                goto insert;
            }

            return NJT_OK;
        }

        largest = smallest - r->gap - 2;
        smallest = largest - r->range;

        if (pn >= smallest && pn <= largest) {
            /* this packet number is already known */
            return NJT_OK;
        }

    }

    if (pn == smallest - 1) {
        /* extend first or last range */

        if (i == 0) {
            ctx->first_range++;

        } else {
            r->range++;
        }

        return NJT_OK;
    }

    /* nothing found, add new range at the tail  */

    if (ctx->nranges == NJT_QUIC_MAX_RANGES) {
        /* packet is too old to keep it */

        if (pkt->need_ack) {
            return njt_quic_send_ack_range(c, ctx, pn, pn);
        }

        return NJT_OK;
    }

    gap = smallest - 2 - pn;
    range = 0;

insert:

    if (ctx->nranges < NJT_QUIC_MAX_RANGES) {
        ctx->nranges++;
    }

    njt_memmove(&ctx->ranges[i + 1], &ctx->ranges[i],
                sizeof(njt_quic_ack_range_t) * (ctx->nranges - i - 1));

    ctx->ranges[i].gap = gap;
    ctx->ranges[i].range = range;

    return NJT_OK;
}


njt_int_t
njt_quic_generate_ack(njt_connection_t *c, njt_quic_send_ctx_t *ctx)
{
    njt_msec_t              delay;
    njt_quic_connection_t  *qc;

    if (!ctx->send_ack) {
        return NJT_OK;
    }

    if (ctx->level == ssl_encryption_application) {

        delay = njt_current_msec - ctx->ack_delay_start;
        qc = njt_quic_get_connection(c);

        if (njt_queue_empty(&ctx->frames)
            && ctx->send_ack < NJT_QUIC_MAX_ACK_GAP
            && delay < qc->tp.max_ack_delay)
        {
            if (!qc->push.timer_set && !qc->closing) {
                njt_add_timer(&qc->push,
                              qc->tp.max_ack_delay - delay);
            }

            return NJT_OK;
        }
    }

    if (njt_quic_send_ack(c, ctx) != NJT_OK) {
        return NJT_ERROR;
    }

    ctx->send_ack = 0;

    return NJT_OK;
}
