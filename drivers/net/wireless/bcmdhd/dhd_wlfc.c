/*
 * DHD PROP_TXSTATUS Module.
 *
 * Copyright (C) 1999-2016, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 * $Id: dhd_wlfc.c 587005 2015-09-17 11:26:26Z $
 *
 */

#include <typedefs.h>
#include <osl.h>

#include <bcmutils.h>
#include <bcmendian.h>

#include <dngl_stats.h>
#include <dhd.h>

#include <dhd_bus.h>
#include <dhd_dbg.h>

#ifdef PROP_TXSTATUS
#include <wlfc_proto.h>
#include <dhd_wlfc.h>
#endif
#ifdef DHDTCPACK_SUPPRESS
#include <dhd_ip.h>
#endif /* DHDTCPACK_SUPPRESS */


/*
 * wlfc naming and lock rules:
 *
 * 1. Private functions name like _dhd_wlfc_XXX, declared as static and avoid wlfc lock operation.
 * 2. Public functions name like dhd_wlfc_XXX, use wlfc lock if needed.
 * 3. Non-Proptxstatus module call public functions only and avoid wlfc lock operation.
 *
 */


#ifdef PROP_TXSTATUS

#define DHD_WLFC_QMON_COMPLETE(entry)

#define LIMIT_BORROW


static uint16
_dhd_wlfc_adjusted_seq(void* p, uint8 current_seq)
{
	uint16 seq;

	if (!p) {
		return 0xffff;
	}

	seq = WL_TXSTATUS_GET_FREERUNCTR(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
	if (seq < current_seq) {
		/* wrap around */
		seq += 256;
	}

	return seq;
}

static void
_dhd_wlfc_prec_enque(struct pktq *pq, int prec, void* p, bool qHead,
	uint8 current_seq, bool reOrder)
{
	struct pktq_prec *q;
	uint16 seq, seq2;
	void *p2, *p2_prev;

	if (!p)
		return;


	ASSERT(prec >= 0 && prec < pq->num_prec);
	ASSERT(PKTLINK(p) == NULL);         /* queueing chains not allowed */

	ASSERT(!pktq_full(pq));
	ASSERT(!pktq_pfull(pq, prec));

	q = &pq->q[prec];

	PKTSETLINK(p, NULL);
	if (q->head == NULL) {
		/* empty queue */
		q->head = p;
		q->tail = p;
	} else {
		if (reOrder && (prec & 1)) {
			seq = _dhd_wlfc_adjusted_seq(p, current_seq);
			p2 = qHead ? q->head : q->tail;
			seq2 = _dhd_wlfc_adjusted_seq(p2, current_seq);

			if ((qHead &&((seq+1) > seq2)) || (!qHead && ((seq2+1) > seq))) {
				/* need reorder */
				p2 = q->head;
				p2_prev = NULL;
				seq2 = _dhd_wlfc_adjusted_seq(p2, current_seq);

				while (seq > seq2) {
					p2_prev = p2;
					p2 = PKTLINK(p2);
					if (!p2) {
						break;
					}
					seq2 = _dhd_wlfc_adjusted_seq(p2, current_seq);
				}

				if (p2_prev == NULL) {
					/* insert head */
					PKTSETLINK(p, q->head);
					q->head = p;
				} else if (p2 == NULL) {
					/* insert tail */
					PKTSETLINK(p2_prev, p);
					q->tail = p;
				} else {
					/* insert after p2_prev */
					PKTSETLINK(p, PKTLINK(p2_prev));
					PKTSETLINK(p2_prev, p);
				}
				goto exit;
			}
		}

		if (qHead) {
			PKTSETLINK(p, q->head);
			q->head = p;
		} else {
			PKTSETLINK(q->tail, p);
			q->tail = p;
		}
	}

exit:

	q->len++;
	pq->len++;

	if (pq->hi_prec < prec)
		pq->hi_prec = (uint8)prec;
}

/* Create a place to store all packet pointers submitted to the firmware until
	a status comes back, suppress or otherwise.

	hang-er: noun, a contrivance on which things are hung, as a hook.
*/
static void*
_dhd_wlfc_hanger_create(dhd_pub_t *dhd, int max_items)
{
	int i;
	wlfc_hanger_t* hanger;

	/* allow only up to a specific size for now */
	ASSERT(max_items == WLFC_HANGER_MAXITEMS);

	if ((hanger = (wlfc_hanger_t*)DHD_OS_PREALLOC(dhd, DHD_PREALLOC_DHD_WLFC_HANGER,
			WLFC_HANGER_SIZE(max_items))) == NULL)
		return NULL;

	memset(hanger, 0, WLFC_HANGER_SIZE(max_items));
	hanger->max_items = max_items;

	for (i = 0; i < hanger->max_items; i++) {
		hanger->items[i].state = WLFC_HANGER_ITEM_STATE_FREE;
	}
	return hanger;
}

static int
_dhd_wlfc_hanger_delete(dhd_pub_t *dhd, void* hanger)
{
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if (h) {
		DHD_OS_PREFREE(dhd, DHD_PREALLOC_DHD_WLFC_HANGER,
				h, WLFC_HANGER_SIZE(h->max_items));
		return BCME_OK;
	}
	return BCME_BADARG;
}

static uint16
_dhd_wlfc_hanger_get_free_slot(void* hanger)
{
	uint32 i;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if (h) {
		i = h->slot_pos + 1;
		if (i == h->max_items) {
			i = 0;
		}
		while (i != h->slot_pos) {
			if (h->items[i].state == WLFC_HANGER_ITEM_STATE_FREE) {
				h->slot_pos = i;
				return (uint16)i;
			}
			i++;
			if (i == h->max_items)
				i = 0;
		}
		h->failed_slotfind++;
	}
	return WLFC_HANGER_MAXITEMS;
}

static int
_dhd_wlfc_hanger_get_genbit(void* hanger, void* pkt, uint32 slot_id, int* gen)
{
	int rc = BCME_OK;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	*gen = 0xff;

	/* this packet was not pushed at the time it went to the firmware */
	if (slot_id == WLFC_HANGER_MAXITEMS)
		return BCME_NOTFOUND;

	if (h) {
		if ((h->items[slot_id].state == WLFC_HANGER_ITEM_STATE_INUSE) ||
			(h->items[slot_id].state == WLFC_HANGER_ITEM_STATE_INUSE_SUPPRESSED)) {
			*gen = h->items[slot_id].gen;
		}
		else {
			rc = BCME_NOTFOUND;
		}
	}
	else
		rc = BCME_BADARG;
	return rc;
}

static int
_dhd_wlfc_hanger_pushpkt(void* hanger, void* pkt, uint32 slot_id)
{
	int rc = BCME_OK;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if (h && (slot_id < WLFC_HANGER_MAXITEMS)) {
		if (h->items[slot_id].state == WLFC_HANGER_ITEM_STATE_FREE) {
			h->items[slot_id].state = WLFC_HANGER_ITEM_STATE_INUSE;
			h->items[slot_id].pkt = pkt;
			h->pushed++;
		}
		else {
			h->failed_to_push++;
			rc = BCME_NOTFOUND;
		}
	}
	else
		rc = BCME_BADARG;
	return rc;
}

static int
_dhd_wlfc_hanger_poppkt(void* hanger, uint32 slot_id, void** pktout, int remove_from_hanger)
{
	int rc = BCME_OK;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	/* this packet was not pushed at the time it went to the firmware */
	if (slot_id == WLFC_HANGER_MAXITEMS)
		return BCME_NOTFOUND;

	if (h) {
		if (h->items[slot_id].state != WLFC_HANGER_ITEM_STATE_FREE) {
			*pktout = h->items[slot_id].pkt;
			if (remove_from_hanger) {
				h->items[slot_id].state =
					WLFC_HANGER_ITEM_STATE_FREE;
				h->items[slot_id].pkt = NULL;
				h->items[slot_id].gen = 0xff;
				h->items[slot_id].identifier = 0;
				h->popped++;
			}
		}
		else {
			h->failed_to_pop++;
			rc = BCME_NOTFOUND;
		}
	}
	else
		rc = BCME_BADARG;
	return rc;
}

static int
_dhd_wlfc_hanger_mark_suppressed(void* hanger, uint32 slot_id, uint8 gen)
{
	int rc = BCME_OK;
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	/* this packet was not pushed at the time it went to the firmware */
	if (slot_id == WLFC_HANGER_MAXITEMS)
		return BCME_NOTFOUND;
	if (h) {
		h->items[slot_id].gen = gen;
		if (h->items[slot_id].state == WLFC_HANGER_ITEM_STATE_INUSE) {
			h->items[slot_id].state = WLFC_HANGER_ITEM_STATE_INUSE_SUPPRESSED;
		}
		else
			rc = BCME_BADARG;
	}
	else
		rc = BCME_BADARG;

	return rc;
}

static void
_dhd_wlfc_hanger_waitevent_set(void* hanger, uint32 hslot, uint8 waitevent)
{
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;
	ASSERT(h && (hslot < (uint32) h->max_items));

	h->items[hslot].waitevent = waitevent;
}

static uint8
_dhd_wlfc_hanger_waitevent_decreturn(void* hanger, uint32 hslot)
{
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;
	ASSERT(h && (hslot < (uint32) h->max_items));

	h->items[hslot].waitevent--;
	return h->items[hslot].waitevent;
}

/* return true if the slot is only waiting for clean */
static bool
_dhd_wlfc_hanger_wait_clean(void* hanger, uint32 hslot)
{
	wlfc_hanger_t* h = (wlfc_hanger_t*)hanger;

	if ((hslot < (uint32) h->max_items) &&
		(h->items[hslot].state == WLFC_HANGER_ITEM_STATE_WAIT_CLEAN)) {
		/* the packet should be already freed by _dhd_wlfc_cleanup */
		h->items[hslot].state = WLFC_HANGER_ITEM_STATE_FREE;
		h->items[hslot].pkt = NULL;
		h->items[hslot].gen = 0xff;
		h->items[hslot].identifier = 0;
		return TRUE;
	}

	return FALSE;
}

/* remove reference of specific packet in hanger */
static bool
_dhd_wlfc_hanger_remove_reference(wlfc_hanger_t* h, void* pkt)
{
	int i;

	if (!h || !pkt) {
		return FALSE;
	}

	for (i = 0; i < h->max_items; i++) {
		if (pkt == h->items[i].pkt) {
			if ((h->items[i].state == WLFC_HANGER_ITEM_STATE_INUSE) ||
				(h->items[i].state == WLFC_HANGER_ITEM_STATE_INUSE_SUPPRESSED)) {
				h->items[i].state = WLFC_HANGER_ITEM_STATE_FREE;
				h->items[i].pkt = NULL;
				h->items[i].gen = 0xff;
				h->items[i].identifier = 0;
			}
			return TRUE;
		}
	}

	return FALSE;
}


static int
_dhd_wlfc_enque_afq(athost_wl_status_info_t* ctx, void *p)
{
	wlfc_mac_descriptor_t* entry;
	uint16 entry_idx = WL_TXSTATUS_GET_HSLOT(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
	uint8 prec = DHD_PKTTAG_FIFO(PKTTAG(p));

	if (entry_idx < WLFC_MAC_DESC_TABLE_SIZE)
		entry  = &ctx->destination_entries.nodes[entry_idx];
	else if (entry_idx < (WLFC_MAC_DESC_TABLE_SIZE + WLFC_MAX_IFNUM))
		entry = &ctx->destination_entries.interfaces[entry_idx - WLFC_MAC_DESC_TABLE_SIZE];
	else
		entry = &ctx->destination_entries.other;

	pktq_penq(&entry->afq, prec, p);

	return BCME_OK;
}

static int
_dhd_wlfc_deque_afq(athost_wl_status_info_t* ctx, uint16 hslot, uint8 hcnt, uint8 prec,
	void **pktout)
{
	wlfc_mac_descriptor_t *entry;
	struct pktq *pq;
	struct pktq_prec *q;
	void *p, *b;

	if (!ctx) {
		DHD_ERROR(("%s: ctx(%p), pktout(%p)\n", __FUNCTION__, ctx, pktout));
		return BCME_BADARG;
	}

	if (pktout) {
		*pktout = NULL;
	}

	ASSERT(hslot < (WLFC_MAC_DESC_TABLE_SIZE + WLFC_MAX_IFNUM + 1));

	if (hslot < WLFC_MAC_DESC_TABLE_SIZE)
		entry  = &ctx->destination_entries.nodes[hslot];
	else if (hslot < (WLFC_MAC_DESC_TABLE_SIZE + WLFC_MAX_IFNUM))
		entry = &ctx->destination_entries.interfaces[hslot - WLFC_MAC_DESC_TABLE_SIZE];
	else
		entry = &ctx->destination_entries.other;

	pq = &entry->afq;

	ASSERT(prec < pq->num_prec);

	q = &pq->q[prec];

	b = NULL;
	p = q->head;

	while (p && (hcnt != WL_TXSTATUS_GET_FREERUNCTR(DHD_PKTTAG_H2DTAG(PKTTAG(p)))))
	{
		b = p;
		p = PKTLINK(p);
	}

	if (p == NULL) {
		/* none is matched */
		if (b) {
			DHD_ERROR(("%s: can't find matching seq(%d)\n", __FUNCTION__, hcnt));
		} else {
			DHD_ERROR(("%s: queue is empty\n", __FUNCTION__));
		}

		return BCME_ERROR;
	}

	if (!b) {
		/* head packet is matched */
		if ((q->head = PKTLINK(p)) == NULL) {
			q->tail = NULL;
		}
	} else {
		/* middle packet is matched */
		DHD_INFO(("%s: out of order, seq(%d), head_seq(%d)\n", __FUNCTION__, hcnt,
			WL_TXSTATUS_GET_FREERUNCTR(DHD_PKTTAG_H2DTAG(PKTTAG(q->head)))));
		ctx->stats.ooo_pkts[prec]++;
		PKTSETLINK(b, PKTLINK(p));
		if (PKTLINK(p) == NULL) {
			q->tail = b;
		}
	}

	q->len--;
	pq->len--;

	PKTSETLINK(p, NULL);

	if (pktout) {
		*pktout = p;
	}

	return BCME_OK;
}

static int
_dhd_wlfc_pushheader(athost_wl_status_info_t* ctx, void* p, bool tim_signal,
	uint8 tim_bmp, uint8 mac_handle, uint32 htodtag, uint16 htodseq, bool skip_wlfc_hdr)
{
	uint32 wl_pktinfo = 0;
	uint8* wlh;
	uint8 dataOffset = 0;
	uint8 fillers;
	uint8 tim_signal_len = 0;
	dhd_pub_t *dhdp = (dhd_pub_t *)ctx->dhdp;

	struct bdc_header *h;

	if (skip_wlfc_hdr)
		goto push_bdc_hdr;

	if (tim_signal) {
		tim_signal_len = TLV_HDR_LEN + WLFC_CTL_VALUE_LEN_PENDING_TRAFFIC_BMP;
	}

	/* +2 is for Type[1] and Len[1] in TLV, plus TIM signal */
	dataOffset = WLFC_CTL_VALUE_LEN_PKTTAG + TLV_HDR_LEN + tim_signal_len;
	if (WLFC_GET_REUSESEQ(dhdp->wlfc_mode)) {
		dataOffset += WLFC_CTL_VALUE_LEN_SEQ;
	}

	fillers = ROUNDUP(dataOffset, 4) - dataOffset;
	dataOffset += fillers;

	PKTPUSH(ctx->osh, p, dataOffset);
	wlh = (uint8*) PKTDATA(ctx->osh, p);

	wl_pktinfo = htol32(htodtag);

	wlh[TLV_TAG_OFF] = WLFC_CTL_TYPE_PKTTAG;
	wlh[TLV_LEN_OFF] = WLFC_CTL_VALUE_LEN_PKTTAG;
	memcpy(&wlh[TLV_HDR_LEN], &wl_pktinfo, sizeof(uint32));

	if (WLFC_GET_REUSESEQ(dhdp->wlfc_mode)) {
		uint16 wl_seqinfo = htol16(htodseq);
		wlh[TLV_LEN_OFF] += WLFC_CTL_VALUE_LEN_SEQ;
		memcpy(&wlh[TLV_HDR_LEN + WLFC_CTL_VALUE_LEN_PKTTAG], &wl_seqinfo,
			WLFC_CTL_VALUE_LEN_SEQ);
	}

	if (tim_signal_len) {
		wlh[dataOffset - fillers - tim_signal_len ] =
			WLFC_CTL_TYPE_PENDING_TRAFFIC_BMP;
		wlh[dataOffset - fillers - tim_signal_len + 1] =
			WLFC_CTL_VALUE_LEN_PENDING_TRAFFIC_BMP;
		wlh[dataOffset - fillers - tim_signal_len + 2] = mac_handle;
		wlh[dataOffset - fillers - tim_signal_len + 3] = tim_bmp;
	}
	if (fillers)
		memset(&wlh[dataOffset - fillers], WLFC_CTL_TYPE_FILLER, fillers);

push_bdc_hdr:

	PKTPUSH(ctx->osh, p, BDC_HEADER_LEN);
	h = (struct bdc_header *)PKTDATA(ctx->osh, p);
	h->flags = (BDC_PROTO_VER << BDC_FLAG_VER_SHIFT);
	if (PKTSUMNEEDED(p))
		h->flags |= BDC_FLAG_SUM_NEEDED;


	h->priority = (PKTPRIO(p) & BDC_PRIORITY_MASK);
	h->flags2 = 0;
	h->dataOffset = dataOffset >> 2;
	BDC_SET_IF_IDX(h, DHD_PKTTAG_IF(PKTTAG(p)));
	return BCME_OK;
}

static int
_dhd_wlfc_pullheader(athost_wl_status_info_t* ctx, void* pktbuf)
{
	struct bdc_header *h;

	if (PKTLEN(ctx->osh, pktbuf) < BDC_HEADER_LEN) {
		DHD_ERROR(("%s: rx data too short (%d < %d)\n", __FUNCTION__,
		           PKTLEN(ctx->osh, pktbuf), BDC_HEADER_LEN));
		return BCME_ERROR;
	}
	h = (struct bdc_header *)PKTDATA(ctx->osh, pktbuf);

	/* pull BDC header */
	PKTPULL(ctx->osh, pktbuf, BDC_HEADER_LEN);

	if (PKTLEN(ctx->osh, pktbuf) < (uint)(h->dataOffset << 2)) {
		DHD_ERROR(("%s: rx data too short (%d < %d)\n", __FUNCTION__,
		           PKTLEN(ctx->osh, pktbuf), (h->dataOffset << 2)));
		return BCME_ERROR;
	}

	/* pull wl-header */
	PKTPULL(ctx->osh, pktbuf, (h->dataOffset << 2));
	return BCME_OK;
}

static wlfc_mac_descriptor_t*
_dhd_wlfc_find_table_entry(athost_wl_status_info_t* ctx, void* p)
{
	int i;
	wlfc_mac_descriptor_t* table = ctx->destination_entries.nodes;
	uint8 ifid = DHD_PKTTAG_IF(PKTTAG(p));
	uint8* dstn = DHD_PKTTAG_DSTN(PKTTAG(p));
	wlfc_mac_descriptor_t* entry = DHD_PKTTAG_ENTRY(PKTTAG(p));
	int iftype = ctx->destination_entries.interfaces[ifid].iftype;

	/* saved one exists, return it */
	if (entry)
		return entry;

	/* Multicast destination, STA and P2P clients get the interface entry.
	 * STA/GC gets the Mac Entry for TDLS destinations, TDLS destinations
	 * have their own entry.
	 */
	if ((iftype == WLC_E_IF_ROLE_STA || ETHER_ISMULTI(dstn) ||
		iftype == WLC_E_IF_ROLE_P2P_CLIENT) &&
		(ctx->destination_entries.interfaces[ifid].occupied)) {
			entry = &ctx->destination_entries.interfaces[ifid];
	}

	if (entry && ETHER_ISMULTI(dstn)) {
		DHD_PKTTAG_SET_ENTRY(PKTTAG(p), entry);
		return entry;
	}

	for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
		if (table[i].occupied) {
			if (table[i].interface_id == ifid) {
				if (!memcmp(table[i].ea, dstn, ETHER_ADDR_LEN)) {
					entry = &table[i];
					break;
				}
			}
		}
	}

	if (entry == NULL)
		entry = &ctx->destination_entries.other;

	DHD_PKTTAG_SET_ENTRY(PKTTAG(p), entry);

	return entry;
}

static int
_dhd_wlfc_prec_drop(dhd_pub_t *dhdp, int prec, void* p, bool bPktInQ)
{
	athost_wl_status_info_t* ctx;
	void *pout = NULL;

	ASSERT(dhdp && p);
	ASSERT(prec >= 0 && prec <= WLFC_PSQ_PREC_COUNT);

	ctx = (athost_wl_status_info_t*)dhdp->wlfc_state;

	if (!WLFC_GET_AFQ(dhdp->wlfc_mode) && (prec & 1)) {
		/* suppressed queue, need pop from hanger */
		_dhd_wlfc_hanger_poppkt(ctx->hanger, WL_TXSTATUS_GET_HSLOT(DHD_PKTTAG_H2DTAG
					(PKTTAG(p))), &pout, 1);
		ASSERT(p == pout);
	}

	if (!(prec & 1)) {
#ifdef DHDTCPACK_SUPPRESS
		/* pkt in delayed q, so fake push BDC header for
		 * dhd_tcpack_check_xmit() and dhd_txcomplete().
		 */
		_dhd_wlfc_pushheader(ctx, p, FALSE, 0, 0, 0, 0, TRUE);

		/* This packet is about to be freed, so remove it from tcp_ack_info_tbl
		 * This must be one of...
		 * 1. A pkt already in delayQ is evicted by another pkt with higher precedence
		 * in _dhd_wlfc_prec_enq_with_drop()
		 * 2. A pkt could not be enqueued to delayQ because it is full,
		 * in _dhd_wlfc_enque_delayq().
		 * 3. A pkt could not be enqueued to delayQ because it is full,
		 * in _dhd_wlfc_rollback_packet_toq().
		 */
		if (dhd_tcpack_check_xmit(dhdp, p) == BCME_ERROR) {
			DHD_ERROR(("%s %d: tcpack_suppress ERROR!!!"
				" Stop using it\n",
				__FUNCTION__, __LINE__));
			dhd_tcpack_suppress_set(dhdp, TCPACK_SUP_OFF);
		}
#endif /* DHDTCPACK_SUPPRESS */
	}

	if (bPktInQ) {
		ctx->pkt_cnt_in_q[DHD_PKTTAG_IF(PKTTAG(p))][prec>>1]--;
		ctx->pkt_cnt_per_ac[prec>>1]--;
	}

	dhd_txcomplete(dhdp, p, FALSE);
	PKTFREE(ctx->osh, p, TRUE);
	ctx->stats.pktout++;
	ctx->stats.drop_pkts[prec]++;

	return 0;
}

static bool
_dhd_wlfc_prec_enq_with_drop(dhd_pub_t *dhdp, struct pktq *pq, void *pkt, int prec, bool qHead,
	uint8 current_seq)
{
	void *p = NULL;
	int eprec = -1;		/* precedence to evict from */
	athost_wl_status_info_t* ctx;

	ASSERT(dhdp && pq && pkt);
	ASSERT(prec >= 0 && prec < pq->num_prec);

	ctx = (athost_wl_status_info_t*)dhdp->wlfc_state;

	/* Fast case, precedence queue is not full and we are also not
	 * exceeding total queue length
	 */
	if (!pktq_pfull(pq, prec) && !pktq_full(pq)) {
		goto exit;
	}

	/* Determine precedence from which to evict packet, if any */
	if (pktq_pfull(pq, prec))
		eprec = prec;
	else if (pktq_full(pq)) {
		p = pktq_peek_tail(pq, &eprec);
		if (!p) {
			DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
			return FALSE;
		}
		if ((eprec > prec) || (eprec < 0)) {
			if (!pktq_pempty(pq, prec)) {
				eprec = prec;
			} else {
				return FALSE;
			}
		}
	}

	/* Evict if needed */
	if (eprec >= 0) {
		/* Detect queueing to unconfigured precedence */
		ASSERT(!pktq_pempty(pq, eprec));
		/* Evict all fragmented frames */
		dhd_prec_drop_pkts(dhdp, pq, eprec, _dhd_wlfc_prec_drop);
	}

exit:
	/* Enqueue */
	_dhd_wlfc_prec_enque(pq, prec, pkt, qHead, current_seq,
		WLFC_GET_REORDERSUPP(dhdp->wlfc_mode));
	ctx->pkt_cnt_in_q[DHD_PKTTAG_IF(PKTTAG(pkt))][prec>>1]++;
	ctx->pkt_cnt_per_ac[prec>>1]++;

	return TRUE;
}


static int
_dhd_wlfc_rollback_packet_toq(athost_wl_status_info_t* ctx,
	void* p, ewlfc_packet_state_t pkt_type, uint32 hslot)
{
	/*
	put the packet back to the head of queue

	- suppressed packet goes back to suppress sub-queue
	- pull out the header, if new or delayed packet

	Note: hslot is used only when header removal is done.
	*/
	wlfc_mac_descriptor_t* entry;
	int rc = BCME_OK;
	int prec, fifo_id;

	entry = _dhd_wlfc_find_table_entry(ctx, p);
	prec = DHD_PKTTAG_FIFO(PKTTAG(p));
	fifo_id = prec << 1;
	if (pkt_type == eWLFC_PKTTYPE_SUPPRESSED)
		fifo_id += 1;
	if (entry != NULL) {
		/*
		if this packet did not count against FIFO credit, it must have
		taken a requested_credit from the firmware (for pspoll etc.)
		*/
		if ((prec != AC_COUNT) && !DHD_PKTTAG_CREDITCHECK(PKTTAG(p)))
			entry->requested_credit++;

		if (pkt_type == eWLFC_PKTTYPE_DELAYED) {
			/* decrement sequence count */
			WLFC_DECR_SEQCOUNT(entry, prec);
			/* remove header first */
			rc = _dhd_wlfc_pullheader(ctx, p);
			if (rc != BCME_OK) {
				DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
				goto exit;
			}
		}

		if (_dhd_wlfc_prec_enq_with_drop(ctx->dhdp, &entry->psq, p, fifo_id, TRUE,
			WLFC_SEQCOUNT(entry, fifo_id>>1))
			== FALSE) {
			/* enque failed */
			DHD_ERROR(("Error: %s():%d, fifo_id(%d)\n",
				__FUNCTION__, __LINE__, fifo_id));
			rc = BCME_ERROR;
		}
	} else {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		rc = BCME_ERROR;
	}
exit:
	if (rc != BCME_OK) {
		ctx->stats.rollback_failed++;
		_dhd_wlfc_prec_drop(ctx->dhdp, fifo_id, p, FALSE);
	}
	else
		ctx->stats.rollback++;

	return rc;
}

static bool
_dhd_wlfc_allow_fc(athost_wl_status_info_t* ctx, uint8 ifid)
{
	int prec, ac_traffic = WLFC_NO_TRAFFIC;

	for (prec = 0; prec < AC_COUNT; prec++) {
		if (ctx->pkt_cnt_in_q[ifid][prec] > 0) {
			if (ac_traffic == WLFC_NO_TRAFFIC)
				ac_traffic = prec + 1;
			else if (ac_traffic != (prec + 1))
				ac_traffic = WLFC_MULTI_TRAFFIC;
		}
	}

	if (ac_traffic >= 1 && ac_traffic <= AC_COUNT) {
		/* single AC (BE/BK/VI/VO) in queue */
		if (ctx->allow_fc) {
			return TRUE;
		} else {
			uint32 delta;
			uint32 curr_t = OSL_SYSUPTIME();

			if (ctx->fc_defer_timestamp == 0) {
				/* first signle ac scenario */
				ctx->fc_defer_timestamp = curr_t;
				return FALSE;
			}

			/* single AC duration, this handles wrap around, e.g. 1 - ~0 = 2. */
			delta = curr_t - ctx->fc_defer_timestamp;
			if (delta >= WLFC_FC_DEFER_PERIOD_MS) {
				ctx->allow_fc = TRUE;
			}
		}
	} else {
		/* multiple ACs or BCMC in queue */
		ctx->allow_fc = FALSE;
		ctx->fc_defer_timestamp = 0;
	}

	return ctx->allow_fc;
}

static void
_dhd_wlfc_flow_control_check(athost_wl_status_info_t* ctx, struct pktq* pq, uint8 if_id)
{
	dhd_pub_t *dhdp;

	ASSERT(ctx);

	dhdp = (dhd_pub_t *)ctx->dhdp;
	ASSERT(dhdp);

	if (dhdp->skip_fc && dhdp->skip_fc())
		return;

	if ((ctx->hostif_flow_state[if_id] == OFF) && !_dhd_wlfc_allow_fc(ctx, if_id))
		return;

	if ((pq->len <= WLFC_FLOWCONTROL_LOWATER) && (ctx->hostif_flow_state[if_id] == ON)) {
		/* start traffic */
		ctx->hostif_flow_state[if_id] = OFF;
		/*
		WLFC_DBGMESG(("qlen:%02d, if:%02d, ->OFF, start traffic %s()\n",
		pq->len, if_id, __FUNCTION__));
		*/
		WLFC_DBGMESG(("F"));

		dhd_txflowcontrol(dhdp, if_id, OFF);

		ctx->toggle_host_if = 0;
	}

	if ((pq->len >= WLFC_FLOWCONTROL_HIWATER) && (ctx->hostif_flow_state[if_id] == OFF)) {
		/* stop traffic */
		ctx->hostif_flow_state[if_id] = ON;
		/*
		WLFC_DBGMESG(("qlen:%02d, if:%02d, ->ON, stop traffic   %s()\n",
		pq->len, if_id, __FUNCTION__));
		*/
		WLFC_DBGMESG(("N"));

		dhd_txflowcontrol(dhdp, if_id, ON);

		ctx->host_ifidx = if_id;
		ctx->toggle_host_if = 1;
	}

	return;
}

static int
_dhd_wlfc_send_signalonly_packet(athost_wl_status_info_t* ctx, wlfc_mac_descriptor_t* entry,
	uint8 ta_bmp)
{
	int rc = BCME_OK;
	void* p = NULL;
	int dummylen = ((dhd_pub_t *)ctx->dhdp)->hdrlen+ 16;
	dhd_pub_t *dhdp = (dhd_pub_t *)ctx->dhdp;

	if (dhdp->proptxstatus_txoff) {
		rc = BCME_NORESOURCE;
		return rc;
	}

	/* allocate a dummy packet */
	p = PKTGET(ctx->osh, dummylen, TRUE);
	if (p) {
		PKTPULL(ctx->osh, p, dummylen);
		DHD_PKTTAG_SET_H2DTAG(PKTTAG(p), 0);
		_dhd_wlfc_pushheader(ctx, p, TRUE, ta_bmp, entry->mac_handle, 0, 0, FALSE);
		DHD_PKTTAG_SETSIGNALONLY(PKTTAG(p), 1);
		DHD_PKTTAG_WLFCPKT_SET(PKTTAG(p), 1);
#ifdef PROP_TXSTATUS_DEBUG
		ctx->stats.signal_only_pkts_sent++;
#endif

#if defined(BCMPCIE)
		rc = dhd_bus_txdata(dhdp->bus, p, ctx->host_ifidx);
#else
		rc = dhd_bus_txdata(dhdp->bus, p);
#endif
		if (rc != BCME_OK) {
			_dhd_wlfc_pullheader(ctx, p);
			PKTFREE(ctx->osh, p, TRUE);
		}
	}
	else {
		DHD_ERROR(("%s: couldn't allocate new %d-byte packet\n",
		           __FUNCTION__, dummylen));
		rc = BCME_NOMEM;
	}
	return rc;
}

/* Return TRUE if traffic availability changed */
static bool
_dhd_wlfc_traffic_pending_check(athost_wl_status_info_t* ctx, wlfc_mac_descriptor_t* entry,
	int prec)
{
	bool rc = FALSE;

	if (entry->state == WLFC_STATE_CLOSE) {
		if ((pktq_plen(&entry->psq, (prec << 1)) == 0) &&
			(pktq_plen(&entry->psq, ((prec << 1) + 1)) == 0)) {

			if (entry->traffic_pending_bmp & NBITVAL(prec)) {
				rc = TRUE;
				entry->traffic_pending_bmp =
					entry->traffic_pending_bmp & ~ NBITVAL(prec);
			}
		}
		else {
			if (!(entry->traffic_pending_bmp & NBITVAL(prec))) {
				rc = TRUE;
				entry->traffic_pending_bmp =
					entry->traffic_pending_bmp | NBITVAL(prec);
			}
		}
	}
	if (rc) {
		/* request a TIM update to firmware at the next piggyback opportunity */
		if (entry->traffic_lastreported_bmp != entry->traffic_pending_bmp) {
			entry->send_tim_signal = 1;
			_dhd_wlfc_send_signalonly_packet(ctx, entry, entry->traffic_pending_bmp);
			entry->traffic_lastreported_bmp = entry->traffic_pending_bmp;
			entry->send_tim_signal = 0;
		}
		else {
			rc = FALSE;
		}
	}
	return rc;
}

static int
_dhd_wlfc_enque_suppressed(athost_wl_status_info_t* ctx, int prec, void* p)
{
	wlfc_mac_descriptor_t* entry;

	entry = _dhd_wlfc_find_table_entry(ctx, p);
	if (entry == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_NOTFOUND;
	}
	/*
	- suppressed packets go to sub_queue[2*prec + 1] AND
	- delayed packets go to sub_queue[2*prec + 0] to ensure
	order of delivery.
	*/
	if (_dhd_wlfc_prec_enq_with_drop(ctx->dhdp, &entry->psq, p, ((prec << 1) + 1), FALSE,
		WLFC_SEQCOUNT(entry, prec))
		== FALSE) {
		ctx->stats.delayq_full_error++;
		/* WLFC_DBGMESG(("Error: %s():%d\n", __FUNCTION__, __LINE__)); */
		WLFC_DBGMESG(("s"));
		return BCME_ERROR;
	}

	/* A packet has been pushed, update traffic availability bitmap, if applicable */
	_dhd_wlfc_traffic_pending_check(ctx, entry, prec);
	_dhd_wlfc_flow_control_check(ctx, &entry->psq, DHD_PKTTAG_IF(PKTTAG(p)));
	return BCME_OK;
}

static int
_dhd_wlfc_pretx_pktprocess(athost_wl_status_info_t* ctx,
	wlfc_mac_descriptor_t* entry, void* p, int header_needed, uint32* slot)
{
	int rc = BCME_OK;
	int hslot = WLFC_HANGER_MAXITEMS;
	bool send_tim_update = FALSE;
	uint32 htod = 0;
	uint16 htodseq = 0;
	uint8 free_ctr;
	int gen = 0xff;
	dhd_pub_t *dhdp = (dhd_pub_t *)ctx->dhdp;

	*slot = hslot;

	if (entry == NULL) {
		entry = _dhd_wlfc_find_table_entry(ctx, p);
	}

	if (entry == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_ERROR;
	}

	if (entry->send_tim_signal) {
		send_tim_update = TRUE;
		entry->send_tim_signal = 0;
		entry->traffic_lastreported_bmp = entry->traffic_pending_bmp;
	}

	if (header_needed) {
		if (WLFC_GET_AFQ(dhdp->wlfc_mode)) {
			hslot = (uint)(entry - &ctx->destination_entries.nodes[0]);
		} else {
			hslot = _dhd_wlfc_hanger_get_free_slot(ctx->hanger);
		}
		gen = entry->generation;
		free_ctr = WLFC_SEQCOUNT(entry, DHD_PKTTAG_FIFO(PKTTAG(p)));
	} else {
		if (WLFC_GET_REUSESEQ(dhdp->wlfc_mode)) {
			htodseq = DHD_PKTTAG_H2DSEQ(PKTTAG(p));
		}

		hslot = WL_TXSTATUS_GET_HSLOT(DHD_PKTTAG_H2DTAG(PKTTAG(p)));

		if (WLFC_GET_REORDERSUPP(dhdp->wlfc_mode)) {
			gen = entry->generation;
		} else if (WLFC_GET_AFQ(dhdp->wlfc_mode)) {
			gen = WL_TXSTATUS_GET_GENERATION(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
		} else {
			_dhd_wlfc_hanger_get_genbit(ctx->hanger, p, hslot, &gen);
		}

		free_ctr = WL_TXSTATUS_GET_FREERUNCTR(DHD_PKTTAG_H2DTAG(PKTTAG(p)));
		/* remove old header */
		_dhd_wlfc_pullheader(ctx, p);
	}

	if (hslot >= WLFC_HANGER_MAXITEMS) {
		DHD_ERROR(("Error: %s():no hanger slot available\n", __FUNCTION__));
		return BCME_ERROR;
	}

	WL_TXSTATUS_SET_FREERUNCTR(htod, free_ctr);
	WL_TXSTATUS_SET_HSLOT(htod, hslot);
	WL_TXSTATUS_SET_FIFO(htod, DHD_PKTTAG_FIFO(PKTTAG(p)));
	WL_TXSTATUS_SET_FLAGS(htod, WLFC_PKTFLAG_PKTFROMHOST);
	WL_TXSTATUS_SET_GENERATION(htod, gen);
	DHD_PKTTAG_SETPKTDIR(PKTTAG(p), 1);

	if (!DHD_PKTTAG_CREDITCHECK(PKTTAG(p))) {
		/*
		Indicate that this packet is being sent in response to an
		explicit request from the firmware side.
		*/
		WLFC_PKTFLAG_SET_PKTREQUESTED(htod);
	} else {
		WLFC_PKTFLAG_CLR_PKTREQUESTED(htod);
	}

	rc = _dhd_wlfc_pushheader(ctx, p, send_tim_update,
		entry->traffic_lastreported_bmp, entry->mac_handle, htod, htodseq, FALSE);
	if (rc == BCME_OK) {
		DHD_PKTTAG_SET_H2DTAG(PKTTAG(p), htod);

		if (!WLFC_GET_AFQ(dhdp->wlfc_mode) && header_needed) {
			/*
			a new header was created for this packet.
			push to hanger slot and scrub q. Since bus
			send succeeded, increment seq number as well.
			*/
			rc = _dhd_wlfc_hanger_pushpkt(ctx->hanger, p, hslot);
			if (rc == BCME_OK) {
#ifdef PROP_TXSTATUS_DEBUG
				((wlfc_hanger_t*)(ctx->hanger))->items[hslot].push_time =
					OSL_SYSUPTIME();
#endif
			} else {
				DHD_ERROR(("%s() hanger_pushpkt() failed, rc: %d\n",
					__FUNCTION__, rc));
			}
		}

		if ((rc == BCME_OK) && header_needed) {
			/* increment free running sequence count */
			WLFC_INCR_SEQCOUNT(entry, DHD_PKTTAG_FIFO(PKTTAG(p)));
		}
	}
	*slot = hslot;
	return rc;
}

static int
_dhd_wlfc_is_destination_open(athost_wl_status_info_t* ctx,
	wlfc_mac_descriptor_t* entry, int prec)
{
	if (entry->interface_id >= WLFC_MAX_IFNUM) {
		ASSERT(&ctx->destination_entries.other == entry);
		return 1;
	}
	if (ctx->destination_entries.interfaces[entry->interface_id].iftype ==
		WLC_E_IF_ROLE_P2P_GO) {
		/* - destination interface is of type p2p GO.
		For a p2pGO interface, if the destination is OPEN but the interface is
		CLOSEd, do not send traffic. But if the dstn is CLOSEd while there is
		destination-specific-credit left send packets. This is because the
		firmware storing the destination-specific-requested packet in queue.
		*/
		if ((entry->state == WLFC_STATE_CLOSE) && (entry->requested_credit == 0) &&
			(entry->requested_packet == 0)) {
			return 0;
		}
	}
	/* AP, p2p_go -> unicast desc entry, STA/p2p_cl -> interface desc. entry */
	if (((entry->state == WLFC_STATE_CLOSE) && (entry->requested_credit == 0) &&
		(entry->requested_packet == 0)) ||
		(!(entry->ac_bitmap & (1 << prec)))) {
		return 0;
	}

	return 1;
}

static void*
_dhd_wlfc_deque_delayedq(athost_wl_status_info_t* ctx, int prec,
	uint8* ac_credit_spent, uint8* needs_hdr, wlfc_mac_descriptor_t** entry_out,
	bool only_no_credit)
{
	dhd_pub_t *dhdp = (dhd_pub_t *)ctx->dhdp;
	wlfc_mac_descriptor_t* entry;
	int total_entries;
	void* p = NULL;
	int i;

	*entry_out = NULL;
	/* most cases a packet will count against FIFO credit */
	*ac_credit_spent = ((prec == AC_COUNT) && !ctx->bcmc_credit_supported) ? 0 : 1;

	/* search all entries, include nodes as well as interfaces */
	if (only_no_credit) {
		total_entries = ctx->requested_entry_count;
	} else {
		total_entries = ctx->active_entry_count;
	}

	for (i = 0; i < total_entries; i++) {
		if (only_no_credit) {
			entry = ctx->requested_entry[i];
		} else {
			entry = ctx->active_entry_head;
			/* move head to ensure fair round-robin */
			ctx->active_entry_head = ctx->active_entry_head->next;
		}
		ASSERT(entry);

		if (entry->occupied && _dhd_wlfc_is_destination_open(ctx, entry, prec) &&
			(entry->transit_count < WL_TXSTATUS_FREERUNCTR_MASK) &&
			!(WLFC_GET_REORDERSUPP(dhdp->wlfc_mode) && entry->suppressed)) {
			if (entry->state == WLFC_STATE_CLOSE) {
				*ac_credit_spent = 0;
			}

			/* higher precedence will be picked up first,
			 * i.e. suppressed packets before delayed ones
			 */
			p = pktq_pdeq(&entry->psq, PSQ_SUP_IDX(prec));
			*needs_hdr = 0;
			if (p == NULL) {
				if (entry->suppressed == TRUE) {
					/* skip this entry */
					continue;
				}
				/* De-Q from delay Q */
				p = pktq_pdeq(&entry->psq, PSQ_DLY_IDX(prec));
				*needs_hdr = 1;
			}

			if (p != NULL) {
				/* did the packet come from suppress sub-queue? */
				if (entry->requested_credit > 0) {
					entry->requested_credit--;
#ifdef PROP_TXSTATUS_DEBUG
					entry->dstncredit_sent_packets++;
#endif
				} else if (entry->requested_packet > 0) {
					entry->requested_packet--;
					DHD_PKTTAG_SETONETIMEPKTRQST(PKTTAG(p));
				}

				*entry_out = entry;
				ctx->pkt_cnt_in_q[DHD_PKTTAG_IF(PKTTAG(p))][prec]--;
				ctx->pkt_cnt_per_ac[prec]--;
				_dhd_wlfc_flow_control_check(ctx, &entry->psq,
					DHD_PKTTAG_IF(PKTTAG(p)));
				/*
				A packet has been picked up, update traffic
				availability bitmap, if applicable
				*/
				_dhd_wlfc_traffic_pending_check(ctx, entry, prec);
				return p;
			}
		}
	}
	return NULL;
}

static int
_dhd_wlfc_enque_delayq(athost_wl_status_info_t* ctx, void* pktbuf, int prec)
{
	wlfc_mac_descriptor_t* entry;

	if (pktbuf != NULL) {
		entry = _dhd_wlfc_find_table_entry(ctx, pktbuf);
		if (entry == NULL) {
			DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
			return BCME_ERROR;
		}

		/*
		- suppressed packets go to sub_queue[2*prec + 1] AND
		- delayed packets go to sub_queue[2*prec + 0] to ensure
		order of delivery.
		*/
		if (_dhd_wlfc_prec_enq_with_drop(ctx->dhdp, &entry->psq, pktbuf, (prec << 1),
			FALSE, WLFC_SEQCOUNT(entry, prec))
			== FALSE) {
			WLFC_DBGMESG(("D"));
			ctx->stats.delayq_full_error++;
			return BCME_ERROR;
		}


		/*
		A packet has been pushed, update traffic availability bitmap,
		if applicable
		*/
		_dhd_wlfc_traffic_pending_check(ctx, entry, prec);
	}

	return BCME_OK;
}

static bool _dhd_wlfc_ifpkt_fn(void* p, void *p_ifid)
{
	if (!p || !p_ifid)
		return FALSE;

	return (DHD_PKTTAG_WLFCPKT(PKTTAG(p))&& (*((uint8 *)p_ifid) == DHD_PKTTAG_IF(PKTTAG(p))));
}

static bool _dhd_wlfc_entrypkt_fn(void* p, void *entry)
{
	if (!p || !entry)
		return FALSE;

	return (DHD_PKTTAG_WLFCPKT(PKTTAG(p))&& (entry == DHD_PKTTAG_ENTRY(PKTTAG(p))));
}

static void
_dhd_wlfc_return_implied_credit(athost_wl_status_info_t* wlfc, void* pkt)
{
	dhd_pub_t *dhdp;

	if (!wlfc || !pkt) {
		return;
	}

	dhdp = (dhd_pub_t *)(wlfc->dhdp);
	if (dhdp && (dhdp->proptxstatus_mode == WLFC_FCMODE_IMPLIED_CREDIT) &&
		DHD_PKTTAG_CREDITCHECK(PKTTAG(pkt))) {
		int lender, credit_returned = 0;
		uint8 fifo_id = DHD_PKTTAG_FIFO(PKTTAG(pkt));

		/* Note that borrower is fifo_id */
		/* Return credits to highest priority lender first */
		for (lender = AC_COUNT; lender >= 0; lender--) {
			if (wlfc->credits_borrowed[fifo_id][lender] > 0) {
				wlfc->FIFO_credit[lender]++;
				wlfc->credits_borrowed[fifo_id][lender]--;
				credit_returned = 1;
				break;
			}
		}

		if (!credit_returned) {
			wlfc->FIFO_credit[fifo_id]++;
		}
	}
}

static void
_dhd_wlfc_pktq_flush(athost_wl_status_info_t* ctx, struct pktq *pq,
	bool dir, f_processpkt_t fn, void *arg, q_type_t q_type)
{
	int prec;
	dhd_pub_t *dhdp = (dhd_pub_t *)ctx->dhdp;

	ASSERT(dhdp);

	/* Optimize flush, if pktq len = 0, just return.
	 * pktq len of 0 means pktq's prec q's are all empty.
	 */
	if (pq->len == 0) {
		return;
	}


	for (prec = 0; prec < pq->num_prec; prec++) {
		struct pktq_prec *q;
		void *p, *prev = NULL;

		q = &pq->q[prec];
		p = q->head;
		while (p) {
			if (fn == NULL || (*fn)(p, arg)) {
				bool head = (p == q->head);
				if (head)
					q->head = PKTLINK(p);
				else
					PKTSETLINK(prev, PKTLINK(p));
				if (q_type == Q_TYPE_PSQ) {
					if (!WLFC_GET_AFQ(dhdp->wlfc_mode) && (prec & 1)) {
						_dhd_wlfc_hanger_remove_reference(ctx->hanger, p);
					}
					ctx->pkt_cnt_in_q[DHD_PKTTAG_IF(PKTTAG(p))][prec>>1]--;
					ctx->pkt_cnt_per_ac[prec>>1]--;
					ctx->stats.cleanup_psq_cnt++;
					if (!(prec & 1)) {
						/* pkt in delayed q, so fake push BDC header for
						 * dhd_tcpack_check_xmit() and dhd_txcomplete().
						 */
						_dhd_wlfc_pushheader(ctx, p, FALSE, 0, 0,
							0, 0, TRUE);
#ifdef DHDTCPACK_SUPPRESS
						if (dhd_tcpack_check_xmit(dhdp, p) == BCME_ERROR) {
							DHD_ERROR(("%s %d: tcpack_suppress ERROR!!!"
								" Stop using it\n",
								__FUNCTION__, __LINE__));
							dhd_tcpack_suppress_set(dhdp,
								TCPACK_SUP_OFF);
						}
#endif /* DHDTCPACK_SUPPRESS */
					}
				} else if (q_type == Q_TYPE_AFQ) {
					wlfc_mac_descriptor_t* entry =
						_dhd_wlfc_find_table_entry(ctx, p);
					entry->transit_count--;
					if (entry->suppressed &&
						(--entry->suppr_transit_count == 0)) {
						entry->suppressed = FALSE;
					}
					_dhd_wlfc_return_implied_credit(ctx, p);
					ctx->stats.cleanup_fw_cnt++;
				}
				PKTSETLINK(p, NULL);
				dhd_txcomplete(dhdp, p, FALSE);
				PKTFREE(ctx->osh, p, dir);
				if (dir) {
					ctx->stats.pktout++;
				}
				q->len--;
				pq->len--;
				p = (head ? q->head : PKTLINK(prev));
			} else {
				prev = p;
				p = PKTLINK(p);
			}
		}

		if (q->head == NULL) {
			ASSERT(q->len == 0);
			q->tail = NULL;
		}

	}

	if (fn == NULL)
		ASSERT(pq->len == 0);
}

static void*
_dhd_wlfc_pktq_pdeq_with_fn(struct pktq *pq, int prec, f_processpkt_t fn, void *arg)
{
	struct pktq_prec *q;
	void *p, *prev = NULL;

	ASSERT(prec >= 0 && prec < pq->num_prec);

	q = &pq->q[prec];
	p = q->head;

	while (p) {
		if (fn == NULL || (*fn)(p, arg)) {
			break;
		} else {
			prev = p;
			p = PKTLINK(p);
		}
	}
	if (p == NULL)
		return NULL;

	if (prev == NULL) {
		if ((q->head = PKTLINK(p)) == NULL) {
			q->tail = NULL;
		}
	} else {
		PKTSETLINK(prev, PKTLINK(p));
		if (q->tail == p) {
			q->tail = prev;
		}
	}

	q->len--;

	pq->len--;

	PKTSETLINK(p, NULL);

	return p;
}

static void
_dhd_wlfc_cleanup_txq(dhd_pub_t *dhd, f_processpkt_t fn, void *arg)
{
	int prec;
	void *pkt = NULL, *head = NULL, *tail = NULL;
	struct pktq *txq = (struct pktq *)dhd_bus_txq(dhd->bus);
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_hanger_t* h = (wlfc_hanger_t*)wlfc->hanger;
	wlfc_mac_descriptor_t* entry;

	dhd_os_sdlock_txq(dhd);
	for (prec = 0; prec < txq->num_prec; prec++) {
		while ((pkt = _dhd_wlfc_pktq_pdeq_with_fn(txq, prec, fn, arg))) {
#ifdef DHDTCPACK_SUPPRESS
			if (dhd_tcpack_check_xmit(dhd, pkt) == BCME_ERROR) {
				DHD_ERROR(("%s %d: tcpack_suppress ERROR!!! Stop using it\n",
					__FUNCTION__, __LINE__));
				dhd_tcpack_suppress_set(dhd, TCPACK_SUP_OFF);
			}
#endif /* DHDTCPACK_SUPPRESS */
			if (!head) {
				head = pkt;
			}
			if (tail) {
				PKTSETLINK(tail, pkt);
			}
			tail = pkt;
		}
	}
	dhd_os_sdunlock_txq(dhd);


	while ((pkt = head)) {
		head = PKTLINK(pkt);
		PKTSETLINK(pkt, NULL);
		entry = _dhd_wlfc_find_table_entry(wlfc, pkt);

		if (!WLFC_GET_AFQ(dhd->wlfc_mode) &&
			!_dhd_wlfc_hanger_remove_reference(h, pkt)) {
			DHD_ERROR(("%s: can't find pkt(%p) in hanger, free it anyway\n",
				__FUNCTION__, pkt));
		}
		entry->transit_count--;
		if (entry->suppressed &&
			(--entry->suppr_transit_count == 0)) {
			entry->suppressed = FALSE;
		}
		_dhd_wlfc_return_implied_credit(wlfc, pkt);
		dhd_txcomplete(dhd, pkt, FALSE);
		PKTFREE(wlfc->osh, pkt, TRUE);
		wlfc->stats.pktout++;
		wlfc->stats.cleanup_txq_cnt++;

	}
}

void
_dhd_wlfc_cleanup(dhd_pub_t *dhd, f_processpkt_t fn, void *arg)
{
	int i;
	int total_entries;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_hanger_t* h = (wlfc_hanger_t*)wlfc->hanger;

	wlfc->stats.cleanup_txq_cnt = 0;
	wlfc->stats.cleanup_psq_cnt = 0;
	wlfc->stats.cleanup_fw_cnt = 0;
	/*
	*  flush sequence shoulde be txq -> psq -> hanger/afq, hanger has to be last one
	*/
	/* flush bus->txq */
	_dhd_wlfc_cleanup_txq(dhd, fn, arg);


	/* flush psq, search all entries, include nodes as well as interfaces */
	total_entries = sizeof(wlfc->destination_entries)/sizeof(wlfc_mac_descriptor_t);
	table = (wlfc_mac_descriptor_t*)&wlfc->destination_entries;

	for (i = 0; i < total_entries; i++) {
		if (table[i].occupied) {
			/* release packets held in PSQ (both delayed and suppressed) */
			if (table[i].psq.len) {
				WLFC_DBGMESG(("%s(): PSQ[%d].len = %d\n",
					__FUNCTION__, i, table[i].psq.len));
				_dhd_wlfc_pktq_flush(wlfc, &table[i].psq, TRUE,
					fn, arg, Q_TYPE_PSQ);
			}

			/* free packets held in AFQ */
			if (WLFC_GET_AFQ(dhd->wlfc_mode) && (table[i].afq.len)) {
				_dhd_wlfc_pktq_flush(wlfc, &table[i].afq, TRUE,
					fn, arg, Q_TYPE_AFQ);
			}

			if ((fn == NULL) && (&table[i] != &wlfc->destination_entries.other)) {
				table[i].occupied = 0;
				if (table[i].transit_count || table[i].suppr_transit_count) {
					DHD_ERROR(("%s: table[%d] transit(%d), suppr_tansit(%d)\n",
						__FUNCTION__, i,
						table[i].transit_count,
						table[i].suppr_transit_count));
				}
			}
		}
	}

	/*
		. flush remained pkt in hanger queue, not in bus->txq nor psq.
		. the remained pkt was successfully downloaded to dongle already.
		. hanger slot state cannot be set to free until receive txstatus update.
	*/
	if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
		for (i = 0; i < h->max_items; i++) {
			if ((h->items[i].state == WLFC_HANGER_ITEM_STATE_INUSE) ||
				(h->items[i].state == WLFC_HANGER_ITEM_STATE_INUSE_SUPPRESSED)) {
				if (fn == NULL || (*fn)(h->items[i].pkt, arg)) {
					table = _dhd_wlfc_find_table_entry(wlfc, h->items[i].pkt);
					table->transit_count--;
					if (table->suppressed &&
						(--table->suppr_transit_count == 0)) {
						table->suppressed = FALSE;
					}
					_dhd_wlfc_return_implied_credit(wlfc, h->items[i].pkt);
					dhd_txcomplete(dhd, h->items[i].pkt, FALSE);
					PKTFREE(wlfc->osh, h->items[i].pkt, TRUE);
					wlfc->stats.pktout++;
					wlfc->stats.cleanup_fw_cnt++;
					h->items[i].state = WLFC_HANGER_ITEM_STATE_WAIT_CLEAN;
				}
			}
		}
	}

	return;
}

static int
_dhd_wlfc_mac_entry_update(athost_wl_status_info_t* ctx, wlfc_mac_descriptor_t* entry,
	uint8 action, uint8 ifid, uint8 iftype, uint8* ea,
	f_processpkt_t fn, void *arg)
{
	int rc = BCME_OK;


	if ((action == eWLFC_MAC_ENTRY_ACTION_ADD) || (action == eWLFC_MAC_ENTRY_ACTION_UPDATE)) {
		entry->occupied = 1;
		entry->state = WLFC_STATE_OPEN;
		entry->requested_credit = 0;
		entry->interface_id = ifid;
		entry->iftype = iftype;
		entry->ac_bitmap = 0xff; /* update this when handling APSD */
		/* for an interface entry we may not care about the MAC address */
		if (ea != NULL)
			memcpy(&entry->ea[0], ea, ETHER_ADDR_LEN);

		if (action == eWLFC_MAC_ENTRY_ACTION_ADD) {
			dhd_pub_t *dhdp = (dhd_pub_t *)(ctx->dhdp);
			pktq_init(&entry->psq, WLFC_PSQ_PREC_COUNT, WLFC_PSQ_LEN);
			if (WLFC_GET_AFQ(dhdp->wlfc_mode)) {
				pktq_init(&entry->afq, WLFC_AFQ_PREC_COUNT, WLFC_PSQ_LEN);
			}

			if (entry->next == NULL) {
				/* not linked to anywhere, add to tail */
				if (ctx->active_entry_head) {
					entry->prev = ctx->active_entry_head->prev;
					ctx->active_entry_head->prev->next = entry;
					ctx->active_entry_head->prev = entry;
					entry->next = ctx->active_entry_head;

				} else {
					ASSERT(ctx->active_entry_count == 0);
					entry->prev = entry->next = entry;
					ctx->active_entry_head = entry;
				}
				ctx->active_entry_count++;
			} else {
				DHD_ERROR(("%s():%d, entry(%d)\n", __FUNCTION__, __LINE__,
					(int)(entry - &ctx->destination_entries.nodes[0])));
			}
		}
	} else if (action == eWLFC_MAC_ENTRY_ACTION_DEL) {
		/* When the entry is deleted, the packets that are queued in the entry must be
		   cleanup. The cleanup action should be before the occupied is set as 0.
		*/
		_dhd_wlfc_cleanup(ctx->dhdp, fn, arg);
		_dhd_wlfc_flow_control_check(ctx, &entry->psq, ifid);

		entry->occupied = 0;
		entry->suppressed = 0;
		entry->state = WLFC_STATE_CLOSE;
		entry->requested_credit = 0;
		entry->transit_count = 0;
		entry->suppr_transit_count = 0;
		memset(&entry->ea[0], 0, ETHER_ADDR_LEN);

		if (entry->next) {
			/* not floating, remove from Q */
			if (ctx->active_entry_count <= 1) {
				/* last item */
				ctx->active_entry_head = NULL;
				ctx->active_entry_count = 0;
			} else {
				entry->prev->next = entry->next;
				entry->next->prev = entry->prev;
				if (entry == ctx->active_entry_head) {
					ctx->active_entry_head = entry->next;
				}
				ctx->active_entry_count--;
			}
			entry->next = entry->prev = NULL;
		} else {
			DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		}
	}
	return rc;
}

#ifdef LIMIT_BORROW
static int
_dhd_wlfc_borrow_credit(athost_wl_status_info_t* ctx, int highest_lender_ac, int borrower_ac)
{
	int lender_ac;
	int rc = -1;

	if (ctx == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return -1;
	}

	/* Borrow from lowest priority available AC (including BC/MC credits) */
	for (lender_ac = 0; lender_ac <= highest_lender_ac; lender_ac++) {
		if (ctx->FIFO_credit[lender_ac] > 0) {
			ctx->credits_borrowed[borrower_ac][lender_ac]++;
			ctx->FIFO_credit[lender_ac]--;
			rc = lender_ac;
			break;
		}
	}

	return rc;
}

static int _dhd_wlfc_return_credit(athost_wl_status_info_t* ctx, int lender_ac, int borrower_ac)
{
	if ((ctx == NULL) || (lender_ac < 0) || (lender_ac > AC_COUNT) ||
		(borrower_ac < 0) || (borrower_ac > AC_COUNT)) {
		DHD_ERROR(("Error: %s():%d, ctx(%p), lender_ac(%d), borrower_ac(%d)\n",
			__FUNCTION__, __LINE__, ctx, lender_ac, borrower_ac));

		return BCME_BADARG;
	}

	ctx->credits_borrowed[borrower_ac][lender_ac]--;
	ctx->FIFO_credit[lender_ac]++;

	return BCME_OK;
}
#endif /* LIMIT_BORROW */

static int
_dhd_wlfc_interface_entry_update(void* state,
	uint8 action, uint8 ifid, uint8 iftype, uint8* ea)
{
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;
	wlfc_mac_descriptor_t* entry;

	if (ifid >= WLFC_MAX_IFNUM)
		return BCME_BADARG;

	entry = &ctx->destination_entries.interfaces[ifid];

	return _dhd_wlfc_mac_entry_update(ctx, entry, action, ifid, iftype, ea,
		_dhd_wlfc_ifpkt_fn, &ifid);
}

static int
_dhd_wlfc_BCMCCredit_support_update(void* state)
{
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;

	ctx->bcmc_credit_supported = TRUE;
	return BCME_OK;
}

static int
_dhd_wlfc_FIFOcreditmap_update(void* state, uint8* credits)
{
	athost_wl_status_info_t* ctx = (athost_wl_status_info_t*)state;
	int i;

	for (i = 0; i <= 4; i++) {
		if (ctx->Init_FIFO_credit[i] != ctx->FIFO_credit[i]) {
			DHD_ERROR(("%s: credit[i] is not returned, (%d %d)\n",
				__FUNCTION__, ctx->Init_FIFO_credit[i], ctx->FIFO_credit[i]));
		}
	}

	/* update the AC FIFO credit map */
	ctx->FIFO_credit[0] += (credits[0] - ctx->Init_FIFO_credit[0]);
	ctx->FIFO_credit[1] += (credits[1] - ctx->Init_FIFO_credit[1]);
	ctx->FIFO_credit[2] += (credits[2] - ctx->Init_FIFO_credit[2]);
	ctx->FIFO_credit[3] += (credits[3] - ctx->Init_FIFO_credit[3]);
	ctx->FIFO_credit[4] += (credits[4] - ctx->Init_FIFO_credit[4]);

	ctx->Init_FIFO_credit[0] = credits[0];
	ctx->Init_FIFO_credit[1] = credits[1];
	ctx->Init_FIFO_credit[2] = credits[2];
	ctx->Init_FIFO_credit[3] = credits[3];
	ctx->Init_FIFO_credit[4] = credits[4];

	/* credit for ATIM FIFO is not used yet. */
	ctx->Init_FIFO_credit[5] = ctx->FIFO_credit[5] = 0;

	return BCME_OK;
}

static int
_dhd_wlfc_handle_packet_commit(athost_wl_status_info_t* ctx, int ac,
    dhd_wlfc_commit_info_t *commit_info, f_commitpkt_t fcommit, void* commit_ctx)
{
	uint32 hslot;
	int	rc;
	dhd_pub_t *dhdp = (dhd_pub_t *)(ctx->dhdp);

	/*
		if ac_fifo_credit_spent = 0

		This packet will not count against the FIFO credit.
		To ensure the txstatus corresponding to this packet
		does not provide an implied credit (default behavior)
		mark the packet accordingly.

		if ac_fifo_credit_spent = 1

		This is a normal packet and it counts against the FIFO
		credit count.
	*/
	DHD_PKTTAG_SETCREDITCHECK(PKTTAG(commit_info->p), commit_info->ac_fifo_credit_spent);
	rc = _dhd_wlfc_pretx_pktprocess(ctx, commit_info->mac_entry, commit_info->p,
	     commit_info->needs_hdr, &hslot);

	if (rc == BCME_OK) {
		DHD_PKTTAG_WLFCPKT_SET(PKTTAG(commit_info->p), 1);
		rc = fcommit(commit_ctx, commit_info->p);
		if (rc == BCME_OK) {
			uint8 gen = WL_TXSTATUS_GET_GENERATION(
				DHD_PKTTAG_H2DTAG(PKTTAG(commit_info->p)));
			if (!WLFC_GET_AFQ(dhdp->wlfc_mode)) {
				_dhd_wlfc_hanger_waitevent_set(ctx->hanger, hslot,
					WLFC_HANGER_ITEM_WAIT_EVENT_COUNT);
			}
			ctx->stats.pkt2bus++;
			if (commit_info->ac_fifo_credit_spent || (ac == AC_COUNT)) {
				ctx->stats.send_pkts[ac]++;
				WLFC_HOST_FIFO_CREDIT_INC_SENTCTRS(ctx, ac);
			}

			if (gen != commit_info->mac_entry->generation) {
				/* will be suppressed back by design */
				if (!commit_info->mac_entry->suppressed) {
					commit_info->mac_entry->suppressed = TRUE;
				}
				commit_info->mac_entry->suppr_transit_count++;
			}
			commit_info->mac_entry->transit_count++;
		} else if (commit_info->needs_hdr) {
			if (!WLFC_GET_AFQ(dhdp->wlfc_mode)) {
				void *pout = NULL;
				/* pop hanger for delayed packet */
				_dhd_wlfc_hanger_poppkt(ctx->hanger, WL_TXSTATUS_GET_HSLOT(
					DHD_PKTTAG_H2DTAG(PKTTAG(commit_info->p))), &pout, 1);
				ASSERT(commit_info->p == pout);
			}
		}
	} else {
		ctx->stats.generic_error++;
	}

	if (rc != BCME_OK) {
		/*
		   pretx pkt process or bus commit has failed, rollback.
		   - remove wl-header for a delayed packet
		   - save wl-header header for suppressed packets
		   - reset credit check flag
		*/
		_dhd_wlfc_rollback_packet_toq(ctx, commit_info->p, commit_info->pkt_type, hslot);
		DHD_PKTTAG_SETCREDITCHECK(PKTTAG(commit_info->p), 0);
	}

	return rc;
}

static uint8
_dhd_wlfc_find_mac_desc_id_from_mac(dhd_pub_t *dhdp, uint8* ea)
{
	wlfc_mac_descriptor_t* table =
		((athost_wl_status_info_t*)dhdp->wlfc_state)->destination_entries.nodes;
	uint8 table_index;

	if (ea != NULL) {
		for (table_index = 0; table_index < WLFC_MAC_DESC_TABLE_SIZE; table_index++) {
			if ((memcmp(ea, &table[table_index].ea[0], ETHER_ADDR_LEN) == 0) &&
				table[table_index].occupied)
				return table_index;
		}
	}
	return WLFC_MAC_DESC_ID_INVALID;
}

static int
dhd_wlfc_suppressed_acked_update(dhd_pub_t *dhd, uint16 hslot, uint8 prec, uint8 hcnt)
{
	athost_wl_status_info_t* ctx;
	wlfc_mac_descriptor_t* entry = NULL;
	struct pktq *pq;
	struct pktq_prec *q;
	void *p, *b;

	if (!dhd) {
		DHD_ERROR(("%s: dhd(%p)\n", __FUNCTION__, dhd));
		return BCME_BADARG;
	}
	ctx = (athost_wl_status_info_t*)dhd->wlfc_state;
	if (!ctx) {
		DHD_ERROR(("%s: ctx(%p)\n", __FUNCTION__, ctx));
		return BCME_ERROR;
	}

	ASSERT(hslot < (WLFC_MAC_DESC_TABLE_SIZE + WLFC_MAX_IFNUM + 1));

	if (hslot < WLFC_MAC_DESC_TABLE_SIZE)
		entry  = &ctx->destination_entries.nodes[hslot];
	else if (hslot < (WLFC_MAC_DESC_TABLE_SIZE + WLFC_MAX_IFNUM))
		entry = &ctx->destination_entries.interfaces[hslot - WLFC_MAC_DESC_TABLE_SIZE];
	else
		entry = &ctx->destination_entries.other;

	pq = &entry->psq;

	ASSERT(((prec << 1) + 1) < pq->num_prec);

	q = &pq->q[((prec << 1) + 1)];

	b = NULL;
	p = q->head;

	while (p && (hcnt != WL_TXSTATUS_GET_FREERUNCTR(DHD_PKTTAG_H2DTAG(PKTTAG(p))))) {
		b = p;
		p = PKTLINK(p);
	}

	if (p == NULL) {
		/* none is matched */
		if (b) {
			DHD_ERROR(("%s: can't find matching seq(%d)\n", __FUNCTION__, hcnt));
		} else {
			DHD_ERROR(("%s: queue is empty\n", __FUNCTION__));
		}

		return BCME_ERROR;
	}

	if (!b) {
		/* head packet is matched */
		if ((q->head = PKTLINK(p)) == NULL) {
			q->tail = NULL;
		}
	} else {
		/* middle packet is matched */
		PKTSETLINK(b, PKTLINK(p));
		if (PKTLINK(p) == NULL) {
			q->tail = b;
		}
	}

	q->len--;
	pq->len--;
	ctx->pkt_cnt_in_q[DHD_PKTTAG_IF(PKTTAG(p))][prec]--;
	ctx->pkt_cnt_per_ac[prec]--;

	PKTSETLINK(p, NULL);

	if (WLFC_GET_AFQ(dhd->wlfc_mode)) {
		_dhd_wlfc_enque_afq(ctx, p);
	} else {
		_dhd_wlfc_hanger_pushpkt(ctx->hanger, p, hslot);
	}

	entry->transit_count++;

	return BCME_OK;
}

static int
_dhd_wlfc_compressed_txstatus_update(dhd_pub_t *dhd, uint8* pkt_info, uint8 len, void** p_mac)
{
	uint8 status_flag;
	uint32 status;
	int ret = BCME_OK;
	int remove_from_hanger = 1;
	void* pktbuf = NULL;
	uint8 fifo_id = 0, gen = 0, count = 0, hcnt;
	uint16 hslot;
	wlfc_mac_descriptor_t* entry = NULL;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	uint16 seq = 0, seq_fromfw = 0, seq_num = 0;

	memcpy(&status, pkt_info, sizeof(uint32));
	status_flag = WL_TXSTATUS_GET_FLAGS(status);
	hcnt = WL_TXSTATUS_GET_FREERUNCTR(status);
	hslot = WL_TXSTATUS_GET_HSLOT(status);
	fifo_id = WL_TXSTATUS_GET_FIFO(status);
	gen = WL_TXSTATUS_GET_GENERATION(status);

	if (WLFC_GET_REUSESEQ(dhd->wlfc_mode)) {
		memcpy(&seq, pkt_info + WLFC_CTL_VALUE_LEN_TXSTATUS, WLFC_CTL_VALUE_LEN_SEQ);
		seq_fromfw = WL_SEQ_GET_FROMFW(seq);
		seq_num = WL_SEQ_GET_NUM(seq);
	}

	wlfc->stats.txstatus_in += len;

	if (status_flag == WLFC_CTL_PKTFLAG_DISCARD) {
		wlfc->stats.pkt_freed += len;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_DISCARD_NOACK) {
		wlfc->stats.pkt_freed += len;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_D11SUPPRESS) {
		wlfc->stats.d11_suppress += len;
		remove_from_hanger = 0;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_WLSUPPRESS) {
		wlfc->stats.wl_suppress += len;
		remove_from_hanger = 0;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_TOSSED_BYWLC) {
		wlfc->stats.wlc_tossed_pkts += len;
	}

	else if (status_flag == WLFC_CTL_PKTFLAG_SUPPRESS_ACKED) {
		wlfc->stats.pkt_freed += len;
	}

	if (dhd->proptxstatus_txstatus_ignore) {
		if (!remove_from_hanger) {
			DHD_ERROR(("suppress txstatus: %d\n", status_flag));
		}
		return BCME_OK;
	}

	while (count < len) {
		if (status_flag == WLFC_CTL_PKTFLAG_SUPPRESS_ACKED) {
			dhd_wlfc_suppressed_acked_update(dhd, hslot, fifo_id, hcnt);
		}
		if (WLFC_GET_AFQ(dhd->wlfc_mode)) {
			ret = _dhd_wlfc_deque_afq(wlfc, hslot, hcnt, fifo_id, &pktbuf);
		} else {
			if (_dhd_wlfc_hanger_wait_clean(wlfc->hanger, hslot)) {
				goto cont;
			}

			ret = _dhd_wlfc_hanger_poppkt(wlfc->hanger, hslot, &pktbuf, 0);
		}

		if ((ret != BCME_OK) || !pktbuf) {
			goto cont;
		}

		/* set fifo_id to correct value because not all FW does that */
		fifo_id = DHD_PKTTAG_FIFO(PKTTAG(pktbuf));

		entry = _dhd_wlfc_find_table_entry(wlfc, pktbuf);

		if (!remove_from_hanger) {
			/* this packet was suppressed */
			if (!entry->suppressed || (entry->generation != gen)) {
				if (!entry->suppressed) {
					entry->suppr_transit_count = entry->transit_count;
					if (p_mac) {
						*p_mac = entry;
					}
				} else {
					DHD_ERROR(("gen(%d), entry->generation(%d)\n",
						gen, entry->generation));
				}
				entry->suppressed = TRUE;

			}
			entry->generation = gen;
		}

#ifdef PROP_TXSTATUS_DEBUG
		if (!WLFC_GET_AFQ(dhd->wlfc_mode))
		{
			uint32 new_t = OSL_SYSUPTIME();
			uint32 old_t;
			uint32 delta;
			old_t = ((wlfc_hanger_t*)(wlfc->hanger))->items[hslot].push_time;


			wlfc->stats.latency_sample_count++;
			if (new_t > old_t)
				delta = new_t - old_t;
			else
				delta = 0xffffffff + new_t - old_t;
			wlfc->stats.total_status_latency += delta;
			wlfc->stats.latency_most_recent = delta;

			wlfc->stats.deltas[wlfc->stats.idx_delta++] = delta;
			if (wlfc->stats.idx_delta == sizeof(wlfc->stats.deltas)/sizeof(uint32))
				wlfc->stats.idx_delta = 0;
		}
#endif /* PROP_TXSTATUS_DEBUG */

		/* pick up the implicit credit from this packet */
		if (DHD_PKTTAG_CREDITCHECK(PKTTAG(pktbuf))) {
			_dhd_wlfc_return_implied_credit(wlfc, pktbuf);
		} else {
			/*
			if this packet did not count against FIFO credit, it must have
			taken a requested_credit from the destination entry (for pspoll etc.)
			*/
			if (!DHD_PKTTAG_ONETIMEPKTRQST(PKTTAG(pktbuf)))
				entry->requested_credit++;
#ifdef PROP_TXSTATUS_DEBUG
			entry->dstncredit_acks++;
#endif
		}

		if ((status_flag == WLFC_CTL_PKTFLAG_D11SUPPRESS) ||
			(status_flag == WLFC_CTL_PKTFLAG_WLSUPPRESS)) {
			/* save generation bit inside packet */
			WL_TXSTATUS_SET_GENERATION(DHD_PKTTAG_H2DTAG(PKTTAG(pktbuf)), gen);

			if (WLFC_GET_REUSESEQ(dhd->wlfc_mode)) {
				WL_SEQ_SET_FROMDRV(DHD_PKTTAG_H2DSEQ(PKTTAG(pktbuf)), seq_fromfw);
				WL_SEQ_SET_NUM(DHD_PKTTAG_H2DSEQ(PKTTAG(pktbuf)), seq_num);
			}

			ret = _dhd_wlfc_enque_suppressed(wlfc, fifo_id, pktbuf);
			if (ret != BCME_OK) {
				/* delay q is full, drop this packet */
				DHD_WLFC_QMON_COMPLETE(entry);
				_dhd_wlfc_prec_drop(dhd, (fifo_id << 1) + 1, pktbuf, FALSE);
			} else {
				if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
					/* Mark suppressed to avoid a double free
					during wlfc cleanup
					*/
					_dhd_wlfc_hanger_mark_suppressed(wlfc->hanger, hslot, gen);
				}
			}
		} else {
			uint8 waitevent = 0;
			void *pktbuf_tmp = NULL;
			dhd_txcomplete(dhd, pktbuf, TRUE);

			DHD_WLFC_QMON_COMPLETE(entry);
			wlfc->stats.pktout++;

			if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
				waitevent = _dhd_wlfc_hanger_waitevent_decreturn(wlfc->hanger,
				        hslot);
				if (!waitevent) {
					ret = _dhd_wlfc_hanger_poppkt(wlfc->hanger,
					        hslot, &pktbuf_tmp, 1);
					ASSERT((ret == BCME_OK) && (pktbuf == pktbuf_tmp));
				}
			}
			if (!waitevent) {
				/* free the packet */
				PKTFREE(wlfc->osh, pktbuf, TRUE);
			}
		}
		/* pkt back from firmware side */
		entry->transit_count--;
		if (entry->suppressed && (--entry->suppr_transit_count == 0)) {
			entry->suppressed = FALSE;
		}

cont:
		hcnt = (hcnt + 1) & WL_TXSTATUS_FREERUNCTR_MASK;
		if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
			hslot = (hslot + 1) & WL_TXSTATUS_HSLOT_MASK;
		}

		if (WLFC_GET_REUSESEQ(dhd->wlfc_mode) && seq_fromfw) {
			seq_num = (seq_num + 1) & WL_SEQ_NUM_MASK;
		}

		count++;
	}
	return BCME_OK;
}

static int
_dhd_wlfc_fifocreditback_indicate(dhd_pub_t *dhd, uint8* credits)
{
	int i;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	for (i = 0; i < WLFC_CTL_VALUE_LEN_FIFO_CREDITBACK; i++) {
#ifdef PROP_TXSTATUS_DEBUG
		wlfc->stats.fifo_credits_back[i] += credits[i];
#endif

		/* update FIFO credits */
		if (dhd->proptxstatus_mode == WLFC_FCMODE_EXPLICIT_CREDIT)
		{
			int lender; /* Note that borrower is i */

			/* Return credits to highest priority lender first */
			for (lender = AC_COUNT; (lender >= 0) && (credits[i] > 0); lender--) {
				if (wlfc->credits_borrowed[i][lender] > 0) {
					if (credits[i] >= wlfc->credits_borrowed[i][lender]) {
						credits[i] -=
							(uint8)wlfc->credits_borrowed[i][lender];
						wlfc->FIFO_credit[lender] +=
						    wlfc->credits_borrowed[i][lender];
						wlfc->credits_borrowed[i][lender] = 0;
					}
					else {
						wlfc->credits_borrowed[i][lender] -= credits[i];
						wlfc->FIFO_credit[lender] += credits[i];
						credits[i] = 0;
					}
				}
			}

			/* If we have more credits left over, these must belong to the AC */
			if (credits[i] > 0) {
				wlfc->FIFO_credit[i] += credits[i];
			}

			if (wlfc->FIFO_credit[i] > wlfc->Init_FIFO_credit[i]) {
				wlfc->FIFO_credit[i] = wlfc->Init_FIFO_credit[i];
			}
		}
	}

	return BCME_OK;
}

static void
_dhd_wlfc_suppress_txq(dhd_pub_t *dhd, f_processpkt_t fn, void *arg)
{
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* entry;
	int prec;
	void *pkt = NULL, *head = NULL, *tail = NULL;
	struct pktq *txq = (struct pktq *)dhd_bus_txq(dhd->bus);
	uint8	results[WLFC_CTL_VALUE_LEN_TXSTATUS+WLFC_CTL_VALUE_LEN_SEQ];
	uint8 credits[WLFC_CTL_VALUE_LEN_FIFO_CREDITBACK] = {0};
	uint32 htod = 0;
	uint16 htodseq = 0;
	bool bCreditUpdate = FALSE;

	dhd_os_sdlock_txq(dhd);
	for (prec = 0; prec < txq->num_prec; prec++) {
		while ((pkt = _dhd_wlfc_pktq_pdeq_with_fn(txq, prec, fn, arg))) {
			if (!head) {
				head = pkt;
			}
			if (tail) {
				PKTSETLINK(tail, pkt);
			}
			tail = pkt;
		}
	}
	dhd_os_sdunlock_txq(dhd);

	while ((pkt = head)) {
		head = PKTLINK(pkt);
		PKTSETLINK(pkt, NULL);

		entry = _dhd_wlfc_find_table_entry(wlfc, pkt);

		/* fake a suppression txstatus */
		htod = DHD_PKTTAG_H2DTAG(PKTTAG(pkt));
		WL_TXSTATUS_SET_FLAGS(htod, WLFC_CTL_PKTFLAG_WLSUPPRESS);
		WL_TXSTATUS_SET_GENERATION(htod, entry->generation);
		memcpy(results, &htod, WLFC_CTL_VALUE_LEN_TXSTATUS);
		if (WLFC_GET_REUSESEQ(dhd->wlfc_mode)) {
			htodseq = DHD_PKTTAG_H2DSEQ(PKTTAG(pkt));
			if (WL_SEQ_GET_FROMDRV(htodseq)) {
				WL_SEQ_SET_FROMFW(htodseq, 1);
				WL_SEQ_SET_FROMDRV(htodseq, 0);
			}
			memcpy(results + WLFC_CTL_VALUE_LEN_TXSTATUS, &htodseq,
				WLFC_CTL_VALUE_LEN_SEQ);
		}
		if (WLFC_GET_AFQ(dhd->wlfc_mode)) {
			_dhd_wlfc_enque_afq(wlfc, pkt);
		}
		_dhd_wlfc_compressed_txstatus_update(dhd, results, 1, NULL);

		/* fake a fifo credit back */
		if (DHD_PKTTAG_CREDITCHECK(PKTTAG(pkt))) {
			credits[DHD_PKTTAG_FIFO(PKTTAG(pkt))]++;
			bCreditUpdate = TRUE;
		}
	}

	if (bCreditUpdate) {
		_dhd_wlfc_fifocreditback_indicate(dhd, credits);
	}
}


static int
_dhd_wlfc_dbg_senum_check(dhd_pub_t *dhd, uint8 *value)
{
	uint32 timestamp;

	(void)dhd;

	bcopy(&value[2], &timestamp, sizeof(uint32));
	DHD_INFO(("RXPKT: SEQ: %d, timestamp %d\n", value[1], timestamp));
	return BCME_OK;
}

static int
_dhd_wlfc_rssi_indicate(dhd_pub_t *dhd, uint8* rssi)
{
	(void)dhd;
	(void)rssi;
	return BCME_OK;
}

static void
_dhd_wlfc_add_requested_entry(athost_wl_status_info_t* wlfc, wlfc_mac_descriptor_t* entry)
{
	int i;

	if (!wlfc || !entry) {
		return;
	}

	for (i = 0; i < wlfc->requested_entry_count; i++) {
		if (entry == wlfc->requested_entry[i]) {
			break;
		}
	}

	if (i == wlfc->requested_entry_count) {
		/* no match entry found */
		ASSERT(wlfc->requested_entry_count <= (WLFC_MAC_DESC_TABLE_SIZE-1));
		wlfc->requested_entry[wlfc->requested_entry_count++] = entry;
	}
}

static void
_dhd_wlfc_remove_requested_entry(athost_wl_status_info_t* wlfc, wlfc_mac_descriptor_t* entry)
{
	int i;

	if (!wlfc || !entry) {
		return;
	}

	for (i = 0; i < wlfc->requested_entry_count; i++) {
		if (entry == wlfc->requested_entry[i]) {
			break;
		}
	}

	if (i < wlfc->requested_entry_count) {
		/* found */
		ASSERT(wlfc->requested_entry_count > 0);
		wlfc->requested_entry_count--;
		if (i != wlfc->requested_entry_count) {
			wlfc->requested_entry[i] =
				wlfc->requested_entry[wlfc->requested_entry_count];
		}
		wlfc->requested_entry[wlfc->requested_entry_count] = NULL;
	}
}

static int
_dhd_wlfc_mac_table_update(dhd_pub_t *dhd, uint8* value, uint8 type)
{
	int rc;
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	uint8 existing_index;
	uint8 table_index;
	uint8 ifid;
	uint8* ea;

	WLFC_DBGMESG(("%s(), mac [%02x:%02x:%02x:%02x:%02x:%02x],%s,idx:%d,id:0x%02x\n",
		__FUNCTION__, value[2], value[3], value[4], value[5], value[6], value[7],
		((type == WLFC_CTL_TYPE_MACDESC_ADD) ? "ADD":"DEL"),
		WLFC_MAC_DESC_GET_LOOKUP_INDEX(value[0]), value[0]));

	table = wlfc->destination_entries.nodes;
	table_index = WLFC_MAC_DESC_GET_LOOKUP_INDEX(value[0]);
	ifid = value[1];
	ea = &value[2];

	_dhd_wlfc_remove_requested_entry(wlfc, &table[table_index]);
	if (type == WLFC_CTL_TYPE_MACDESC_ADD) {
		existing_index = _dhd_wlfc_find_mac_desc_id_from_mac(dhd, &value[2]);
		if ((existing_index != WLFC_MAC_DESC_ID_INVALID) &&
			(existing_index != table_index) && table[existing_index].occupied) {
			/*
			there is an existing different entry, free the old one
			and move it to new index if necessary.
			*/
			rc = _dhd_wlfc_mac_entry_update(wlfc, &table[existing_index],
				eWLFC_MAC_ENTRY_ACTION_DEL, table[existing_index].interface_id,
				table[existing_index].iftype, NULL, _dhd_wlfc_entrypkt_fn,
				&table[existing_index]);
		}

		if (!table[table_index].occupied) {
			/* this new MAC entry does not exist, create one */
			table[table_index].mac_handle = value[0];
			rc = _dhd_wlfc_mac_entry_update(wlfc, &table[table_index],
				eWLFC_MAC_ENTRY_ACTION_ADD, ifid,
				wlfc->destination_entries.interfaces[ifid].iftype,
				ea, NULL, NULL);
		} else {
			/* the space should have been empty, but it's not */
			wlfc->stats.mac_update_failed++;
		}
	}

	if (type == WLFC_CTL_TYPE_MACDESC_DEL) {
		if (table[table_index].occupied) {
				rc = _dhd_wlfc_mac_entry_update(wlfc, &table[table_index],
					eWLFC_MAC_ENTRY_ACTION_DEL, ifid,
					wlfc->destination_entries.interfaces[ifid].iftype,
					ea, _dhd_wlfc_entrypkt_fn, &table[table_index]);
		} else {
			/* the space should have been occupied, but it's not */
			wlfc->stats.mac_update_failed++;
		}
	}
	BCM_REFERENCE(rc);
	return BCME_OK;
}

static int
_dhd_wlfc_psmode_update(dhd_pub_t *dhd, uint8* value, uint8 type)
{
	/* Handle PS on/off indication */
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_mac_descriptor_t* desc;
	uint8 mac_handle = value[0];
	int i;

	table = wlfc->destination_entries.nodes;
	desc = &table[WLFC_MAC_DESC_GET_LOOKUP_INDEX(mac_handle)];
	if (desc->occupied) {
		/* a fresh PS mode should wipe old ps credits? */
		desc->requested_credit = 0;
		if (type == WLFC_CTL_TYPE_MAC_OPEN) {
			desc->state = WLFC_STATE_OPEN;
			desc->ac_bitmap = 0xff;
			DHD_WLFC_CTRINC_MAC_OPEN(desc);
			_dhd_wlfc_remove_requested_entry(wlfc, desc);
		}
		else {
			desc->state = WLFC_STATE_CLOSE;
			DHD_WLFC_CTRINC_MAC_CLOSE(desc);
			/*
			Indicate to firmware if there is any traffic pending.
			*/
			for (i = 0; i < AC_COUNT; i++) {
				_dhd_wlfc_traffic_pending_check(wlfc, desc, i);
			}
		}
	}
	else {
		wlfc->stats.psmode_update_failed++;
	}
	return BCME_OK;
}

static int
_dhd_wlfc_interface_update(dhd_pub_t *dhd, uint8* value, uint8 type)
{
	/* Handle PS on/off indication */
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	uint8 if_id = value[0];

	if (if_id < WLFC_MAX_IFNUM) {
		table = wlfc->destination_entries.interfaces;
		if (table[if_id].occupied) {
			if (type == WLFC_CTL_TYPE_INTERFACE_OPEN) {
				table[if_id].state = WLFC_STATE_OPEN;
				/* WLFC_DBGMESG(("INTERFACE[%d] OPEN\n", if_id)); */
			}
			else {
				table[if_id].state = WLFC_STATE_CLOSE;
				/* WLFC_DBGMESG(("INTERFACE[%d] CLOSE\n", if_id)); */
			}
			return BCME_OK;
		}
	}
	wlfc->stats.interface_update_failed++;

	return BCME_OK;
}

static int
_dhd_wlfc_credit_request(dhd_pub_t *dhd, uint8* value)
{
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_mac_descriptor_t* desc;
	uint8 mac_handle;
	uint8 credit;

	table = wlfc->destination_entries.nodes;
	mac_handle = value[1];
	credit = value[0];

	desc = &table[WLFC_MAC_DESC_GET_LOOKUP_INDEX(mac_handle)];
	if (desc->occupied) {
		desc->requested_credit = credit;

		desc->ac_bitmap = value[2] & (~(1<<AC_COUNT));
		_dhd_wlfc_add_requested_entry(wlfc, desc);
	}
	else {
		wlfc->stats.credit_request_failed++;
	}
	return BCME_OK;
}

static int
_dhd_wlfc_packet_request(dhd_pub_t *dhd, uint8* value)
{
	athost_wl_status_info_t* wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	wlfc_mac_descriptor_t* table;
	wlfc_mac_descriptor_t* desc;
	uint8 mac_handle;
	uint8 packet_count;

	table = wlfc->destination_entries.nodes;
	mac_handle = value[1];
	packet_count = value[0];

	desc = &table[WLFC_MAC_DESC_GET_LOOKUP_INDEX(mac_handle)];
	if (desc->occupied) {
		desc->requested_packet = packet_count;

		desc->ac_bitmap = value[2] & (~(1<<AC_COUNT));
		_dhd_wlfc_add_requested_entry(wlfc, desc);
	}
	else {
		wlfc->stats.packet_request_failed++;
	}
	return BCME_OK;
}

static void
_dhd_wlfc_reorderinfo_indicate(uint8 *val, uint8 len, uchar *info_buf, uint *info_len)
{
	if (info_len) {
		if (info_buf) {
			bcopy(val, info_buf, len);
			*info_len = len;
		}
		else
			*info_len = 0;
	}
}

static void
_dhd_wlfc_save_rxpath_ac_time(athost_wl_status_info_t* wlfc, uint8 prio)
{
	int rx_path_ac = -1;

	if (!wlfc || (prio >= NUMPRIO)) {
		return;
	}

	rx_path_ac = prio2fifo[prio];
	wlfc->rx_timestamp[rx_path_ac] = OSL_SYSUPTIME();
}

/*
 * public functions
 */

bool dhd_wlfc_is_supported(dhd_pub_t *dhd)
{
	bool rc = TRUE;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return FALSE;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		rc =  FALSE;
	}

	dhd_os_wlfc_unblock(dhd);

	return rc;
}

int dhd_wlfc_enable(dhd_pub_t *dhd)
{
	int i, rc = BCME_OK;
	athost_wl_status_info_t* wlfc;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_enabled || dhd->wlfc_state) {
		rc = BCME_OK;
		goto exit;
	}

	/* allocate space to track txstatus propagated from firmware */
	dhd->wlfc_state = DHD_OS_PREALLOC(dhd, DHD_PREALLOC_DHD_WLFC_INFO,
			sizeof(athost_wl_status_info_t));

	if (dhd->wlfc_state == NULL) {
		rc = BCME_NOMEM;
		goto exit;
	}

	/* initialize state space */
	wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	memset(wlfc, 0, sizeof(athost_wl_status_info_t));

	/* remember osh & dhdp */
	wlfc->osh = dhd->osh;
	wlfc->dhdp = dhd;

	if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
		wlfc->hanger = _dhd_wlfc_hanger_create(dhd, WLFC_HANGER_MAXITEMS);
		if (wlfc->hanger == NULL) {
			DHD_OS_PREFREE(dhd, DHD_PREALLOC_DHD_WLFC_INFO,
				dhd->wlfc_state, sizeof(athost_wl_status_info_t));
			dhd->wlfc_state = NULL;
			rc = BCME_NOMEM;
			goto exit;
		}
	}

	dhd->proptxstatus_mode = WLFC_FCMODE_EXPLICIT_CREDIT;
	/* default to check rx pkt */
	if (dhd->op_mode & DHD_FLAG_IBSS_MODE) {
		dhd->wlfc_rxpkt_chk = FALSE;
	} else {
		dhd->wlfc_rxpkt_chk = TRUE;
	}


	/* initialize all interfaces to accept traffic */
	for (i = 0; i < WLFC_MAX_IFNUM; i++) {
		wlfc->hostif_flow_state[i] = OFF;
	}

	_dhd_wlfc_mac_entry_update(wlfc, &wlfc->destination_entries.other,
		eWLFC_MAC_ENTRY_ACTION_ADD, 0xff, 0, NULL, NULL, NULL);

	wlfc->allow_credit_borrow = 0;
	wlfc->single_ac = 0;
	wlfc->single_ac_timestamp = 0;


exit:
	dhd_os_wlfc_unblock(dhd);

	return rc;
}

int
dhd_wlfc_parse_header_info(dhd_pub_t *dhd, void* pktbuf, int tlv_hdr_len, uchar *reorder_info_buf,
	uint *reorder_info_len)
{
	uint8 type, len;
	uint8* value;
	uint8* tmpbuf;
	uint16 remainder = (uint16)tlv_hdr_len;
	uint16 processed = 0;
	athost_wl_status_info_t* wlfc = NULL;
	void* entry;

	if ((dhd == NULL) || (pktbuf == NULL)) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (dhd->proptxstatus_mode != WLFC_ONLY_AMPDU_HOSTREORDER) {
		if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
			dhd_os_wlfc_unblock(dhd);
			return WLFC_UNSUPPORTED;
		}
		wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	}

	if (dhd->wlfc_rxpkt_chk) {
		_dhd_wlfc_save_rxpath_ac_time(wlfc, PKTPRIO(pktbuf));
	}

	tmpbuf = (uint8*)PKTDATA(dhd->osh, pktbuf);

	if (remainder) {
		while ((processed < (WLFC_MAX_PENDING_DATALEN * 2)) && (remainder > 0)) {
			type = tmpbuf[processed];
			if (type == WLFC_CTL_TYPE_FILLER) {
				remainder -= 1;
				processed += 1;
				continue;
			}

			len  = tmpbuf[processed + 1];
			value = &tmpbuf[processed + 2];

			if (remainder < (2 + len))
				break;

			remainder -= 2 + len;
			processed += 2 + len;
			entry = NULL;

			DHD_INFO(("%s():%d type %d remainder %d processed %d\n",
				__FUNCTION__, __LINE__, type, remainder, processed));

			if (type == WLFC_CTL_TYPE_HOST_REORDER_RXPKTS)
				_dhd_wlfc_reorderinfo_indicate(value, len, reorder_info_buf,
					reorder_info_len);

			if (wlfc == NULL) {
				ASSERT(dhd->proptxstatus_mode == WLFC_ONLY_AMPDU_HOSTREORDER);

				if (type != WLFC_CTL_TYPE_HOST_REORDER_RXPKTS &&
					type != WLFC_CTL_TYPE_TRANS_ID)
					DHD_INFO(("%s():%d dhd->wlfc_state is NULL yet!"
					" type %d remainder %d processed %d\n",
					__FUNCTION__, __LINE__, type, remainder, processed));
				continue;
			}

			if (type == WLFC_CTL_TYPE_TXSTATUS) {
				_dhd_wlfc_compressed_txstatus_update(dhd, value, 1, &entry);
			}
			else if (type == WLFC_CTL_TYPE_COMP_TXSTATUS) {
				uint8 compcnt_offset = WLFC_CTL_VALUE_LEN_TXSTATUS;

				if (WLFC_GET_REUSESEQ(dhd->wlfc_mode)) {
					compcnt_offset += WLFC_CTL_VALUE_LEN_SEQ;
				}
				_dhd_wlfc_compressed_txstatus_update(dhd, value,
					value[compcnt_offset], &entry);
			}
			else if (type == WLFC_CTL_TYPE_FIFO_CREDITBACK)
				_dhd_wlfc_fifocreditback_indicate(dhd, value);

			else if (type == WLFC_CTL_TYPE_RSSI)
				_dhd_wlfc_rssi_indicate(dhd, value);

			else if (type == WLFC_CTL_TYPE_MAC_REQUEST_CREDIT)
				_dhd_wlfc_credit_request(dhd, value);

			else if (type == WLFC_CTL_TYPE_MAC_REQUEST_PACKET)
				_dhd_wlfc_packet_request(dhd, value);

			else if ((type == WLFC_CTL_TYPE_MAC_OPEN) ||
				(type == WLFC_CTL_TYPE_MAC_CLOSE))
				_dhd_wlfc_psmode_update(dhd, value, type);

			else if ((type == WLFC_CTL_TYPE_MACDESC_ADD) ||
				(type == WLFC_CTL_TYPE_MACDESC_DEL))
				_dhd_wlfc_mac_table_update(dhd, value, type);

			else if (type == WLFC_CTL_TYPE_TRANS_ID)
				_dhd_wlfc_dbg_senum_check(dhd, value);

			else if ((type == WLFC_CTL_TYPE_INTERFACE_OPEN) ||
				(type == WLFC_CTL_TYPE_INTERFACE_CLOSE)) {
				_dhd_wlfc_interface_update(dhd, value, type);
			}

			if (entry && WLFC_GET_REORDERSUPP(dhd->wlfc_mode)) {
				/* suppress all packets for this mac entry from bus->txq */
				_dhd_wlfc_suppress_txq(dhd, _dhd_wlfc_entrypkt_fn, entry);
			}
		}
		if (remainder != 0 && wlfc) {
			/* trouble..., something is not right */
			wlfc->stats.tlv_parse_failed++;
		}
	}

	if (wlfc)
		wlfc->stats.dhd_hdrpulls++;

	dhd_os_wlfc_unblock(dhd);
	return BCME_OK;
}

int
dhd_wlfc_commit_packets(dhd_pub_t *dhdp, f_commitpkt_t fcommit, void* commit_ctx, void *pktbuf,
	bool need_toggle_host_if)
{
	int ac, single_ac = 0, rc = BCME_OK;
	dhd_wlfc_commit_info_t  commit_info;
	athost_wl_status_info_t* ctx;
	int bus_retry_count = 0;

	uint8 traffic_map = 0; /* packets (send + in queue), Bitmask for 4 ACs + BC/MC */
	uint8 packets_map = 0; /* packets in queue, Bitmask for 4 ACs + BC/MC */
	bool no_credit = FALSE;

#ifdef LIMIT_BORROW
	int lender;
#endif

	if ((dhdp == NULL) || (fcommit == NULL)) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhdp);

	if (!dhdp->wlfc_state || (dhdp->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		if (pktbuf) {
			DHD_PKTTAG_WLFCPKT_SET(PKTTAG(pktbuf), 0);
		}
		rc =  WLFC_UNSUPPORTED;
		goto exit2;
	}

	ctx = (athost_wl_status_info_t*)dhdp->wlfc_state;


	if (dhdp->proptxstatus_module_ignore) {
		if (pktbuf) {
			uint32 htod = 0;
			WL_TXSTATUS_SET_FLAGS(htod, WLFC_PKTFLAG_PKTFROMHOST);
			_dhd_wlfc_pushheader(ctx, pktbuf, FALSE, 0, 0, htod, 0, FALSE);
			if (!fcommit(commit_ctx, pktbuf))
				PKTFREE(ctx->osh, pktbuf, TRUE);
			rc = BCME_OK;
		}
		goto exit;
	}

	memset(&commit_info, 0, sizeof(commit_info));

	/*
	Commit packets for regular AC traffic. Higher priority first.
	First, use up FIFO credits available to each AC. Based on distribution
	and credits left, borrow from other ACs as applicable

	-NOTE:
	If the bus between the host and firmware is overwhelmed by the
	traffic from host, it is possible that higher priority traffic
	starves the lower priority queue. If that occurs often, we may
	have to employ weighted round-robin or ucode scheme to avoid
	low priority packet starvation.
	*/

	if (pktbuf) {
		ac = DHD_PKTTAG_FIFO(PKTTAG(pktbuf));
		/* en-queue the packets to respective queue. */
		rc = _dhd_wlfc_enque_delayq(ctx, pktbuf, ac);
		if (rc)
			_dhd_wlfc_prec_drop(ctx->dhdp, (ac << 1), pktbuf, FALSE);
		else
			ctx->stats.pktin++;
	}

	for (ac = AC_COUNT; ac >= 0; ac--) {
		if (dhdp->wlfc_rxpkt_chk) {
			/* check rx packet */
			uint32 curr_t = OSL_SYSUPTIME(), delta;

			delta = curr_t - ctx->rx_timestamp[ac];
			if (delta < WLFC_RX_DETECTION_THRESHOLD_MS) {
				traffic_map |= (1 << ac);
				single_ac = ac + 1;
			}
		}

		if (ctx->pkt_cnt_per_ac[ac] == 0) {
			continue;
		}
		traffic_map |= (1 << ac);
		single_ac = ac + 1;
		while (FALSE == dhdp->proptxstatus_txoff) {
			/* packets from delayQ with less priority are fresh and
			 * they'd need header and have no MAC entry
			 */
			no_credit = (ctx->FIFO_credit[ac] < 1);
			if (dhdp->proptxstatus_credit_ignore ||
				((ac == AC_COUNT) && !ctx->bcmc_credit_supported)) {
				no_credit = FALSE;
			}
			commit_info.needs_hdr = 1;
			commit_info.mac_entry = NULL;
			commit_info.p = _dhd_wlfc_deque_delayedq(ctx, ac,
				&(commit_info.ac_fifo_credit_spent),
				&(commit_info.needs_hdr),
				&(commit_info.mac_entry),
				no_credit);
			commit_info.pkt_type = (commit_info.needs_hdr) ? eWLFC_PKTTYPE_DELAYED :
				eWLFC_PKTTYPE_SUPPRESSED;

			if (commit_info.p == NULL) {
				break;
			}

			if (!dhdp->proptxstatus_credit_ignore) {
				ASSERT(ctx->FIFO_credit[ac] >= commit_info.ac_fifo_credit_spent);
			}
			/* here we can ensure have credit or no credit needed */
			rc = _dhd_wlfc_handle_packet_commit(ctx, ac, &commit_info, fcommit,
				commit_ctx);

			/* Bus commits may fail (e.g. flow control); abort after retries */
			if (rc == BCME_OK) {
				if (commit_info.ac_fifo_credit_spent)
					ctx->FIFO_credit[ac]--;
			} else {
				bus_retry_count++;
				if (bus_retry_count >= BUS_RETRIES) {
					DHD_ERROR(("%s: bus error %d\n", __FUNCTION__, rc));
					goto exit;
				}
			}
		}

		if (ctx->pkt_cnt_per_ac[ac]) {
			packets_map |= (1 << ac);
		}
	}

	if ((traffic_map == 0) || dhdp->proptxstatus_credit_ignore) {
		/* nothing send out or remain in queue */
		rc = BCME_OK;
		goto exit;
	}

	if ((traffic_map & (traffic_map - 1)) == 0) {
		/* only one ac exist */
		if ((single_ac == ctx->single_ac) && ctx->allow_credit_borrow) {
			ac = single_ac - 1;
		} else {
			uint32 delta;
			uint32 curr_t = OSL_SYSUPTIME();

			if (single_ac != ctx->single_ac) {
				/* new single ac traffic (first single ac or different single ac) */
				ctx->allow_credit_borrow = 0;
				ctx->single_ac_timestamp = curr_t;
				ctx->single_ac = (uint8)single_ac;
				rc = BCME_OK;
				goto exit;
			}
			/* same ac traffic, check if it lasts enough time */
			delta = curr_t - ctx->single_ac_timestamp;

			if (delta >= WLFC_BORROW_DEFER_PERIOD_MS) {
				/* wait enough time, can borrow now */
				ctx->allow_credit_borrow = 1;
				ac = single_ac - 1;
			} else {
				rc = BCME_OK;
				goto exit;
			}
		}
	} else {
		/* If we have multiple AC traffic, turn off borrowing, mark time and bail out */
		ctx->allow_credit_borrow = 0;
		ctx->single_ac_timestamp = 0;
		ctx->single_ac = 0;
		rc = BCME_OK;
		goto exit;
	}

	if (packets_map == 0) {
		/* nothing to send, skip borrow */
		rc = BCME_OK;
		goto exit;
	}

	/* At this point, borrow all credits only for ac */
	while (FALSE == dhdp->proptxstatus_txoff) {
#ifdef LIMIT_BORROW
		if ((lender = _dhd_wlfc_borrow_credit(ctx, AC_COUNT, ac)) == -1) {
			break;
		}
#endif
		commit_info.p = _dhd_wlfc_deque_delayedq(ctx, ac,
			&(commit_info.ac_fifo_credit_spent),
			&(commit_info.needs_hdr),
			&(commit_info.mac_entry),
			FALSE);
		if (commit_info.p == NULL) {
			/* before borrow only one ac exists and now this only ac is empty */
#ifdef LIMIT_BORROW
			_dhd_wlfc_return_credit(ctx, lender, ac);
#endif
			break;
		}

		commit_info.pkt_type = (commit_info.needs_hdr) ? eWLFC_PKTTYPE_DELAYED :
			eWLFC_PKTTYPE_SUPPRESSED;

		rc = _dhd_wlfc_handle_packet_commit(ctx, ac, &commit_info,
		     fcommit, commit_ctx);

		/* Bus commits may fail (e.g. flow control); abort after retries */
		if (rc == BCME_OK) {

			if (commit_info.ac_fifo_credit_spent) {
#ifndef LIMIT_BORROW
				ctx->FIFO_credit[ac]--;
#endif
			} else {
#ifdef LIMIT_BORROW
				_dhd_wlfc_return_credit(ctx, lender, ac);
#endif
			}
		} else {
#ifdef LIMIT_BORROW
			_dhd_wlfc_return_credit(ctx, lender, ac);
#endif
			bus_retry_count++;
			if (bus_retry_count >= BUS_RETRIES) {
				DHD_ERROR(("%s: bus error %d\n", __FUNCTION__, rc));
				goto exit;
			}
		}
	}

exit:
	if (need_toggle_host_if && ctx->toggle_host_if) {
		ctx->toggle_host_if = 0;
	}

exit2:
	dhd_os_wlfc_unblock(dhdp);
	return rc;
}

int
dhd_wlfc_txcomplete(dhd_pub_t *dhd, void *txp, bool success)
{
	athost_wl_status_info_t* wlfc;
	void* pout = NULL;
	int rtn = BCME_OK;
	if ((dhd == NULL) || (txp == NULL)) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		rtn = WLFC_UNSUPPORTED;
		goto EXIT;
	}

	wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;
	if (DHD_PKTTAG_SIGNALONLY(PKTTAG(txp))) {
#ifdef PROP_TXSTATUS_DEBUG
		wlfc->stats.signal_only_pkts_freed++;
#endif
		/* is this a signal-only packet? */
		_dhd_wlfc_pullheader(wlfc, txp);
		PKTFREE(wlfc->osh, txp, TRUE);
		goto EXIT;
	}

	if (!success || dhd->proptxstatus_txstatus_ignore) {
		wlfc_mac_descriptor_t *entry = _dhd_wlfc_find_table_entry(wlfc, txp);

		WLFC_DBGMESG(("At: %s():%d, bus_complete() failure for %p, htod_tag:0x%08x\n",
			__FUNCTION__, __LINE__, txp, DHD_PKTTAG_H2DTAG(PKTTAG(txp))));
		if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
			_dhd_wlfc_hanger_poppkt(wlfc->hanger, WL_TXSTATUS_GET_HSLOT(
				DHD_PKTTAG_H2DTAG(PKTTAG(txp))), &pout, 1);
			ASSERT(txp == pout);
		}

		/* indicate failure and free the packet */
		dhd_txcomplete(dhd, txp, success);

		/* return the credit, if necessary */
		_dhd_wlfc_return_implied_credit(wlfc, txp);

		entry->transit_count--;
		if (entry->suppressed && (--entry->suppr_transit_count == 0)) {
			entry->suppressed = FALSE;
		}

		PKTFREE(wlfc->osh, txp, TRUE);
		wlfc->stats.pktout++;
	} else {
		/* bus confirmed pkt went to firmware side */
		if (WLFC_GET_AFQ(dhd->wlfc_mode)) {
			_dhd_wlfc_enque_afq(wlfc, txp);
		} else {
			int ret;
			void *pktbuf_tmp = NULL;
			int hslot = WL_TXSTATUS_GET_HSLOT(DHD_PKTTAG_H2DTAG(PKTTAG(txp)));
			if (_dhd_wlfc_hanger_waitevent_decreturn(wlfc->hanger, hslot) == 0) {
				ret = _dhd_wlfc_hanger_poppkt(wlfc->hanger, hslot, &pktbuf_tmp, 1);
				BCM_REFERENCE(ret);
				ASSERT((ret == BCME_OK) && pktbuf_tmp && (txp == pktbuf_tmp));

				/* free the packet */
				PKTFREE(wlfc->osh, txp, TRUE);
			}
		}
	}

EXIT:
	dhd_os_wlfc_unblock(dhd);
	return rtn;
}

int
dhd_wlfc_init(dhd_pub_t *dhd)
{
	char iovbuf[14]; /* Room for "tlv" + '\0' + parameter */
	/* enable all signals & indicate host proptxstatus logic is active */
	uint32 tlv, mode, fw_caps;
	int ret = 0;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);
	if (dhd->wlfc_enabled) {
		DHD_ERROR(("%s():%d, Already enabled!\n", __FUNCTION__, __LINE__));
		dhd_os_wlfc_unblock(dhd);
		return BCME_OK;
	}
	dhd->wlfc_enabled = TRUE;
	dhd_os_wlfc_unblock(dhd);

	tlv = WLFC_FLAGS_RSSI_SIGNALS |
		WLFC_FLAGS_XONXOFF_SIGNALS |
		WLFC_FLAGS_CREDIT_STATUS_SIGNALS |
		WLFC_FLAGS_HOST_PROPTXSTATUS_ACTIVE |
		WLFC_FLAGS_HOST_RXRERODER_ACTIVE;


	/*
	try to enable/disable signaling by sending "tlv" iovar. if that fails,
	fallback to no flow control? Print a message for now.
	*/

	/* enable proptxtstatus signaling by default */
	bcm_mkiovar("tlv", (char *)&tlv, 4, iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) < 0) {
		DHD_ERROR(("dhd_wlfc_init(): failed to enable/disable bdcv2 tlv signaling\n"));
	}
	else {
		/*
		Leaving the message for now, it should be removed after a while; once
		the tlv situation is stable.
		*/
		DHD_ERROR(("dhd_wlfc_init(): successfully %s bdcv2 tlv signaling, %d\n",
			dhd->wlfc_enabled?"enabled":"disabled", tlv));
	}

	/* query caps */
	ret = bcm_mkiovar("wlfc_mode", (char *)&mode, 4, iovbuf, sizeof(iovbuf));
	if (ret > 0) {
		ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0);
	}

	if (ret >= 0) {
		fw_caps = *((uint32 *)iovbuf);
		mode = 0;
		DHD_ERROR(("%s: query wlfc_mode succeed, fw_caps=0x%x\n", __FUNCTION__, fw_caps));

		if (WLFC_IS_OLD_DEF(fw_caps)) {
			/* enable proptxtstatus v2 by default */
			mode = WLFC_MODE_AFQ;
		} else {
			WLFC_SET_AFQ(mode, WLFC_GET_AFQ(fw_caps));
			WLFC_SET_REUSESEQ(mode, WLFC_GET_REUSESEQ(fw_caps));
			WLFC_SET_REORDERSUPP(mode, WLFC_GET_REORDERSUPP(fw_caps));
		}
		ret = bcm_mkiovar("wlfc_mode", (char *)&mode, 4, iovbuf, sizeof(iovbuf));
		if (ret > 0) {
			ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);
		}
	}

	dhd_os_wlfc_block(dhd);

	dhd->wlfc_mode = 0;
	if (ret >= 0) {
		if (WLFC_IS_OLD_DEF(mode)) {
			WLFC_SET_AFQ(dhd->wlfc_mode, (mode == WLFC_MODE_AFQ));
		} else {
			dhd->wlfc_mode = mode;
		}
	}
	DHD_ERROR(("dhd_wlfc_init(): wlfc_mode=0x%x, ret=%d\n", dhd->wlfc_mode, ret));

	dhd_os_wlfc_unblock(dhd);

	if (dhd->plat_init)
		dhd->plat_init((void *)dhd);

	return BCME_OK;
}

int
dhd_wlfc_hostreorder_init(dhd_pub_t *dhd)
{
	char iovbuf[14]; /* Room for "tlv" + '\0' + parameter */
	/* enable only ampdu hostreorder here */
	uint32 tlv;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	DHD_TRACE(("%s():%d Enter\n", __FUNCTION__, __LINE__));

	tlv = WLFC_FLAGS_HOST_RXRERODER_ACTIVE;

	/* enable proptxtstatus signaling by default */
	bcm_mkiovar("tlv", (char *)&tlv, 4, iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) < 0) {
		DHD_ERROR(("%s(): failed to enable/disable bdcv2 tlv signaling\n",
			__FUNCTION__));
	}
	else {
		/*
		Leaving the message for now, it should be removed after a while; once
		the tlv situation is stable.
		*/
		DHD_ERROR(("%s(): successful bdcv2 tlv signaling, %d\n",
			__FUNCTION__, tlv));
	}

	dhd_os_wlfc_block(dhd);
	dhd->proptxstatus_mode = WLFC_ONLY_AMPDU_HOSTREORDER;
	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

#ifdef SUPPORT_P2P_GO_PS
int
dhd_wlfc_suspend(dhd_pub_t *dhd)
{

	uint32 iovbuf[4]; /* Room for "tlv" + '\0' + parameter */
	uint32 tlv = 0;

	DHD_TRACE(("%s: masking wlfc events\n", __FUNCTION__));
	if (!dhd->wlfc_enabled)
		return -1;

	bcm_mkiovar("tlv", NULL, 0, (char*)iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0) < 0) {
		DHD_ERROR(("%s: failed to get bdcv2 tlv signaling\n", __FUNCTION__));
		return -1;
	}
	tlv = iovbuf[0];
	if ((tlv & (WLFC_FLAGS_RSSI_SIGNALS | WLFC_FLAGS_XONXOFF_SIGNALS)) == 0)
		return 0;
	tlv &= ~(WLFC_FLAGS_RSSI_SIGNALS | WLFC_FLAGS_XONXOFF_SIGNALS);
	bcm_mkiovar("tlv", (char *)&tlv, 4, (char*)iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) < 0) {
		DHD_ERROR(("%s: failed to set bdcv2 tlv signaling to 0x%x\n",
			__FUNCTION__, tlv));
		return -1;
	}

	return 0;
}

	int
dhd_wlfc_resume(dhd_pub_t *dhd)
{
	uint32 iovbuf[4]; /* Room for "tlv" + '\0' + parameter */
	uint32 tlv = 0;

	DHD_TRACE(("%s: unmasking wlfc events\n", __FUNCTION__));
	if (!dhd->wlfc_enabled)
		return -1;

	bcm_mkiovar("tlv", NULL, 0, (char*)iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0) < 0) {
		DHD_ERROR(("%s: failed to get bdcv2 tlv signaling\n", __FUNCTION__));
		return -1;
	}
	tlv = iovbuf[0];
	if ((tlv & (WLFC_FLAGS_RSSI_SIGNALS | WLFC_FLAGS_XONXOFF_SIGNALS)) ==
		(WLFC_FLAGS_RSSI_SIGNALS | WLFC_FLAGS_XONXOFF_SIGNALS))
		return 0;
	tlv |= (WLFC_FLAGS_RSSI_SIGNALS | WLFC_FLAGS_XONXOFF_SIGNALS);
	bcm_mkiovar("tlv", (char *)&tlv, 4, (char*)iovbuf, sizeof(iovbuf));
	if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, (char*)iovbuf, sizeof(iovbuf), TRUE, 0) < 0) {
		DHD_ERROR(("%s: failed to set bdcv2 tlv signaling to 0x%x\n",
			__FUNCTION__, tlv));
		return -1;
	}

	return 0;
}
#endif /* SUPPORT_P2P_GO_PS */

int
dhd_wlfc_cleanup_txq(dhd_pub_t *dhd, f_processpkt_t fn, void *arg)
{
	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhd);
		return WLFC_UNSUPPORTED;
	}

	_dhd_wlfc_cleanup_txq(dhd, fn, arg);

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

/* release all packet resources */
int
dhd_wlfc_cleanup(dhd_pub_t *dhd, f_processpkt_t fn, void *arg)
{
	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhd);
		return WLFC_UNSUPPORTED;
	}

	_dhd_wlfc_cleanup(dhd, fn, arg);

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int
dhd_wlfc_deinit(dhd_pub_t *dhd)
{
	char iovbuf[32]; /* Room for "ampdu_hostreorder" or "tlv" + '\0' + parameter */
	/* cleanup all psq related resources */
	athost_wl_status_info_t* wlfc;
	uint32 tlv = 0;
	uint32 hostreorder = 0;
	int ret = BCME_OK;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);
	if (!dhd->wlfc_enabled) {
		DHD_ERROR(("%s():%d, Already disabled!\n", __FUNCTION__, __LINE__));
		dhd_os_wlfc_unblock(dhd);
		return BCME_OK;
	}
	dhd->wlfc_enabled = FALSE;
	dhd_os_wlfc_unblock(dhd);

	/* query ampdu hostreorder */
	bcm_mkiovar("ampdu_hostreorder", NULL, 0, iovbuf, sizeof(iovbuf));
	ret = dhd_wl_ioctl_cmd(dhd, WLC_GET_VAR, iovbuf, sizeof(iovbuf), FALSE, 0);
	if (ret == BCME_OK)
		hostreorder = *((uint32 *)iovbuf);
	else {
		hostreorder = 0;
		DHD_ERROR(("%s():%d, ampdu_hostreorder get failed Err = %d\n",
			__FUNCTION__, __LINE__, ret));
	}

	if (hostreorder) {
		tlv = WLFC_FLAGS_HOST_RXRERODER_ACTIVE;
		DHD_ERROR(("%s():%d, maintain HOST RXRERODER flag in tvl\n",
			__FUNCTION__, __LINE__));
	}

	/* Disable proptxtstatus signaling for deinit */
	bcm_mkiovar("tlv", (char *)&tlv, 4, iovbuf, sizeof(iovbuf));
	ret = dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0);

	if (ret == BCME_OK) {
		/*
		Leaving the message for now, it should be removed after a while; once
		the tlv situation is stable.
		*/
		DHD_ERROR(("%s():%d successfully %s bdcv2 tlv signaling, %d\n",
			__FUNCTION__, __LINE__,
			dhd->wlfc_enabled?"enabled":"disabled", tlv));
	} else
		DHD_ERROR(("%s():%d failed to enable/disable bdcv2 tlv signaling Err = %d\n",
			__FUNCTION__, __LINE__, ret));

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhd);
		return WLFC_UNSUPPORTED;
	}

	wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;

#ifdef PROP_TXSTATUS_DEBUG
	if (!WLFC_GET_AFQ(dhd->wlfc_mode))
	{
		int i;
		wlfc_hanger_t* h = (wlfc_hanger_t*)wlfc->hanger;
		for (i = 0; i < h->max_items; i++) {
			if (h->items[i].state != WLFC_HANGER_ITEM_STATE_FREE) {
				WLFC_DBGMESG(("%s() pkt[%d] = 0x%p, FIFO_credit_used:%d\n",
					__FUNCTION__, i, h->items[i].pkt,
					DHD_PKTTAG_CREDITCHECK(PKTTAG(h->items[i].pkt))));
			}
		}
	}
#endif

	_dhd_wlfc_cleanup(dhd, NULL, NULL);

	if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
		/* delete hanger */
		_dhd_wlfc_hanger_delete(dhd, wlfc->hanger);
	}


	/* free top structure */
	DHD_OS_PREFREE(dhd, DHD_PREALLOC_DHD_WLFC_INFO,
		dhd->wlfc_state, sizeof(athost_wl_status_info_t));
	dhd->wlfc_state = NULL;
	dhd->proptxstatus_mode = hostreorder ?
		WLFC_ONLY_AMPDU_HOSTREORDER : WLFC_FCMODE_NONE;

	dhd_os_wlfc_unblock(dhd);

	if (dhd->plat_deinit)
		dhd->plat_deinit((void *)dhd);
	return BCME_OK;
}

int dhd_wlfc_interface_event(dhd_pub_t *dhdp, uint8 action, uint8 ifid, uint8 iftype, uint8* ea)
{
	int rc;

	if (dhdp == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhdp);

	if (!dhdp->wlfc_state || (dhdp->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhdp);
		return WLFC_UNSUPPORTED;
	}

	rc = _dhd_wlfc_interface_entry_update(dhdp->wlfc_state, action, ifid, iftype, ea);

	dhd_os_wlfc_unblock(dhdp);
	return rc;
}

int dhd_wlfc_FIFOcreditmap_event(dhd_pub_t *dhdp, uint8* event_data)
{
	int rc;

	if (dhdp == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhdp);

	if (!dhdp->wlfc_state || (dhdp->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhdp);
		return WLFC_UNSUPPORTED;
	}

	rc = _dhd_wlfc_FIFOcreditmap_update(dhdp->wlfc_state, event_data);

	dhd_os_wlfc_unblock(dhdp);

	return rc;
}

int dhd_wlfc_BCMCCredit_support_event(dhd_pub_t *dhdp)
{
	int rc;

	if (dhdp == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhdp);

	if (!dhdp->wlfc_state || (dhdp->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhdp);
		return WLFC_UNSUPPORTED;
	}

	rc = _dhd_wlfc_BCMCCredit_support_update(dhdp->wlfc_state);

	dhd_os_wlfc_unblock(dhdp);
	return rc;
}

int
dhd_wlfc_dump(dhd_pub_t *dhdp, struct bcmstrbuf *strbuf)
{
	int i;
	uint8* ea;
	athost_wl_status_info_t* wlfc;
	wlfc_hanger_t* h;
	wlfc_mac_descriptor_t* mac_table;
	wlfc_mac_descriptor_t* interfaces;
	char* iftypes[] = {"STA", "AP", "WDS", "p2pGO", "p2pCL"};

	if (!dhdp || !strbuf) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhdp);

	if (!dhdp->wlfc_state || (dhdp->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhdp);
		return WLFC_UNSUPPORTED;
	}

	wlfc = (athost_wl_status_info_t*)dhdp->wlfc_state;

	h = (wlfc_hanger_t*)wlfc->hanger;
	if (h == NULL) {
		bcm_bprintf(strbuf, "wlfc-hanger not initialized yet\n");
	}

	mac_table = wlfc->destination_entries.nodes;
	interfaces = wlfc->destination_entries.interfaces;
	bcm_bprintf(strbuf, "---- wlfc stats ----\n");

	if (!WLFC_GET_AFQ(dhdp->wlfc_mode)) {
		h = (wlfc_hanger_t*)wlfc->hanger;
		if (h == NULL) {
			bcm_bprintf(strbuf, "wlfc-hanger not initialized yet\n");
		} else {
			bcm_bprintf(strbuf, "wlfc hanger (pushed,popped,f_push,"
				"f_pop,f_slot, pending) = (%d,%d,%d,%d,%d,%d)\n",
				h->pushed,
				h->popped,
				h->failed_to_push,
				h->failed_to_pop,
				h->failed_slotfind,
				(h->pushed - h->popped));
		}
	}

	bcm_bprintf(strbuf, "wlfc fail(tlv,credit_rqst,mac_update,psmode_update), "
		"(dq_full,rollback_fail) = (%d,%d,%d,%d), (%d,%d)\n",
		wlfc->stats.tlv_parse_failed,
		wlfc->stats.credit_request_failed,
		wlfc->stats.mac_update_failed,
		wlfc->stats.psmode_update_failed,
		wlfc->stats.delayq_full_error,
		wlfc->stats.rollback_failed);

	bcm_bprintf(strbuf, "PKTS (init_credit,credit,sent,drop_d,drop_s,outoforder) "
		"(AC0[%d,%d,%d,%d,%d,%d],AC1[%d,%d,%d,%d,%d,%d],AC2[%d,%d,%d,%d,%d,%d],"
		"AC3[%d,%d,%d,%d,%d,%d],BC_MC[%d,%d,%d,%d,%d,%d])\n",
		wlfc->Init_FIFO_credit[0], wlfc->FIFO_credit[0], wlfc->stats.send_pkts[0],
		wlfc->stats.drop_pkts[0], wlfc->stats.drop_pkts[1], wlfc->stats.ooo_pkts[0],
		wlfc->Init_FIFO_credit[1], wlfc->FIFO_credit[1], wlfc->stats.send_pkts[1],
		wlfc->stats.drop_pkts[2], wlfc->stats.drop_pkts[3], wlfc->stats.ooo_pkts[1],
		wlfc->Init_FIFO_credit[2], wlfc->FIFO_credit[2], wlfc->stats.send_pkts[2],
		wlfc->stats.drop_pkts[4], wlfc->stats.drop_pkts[5], wlfc->stats.ooo_pkts[2],
		wlfc->Init_FIFO_credit[3], wlfc->FIFO_credit[3], wlfc->stats.send_pkts[3],
		wlfc->stats.drop_pkts[6], wlfc->stats.drop_pkts[7], wlfc->stats.ooo_pkts[3],
		wlfc->Init_FIFO_credit[4], wlfc->FIFO_credit[4], wlfc->stats.send_pkts[4],
		wlfc->stats.drop_pkts[8], wlfc->stats.drop_pkts[9], wlfc->stats.ooo_pkts[4]);

	bcm_bprintf(strbuf, "\n");
	for (i = 0; i < WLFC_MAX_IFNUM; i++) {
		if (interfaces[i].occupied) {
			char* iftype_desc;

			if (interfaces[i].iftype > WLC_E_IF_ROLE_P2P_CLIENT)
				iftype_desc = "<Unknown";
			else
				iftype_desc = iftypes[interfaces[i].iftype];

			ea = interfaces[i].ea;
			bcm_bprintf(strbuf, "INTERFACE[%d].ea = "
				"[%02x:%02x:%02x:%02x:%02x:%02x], if:%d, type: %s "
				"netif_flow_control:%s\n", i,
				ea[0], ea[1], ea[2], ea[3], ea[4], ea[5],
				interfaces[i].interface_id,
				iftype_desc, ((wlfc->hostif_flow_state[i] == OFF)
				? " OFF":" ON"));

			bcm_bprintf(strbuf, "INTERFACE[%d].PSQ(len,state,credit),(trans,supp_trans)"
				"= (%d,%s,%d),(%d,%d)\n",
				i,
				interfaces[i].psq.len,
				((interfaces[i].state ==
				WLFC_STATE_OPEN) ? "OPEN":"CLOSE"),
				interfaces[i].requested_credit,
				interfaces[i].transit_count, interfaces[i].suppr_transit_count);

			bcm_bprintf(strbuf, "INTERFACE[%d].PSQ"
				"(delay0,sup0,afq0),(delay1,sup1,afq1),(delay2,sup2,afq2),"
				"(delay3,sup3,afq3),(delay4,sup4,afq4) = (%d,%d,%d),"
				"(%d,%d,%d),(%d,%d,%d),(%d,%d,%d),(%d,%d,%d)\n",
				i,
				interfaces[i].psq.q[0].len,
				interfaces[i].psq.q[1].len,
				interfaces[i].afq.q[0].len,
				interfaces[i].psq.q[2].len,
				interfaces[i].psq.q[3].len,
				interfaces[i].afq.q[1].len,
				interfaces[i].psq.q[4].len,
				interfaces[i].psq.q[5].len,
				interfaces[i].afq.q[2].len,
				interfaces[i].psq.q[6].len,
				interfaces[i].psq.q[7].len,
				interfaces[i].afq.q[3].len,
				interfaces[i].psq.q[8].len,
				interfaces[i].psq.q[9].len,
				interfaces[i].afq.q[4].len);
		}
	}

	bcm_bprintf(strbuf, "\n");
	for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
		if (mac_table[i].occupied) {
			ea = mac_table[i].ea;
			bcm_bprintf(strbuf, "MAC_table[%d].ea = "
				"[%02x:%02x:%02x:%02x:%02x:%02x], if:%d \n", i,
				ea[0], ea[1], ea[2], ea[3], ea[4], ea[5],
				mac_table[i].interface_id);

			bcm_bprintf(strbuf, "MAC_table[%d].PSQ(len,state,credit),(trans,supp_trans)"
				"= (%d,%s,%d),(%d,%d)\n",
				i,
				mac_table[i].psq.len,
				((mac_table[i].state ==
				WLFC_STATE_OPEN) ? " OPEN":"CLOSE"),
				mac_table[i].requested_credit,
				mac_table[i].transit_count, mac_table[i].suppr_transit_count);
#ifdef PROP_TXSTATUS_DEBUG
			bcm_bprintf(strbuf, "MAC_table[%d]: (opened, closed) = (%d, %d)\n",
				i, mac_table[i].opened_ct, mac_table[i].closed_ct);
#endif
			bcm_bprintf(strbuf, "MAC_table[%d].PSQ"
				"(delay0,sup0,afq0),(delay1,sup1,afq1),(delay2,sup2,afq2),"
				"(delay3,sup3,afq3),(delay4,sup4,afq4) =(%d,%d,%d),"
				"(%d,%d,%d),(%d,%d,%d),(%d,%d,%d),(%d,%d,%d)\n",
				i,
				mac_table[i].psq.q[0].len,
				mac_table[i].psq.q[1].len,
				mac_table[i].afq.q[0].len,
				mac_table[i].psq.q[2].len,
				mac_table[i].psq.q[3].len,
				mac_table[i].afq.q[1].len,
				mac_table[i].psq.q[4].len,
				mac_table[i].psq.q[5].len,
				mac_table[i].afq.q[2].len,
				mac_table[i].psq.q[6].len,
				mac_table[i].psq.q[7].len,
				mac_table[i].afq.q[3].len,
				mac_table[i].psq.q[8].len,
				mac_table[i].psq.q[9].len,
				mac_table[i].afq.q[4].len);

		}
	}

#ifdef PROP_TXSTATUS_DEBUG
	{
		int avg;
		int moving_avg = 0;
		int moving_samples;

		if (wlfc->stats.latency_sample_count) {
			moving_samples = sizeof(wlfc->stats.deltas)/sizeof(uint32);

			for (i = 0; i < moving_samples; i++)
				moving_avg += wlfc->stats.deltas[i];
			moving_avg /= moving_samples;

			avg = (100 * wlfc->stats.total_status_latency) /
				wlfc->stats.latency_sample_count;
			bcm_bprintf(strbuf, "txstatus latency (average, last, moving[%d]) = "
				"(%d.%d, %03d, %03d)\n",
				moving_samples, avg/100, (avg - (avg/100)*100),
				wlfc->stats.latency_most_recent,
				moving_avg);
		}
	}

	bcm_bprintf(strbuf, "wlfc- fifo[0-5] credit stats: sent = (%d,%d,%d,%d,%d,%d), "
		"back = (%d,%d,%d,%d,%d,%d)\n",
		wlfc->stats.fifo_credits_sent[0],
		wlfc->stats.fifo_credits_sent[1],
		wlfc->stats.fifo_credits_sent[2],
		wlfc->stats.fifo_credits_sent[3],
		wlfc->stats.fifo_credits_sent[4],
		wlfc->stats.fifo_credits_sent[5],

		wlfc->stats.fifo_credits_back[0],
		wlfc->stats.fifo_credits_back[1],
		wlfc->stats.fifo_credits_back[2],
		wlfc->stats.fifo_credits_back[3],
		wlfc->stats.fifo_credits_back[4],
		wlfc->stats.fifo_credits_back[5]);
	{
		uint32 fifo_cr_sent = 0;
		uint32 fifo_cr_acked = 0;
		uint32 request_cr_sent = 0;
		uint32 request_cr_ack = 0;
		uint32 bc_mc_cr_ack = 0;

		for (i = 0; i < sizeof(wlfc->stats.fifo_credits_sent)/sizeof(uint32); i++) {
			fifo_cr_sent += wlfc->stats.fifo_credits_sent[i];
		}

		for (i = 0; i < sizeof(wlfc->stats.fifo_credits_back)/sizeof(uint32); i++) {
			fifo_cr_acked += wlfc->stats.fifo_credits_back[i];
		}

		for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
			if (wlfc->destination_entries.nodes[i].occupied) {
				request_cr_sent +=
					wlfc->destination_entries.nodes[i].dstncredit_sent_packets;
			}
		}
		for (i = 0; i < WLFC_MAX_IFNUM; i++) {
			if (wlfc->destination_entries.interfaces[i].occupied) {
				request_cr_sent +=
				wlfc->destination_entries.interfaces[i].dstncredit_sent_packets;
			}
		}
		for (i = 0; i < WLFC_MAC_DESC_TABLE_SIZE; i++) {
			if (wlfc->destination_entries.nodes[i].occupied) {
				request_cr_ack +=
					wlfc->destination_entries.nodes[i].dstncredit_acks;
			}
		}
		for (i = 0; i < WLFC_MAX_IFNUM; i++) {
			if (wlfc->destination_entries.interfaces[i].occupied) {
				request_cr_ack +=
					wlfc->destination_entries.interfaces[i].dstncredit_acks;
			}
		}
		bcm_bprintf(strbuf, "wlfc- (sent, status) => pq(%d,%d), vq(%d,%d),"
			"other:%d, bc_mc:%d, signal-only, (sent,freed): (%d,%d)",
			fifo_cr_sent, fifo_cr_acked,
			request_cr_sent, request_cr_ack,
			wlfc->destination_entries.other.dstncredit_acks,
			bc_mc_cr_ack,
			wlfc->stats.signal_only_pkts_sent, wlfc->stats.signal_only_pkts_freed);
	}
#endif /* PROP_TXSTATUS_DEBUG */
	bcm_bprintf(strbuf, "\n");
	bcm_bprintf(strbuf, "wlfc- pkt((in,2bus,txstats,hdrpull,out),(dropped,hdr_only,wlc_tossed)"
		"(freed,free_err,rollback)) = "
		"((%d,%d,%d,%d,%d),(%d,%d,%d),(%d,%d,%d))\n",
		wlfc->stats.pktin,
		wlfc->stats.pkt2bus,
		wlfc->stats.txstatus_in,
		wlfc->stats.dhd_hdrpulls,
		wlfc->stats.pktout,

		wlfc->stats.pktdropped,
		wlfc->stats.wlfc_header_only_pkt,
		wlfc->stats.wlc_tossed_pkts,

		wlfc->stats.pkt_freed,
		wlfc->stats.pkt_free_err, wlfc->stats.rollback);

	bcm_bprintf(strbuf, "wlfc- suppress((d11,wlc,err),enq(d11,wl,hq,mac?),retx(d11,wlc,hq)) = "
		"((%d,%d,%d),(%d,%d,%d,%d),(%d,%d,%d))\n",
		wlfc->stats.d11_suppress,
		wlfc->stats.wl_suppress,
		wlfc->stats.bad_suppress,

		wlfc->stats.psq_d11sup_enq,
		wlfc->stats.psq_wlsup_enq,
		wlfc->stats.psq_hostq_enq,
		wlfc->stats.mac_handle_notfound,

		wlfc->stats.psq_d11sup_retx,
		wlfc->stats.psq_wlsup_retx,
		wlfc->stats.psq_hostq_retx);

	bcm_bprintf(strbuf, "wlfc- cleanup(txq,psq,fw) = (%d,%d,%d)\n",
		wlfc->stats.cleanup_txq_cnt,
		wlfc->stats.cleanup_psq_cnt,
		wlfc->stats.cleanup_fw_cnt);

	bcm_bprintf(strbuf, "wlfc- generic error: %d\n", wlfc->stats.generic_error);

	for (i = 0; i < WLFC_MAX_IFNUM; i++) {
		bcm_bprintf(strbuf, "wlfc- if[%d], pkt_cnt_in_q/AC[0-4] = (%d,%d,%d,%d,%d)\n", i,
			wlfc->pkt_cnt_in_q[i][0],
			wlfc->pkt_cnt_in_q[i][1],
			wlfc->pkt_cnt_in_q[i][2],
			wlfc->pkt_cnt_in_q[i][3],
			wlfc->pkt_cnt_in_q[i][4]);
	}
	bcm_bprintf(strbuf, "\n");

	dhd_os_wlfc_unblock(dhdp);
	return BCME_OK;
}

int dhd_wlfc_clear_counts(dhd_pub_t *dhd)
{
	athost_wl_status_info_t* wlfc;
	wlfc_hanger_t* hanger;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhd);
		return WLFC_UNSUPPORTED;
	}

	wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;

	memset(&wlfc->stats, 0, sizeof(athost_wl_stat_counters_t));

	if (!WLFC_GET_AFQ(dhd->wlfc_mode)) {
		hanger = (wlfc_hanger_t*)wlfc->hanger;

		hanger->pushed = 0;
		hanger->popped = 0;
		hanger->failed_slotfind = 0;
		hanger->failed_to_pop = 0;
		hanger->failed_to_push = 0;
	}

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_get_enable(dhd_pub_t *dhd, bool *val)
{
	if (!dhd || !val) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	*val = dhd->wlfc_enabled;

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_get_mode(dhd_pub_t *dhd, int *val)
{
	if (!dhd || !val) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	*val = dhd->wlfc_state ? dhd->proptxstatus_mode : 0;

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_set_mode(dhd_pub_t *dhd, int val)
{
	if (!dhd) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if (dhd->wlfc_state) {
		dhd->proptxstatus_mode = val & 0xff;
	}

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

bool dhd_wlfc_is_header_only_pkt(dhd_pub_t * dhd, void *pktbuf)
{
	athost_wl_status_info_t* wlfc;
	bool rc = FALSE;

	if (dhd == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return FALSE;
	}

	dhd_os_wlfc_block(dhd);

	if (!dhd->wlfc_state || (dhd->proptxstatus_mode == WLFC_FCMODE_NONE)) {
		dhd_os_wlfc_unblock(dhd);
		return FALSE;
	}

	wlfc = (athost_wl_status_info_t*)dhd->wlfc_state;

	if (PKTLEN(wlfc->osh, pktbuf) == 0) {
		wlfc->stats.wlfc_header_only_pkt++;
		rc = TRUE;
	}

	dhd_os_wlfc_unblock(dhd);

	return rc;
}

int dhd_wlfc_flowcontrol(dhd_pub_t *dhdp, bool state, bool bAcquireLock)
{
	if (dhdp == NULL) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	if (bAcquireLock) {
		dhd_os_wlfc_block(dhdp);
	}

	if (!dhdp->wlfc_state || (dhdp->proptxstatus_mode == WLFC_FCMODE_NONE) ||
		dhdp->proptxstatus_module_ignore) {
		if (bAcquireLock) {
			dhd_os_wlfc_unblock(dhdp);
		}
		return WLFC_UNSUPPORTED;
	}

	if (state != dhdp->proptxstatus_txoff) {
		dhdp->proptxstatus_txoff = state;
	}

	if (bAcquireLock) {
		dhd_os_wlfc_unblock(dhdp);
	}

	return BCME_OK;
}

int dhd_wlfc_get_module_ignore(dhd_pub_t *dhd, int *val)
{
	if (!dhd || !val) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	*val = dhd->proptxstatus_module_ignore;

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_set_module_ignore(dhd_pub_t *dhd, int val)
{
	char iovbuf[14]; /* Room for "tlv" + '\0' + parameter */
	uint32 tlv = 0;
	bool bChanged = FALSE;

	if (!dhd) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	if ((bool)val != dhd->proptxstatus_module_ignore) {
		dhd->proptxstatus_module_ignore = (val != 0);
		/* force txstatus_ignore sync with proptxstatus_module_ignore */
		dhd->proptxstatus_txstatus_ignore = dhd->proptxstatus_module_ignore;
		if (FALSE == dhd->proptxstatus_module_ignore) {
			tlv = WLFC_FLAGS_RSSI_SIGNALS |
				WLFC_FLAGS_XONXOFF_SIGNALS |
				WLFC_FLAGS_CREDIT_STATUS_SIGNALS |
				WLFC_FLAGS_HOST_PROPTXSTATUS_ACTIVE;
		}
		/* always enable host reorder */
		tlv |= WLFC_FLAGS_HOST_RXRERODER_ACTIVE;
		bChanged = TRUE;
	}

	dhd_os_wlfc_unblock(dhd);

	if (bChanged) {
		/* select enable proptxtstatus signaling */
		bcm_mkiovar("tlv", (char *)&tlv, 4, iovbuf, sizeof(iovbuf));
		if (dhd_wl_ioctl_cmd(dhd, WLC_SET_VAR, iovbuf, sizeof(iovbuf), TRUE, 0) < 0) {
			DHD_ERROR(("%s: failed to set bdcv2 tlv signaling to 0x%x\n",
				__FUNCTION__, tlv));
		}
		else {
			DHD_ERROR(("%s: successfully set bdcv2 tlv signaling to 0x%x\n",
				__FUNCTION__, tlv));
		}
	}
	return BCME_OK;
}

int dhd_wlfc_get_credit_ignore(dhd_pub_t *dhd, int *val)
{
	if (!dhd || !val) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	*val = dhd->proptxstatus_credit_ignore;

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_set_credit_ignore(dhd_pub_t *dhd, int val)
{
	if (!dhd) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	dhd->proptxstatus_credit_ignore = (val != 0);

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_get_txstatus_ignore(dhd_pub_t *dhd, int *val)
{
	if (!dhd || !val) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	*val = dhd->proptxstatus_txstatus_ignore;

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_set_txstatus_ignore(dhd_pub_t *dhd, int val)
{
	if (!dhd) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	dhd->proptxstatus_txstatus_ignore = (val != 0);

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_get_rxpkt_chk(dhd_pub_t *dhd, int *val)
{
	if (!dhd || !val) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	*val = dhd->wlfc_rxpkt_chk;

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}

int dhd_wlfc_set_rxpkt_chk(dhd_pub_t *dhd, int val)
{
	if (!dhd) {
		DHD_ERROR(("Error: %s():%d\n", __FUNCTION__, __LINE__));
		return BCME_BADARG;
	}

	dhd_os_wlfc_block(dhd);

	dhd->wlfc_rxpkt_chk = (val != 0);

	dhd_os_wlfc_unblock(dhd);

	return BCME_OK;
}
#endif /* PROP_TXSTATUS */
