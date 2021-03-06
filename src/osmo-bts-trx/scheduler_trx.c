/* Scheduler worker functions for OsmoBTS-TRX */

/* (C) 2013 by Andreas Eversberg <jolly@eversberg.eu>
 * (C) 2015 by Alexander Chemeris <Alexander.Chemeris@fairwaves.co>
 * (C) 2015-2017 by Harald Welte <laforge@gnumonks.org>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/timerfd.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/timer_compat.h>
#include <osmocom/codec/codec.h>
#include <osmocom/codec/ecu.h>
#include <osmocom/core/bits.h>
#include <osmocom/gsm/a5.h>
#include <osmocom/coding/gsm0503_coding.h>
#include <osmocom/coding/gsm0503_amr_dtx.h>


#include <osmo-bts/gsm_data.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/l1sap.h>
#include <osmo-bts/msg_utils.h>
#include <osmo-bts/scheduler.h>
#include <osmo-bts/scheduler_backend.h>
#include <osmocom/gsm/gsm0502.h>

#include "l1_if.h"
#include "trx_if.h"
#include "loops.h"

extern void *tall_bts_ctx;

/* Maximum size of a EGPRS message in bytes */
#define EGPRS_0503_MAX_BYTES		155


/* Compute the bit error rate in 1/10000 units */
static inline uint16_t compute_ber10k(int n_bits_total, int n_errors)
{
	if (n_bits_total == 0)
		return 10000;
	else
		return 10000 * n_errors / n_bits_total;
}

/*
 * TX on downlink
 */

/* an IDLE burst returns nothing. on C0 it is replaced by dummy burst */
ubit_t *tx_idle_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting IDLE\n");

	if (nbits)
		*nbits = GSM_BURST_LEN;

	return NULL;
}

/* obtain a to-be-transmitted FCCH (frequency correction channel) burst */
ubit_t *tx_fcch_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting FCCH\n");

	if (nbits)
		*nbits = GSM_BURST_LEN;

	/* BURST BYPASS */

	return (ubit_t *) _sched_fcch_burst;
}

/* obtain a to-be-transmitted SCH (synchronization channel) burst */
ubit_t *tx_sch_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	static ubit_t bits[GSM_BURST_LEN], burst[78];
	uint8_t sb_info[4];
	struct	gsm_time t;
	uint8_t t3p, bsic;

	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting SCH\n");

	/* BURST BYPASS */

	/* create SB info from GSM time and BSIC */
	gsm_fn2gsmtime(&t, fn);
	t3p = t.t3 / 10;
	bsic = l1t->trx->bts->bsic;
	sb_info[0] =
		((bsic &  0x3f) << 2) |
		((t.t1 & 0x600) >> 9);
	sb_info[1] =
		((t.t1 & 0x1fe) >> 1);
	sb_info[2] =
		((t.t1 & 0x001) << 7) |
		((t.t2 &  0x1f) << 2) |
		((t3p  &   0x6) >> 1);
	sb_info[3] =
		 (t3p  &   0x1);

	/* encode bursts */
	gsm0503_sch_encode(burst, sb_info);

	/* compose burst */
	memset(bits, 0, 3);
	memcpy(bits + 3, burst, 39);
	memcpy(bits + 42, _sched_sch_train, 64);
	memcpy(bits + 106, burst + 39, 39);
	memset(bits + 145, 0, 3);

	if (nbits)
		*nbits = GSM_BURST_LEN;

	return bits;
}

/* obtain a to-be-transmitted data (SACCH/SDCCH) burst */
ubit_t *tx_data_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, tn);
	struct gsm_bts_trx_ts *ts = &l1t->trx->ts[tn];
	struct msgb *msg = NULL; /* make GCC happy */
	ubit_t *burst, **bursts_p = &l1ts->chan_state[chan].dl_bursts;
	static ubit_t bits[GSM_BURST_LEN];

	/* send burst, if we already got a frame */
	if (bid > 0) {
		if (!*bursts_p)
			return NULL;
		goto send_burst;
	}

	/* get mac block from queue */
	msg = _sched_dequeue_prim(l1t, tn, fn, chan);
	if (msg)
		goto got_msg;

	LOGL1S(DL1P, LOGL_INFO, l1t, tn, chan, fn, "No prim for transmit.\n");

no_msg:
	/* free burst memory */
	if (*bursts_p) {
		talloc_free(*bursts_p);
		*bursts_p = NULL;
	}
	return NULL;

got_msg:
	/* check validity of message */
	if (msgb_l2len(msg) != GSM_MACBLOCK_LEN) {
		LOGL1S(DL1P, LOGL_FATAL, l1t, tn, chan, fn, "Prim not 23 bytes, please FIX! "
			"(len=%d)\n", msgb_l2len(msg));
		/* free message */
		msgb_free(msg);
		goto no_msg;
	}

	/* BURST BYPASS */

	/* handle loss detection of SACCH */
	if (L1SAP_IS_LINK_SACCH(trx_chan_desc[chan].link_id)) {
		/* count and send BFI */
		if (++(l1ts->chan_state[chan].lost_frames) > 1) {
			/* TODO: Should we pass old TOA here? Otherwise we risk
			 * unnecessary decreasing TA */

			/* Note: RSSI is set to 0 to indicate to the higher
			 * layers that this is a faked ph_data_ind */
			_sched_compose_ph_data_ind(l1t, tn, 0, chan, NULL, 0,
						   0, 0, 0, 10000,
						   PRES_INFO_INVALID);
		}
	}

	/* allocate burst memory, if not already */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx, 464);
		if (!*bursts_p)
			return NULL;
	}

	/* encode bursts */
	gsm0503_xcch_encode(*bursts_p, msg->l2h);

	/* free message */
	msgb_free(msg);

send_burst:
	/* compose burst */
	burst = *bursts_p + bid * 116;
	memset(bits, 0, 3);
	memcpy(bits + 3, burst, 58);
	memcpy(bits + 61, _sched_tsc[gsm_ts_tsc(ts)], 26);
	memcpy(bits + 87, burst + 58, 58);
	memset(bits + 145, 0, 3);

	if (nbits)
		*nbits = GSM_BURST_LEN;

	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting burst=%u.\n", bid);

	return bits;
}

/* obtain a to-be-transmitted PDTCH (packet data) burst */
ubit_t *tx_pdtch_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, tn);
	struct gsm_bts_trx_ts *ts = &l1t->trx->ts[tn];
	struct msgb *msg = NULL; /* make GCC happy */
	ubit_t *burst, **bursts_p = &l1ts->chan_state[chan].dl_bursts;
	enum trx_burst_type *burst_type = &l1ts->chan_state[chan].dl_burst_type;
	static ubit_t bits[EGPRS_BURST_LEN];
	int rc = 0;

	/* send burst, if we already got a frame */
	if (bid > 0) {
		if (!*bursts_p)
			return NULL;
		goto send_burst;
	}

	/* get mac block from queue */
	msg = _sched_dequeue_prim(l1t, tn, fn, chan);
	if (msg)
		goto got_msg;

	LOGL1S(DL1P, LOGL_INFO, l1t, tn, chan, fn, "No prim for transmit.\n");

no_msg:
	/* free burst memory */
	if (*bursts_p) {
		talloc_free(*bursts_p);
		*bursts_p = NULL;
	}
	return NULL;

got_msg:
	/* BURST BYPASS */

	/* allocate burst memory, if not already */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx,
					     GSM0503_EGPRS_BURSTS_NBITS);
		if (!*bursts_p)
			return NULL;
	}

	/* encode bursts */
	rc = gsm0503_pdtch_egprs_encode(*bursts_p, msg->l2h, msg->tail - msg->l2h);
	if (rc < 0)
		rc = gsm0503_pdtch_encode(*bursts_p, msg->l2h, msg->tail - msg->l2h);

	/* check validity of message */
	if (rc < 0) {
		LOGL1S(DL1P, LOGL_FATAL, l1t, tn, chan, fn, "Prim invalid length, please FIX! "
			"(len=%ld)\n", (long)(msg->tail - msg->l2h));
		/* free message */
		msgb_free(msg);
		goto no_msg;
	} else if (rc == GSM0503_EGPRS_BURSTS_NBITS) {
		*burst_type = TRX_BURST_8PSK;
	} else {
		*burst_type = TRX_BURST_GMSK;
	}

	/* free message */
	msgb_free(msg);

send_burst:
	/* compose burst */
	if (*burst_type == TRX_BURST_8PSK) {
		burst = *bursts_p + bid * 348;
		memset(bits, 1, 9);
		memcpy(bits + 9, burst, 174);
		memcpy(bits + 183, _sched_egprs_tsc[gsm_ts_tsc(ts)], 78);
		memcpy(bits + 261, burst + 174, 174);
		memset(bits + 435, 1, 9);

		if (nbits)
			*nbits = EGPRS_BURST_LEN;
	} else {
		burst = *bursts_p + bid * 116;
		memset(bits, 0, 3);
		memcpy(bits + 3, burst, 58);
		memcpy(bits + 61, _sched_tsc[gsm_ts_tsc(ts)], 26);
		memcpy(bits + 87, burst + 58, 58);
		memset(bits + 145, 0, 3);

		if (nbits)
			*nbits = GSM_BURST_LEN;
	}

	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting burst=%u.\n", bid);

	return bits;
}

/* determine if the FN is transmitting a CMR (1) or not (0) */
static inline int fn_is_codec_mode_request(uint32_t fn)
{
	return (((fn + 4) % 26) >> 2) & 1;
}

/* common section for generation of TCH bursts (TCH/H and TCH/F) */
static void tx_tch_common(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, struct msgb **_msg_tch,
	struct msgb **_msg_facch)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, tn);
	struct msgb *msg1, *msg2, *msg_tch = NULL, *msg_facch = NULL;
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	uint8_t rsl_cmode = chan_state->rsl_cmode;
	uint8_t tch_mode = chan_state->tch_mode;
	struct osmo_phsap_prim *l1sap;
	int32_t *toa256_sum = &chan_state->toa256_sum;
	uint8_t *toa_num = &chan_state->toa_num;
	int16_t toa256;

	/* handle loss detection of received TCH frames */
	if (rsl_cmode == RSL_CMOD_SPD_SPEECH
	    && ++(chan_state->lost_frames) > 5) {
		uint8_t tch_data[GSM_FR_BYTES];
		int len;

		LOGL1S(DL1P, LOGL_NOTICE, l1t, tn, chan, fn,
			"Missing TCH bursts detected, sending BFI\n");

		/* indicate bad frame */
		switch (tch_mode) {
		case GSM48_CMODE_SPEECH_V1: /* FR / HR */
			if (chan != TRXC_TCHF) { /* HR */
				tch_data[0] = 0x70; /* F = 0, FT = 111 */
				memset(tch_data + 1, 0, 14);
				len = 15;
				break;
			}
			memset(tch_data, 0, GSM_FR_BYTES);
			len = GSM_FR_BYTES;
			break;
		case GSM48_CMODE_SPEECH_EFR: /* EFR */
			if (chan != TRXC_TCHF)
				goto inval_mode1;
			memset(tch_data, 0, GSM_EFR_BYTES);
			len = GSM_EFR_BYTES;
			break;
		case GSM48_CMODE_SPEECH_AMR: /* AMR */
			len = osmo_amr_rtp_enc(tch_data,
				chan_state->codec[chan_state->dl_cmr],
				chan_state->codec[chan_state->dl_ft], AMR_BAD);
			if (len < 2) {
				LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn,
				       "Failed to encode AMR_BAD frame (rc=%d), "
				       "not sending BFI\n", len);
				return;
			}
			memset(tch_data + 2, 0, len - 2);
			break;
		default:
inval_mode1:
			LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn, "TCH mode invalid, please fix!\n");
			len = 0;
		}

		if (len) {
			if (*toa_num == 0)
				toa256 = 0;
			else
				toa256 = *toa256_sum / *toa_num;

			/* Note: RSSI is set to 0 to indicate to the higher
			 * layers that this is a faked tch_ind */
			_sched_compose_tch_ind(l1t, tn, fn, chan, tch_data, len, toa256, 10000, 0, 0);
		}
	}

	/* get frame and unlink from queue */
	msg1 = _sched_dequeue_prim(l1t, tn, fn, chan);
	msg2 = _sched_dequeue_prim(l1t, tn, fn, chan);
	if (msg1) {
		l1sap = msgb_l1sap_prim(msg1);
		if (l1sap->oph.primitive == PRIM_TCH) {
			msg_tch = msg1;
			if (msg2) {
				l1sap = msgb_l1sap_prim(msg2);
				if (l1sap->oph.primitive == PRIM_TCH) {
					LOGL1S(DL1P, LOGL_FATAL, l1t, tn, chan, fn,
						"TCH twice, please FIX!\n");
					msgb_free(msg2);
				} else
					msg_facch = msg2;
			}
		} else {
			msg_facch = msg1;
			if (msg2) {
				l1sap = msgb_l1sap_prim(msg2);
				if (l1sap->oph.primitive != PRIM_TCH) {
					LOGL1S(DL1P, LOGL_FATAL, l1t, tn, chan, fn,
						"FACCH twice, please FIX!\n");
					msgb_free(msg2);
				} else
					msg_tch = msg2;
			}
		}
	} else if (msg2) {
		l1sap = msgb_l1sap_prim(msg2);
		if (l1sap->oph.primitive == PRIM_TCH)
			msg_tch = msg2;
		else
			msg_facch = msg2;
	}

	/* check validity of message */
	if (msg_facch && msgb_l2len(msg_facch) != GSM_MACBLOCK_LEN) {
		LOGL1S(DL1P, LOGL_FATAL, l1t, tn, chan, fn, "Prim not 23 bytes, please FIX! "
			"(len=%d)\n", msgb_l2len(msg_facch));
		/* free message */
		msgb_free(msg_facch);
		msg_facch = NULL;
	}

	/* check validity of message, get AMR ft and cmr */
	if (!msg_facch && msg_tch) {
		int len;
		uint8_t cmr_codec;
		int cmr, ft, i;
		enum osmo_amr_type ft_codec;
		enum osmo_amr_quality bfi;
		int8_t sti, cmi;

		if (rsl_cmode != RSL_CMOD_SPD_SPEECH) {
			LOGL1S(DL1P, LOGL_NOTICE, l1t, tn, chan, fn, "Dropping speech frame, "
				"because we are not in speech mode\n");
			goto free_bad_msg;
		}

		switch (tch_mode) {
		case GSM48_CMODE_SPEECH_V1: /* FR / HR */
			if (chan != TRXC_TCHF) /* HR */
				len = 15;
			else
				len = GSM_FR_BYTES;
			break;
		case GSM48_CMODE_SPEECH_EFR: /* EFR */
			if (chan != TRXC_TCHF)
				goto inval_mode2;
			len = GSM_EFR_BYTES;
			break;
		case GSM48_CMODE_SPEECH_AMR: /* AMR */
			len = osmo_amr_rtp_dec(msg_tch->l2h, msgb_l2len(msg_tch),
					       &cmr_codec, &cmi, &ft_codec,
					       &bfi, &sti);
			cmr = -1;
			ft = -1;
			for (i = 0; i < chan_state->codecs; i++) {
				if (chan_state->codec[i] == cmr_codec)
					cmr = i;
				if (chan_state->codec[i] == ft_codec)
					ft = i;
			}
			if (cmr >= 0) { /* new request */
				chan_state->dl_cmr = cmr;
				/* disable AMR loop */
				trx_loop_amr_set(chan_state, 0);
			} else {
				/* enable AMR loop */
				trx_loop_amr_set(chan_state, 1);
			}
			if (ft < 0) {
				LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn,
					"Codec (FT = %d) of RTP frame not in list\n", ft_codec);
				goto free_bad_msg;
			}
			if (fn_is_codec_mode_request(fn) && chan_state->dl_ft != ft) {
				LOGL1S(DL1P, LOGL_NOTICE, l1t, tn, chan, fn, "Codec (FT = %d) "
					" of RTP cannot be changed now, but in next frame\n", ft_codec);
				goto free_bad_msg;
			}
			chan_state->dl_ft = ft;
			if (bfi == AMR_BAD) {
				LOGL1S(DL1P, LOGL_NOTICE, l1t, tn, chan, fn,
					"Transmitting 'bad AMR frame'\n");
				goto free_bad_msg;
			}
			break;
		default:
inval_mode2:
			LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn, "TCH mode invalid, please fix!\n");
			goto free_bad_msg;
		}
		if (len < 0) {
			LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn, "Cannot send invalid AMR payload\n");
			goto free_bad_msg;
		}
		if (msgb_l2len(msg_tch) != len) {
			LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn, "Cannot send payload with "
				"invalid length! (expecting %d, received %d)\n",
				len, msgb_l2len(msg_tch));
free_bad_msg:
			/* free message */
			msgb_free(msg_tch);
			msg_tch = NULL;
			goto send_frame;
		}
	}

send_frame:
	*_msg_tch = msg_tch;
	*_msg_facch = msg_facch;
}

/* obtain a to-be-transmitted TCH/F (Full Traffic Channel) burst */
ubit_t *tx_tchf_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	struct msgb *msg_tch = NULL, *msg_facch = NULL;
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, tn);
	struct gsm_bts_trx_ts *ts = &l1t->trx->ts[tn];
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	uint8_t tch_mode = chan_state->tch_mode;
	ubit_t *burst, **bursts_p = &chan_state->dl_bursts;
	static ubit_t bits[GSM_BURST_LEN];

	/* send burst, if we already got a frame */
	if (bid > 0) {
		if (!*bursts_p)
			return NULL;
		goto send_burst;
	}

	tx_tch_common(l1t, tn, fn, chan, bid, &msg_tch, &msg_facch);

	/* BURST BYPASS */

	/* allocate burst memory, if not already,
	 * otherwise shift buffer by 4 bursts for interleaving */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx, 928);
		if (!*bursts_p)
			return NULL;
	} else {
		memcpy(*bursts_p, *bursts_p + 464, 464);
		memset(*bursts_p + 464, 0, 464);
	}

	/* no message at all */
	if (!msg_tch && !msg_facch) {
		LOGL1S(DL1P, LOGL_INFO, l1t, tn, chan, fn, "No TCH or FACCH prim for transmit.\n");
		goto send_burst;
	}

	/* encode bursts (prioritize FACCH) */
	if (msg_facch)
		gsm0503_tch_fr_encode(*bursts_p, msg_facch->l2h, msgb_l2len(msg_facch),
			1);
	else if (tch_mode == GSM48_CMODE_SPEECH_AMR)
		/* the first FN 4,13,21 defines that CMI is included in frame,
		 * the first FN 0,8,17 defines that CMR is included in frame.
		 */
		gsm0503_tch_afs_encode(*bursts_p, msg_tch->l2h + 2,
			msgb_l2len(msg_tch) - 2, fn_is_codec_mode_request(fn),
			chan_state->codec, chan_state->codecs,
			chan_state->dl_ft,
			chan_state->dl_cmr);
	else
		gsm0503_tch_fr_encode(*bursts_p, msg_tch->l2h, msgb_l2len(msg_tch), 1);

	/* free message */
	if (msg_tch)
		msgb_free(msg_tch);
	if (msg_facch)
		msgb_free(msg_facch);

send_burst:
	/* compose burst */
	burst = *bursts_p + bid * 116;
	memset(bits, 0, 3);
	memcpy(bits + 3, burst, 58);
	memcpy(bits + 61, _sched_tsc[gsm_ts_tsc(ts)], 26);
	memcpy(bits + 87, burst + 58, 58);
	memset(bits + 145, 0, 3);

	if (nbits)
		*nbits = GSM_BURST_LEN;

	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting burst=%u.\n", bid);

	return bits;
}

/* obtain a to-be-transmitted TCH/H (Half Traffic Channel) burst */
ubit_t *tx_tchh_fn(struct l1sched_trx *l1t, uint8_t tn, uint32_t fn,
	enum trx_chan_type chan, uint8_t bid, uint16_t *nbits)
{
	struct msgb *msg_tch = NULL, *msg_facch = NULL;
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, tn);
	struct gsm_bts_trx_ts *ts = &l1t->trx->ts[tn];
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	uint8_t tch_mode = chan_state->tch_mode;
	ubit_t *burst, **bursts_p = &chan_state->dl_bursts;
	static ubit_t bits[GSM_BURST_LEN];

	/* send burst, if we already got a frame */
	if (bid > 0) {
		if (!*bursts_p)
			return NULL;
		goto send_burst;
	}

	/* get TCH and/or FACCH */
	tx_tch_common(l1t, tn, fn, chan, bid, &msg_tch, &msg_facch);

	/* check for FACCH alignment */
	if (msg_facch && ((((fn + 4) % 26) >> 2) & 1)) {
		LOGL1S(DL1P, LOGL_ERROR, l1t, tn, chan, fn, "Cannot transmit FACCH starting on "
			"even frames, please fix RTS!\n");
		msgb_free(msg_facch);
		msg_facch = NULL;
	}

	/* BURST BYPASS */

	/* allocate burst memory, if not already,
	 * otherwise shift buffer by 2 bursts for interleaving */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx, 696);
		if (!*bursts_p)
			return NULL;
	} else {
		memcpy(*bursts_p, *bursts_p + 232, 232);
		if (chan_state->dl_ongoing_facch) {
			memcpy(*bursts_p + 232, *bursts_p + 464, 232);
			memset(*bursts_p + 464, 0, 232);
		} else {
			memset(*bursts_p + 232, 0, 232);
		}
	}

	/* no message at all */
	if (!msg_tch && !msg_facch && !chan_state->dl_ongoing_facch) {
		LOGL1S(DL1P, LOGL_INFO, l1t, tn, chan, fn, "No TCH or FACCH prim for transmit.\n");
		goto send_burst;
	}

	/* encode bursts (prioritize FACCH) */
	if (msg_facch) {
		gsm0503_tch_hr_encode(*bursts_p, msg_facch->l2h, msgb_l2len(msg_facch));
		chan_state->dl_ongoing_facch = 1; /* first of two TCH frames */
	} else if (chan_state->dl_ongoing_facch) /* second of two TCH frames */
		chan_state->dl_ongoing_facch = 0; /* we are done with FACCH */
	else if (tch_mode == GSM48_CMODE_SPEECH_AMR)
		/* the first FN 4,13,21 or 5,14,22 defines that CMI is included
		 * in frame, the first FN 0,8,17 or 1,9,18 defines that CMR is
		 * included in frame. */
		gsm0503_tch_ahs_encode(*bursts_p, msg_tch->l2h + 2,
			msgb_l2len(msg_tch) - 2, fn_is_codec_mode_request(fn),
			chan_state->codec, chan_state->codecs,
			chan_state->dl_ft,
			chan_state->dl_cmr);
	else
		gsm0503_tch_hr_encode(*bursts_p, msg_tch->l2h, msgb_l2len(msg_tch));

	/* free message */
	if (msg_tch)
		msgb_free(msg_tch);
	if (msg_facch)
		msgb_free(msg_facch);

send_burst:
	/* compose burst */
	burst = *bursts_p + bid * 116;
	memset(bits, 0, 3);
	memcpy(bits + 3, burst, 58);
	memcpy(bits + 61, _sched_tsc[gsm_ts_tsc(ts)], 26);
	memcpy(bits + 87, burst + 58, 58);
	memset(bits + 145, 0, 3);

	if (nbits)
		*nbits = GSM_BURST_LEN;

	LOGL1S(DL1P, LOGL_DEBUG, l1t, tn, chan, fn, "Transmitting burst=%u.\n", bid);

	return bits;
}


/*
 * RX on uplink (indication to upper layer)
 */

/* 3GPP TS 05.02, section 5.2.7 */
#define RACH_EXT_TAIL_LEN	8
#define RACH_SYNCH_SEQ_LEN	41

enum rach_synch_seq_t {
	RACH_SYNCH_SEQ_UNKNOWN = -1,
	RACH_SYNCH_SEQ_TS0, /* GSM, GMSK (default) */
	RACH_SYNCH_SEQ_TS1, /* EGPRS, 8-PSK */
	RACH_SYNCH_SEQ_TS2, /* EGPRS, GMSK */
	RACH_SYNCH_SEQ_NUM
};

static struct value_string rach_synch_seq_names[] = {
	{ RACH_SYNCH_SEQ_UNKNOWN,	"UNKNOWN" },
	{ RACH_SYNCH_SEQ_TS0,		"TS0: GSM, GMSK" },
	{ RACH_SYNCH_SEQ_TS1,		"TS1: EGPRS, 8-PSK" },
	{ RACH_SYNCH_SEQ_TS2,		"TS2: EGPRS, GMSK" },
	{ 0, NULL },
};

static enum rach_synch_seq_t rach_get_synch_seq(sbit_t *bits, int *best_score)
{
	sbit_t *synch_seq_burst = bits + RACH_EXT_TAIL_LEN;
	enum rach_synch_seq_t seq = RACH_SYNCH_SEQ_TS0;
	int score[RACH_SYNCH_SEQ_NUM] = { 0 };
	int max_score = INT_MIN;
	int i, j;

	/* 3GPP TS 05.02, section 5.2.7 "Access burst (AB)", synch. sequence bits */
	static const char synch_seq_ref[RACH_SYNCH_SEQ_NUM][RACH_SYNCH_SEQ_LEN] = {
		[RACH_SYNCH_SEQ_TS0] = "01001011011111111001100110101010001111000",
		[RACH_SYNCH_SEQ_TS1] = "01010100111110001000011000101111001001101",
		[RACH_SYNCH_SEQ_TS2] = "11101111001001110101011000001101101110111",
	};

	/* Get a multiplier for j-th bit of i-th synch. sequence */
#define RACH_SYNCH_SEQ_MULT \
	(synch_seq_ref[i][j] == '1' ? -1 : 1)

	/* For each synch. sequence, count the bit match score. Since we deal with
	 * soft-bits (-127...127), we sum the absolute values of matching ones,
	 * and subtract the absolute values of different ones, so the resulting
	 * score is more accurate than it could be with hard-bits. */
	for (i = 0; i < RACH_SYNCH_SEQ_NUM; i++) {
		for (j = 0; j < RACH_SYNCH_SEQ_LEN; j++)
			score[i] += RACH_SYNCH_SEQ_MULT * synch_seq_burst[j];

		/* Keep the maximum value updated */
		if (score[i] > max_score) {
			max_score = score[i];
			seq = i;
		}
	}

	/* Calculate an approximate level of our confidence */
	if (best_score != NULL)
		*best_score = max_score;

	/* At least 1/3 of a synch. sequence shall match */
	if (max_score < (127 * RACH_SYNCH_SEQ_LEN / 3))
		return RACH_SYNCH_SEQ_UNKNOWN;

	return seq;
}

int rx_rach_fn(struct l1sched_trx *l1t, enum trx_chan_type chan,
	       uint8_t bid, const struct trx_ul_burst_ind *bi)
{
	struct osmo_phsap_prim l1sap;
	int n_errors = 0;
	int n_bits_total = 0;
	uint16_t ra11;
	uint8_t ra;
	int rc;

	/* TSC (Training Sequence Code) is an optional parameter of the UL burst
	 * indication. We need this information in order to decide whether an
	 * Access Burst is 11-bit encoded or not (see OS#1854). If this information
	 * is absent, we try to correlate the received synch. sequence with the
	 * known ones (3GPP TS 05.02, section 5.2.7), and fall-back to the default
	 * TS0 if it fails. */
	enum rach_synch_seq_t synch_seq = RACH_SYNCH_SEQ_TS0;
	int best_score = 127 * RACH_SYNCH_SEQ_LEN;

	/* If logical channel is not either of RACH, PDTCH or PTCCH, this is a
	 * handover Access Burst, which is always encoded as 8-bit and shall
	 * contain the generic training sequence (TS0). */
	if (chan == TRXC_RACH || chan == TRXC_PDTCH || chan == TRXC_PTCCH) {
		if (bi->flags & TRX_BI_F_TS_INFO)
			synch_seq = (enum rach_synch_seq_t) bi->tsc;
		else
			synch_seq = rach_get_synch_seq((sbit_t *) bi->burst, &best_score);
	}

	LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
	       "Received%s RACH (%s): rssi=%d toa256=%d",
	       (chan != TRXC_RACH) ? " handover" : "",
	       get_value_string(rach_synch_seq_names, synch_seq),
	       bi->rssi, bi->toa256);
	if (bi->flags & TRX_BI_F_CI_CB)
		LOGPC(DL1P, LOGL_DEBUG, " C/I=%d cB", bi->ci_cb);
	else
		LOGPC(DL1P, LOGL_DEBUG, " match=%.1f%%",
		      best_score * 100.0 / (127 * RACH_SYNCH_SEQ_LEN));
	LOGPC(DL1P, LOGL_DEBUG, "\n");

	/* Compose a new L1SAP primitive */
	memset(&l1sap, 0x00, sizeof(l1sap));
	osmo_prim_init(&l1sap.oph, SAP_GSM_PH, PRIM_PH_RACH, PRIM_OP_INDICATION, NULL);
	l1sap.u.rach_ind.chan_nr = trx_chan_desc[chan].chan_nr | bi->tn;
	l1sap.u.rach_ind.acc_delay = (bi->toa256 >= 0) ? bi->toa256 / 256 : 0;
	l1sap.u.rach_ind.acc_delay_256bits = bi->toa256;
	l1sap.u.rach_ind.rssi = bi->rssi;
	l1sap.u.rach_ind.fn = bi->fn;

	/* Link quality is defined by C/I (Carrier-to-Interference ratio),
	 * which has optional presence. If it's absent, report the
	 * minimum acceptable value to pass L1SAP checks. */
	if (bi->flags & TRX_BI_F_CI_CB)
		l1sap.u.rach_ind.lqual_cb = bi->ci_cb;
	else
		l1sap.u.rach_ind.lqual_cb = l1t->trx->bts->min_qual_rach;

	/* Decode RACH depending on its synch. sequence */
	switch (synch_seq) {
	case RACH_SYNCH_SEQ_TS1:
	case RACH_SYNCH_SEQ_TS2:
		rc = gsm0503_rach_ext_decode_ber(&ra11, bi->burst + RACH_EXT_TAIL_LEN + RACH_SYNCH_SEQ_LEN,
						 l1t->trx->bts->bsic, &n_errors, &n_bits_total);
		if (rc) {
			LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			       "Received bad Access Burst\n");
			return 0;
		}

		if (synch_seq == RACH_SYNCH_SEQ_TS1)
			l1sap.u.rach_ind.burst_type = GSM_L1_BURST_TYPE_ACCESS_1;
		else
			l1sap.u.rach_ind.burst_type = GSM_L1_BURST_TYPE_ACCESS_2;

		l1sap.u.rach_ind.is_11bit = 1;
		l1sap.u.rach_ind.ra = ra11;
		break;

	case RACH_SYNCH_SEQ_TS0:
	default:
		/* Fall-back to the default TS0 if needed */
		if (synch_seq != RACH_SYNCH_SEQ_TS0) {
			LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			       "Falling-back to the default TS0\n");
			synch_seq = RACH_SYNCH_SEQ_TS0;
		}

		rc = gsm0503_rach_decode_ber(&ra, bi->burst + RACH_EXT_TAIL_LEN + RACH_SYNCH_SEQ_LEN,
					     l1t->trx->bts->bsic, &n_errors, &n_bits_total);
		if (rc) {
			LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			       "Received bad Access Burst\n");
			return 0;
		}

		l1sap.u.rach_ind.burst_type = GSM_L1_BURST_TYPE_ACCESS_0;
		l1sap.u.rach_ind.is_11bit = 0;
		l1sap.u.rach_ind.ra = ra;
		break;
	}

	l1sap.u.rach_ind.ber10k = compute_ber10k(n_bits_total, n_errors);

	/* forward primitive */
	l1sap_up(l1t->trx, &l1sap);

	return 0;
}

/*! \brief a single (SDCCH/SACCH) burst was received by the PHY, process it */
int rx_data_fn(struct l1sched_trx *l1t, enum trx_chan_type chan,
	       uint8_t bid, const struct trx_ul_burst_ind *bi)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, bi->tn);
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	sbit_t *burst, **bursts_p = &chan_state->ul_bursts;
	uint32_t *first_fn = &chan_state->ul_first_fn;
	uint8_t *mask = &chan_state->ul_mask;
	float *rssi_sum = &chan_state->rssi_sum;
	uint8_t *rssi_num = &chan_state->rssi_num;
	int32_t *toa256_sum = &chan_state->toa256_sum;
	uint8_t *toa_num = &chan_state->toa_num;
	int32_t *ci_cb_sum = &chan_state->ci_cb_sum;
	uint8_t *ci_cb_num = &chan_state->ci_cb_num;
	uint8_t l2[GSM_MACBLOCK_LEN], l2_len;
	int n_errors = 0;
	int n_bits_total = 0;
	int16_t lqual_cb;
	uint16_t ber10k;
	int rc;

	/* handle RACH, if handover RACH detection is turned on */
	if (chan_state->ho_rach_detect == 1)
		return rx_rach_fn(l1t, chan, bid, bi);

	LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
	       "Received Data, bid=%u\n", bid);

	/* allocate burst memory, if not already */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx, 464);
		if (!*bursts_p)
			return -ENOMEM;
	}

	/* clear burst & store frame number of first burst */
	if (bid == 0) {
		memset(*bursts_p, 0, 464);
		*mask = 0x0;
		*first_fn = bi->fn;
		*rssi_sum = 0;
		*rssi_num = 0;
		*toa256_sum = 0;
		*toa_num = 0;
		*ci_cb_sum = 0;
		*ci_cb_num = 0;
	}

	/* update mask + RSSI */
	*mask |= (1 << bid);
	*rssi_sum += bi->rssi;
	(*rssi_num)++;
	*toa256_sum += bi->toa256;
	(*toa_num)++;

	/* C/I: Carrier-to-Interference ratio (in centiBels) */
	if (bi->flags & TRX_BI_F_CI_CB) {
		*ci_cb_sum += bi->ci_cb;
		(*ci_cb_num)++;
	}

	/* Copy burst to buffer of 4 bursts. If the burst indication contains
	 * no data, ensure that the buffer does not stay uninitialized */
	burst = *bursts_p + bid * 116;
	if (bi->burst_len > 0) {
		memcpy(burst, bi->burst + 3, 58);
		memcpy(burst + 58, bi->burst + 87, 58);
	} else
		memset(burst, 0, 58 * 2);

	/* wait until complete set of bursts */
	if (bid != 3)
		return 0;

	/* check for complete set of bursts */
	if ((*mask & 0xf) != 0xf) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received incomplete data (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);

		/* we require first burst to have correct FN */
		if (!(*mask & 0x1)) {
			*mask = 0x0;
			return 0;
		}
	}
	*mask = 0x0;

	/* decode */
	rc = gsm0503_xcch_decode(l2, *bursts_p, &n_errors, &n_bits_total);
	if (rc) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received bad data (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
		l2_len = 0;
	} else
		l2_len = GSM_MACBLOCK_LEN;

	lqual_cb = *ci_cb_num ? (*ci_cb_sum / *ci_cb_num) : 0;
	ber10k = compute_ber10k(n_bits_total, n_errors);
	return _sched_compose_ph_data_ind(l1t, bi->tn, *first_fn,
					  chan, l2, l2_len,
					  *rssi_sum / *rssi_num,
					  *toa256_sum / *toa_num,
					  lqual_cb, ber10k,
					  PRES_INFO_UNKNOWN);
}

/*! \brief a single PDTCH burst was received by the PHY, process it */
int rx_pdtch_fn(struct l1sched_trx *l1t, enum trx_chan_type chan,
	       uint8_t bid, const struct trx_ul_burst_ind *bi)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, bi->tn);
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	sbit_t *burst, **bursts_p = &chan_state->ul_bursts;
	uint32_t *first_fn = &chan_state->ul_first_fn;
	uint8_t *mask = &chan_state->ul_mask;
	float *rssi_sum = &chan_state->rssi_sum;
	uint8_t *rssi_num = &chan_state->rssi_num;
	int32_t *toa256_sum = &chan_state->toa256_sum;
	uint8_t *toa_num = &chan_state->toa_num;
	int32_t *ci_cb_sum = &chan_state->ci_cb_sum;
	uint8_t *ci_cb_num = &chan_state->ci_cb_num;
	uint8_t l2[EGPRS_0503_MAX_BYTES];
	int n_errors = 0;
	int n_bursts_bits = 0;
	int n_bits_total = 0;
	int16_t lqual_cb;
	uint16_t ber10k;
	int rc;

	LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
		"Received PDTCH bid=%u\n", bid);

	/* allocate burst memory, if not already */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx,
					     GSM0503_EGPRS_BURSTS_NBITS);
		if (!*bursts_p)
			return -ENOMEM;
	}

	/* clear burst */
	if (bid == 0) {
		memset(*bursts_p, 0, GSM0503_EGPRS_BURSTS_NBITS);
		*mask = 0x0;
		*first_fn = bi->fn;
		*rssi_sum = 0;
		*rssi_num = 0;
		*toa256_sum = 0;
		*toa_num = 0;
		*ci_cb_sum = 0;
		*ci_cb_num = 0;
	}

	/* update mask + rssi */
	*mask |= (1 << bid);
	*rssi_sum += bi->rssi;
	(*rssi_num)++;
	*toa256_sum += bi->toa256;
	(*toa_num)++;

	/* C/I: Carrier-to-Interference ratio (in centiBels) */
	if (bi->flags & TRX_BI_F_CI_CB) {
		*ci_cb_sum += bi->ci_cb;
		(*ci_cb_num)++;
	}

	/* copy burst to buffer of 4 bursts */
	if (bi->burst_len == EGPRS_BURST_LEN) {
		burst = *bursts_p + bid * 348;
		memcpy(burst, bi->burst + 9, 174);
		memcpy(burst + 174, bi->burst + 261, 174);
		n_bursts_bits = GSM0503_EGPRS_BURSTS_NBITS;
	} else {
		burst = *bursts_p + bid * 116;
		memcpy(burst, bi->burst + 3, 58);
		memcpy(burst + 58, bi->burst + 87, 58);
		n_bursts_bits = GSM0503_GPRS_BURSTS_NBITS;
	}

	/* wait until complete set of bursts */
	if (bid != 3)
		return 0;

	/* check for complete set of bursts */
	if ((*mask & 0xf) != 0xf) {
		LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			"Received incomplete frame (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
	}
	*mask = 0x0;

	/*
	 * Attempt to decode EGPRS bursts first. For 8-PSK EGPRS this is all we
	 * do. Attempt GPRS decoding on EGPRS failure. If the burst is GPRS,
	 * then we incur decoding overhead of 31 bits on the Type 3 EGPRS
	 * header, which is tolerable.
	 */
	rc = gsm0503_pdtch_egprs_decode(l2, *bursts_p, n_bursts_bits,
				NULL, &n_errors, &n_bits_total);

	if ((bi->burst_len == GSM_BURST_LEN) && (rc < 0)) {
		rc = gsm0503_pdtch_decode(l2, *bursts_p, NULL,
				  &n_errors, &n_bits_total);
	}

	if (rc <= 0) {
		LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			"Received bad PDTCH (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
		return 0;
	}

	lqual_cb = *ci_cb_num ? (*ci_cb_sum / *ci_cb_num) : 0;
	ber10k = compute_ber10k(n_bits_total, n_errors);
	return _sched_compose_ph_data_ind(l1t, bi->tn,
					  *first_fn, chan, l2, rc,
					  *rssi_sum / *rssi_num,
					  *toa256_sum / *toa_num,
					  lqual_cb, ber10k,
					  PRES_INFO_BOTH);
}

/*! \brief a single TCH/F burst was received by the PHY, process it */
int rx_tchf_fn(struct l1sched_trx *l1t, enum trx_chan_type chan,
	       uint8_t bid, const struct trx_ul_burst_ind *bi)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, bi->tn);
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	sbit_t *burst, **bursts_p = &chan_state->ul_bursts;
	uint32_t *first_fn = &chan_state->ul_first_fn;
	uint8_t *mask = &chan_state->ul_mask;
	uint8_t rsl_cmode = chan_state->rsl_cmode;
	uint8_t tch_mode = chan_state->tch_mode;
	uint8_t tch_data[128]; /* just to be safe */
	int rc, amr = 0;
	int n_errors = 0;
	int n_bits_total = 0;
	bool bfi_flag = false;
	struct gsm_lchan *lchan =
		get_lchan_by_chan_nr(l1t->trx, trx_chan_desc[chan].chan_nr | bi->tn);
	unsigned int fn_begin;
	uint16_t ber10k;
	uint8_t is_sub = 0;
	uint8_t ft;

	/* handle rach, if handover rach detection is turned on */
	if (chan_state->ho_rach_detect == 1)
		return rx_rach_fn(l1t, chan, bid, bi);

	LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
	       "Received TCH/F, bid=%u\n", bid);

	/* allocate burst memory, if not already */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx, 928);
		if (!*bursts_p)
			return -ENOMEM;
	}

	/* clear burst */
	if (bid == 0) {
		memset(*bursts_p + 464, 0, 464);
		*mask = 0x0;
		*first_fn = bi->fn;
	}

	/* update mask */
	*mask |= (1 << bid);

	/* copy burst to end of buffer of 8 bursts */
	burst = *bursts_p + bid * 116 + 464;
	if (bi->burst_len > 0) {
		memcpy(burst, bi->burst + 3, 58);
		memcpy(burst + 58, bi->burst + 87, 58);
	} else
		memset(burst, 0, 116);

	/* wait until complete set of bursts */
	if (bid != 3)
		return 0;

	/* check for complete set of bursts */
	if ((*mask & 0xf) != 0xf) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received incomplete frame (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
	}
	*mask = 0x0;

	/* decode
	 * also shift buffer by 4 bursts for interleaving */
	switch ((rsl_cmode != RSL_CMOD_SPD_SPEECH) ? GSM48_CMODE_SPEECH_V1
								: tch_mode) {
	case GSM48_CMODE_SPEECH_V1: /* FR */
		rc = gsm0503_tch_fr_decode(tch_data, *bursts_p, 1, 0, &n_errors, &n_bits_total);
		if (rc >= 0)
			lchan_set_marker(osmo_fr_check_sid(tch_data, rc), lchan); /* DTXu */
		break;
	case GSM48_CMODE_SPEECH_EFR: /* EFR */
		rc = gsm0503_tch_fr_decode(tch_data, *bursts_p, 1, 1, &n_errors, &n_bits_total);
		break;
	case GSM48_CMODE_SPEECH_AMR: /* AMR */
		/* the first FN 0,8,17 defines that CMI is included in frame,
		 * the first FN 4,13,21 defines that CMR is included in frame.
		 * NOTE: A frame ends 7 FN after start.
		 */

		/* The AFS_ONSET frame itself does not result into an RTP frame
		 * since it only contains a recognition pattern that marks the
		 * end of the DTX interval. To mark the end of the DTX interval
		 * in the RTP stream as well, the voice frame after the
		 * AFS_ONSET frame is used. */
		if (chan_state->amr_last_dtx == AFS_ONSET)
			lchan_set_marker(false, lchan);

		/* we store tch_data + 2 header bytes, the amr variable set to
		 * 2 will allow us to skip the first 2 bytes in case we did
		 * receive an FACCH frame instead of a voice frame (we do not
		 * know this before we actually decode the frame) */
		amr = 2;
		rc = gsm0503_tch_afs_decode_dtx(tch_data + amr, *bursts_p,
			(((bi->fn + 26 - 7) % 26) >> 2) & 1, chan_state->codec,
			chan_state->codecs, &chan_state->ul_ft,
			&chan_state->ul_cmr, &n_errors, &n_bits_total, &chan_state->amr_last_dtx);

		/* Tag all frames that are not regular AMR voice frames as
		 * SUB-Frames */
		if (chan_state->amr_last_dtx != AMR_OTHER) {
			LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			       "Received AMR SID frame: %s\n",
			       gsm0503_amr_dtx_frame_name(chan_state->amr_last_dtx));
			is_sub = 1;
		}

		/* The occurrence of the following frames indicates that we
		 * are either at the beginning or in the middle of a talk
		 * spurt. We update the SID status accordingly, but we do
		 * not want the marker to be set, since this must only
		 * happen when the talk spurt is over (see above) */
		switch (chan_state->amr_last_dtx) {
		case AFS_SID_FIRST:
		case AFS_SID_UPDATE:
		case AFS_SID_UPDATE_CN:
			lchan_set_marker(true, lchan);
			lchan->rtp_tx_marker = false;
			break;
		}

		if (rc)
			trx_loop_amr_input(l1t,
				trx_chan_desc[chan].chan_nr | bi->tn, chan_state,
				n_errors, n_bits_total);
		/* only good speech frames get rtp header */
		if (rc != GSM_MACBLOCK_LEN && rc >= 4) {
			if (chan_state->amr_last_dtx == AMR_OTHER) {
				ft = chan_state->codec[chan_state->ul_cmr];
			} else {
				/* SID frames will always get Frame Type Index 8 (AMR_SID) */
				ft = AMR_SID;
			}
			rc = osmo_amr_rtp_enc(tch_data,
				chan_state->codec[chan_state->ul_cmr],
			        ft, AMR_GOOD);
		}

		break;
	default:
		LOGL1S(DL1P, LOGL_ERROR, l1t, bi->tn, chan, bi->fn,
			"TCH mode %u invalid, please fix!\n",
			tch_mode);
		return -EINVAL;
	}
	memcpy(*bursts_p, *bursts_p + 464, 464);

	/* Check if the frame is bad */
	if (rc < 0) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received bad data (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
		bfi_flag = true;
	} else if (rc < 4) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received bad data (%u/%u) with invalid codec mode %d\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period, rc);
		bfi_flag = true;
	}

	if (rc != GSM_MACBLOCK_LEN && lchan->ecu_state)
		osmo_ecu_frame_in(lchan->ecu_state, bfi_flag, tch_data, rc);

	ber10k = compute_ber10k(n_bits_total, n_errors);
	if (bfi_flag)
		goto bfi;

	/* FACCH */
	if (rc == GSM_MACBLOCK_LEN) {
		fn_begin = gsm0502_fn_remap(bi->fn, FN_REMAP_FACCH_F);
		_sched_compose_ph_data_ind(l1t, bi->tn, fn_begin, chan,
			tch_data + amr, GSM_MACBLOCK_LEN,
			/* FIXME: AVG RSSI and ToA256 */
			bi->rssi, bi->toa256,
			0 /* FIXME: AVG C/I */,
			ber10k, PRES_INFO_UNKNOWN);
bfi:
		if (rsl_cmode == RSL_CMOD_SPD_SPEECH) {
			/* indicate bad frame */
			if (lchan->tch.dtx.ul_sid) {
				/* DTXu: pause in progress. Push empty payload to upper layers */
				rc = 0;
				goto compose_l1sap;
			}

			/* If there is an ECU active on this channel, use its output */
			if (lchan->ecu_state) {
				rc = osmo_ecu_frame_out(lchan->ecu_state, tch_data);
				if (rc >= 0) /* Otherwise we send a BFI */
					goto compose_l1sap;
			}

			switch (tch_mode) {
			case GSM48_CMODE_SPEECH_V1: /* FR */
				memset(tch_data, 0, GSM_FR_BYTES);
				tch_data[0] = 0xd0;
				rc = GSM_FR_BYTES;
				break;
			case GSM48_CMODE_SPEECH_EFR: /* EFR */
				memset(tch_data, 0, GSM_EFR_BYTES);
				tch_data[0] = 0xc0;
				rc = GSM_EFR_BYTES;
				break;
			case GSM48_CMODE_SPEECH_AMR: /* AMR */
				rc = osmo_amr_rtp_enc(tch_data,
					chan_state->codec[chan_state->dl_cmr],
					chan_state->codec[chan_state->dl_ft],
					AMR_BAD);
				if (rc < 2) {
					LOGL1S(DL1P, LOGL_ERROR, l1t, bi->tn, chan, bi->fn,
					       "Failed to encode AMR_BAD frame (rc=%d), "
					       "not sending BFI\n", rc);
					return -EINVAL;
				}
				memset(tch_data + 2, 0, rc - 2);
				break;
			default:
				LOGL1S(DL1P, LOGL_ERROR, l1t, bi->tn, chan, bi->fn,
					"TCH mode %u invalid, please fix!\n", tch_mode);
				return -EINVAL;
			}
		}
	}

	if (rsl_cmode != RSL_CMOD_SPD_SPEECH)
		return 0;

	/* TCH or BFI */
compose_l1sap:
	fn_begin = gsm0502_fn_remap(bi->fn, FN_REMAP_TCH_F);
	return _sched_compose_tch_ind(l1t, bi->tn, fn_begin, chan,
				      tch_data, rc, bi->toa256, ber10k, bi->rssi, is_sub);
}

/*! \brief a single TCH/H burst was received by the PHY, process it */
int rx_tchh_fn(struct l1sched_trx *l1t, enum trx_chan_type chan,
	       uint8_t bid, const struct trx_ul_burst_ind *bi)
{
	struct l1sched_ts *l1ts = l1sched_trx_get_ts(l1t, bi->tn);
	struct l1sched_chan_state *chan_state = &l1ts->chan_state[chan];
	sbit_t *burst, **bursts_p = &chan_state->ul_bursts;
	uint32_t *first_fn = &chan_state->ul_first_fn;
	uint8_t *mask = &chan_state->ul_mask;
	uint8_t rsl_cmode = chan_state->rsl_cmode;
	uint8_t tch_mode = chan_state->tch_mode;
	uint8_t tch_data[128]; /* just to be safe */
	int rc, amr = 0;
	int n_errors = 0;
	int n_bits_total = 0;
	bool bfi_flag = false;
	struct gsm_lchan *lchan =
		get_lchan_by_chan_nr(l1t->trx, trx_chan_desc[chan].chan_nr | bi->tn);
	/* Note on FN-10: If we are at FN 10, we decoded an even aligned
	 * TCH/FACCH frame, because our burst buffer carries 6 bursts.
	 * Even FN ending at: 10,11,19,20,2,3
	 */
	int fn_is_odd = (((bi->fn + 26 - 10) % 26) >> 2) & 1;
	unsigned int fn_begin;
	uint16_t ber10k;
	uint8_t is_sub = 0;
	uint8_t ft;

	/* handle RACH, if handover RACH detection is turned on */
	if (chan_state->ho_rach_detect == 1)
		return rx_rach_fn(l1t, chan, bid, bi);

	LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
		"Received TCH/H, bid=%u\n", bid);

	/* allocate burst memory, if not already */
	if (!*bursts_p) {
		*bursts_p = talloc_zero_size(tall_bts_ctx, 696);
		if (!*bursts_p)
			return -ENOMEM;
	}

	/* clear burst */
	if (bid == 0) {
		memset(*bursts_p + 464, 0, 232);
		*mask = 0x0;
		*first_fn = bi->fn;
	}

	/* update mask */
	*mask |= (1 << bid);

	/* copy burst to end of buffer of 6 bursts */
	burst = *bursts_p + bid * 116 + 464;
	if (bi->burst_len > 0) {
		memcpy(burst, bi->burst + 3, 58);
		memcpy(burst + 58, bi->burst + 87, 58);
	} else
		memset(burst, 0, 116);

	/* wait until complete set of bursts */
	if (bid != 1)
		return 0;

	/* check for complete set of bursts */
	if ((*mask & 0x3) != 0x3) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received incomplete frame (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
	}
	*mask = 0x0;

	/* skip second of two TCH frames of FACCH was received */
	if (chan_state->ul_ongoing_facch) {
		chan_state->ul_ongoing_facch = 0;
		memcpy(*bursts_p, *bursts_p + 232, 232);
		memcpy(*bursts_p + 232, *bursts_p + 464, 232);
		ber10k = 0;
		goto bfi;
	}

	/* decode
	 * also shift buffer by 4 bursts for interleaving */
	switch ((rsl_cmode != RSL_CMOD_SPD_SPEECH) ? GSM48_CMODE_SPEECH_V1
								: tch_mode) {
	case GSM48_CMODE_SPEECH_V1: /* HR or signalling */
		/* Note on FN-10: If we are at FN 10, we decoded an even aligned
		 * TCH/FACCH frame, because our burst buffer carries 6 bursts.
		 * Even FN ending at: 10,11,19,20,2,3
		 */
		rc = gsm0503_tch_hr_decode(tch_data, *bursts_p,
			fn_is_odd, &n_errors, &n_bits_total);
		if (rc >= 0) /* DTXu */
			lchan_set_marker(osmo_hr_check_sid(tch_data, rc), lchan);
		break;
	case GSM48_CMODE_SPEECH_AMR: /* AMR */
		/* the first FN 0,8,17 or 1,9,18 defines that CMI is included
		 * in frame, the first FN 4,13,21 or 5,14,22 defines that CMR
		 * is included in frame.
		 */

		/* See comment in function rx_tchf_fn() */
		switch (chan_state->amr_last_dtx) {
		case AHS_ONSET:
		case AHS_SID_FIRST_INH:
		case AHS_SID_UPDATE_INH:
			lchan_set_marker(false, lchan);
			break;
		}

		/* See comment in function rx_tchf_fn() */
		amr = 2;
		rc = gsm0503_tch_ahs_decode_dtx(tch_data + amr, *bursts_p,
			fn_is_odd, fn_is_odd, chan_state->codec,
			chan_state->codecs, &chan_state->ul_ft,
			&chan_state->ul_cmr, &n_errors, &n_bits_total, &chan_state->amr_last_dtx);

		/* Tag all frames that are not regular AMR voice frames
		   as SUB-Frames */
		if (chan_state->amr_last_dtx != AMR_OTHER) {
			LOGL1S(DL1P, LOGL_DEBUG, l1t, bi->tn, chan, bi->fn,
			       "Received AMR SID frame: %s\n",
			       gsm0503_amr_dtx_frame_name(chan_state->amr_last_dtx));
			is_sub = 1;
		}

		/* See comment in function rx_tchf_fn() */
		switch (chan_state->amr_last_dtx) {
		case AHS_SID_FIRST_P1:
		case AHS_SID_FIRST_P2:
		case AHS_SID_UPDATE:
		case AHS_SID_UPDATE_CN:
			lchan_set_marker(true, lchan);
			lchan->rtp_tx_marker = false;
			break;
		}

		if (rc)
			trx_loop_amr_input(l1t,
				trx_chan_desc[chan].chan_nr | bi->tn, chan_state,
				n_errors, n_bits_total);

		/* only good speech frames get rtp header */
		if (rc != GSM_MACBLOCK_LEN && rc >= 4) {
			if (chan_state->amr_last_dtx == AMR_OTHER) {
				ft = chan_state->codec[chan_state->ul_cmr];
			} else {
				/* SID frames will always get Frame Type Index 8 (AMR_SID) */
				ft = AMR_SID;
			}
			rc = osmo_amr_rtp_enc(tch_data,
				chan_state->codec[chan_state->ul_cmr],
			        ft, AMR_GOOD);
		}

		break;
	default:
		LOGL1S(DL1P, LOGL_ERROR, l1t, bi->tn, chan, bi->fn,
			"TCH mode %u invalid, please fix!\n",
			tch_mode);
		return -EINVAL;
	}
	memcpy(*bursts_p, *bursts_p + 232, 232);
	memcpy(*bursts_p + 232, *bursts_p + 464, 232);
	ber10k = compute_ber10k(n_bits_total, n_errors);


	/* Check if the frame is bad */
	if (rc < 0) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received bad data (%u/%u)\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period);
		bfi_flag = true;
	} else if (rc < 4) {
		LOGL1S(DL1P, LOGL_NOTICE, l1t, bi->tn, chan, bi->fn,
			"Received bad data (%u/%u) with invalid codec mode %d\n",
			bi->fn % l1ts->mf_period, l1ts->mf_period, rc);
		bfi_flag = true;
	}

	if (rc != GSM_MACBLOCK_LEN && lchan->ecu_state)
		osmo_ecu_frame_in(lchan->ecu_state, bfi_flag, tch_data, rc);

	if (bfi_flag)
		goto bfi;

	/* FACCH */
	if (rc == GSM_MACBLOCK_LEN) {
		chan_state->ul_ongoing_facch = 1;
		uint16_t ber10k = compute_ber10k(n_bits_total, n_errors);
		if (lchan->nr == 0)
			fn_begin = gsm0502_fn_remap(bi->fn, FN_REMAP_FACCH_H0);
		else
			fn_begin = gsm0502_fn_remap(bi->fn, FN_REMAP_FACCH_H1);
		_sched_compose_ph_data_ind(l1t, bi->tn, fn_begin, chan,
			tch_data + amr, GSM_MACBLOCK_LEN,
			/* FIXME: AVG both RSSI and ToA */
			bi->rssi, bi->toa256,
			0 /* FIXME: AVG C/I */,
			ber10k, PRES_INFO_UNKNOWN);
bfi:
		/* FIXME: a FACCH/H frame replaces two speech frames,
		 * so we actually need to send two bad frame indications! */
		if (rsl_cmode == RSL_CMOD_SPD_SPEECH) {
			/* indicate bad frame */
			if (lchan->tch.dtx.ul_sid) {
				/* DTXu: pause in progress. Push empty payload to upper layers */
				rc = 0;
				goto compose_l1sap;
			}

			/* If there is an ECU active on this channel, use its output */
			if (lchan->ecu_state) {
				rc = osmo_ecu_frame_out(lchan->ecu_state, tch_data);
				if (rc >= 0) /* Otherwise we send a BFI */
					goto compose_l1sap;
			}

			switch (tch_mode) {
			case GSM48_CMODE_SPEECH_V1: /* HR */
				tch_data[0] = 0x70; /* F = 0, FT = 111 */
				memset(tch_data + 1, 0, 14);
				rc = 15;
				break;
			case GSM48_CMODE_SPEECH_AMR: /* AMR */
				rc = osmo_amr_rtp_enc(tch_data,
					chan_state->codec[chan_state->dl_cmr],
					chan_state->codec[chan_state->dl_ft],
					AMR_BAD);
				if (rc < 2) {
					LOGL1S(DL1P, LOGL_ERROR, l1t, bi->tn, chan, bi->fn,
					       "Failed to encode AMR_BAD frame (rc=%d), "
					       "not sending BFI\n", rc);
					return -EINVAL;
				}
				memset(tch_data + 2, 0, rc - 2);
				break;
			default:
				LOGL1S(DL1P, LOGL_ERROR, l1t, bi->tn, chan, bi->fn,
					"TCH mode %u invalid, please fix!\n", tch_mode);
				return -EINVAL;
			}
		}
	}

	if (rsl_cmode != RSL_CMOD_SPD_SPEECH)
		return 0;

compose_l1sap:
	/* TCH or BFI */
	/* Note on FN 19 or 20: If we received the last burst of a frame,
	 * it actually starts at FN 8 or 9. A burst starting there, overlaps
	 * with the slot 12, so an extra FN must be subtracted to get correct
	 * start of frame.
	 */
	if (lchan->nr == 0)
		fn_begin = gsm0502_fn_remap(bi->fn, FN_REMAP_TCH_H0);
	else
		fn_begin = gsm0502_fn_remap(bi->fn, FN_REMAP_TCH_H1);
	return _sched_compose_tch_ind(l1t, bi->tn, fn_begin, chan,
				      tch_data, rc, bi->toa256, ber10k, bi->rssi, is_sub);
}

/* schedule all frames of all TRX for given FN */
static int trx_sched_fn(struct gsm_bts *bts, uint32_t fn)
{
	struct gsm_bts_trx *trx;
	uint8_t tn;
	const ubit_t *bits;
	uint8_t gain;
	uint16_t nbits = 0;

	/* send time indication */
	l1if_mph_time_ind(bts, fn);

	/* process every TRX */
	llist_for_each_entry(trx, &bts->trx_list, list) {
		struct phy_instance *pinst = trx_phy_instance(trx);
		struct phy_link *plink = pinst->phy_link;
		struct trx_l1h *l1h = pinst->u.osmotrx.hdl;
		struct l1sched_trx *l1t = &l1h->l1s;

		/* advance frame number, so the transceiver has more
		 * time until it must be transmitted. */
		fn = (fn + plink->u.osmotrx.clock_advance) % GSM_HYPERFRAME;

		/* we don't schedule, if power is off */
		if (!trx_if_powered(l1h))
			continue;

		/* process every TS of TRX */
		for (tn = 0; tn < ARRAY_SIZE(l1t->ts); tn++) {
			/* ready-to-send */
			_sched_rts(l1t, tn,
				(fn + plink->u.osmotrx.rts_advance) % GSM_HYPERFRAME);
			/* get burst for FN */
			bits = _sched_dl_burst(l1t, tn, fn, &nbits);
			if (!bits) {
				/* if no bits, send no burst */
				continue;
			} else
				gain = 0;
			if (nbits)
				trx_if_send_burst(l1h, tn, fn, gain, bits, nbits);
		}
	}

	return 0;
}

/*! duration of a GSM frame in nano-seconds. (120ms/26) */
#define FRAME_DURATION_nS	4615384
/*! duration of a GSM frame in micro-seconds (120s/26) */
#define FRAME_DURATION_uS	(FRAME_DURATION_nS/1000)
/*! maximum number of 'missed' frame periods we can tolerate of OS doesn't schedule us*/
#define MAX_FN_SKEW		50
/*! maximum number of frame periods we can tolerate without TRX Clock Indication*/
#define TRX_LOSS_FRAMES		400

/*! compute the number of micro-seconds difference elapsed between \a last and \a now */
static inline int64_t compute_elapsed_us(const struct timespec *last, const struct timespec *now)
{
	struct timespec elapsed;

	timespecsub(now, last, &elapsed);
	return (int64_t)(elapsed.tv_sec * 1000000) + (elapsed.tv_nsec / 1000);
}

/*! compute the number of frame number intervals elapsed between \a last and \a now */
static inline int compute_elapsed_fn(const uint32_t last, const uint32_t now)
{
	int elapsed_fn = (now + GSM_HYPERFRAME - last) % GSM_HYPERFRAME;
	if (elapsed_fn >= 135774)
		elapsed_fn -= GSM_HYPERFRAME;
	return elapsed_fn;
}

/*! normalise given 'struct timespec', i.e. carry nanoseconds into seconds */
static inline void normalize_timespec(struct timespec *ts)
{
	ts->tv_sec += ts->tv_nsec / 1000000000;
	ts->tv_nsec = ts->tv_nsec % 1000000000;
}

/*! Increment a GSM frame number modulo GSM_HYPERFRAME */
#define INCREMENT_FN(fn)	(fn) = (((fn) + 1) % GSM_HYPERFRAME)

extern int quit;

/*! this is the timerfd-callback firing for every FN to be processed */
static int trx_fn_timer_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct gsm_bts *bts = ofd->data;
	struct bts_trx_priv *bts_trx = (struct bts_trx_priv *)bts->model_priv;
	struct osmo_trx_clock_state *tcs = &bts_trx->clk_s;
	struct timespec tv_now;
	uint64_t expire_count;
	int64_t elapsed_us, error_us;
	int rc, i;

	if (!(what & BSC_FD_READ))
		return 0;

	/* read from timerfd: number of expirations of periodic timer */
	rc = read(ofd->fd, (void *) &expire_count, sizeof(expire_count));
	if (rc < 0 && errno == EAGAIN)
		return 0;
	OSMO_ASSERT(rc == sizeof(expire_count));

	if (expire_count > 1) {
		LOGP(DL1C, LOGL_NOTICE, "FN timer expire_count=%"PRIu64": We missed %"PRIu64" timers\n",
			expire_count, expire_count-1);
	}

	/* check if transceiver is still alive */
	if (tcs->fn_without_clock_ind++ == TRX_LOSS_FRAMES) {
		LOGP(DL1C, LOGL_NOTICE, "No more clock from transceiver\n");
		goto no_clock;
	}

	/* compute actual elapsed time and resulting OS scheduling error */
	clock_gettime(CLOCK_MONOTONIC, &tv_now);
	elapsed_us = compute_elapsed_us(&tcs->last_fn_timer.tv, &tv_now);
	error_us = elapsed_us - FRAME_DURATION_uS;
#ifdef DEBUG_CLOCK
	printf("%s(): %09ld, elapsed_us=%05" PRId64 ", error_us=%-d: fn=%d\n", __func__,
		tv_now.tv_nsec, elapsed_us, error_us, tcs->last_fn_timer.fn+1);
#endif
	tcs->last_fn_timer.tv = tv_now;

	/* if someone played with clock, or if the process stalled */
	if (elapsed_us > FRAME_DURATION_uS * MAX_FN_SKEW || elapsed_us < 0) {
		LOGP(DL1C, LOGL_ERROR, "PC clock skew: elapsed_us=%" PRId64 ", error_us=%" PRId64 "\n",
			elapsed_us, error_us);
		goto no_clock;
	}

	/* call trx_sched_fn() for all expired FN */
	for (i = 0; i < expire_count; i++) {
		INCREMENT_FN(tcs->last_fn_timer.fn);
		trx_sched_fn(bts, tcs->last_fn_timer.fn);
	}

	return 0;

no_clock:
	osmo_timerfd_disable(&tcs->fn_timer_ofd);
	bts_shutdown(bts, "No clock from osmo-trx");
	return -1;
}

/*! \brief This is the cb of the initial timer set upon start. On timeout, it
 *  means it wasn't replaced and hence no CLOCK IND was received. */
static int trx_start_noclockind_to_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct gsm_bts *bts = ofd->data;
	struct bts_trx_priv *bts_trx = (struct bts_trx_priv *)bts->model_priv;
	struct osmo_trx_clock_state *tcs = &bts_trx->clk_s;

	osmo_fd_close(&tcs->fn_timer_ofd); /* Avoid being called again */
	bts_shutdown(bts, "No clock since TRX was started");
	return -1;
}

/*! \brief PHY informs us clock indications should start to be received */
int trx_sched_clock_started(struct gsm_bts *bts)
{
	struct bts_trx_priv *bts_trx = (struct bts_trx_priv *)bts->model_priv;
	struct osmo_trx_clock_state *tcs = &bts_trx->clk_s;
	const struct timespec it_val = {3, 0};
	const struct timespec it_intval = {0, 0};

	LOGP(DL1C, LOGL_NOTICE, "GSM clock started, waiting for clock indications\n");
	osmo_fd_close(&tcs->fn_timer_ofd);
	memset(tcs, 0, sizeof(*tcs));
	tcs->fn_timer_ofd.fd = -1;
	/* Set up timeout to shutdown BTS if no clock ind is received in a few
	 * seconds. Upon clock ind receival, fn_timer_ofd will be reused and
	 * timeout won't trigger.
	 */
	osmo_timerfd_setup(&tcs->fn_timer_ofd, trx_start_noclockind_to_cb, bts);
	osmo_timerfd_schedule(&tcs->fn_timer_ofd, &it_val, &it_intval);
	return 0;
}

/*! \brief PHY informs us no more clock indications should be received anymore */
int trx_sched_clock_stopped(struct gsm_bts *bts)
{
	struct bts_trx_priv *bts_trx = (struct bts_trx_priv *)bts->model_priv;
	struct osmo_trx_clock_state *tcs = &bts_trx->clk_s;

	LOGP(DL1C, LOGL_NOTICE, "GSM clock stopped\n");
	osmo_fd_close(&tcs->fn_timer_ofd);

	return 0;
}

/*! reset clock with current fn and schedule it. Called when trx becomes
 *  available or when max clock skew is reached */
static int trx_setup_clock(struct gsm_bts *bts, struct osmo_trx_clock_state *tcs,
	struct timespec *tv_now, const struct timespec *interval, uint32_t fn)
{
	tcs->last_fn_timer.fn = fn;
	/* call trx cheduler function for new 'last' FN */
	trx_sched_fn(bts, tcs->last_fn_timer.fn);

	/* schedule first FN clock timer */
	osmo_timerfd_setup(&tcs->fn_timer_ofd, trx_fn_timer_cb, bts);
	osmo_timerfd_schedule(&tcs->fn_timer_ofd, NULL, interval);

	tcs->last_fn_timer.tv = *tv_now;
	tcs->last_clk_ind.tv = *tv_now;
	tcs->last_clk_ind.fn = fn;

	return 0;
}

/*! called every time we receive a clock indication from TRX */
int trx_sched_clock(struct gsm_bts *bts, uint32_t fn)
{
	struct bts_trx_priv *bts_trx = (struct bts_trx_priv *)bts->model_priv;
	struct osmo_trx_clock_state *tcs = &bts_trx->clk_s;
	struct timespec tv_now;
	int elapsed_fn;
	int64_t elapsed_us, elapsed_us_since_clk, elapsed_fn_since_clk, error_us_since_clk;
	unsigned int fn_caught_up = 0;
	const struct timespec interval = { .tv_sec = 0, .tv_nsec = FRAME_DURATION_nS };

	if (quit)
		return 0;

	/* reset lost counter */
	tcs->fn_without_clock_ind = 0;

	clock_gettime(CLOCK_MONOTONIC, &tv_now);

	/* calculate elapsed time +fn since last timer */
	elapsed_us = compute_elapsed_us(&tcs->last_fn_timer.tv, &tv_now);
	elapsed_fn = compute_elapsed_fn(tcs->last_fn_timer.fn, fn);
#ifdef DEBUG_CLOCK
	printf("%s(): LAST_TIMER %9ld, elapsed_us=%7d, elapsed_fn=%+3d\n", __func__,
		tv_now.tv_nsec, elapsed_us, elapsed_fn);
#endif
	/* negative elapsed_fn values mean that we've already processed
	 * more FN based on the local interval timer than what the TRX
	 * now reports in the clock indication.   Positive elapsed_fn
	 * values mean we still have a backlog to process */

	/* calculate elapsed time +fn since last clk ind */
	elapsed_us_since_clk = compute_elapsed_us(&tcs->last_clk_ind.tv, &tv_now);
	elapsed_fn_since_clk = compute_elapsed_fn(tcs->last_clk_ind.fn, fn);
	/* error (delta) between local clock since last CLK and CLK based on FN clock at TRX */
	error_us_since_clk = elapsed_us_since_clk - (FRAME_DURATION_uS * elapsed_fn_since_clk);
	LOGP(DL1C, LOGL_INFO, "TRX Clock Ind: elapsed_us=%7"PRId64", "
		"elapsed_fn=%3"PRId64", error_us=%+5"PRId64"\n",
		elapsed_us_since_clk, elapsed_fn_since_clk, error_us_since_clk);

	/* TODO: put this computed error_us_since_clk into some filter
	 * function and use that to adjust our regular timer interval to
	 * compensate for clock drift between the PC clock and the
	 * TRX/SDR clock */

	tcs->last_clk_ind.tv = tv_now;
	tcs->last_clk_ind.fn = fn;

	/* check for max clock skew */
	if (elapsed_fn > MAX_FN_SKEW || elapsed_fn < -MAX_FN_SKEW) {
		LOGP(DL1C, LOGL_NOTICE, "GSM clock skew: old fn=%u, "
			"new fn=%u\n", tcs->last_fn_timer.fn, fn);
		return trx_setup_clock(bts, tcs, &tv_now, &interval, fn);
	}

	LOGP(DL1C, LOGL_INFO, "GSM clock jitter: %" PRId64 "us (elapsed_fn=%d)\n",
		elapsed_fn * FRAME_DURATION_uS - elapsed_us, elapsed_fn);

	/* too many frames have been processed already */
	if (elapsed_fn < 0) {
		struct timespec first = interval;
		/* set clock to the time or last FN should have been
		 * transmitted. */
		first.tv_nsec += (0 - elapsed_fn) * FRAME_DURATION_nS;
		normalize_timespec(&first);
		LOGP(DL1C, LOGL_NOTICE, "We were %d FN faster than TRX, compensating\n", -elapsed_fn);
		/* set time to the time our next FN has to be transmitted */
		osmo_timerfd_schedule(&tcs->fn_timer_ofd, &first, &interval);
		return 0;
	}

	/* transmit what we still need to transmit */
	while (fn != tcs->last_fn_timer.fn) {
		INCREMENT_FN(tcs->last_fn_timer.fn);
		trx_sched_fn(bts, tcs->last_fn_timer.fn);
		fn_caught_up++;
	}

	if (fn_caught_up) {
		LOGP(DL1C, LOGL_NOTICE, "We were %d FN slower than TRX, compensated\n", elapsed_fn);
		tcs->last_fn_timer.tv = tv_now;
	}

	return 0;
}

void _sched_act_rach_det(struct l1sched_trx *l1t, uint8_t tn, uint8_t ss, int activate)
{
	struct phy_instance *pinst = trx_phy_instance(l1t->trx);
	struct trx_l1h *l1h = pinst->u.osmotrx.hdl;

	if (activate)
		trx_if_cmd_handover(l1h, tn, ss);
	else
		trx_if_cmd_nohandover(l1h, tn, ss);
}
