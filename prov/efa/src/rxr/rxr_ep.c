/*
 * Copyright (c) 2019-2020 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "ofi.h"
#include <ofi_util.h>
#include <ofi_iov.h>
#include <ofi_shm.h>
#include "rxr.h"
#include "efa.h"
#include "rxr_msg.h"
#include "rxr_rma.h"
#include "rxr_pkt_cmd.h"
#include "rxr_read.h"
#include "rxr_atomic.h"

struct efa_ep_addr *rxr_ep_raw_addr(struct rxr_ep *ep)
{
	return (struct efa_ep_addr *)ep->core_addr;
}

const char *rxr_ep_raw_addr_str(struct rxr_ep *ep, char *buf, size_t *buflen)
{
	return ofi_straddr(buf, buflen, FI_ADDR_EFA, rxr_ep_raw_addr(ep));
}

struct efa_ep_addr *rxr_peer_raw_addr(struct rxr_ep *ep, fi_addr_t addr)
{
	struct efa_ep *efa_ep;
	struct efa_av *efa_av;
	struct efa_conn *efa_conn;

	efa_ep = container_of(ep->rdm_ep, struct efa_ep, util_ep.ep_fid);
	efa_av = efa_ep->av;
	efa_conn = efa_av_addr_to_conn(efa_av, addr);
	return efa_conn ? efa_conn->ep_addr : NULL;
}

const char *rxr_peer_raw_addr_str(struct rxr_ep *ep, fi_addr_t addr, char *buf, size_t *buflen)
{
	return ofi_straddr(buf, buflen, FI_ADDR_EFA, rxr_peer_raw_addr(ep, addr));
}

/**
 * @brief allocate an rx entry for an operation
 *
 * @param ep[in]	end point
 * @param addr[in]	fi address of the sender/requester.
 * @param op[in]	operation type (ofi_op_msg/ofi_op_tagged/ofi_op_read/ofi_op_write/ofi_op_atomic_xxx)
 * @return		if allocation succeeded, return pointer to rx_entry
 * 			if allocation failed, return NULL
 */
struct rxr_rx_entry *rxr_ep_alloc_rx_entry(struct rxr_ep *ep, fi_addr_t addr, uint32_t op)
{
	struct rxr_rx_entry *rx_entry;

	rx_entry = ofi_buf_alloc(ep->rx_entry_pool);
	if (OFI_UNLIKELY(!rx_entry)) {
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "RX entries exhausted\n");
		return NULL;
	}
	memset(rx_entry, 0, sizeof(struct rxr_rx_entry));

	dlist_insert_tail(&rx_entry->ep_entry, &ep->rx_entry_list);
	rx_entry->type = RXR_RX_ENTRY;
	rx_entry->rx_id = ofi_buf_index(rx_entry);
	dlist_init(&rx_entry->queued_pkts);

	rx_entry->state = RXR_RX_INIT;
	rx_entry->addr = addr;
	if (addr != FI_ADDR_UNSPEC) {
		rx_entry->peer = rxr_ep_get_peer(ep, addr);
		assert(rx_entry->peer);
		dlist_insert_tail(&rx_entry->peer_entry, &rx_entry->peer->rx_entry_list);
	} else {
		/*
		 * If msg->addr is not provided, rx_entry->peer will be set
		 * after it is matched with a message.
		 */
		assert(op == ofi_op_msg || op == ofi_op_tagged);
		rx_entry->peer = NULL;
	} 

	rx_entry->op = op;
	switch (op) {
	case ofi_op_tagged:
		rx_entry->cq_entry.flags = (FI_RECV | FI_MSG | FI_TAGGED);
		break;
	case ofi_op_msg:
		rx_entry->cq_entry.flags = (FI_RECV | FI_MSG);
		break;
	case ofi_op_read_rsp:
		rx_entry->cq_entry.flags = (FI_REMOTE_READ | FI_RMA);
		break;
	case ofi_op_write:
		rx_entry->cq_entry.flags = (FI_REMOTE_WRITE | FI_RMA);
		break;
	case ofi_op_atomic:
		rx_entry->cq_entry.flags = (FI_REMOTE_WRITE | FI_ATOMIC);
		break;
	case ofi_op_atomic_fetch:
	case ofi_op_atomic_compare:
		rx_entry->cq_entry.flags = (FI_REMOTE_READ | FI_ATOMIC);
		break;
	default:
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Unknown operation while %s\n", __func__);
		assert(0 && "Unknown operation");
	}

	return rx_entry;
}

/**
 * @brief post user provided receiving buffer to the device.
 *
 * The user receive buffer was converted to an RX packet, then posted to the device.
 *
 * @param[in]	ep		endpint
 * @param[in]	rx_entry	rx_entry that contain user buffer information
 * @param[in]	flags		user supplied flags passed to fi_recv
 */
int rxr_ep_post_user_recv_buf(struct rxr_ep *ep, struct rxr_rx_entry *rx_entry, uint64_t flags)
{
	struct rxr_pkt_entry *pkt_entry;
	struct efa_mr *mr;
	struct iovec msg_iov;
	struct fi_msg msg = {0};
	int err;

	assert(rx_entry->iov_count == 1);
	assert(rx_entry->iov[0].iov_len >= ep->msg_prefix_size);
	pkt_entry = (struct rxr_pkt_entry *)rx_entry->iov[0].iov_base;
	assert(pkt_entry);

	/*
	 * The ownership of the prefix buffer lies with the application, do not
	 * put it on the dbg list for cleanup during shutdown or poison it. The
	 * provider loses jurisdiction over it soon after writing the rx
	 * completion.
	 */
	dlist_init(&pkt_entry->entry);
	mr = (struct efa_mr *)rx_entry->desc[0];
	pkt_entry->mr = &mr->mr_fid;
	pkt_entry->alloc_type = RXR_PKT_FROM_USER_BUFFER;
	pkt_entry->flags = RXR_PKT_ENTRY_IN_USE;
	pkt_entry->next = NULL;
	/*
	 * The actual receiving buffer size (pkt_size) is
	 *    rx_entry->total_len - sizeof(struct rxr_pkt_entry)
	 * because the first part of user buffer was used to
	 * construct pkt_entry. The actual receiving buffer
	 * posted to device starts from pkt_entry->pkt.
	 */
	pkt_entry->pkt_size = rx_entry->iov[0].iov_len - sizeof(struct rxr_pkt_entry);

	pkt_entry->x_entry = rx_entry;
	rx_entry->state = RXR_RX_MATCHED;

	msg_iov.iov_base = pkt_entry->pkt;
	msg_iov.iov_len = pkt_entry->pkt_size;
	assert(msg_iov.iov_len <= ep->mtu_size);

	msg.iov_count = 1;
	msg.msg_iov = &msg_iov;
	msg.desc = rx_entry->desc;
	msg.addr = FI_ADDR_UNSPEC;
	msg.context = pkt_entry;
	msg.data = 0;

	err = fi_recvmsg(ep->rdm_ep, &msg, flags);
	if (OFI_UNLIKELY(err)) {
		rxr_pkt_entry_release_rx(ep, pkt_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"failed to post user supplied buffer %d (%s)\n", -err,
			fi_strerror(-err));
		return err;
	}

	ep->efa_rx_pkts_posted++;
	return 0;
}

/**
 * @brief post an internal receive buffer to lower endpoint
 *
 * The buffer was posted as undirected recv, (address was set to FI_ADDR_UNSPEC)
 *
 * @param[in]	ep		endpoint
 * @param[in]	flags		flags passed to lower provider, can have FI_MORE
 * @param[in]	lower_ep_type	lower endpoint type, can be either SHM_EP or EFA_EP
 * @return	On success, return 0
 * 		On failure, return a negative error code.
 */
int rxr_ep_post_internal_rx_pkt(struct rxr_ep *ep, uint64_t flags, enum rxr_lower_ep_type lower_ep_type)
{
	struct fi_msg msg = {0};
	struct iovec msg_iov;
	void *desc;
	struct rxr_pkt_entry *rx_pkt_entry = NULL;
	int ret = 0;

	switch (lower_ep_type) {
	case SHM_EP:
		rx_pkt_entry = rxr_pkt_entry_alloc(ep, ep->shm_rx_pkt_pool, RXR_PKT_FROM_SHM_RX_POOL);
		break;
	case EFA_EP:
		rx_pkt_entry = rxr_pkt_entry_alloc(ep, ep->efa_rx_pkt_pool, RXR_PKT_FROM_EFA_RX_POOL);
		break;
	default:
		/* Coverity will complain about this being a dead code segment,
		 * but it is useful for future proofing.
		 */
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"invalid lower EP type %d\n", lower_ep_type);
		assert(0 && "invalid lower EP type\n");
	}
	if (OFI_UNLIKELY(!rx_pkt_entry)) {
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Unable to allocate rx_pkt_entry\n");
		return -FI_ENOMEM;
	}

	rx_pkt_entry->x_entry = NULL;

	msg_iov.iov_base = (void *)rxr_pkt_start(rx_pkt_entry);
	msg_iov.iov_len = ep->mtu_size;
	rxr_setup_msg(&msg, &msg_iov, NULL, 1, FI_ADDR_UNSPEC, rx_pkt_entry, 0);

	switch (lower_ep_type) {
	case SHM_EP:
		/* pre-post buffer with shm */
#if ENABLE_DEBUG
		dlist_insert_tail(&rx_pkt_entry->dbg_entry,
				  &ep->rx_posted_buf_shm_list);
#endif
		desc = NULL;
		msg.desc = &desc;
		ret = fi_recvmsg(ep->shm_ep, &msg, flags);
		if (OFI_UNLIKELY(ret)) {
			rxr_pkt_entry_release_rx(ep, rx_pkt_entry);
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"failed to post buf for shm  %d (%s)\n", -ret,
				fi_strerror(-ret));
			return ret;
		}
		ep->shm_rx_pkts_posted++;
		break;
	case EFA_EP:
#if ENABLE_DEBUG
		dlist_insert_tail(&rx_pkt_entry->dbg_entry,
				  &ep->rx_posted_buf_list);
#endif
		desc = fi_mr_desc(rx_pkt_entry->mr);
		msg.desc = &desc;
		ret = fi_recvmsg(ep->rdm_ep, &msg, flags);
		if (OFI_UNLIKELY(ret)) {
			rxr_pkt_entry_release_rx(ep, rx_pkt_entry);
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"failed to post buf %d (%s)\n", -ret,
				fi_strerror(-ret));
			return ret;
		}
		ep->efa_rx_pkts_posted++;
		break;
	default:
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"invalid lower EP type %d\n", lower_ep_type);
		assert(0 && "invalid lower EP type\n");
	}

	return 0;
}

/**
 * @brief bulk post internal receive buffer(s) to device
 *
 * When posting multiple buffers, this function will use
 * FI_MORE flag to achieve better performance.
 *
 * @param[in]	ep		endpint
 * @param[in]	nrecv		number of receive buffers to post
 * @param[in]	lower_ep_type	device type, can be SHM_EP or EFA_EP
 * @return	On success, return 0
 * 		On failure, return negative libfabric error code
 */
static inline
ssize_t rxr_ep_bulk_post_internal_rx_pkts(struct rxr_ep *ep, int nrecv,
					  enum rxr_lower_ep_type lower_ep_type)
{
	int i;
	ssize_t err;
	uint64_t flags;

	flags = FI_MORE;
	for (i = 0; i < nrecv; ++i) {
		if (i == nrecv - 1)
			flags = 0;

		err = rxr_ep_post_internal_rx_pkt(ep, flags, lower_ep_type);
		if (OFI_UNLIKELY(err))
			return err;
	}

	return 0;
}

void rxr_tx_entry_init(struct rxr_ep *ep, struct rxr_tx_entry *tx_entry,
		       const struct fi_msg *msg, uint32_t op, uint64_t flags)
{
	uint64_t tx_op_flags;

	tx_entry->type = RXR_TX_ENTRY;
	tx_entry->op = op;
	tx_entry->tx_id = ofi_buf_index(tx_entry);
	tx_entry->state = RXR_TX_REQ;
	tx_entry->addr = msg->addr;
	tx_entry->peer = rxr_ep_get_peer(ep, tx_entry->addr);
	assert(tx_entry->peer);
	dlist_insert_tail(&tx_entry->peer_entry, &tx_entry->peer->tx_entry_list);

	tx_entry->send_flags = 0;
	tx_entry->rxr_flags = 0;
	tx_entry->bytes_acked = 0;
	tx_entry->bytes_sent = 0;
	tx_entry->window = 0;
	tx_entry->iov_count = msg->iov_count;
	tx_entry->iov_index = 0;
	tx_entry->iov_mr_start = 0;
	tx_entry->iov_offset = 0;
	tx_entry->msg_id = 0;
	dlist_init(&tx_entry->queued_pkts);

	memcpy(&tx_entry->iov[0], msg->msg_iov, sizeof(struct iovec) * msg->iov_count);
	memset(tx_entry->mr, 0, sizeof(*tx_entry->mr) * msg->iov_count);
	if (msg->desc)
		memcpy(tx_entry->desc, msg->desc, sizeof(*msg->desc) * msg->iov_count);
	else
		memset(tx_entry->desc, 0, sizeof(tx_entry->desc));

	if (ep->msg_prefix_size > 0) {
		assert(tx_entry->iov[0].iov_len >= ep->msg_prefix_size);
		tx_entry->iov[0].iov_base = (char *)tx_entry->iov[0].iov_base + ep->msg_prefix_size;
		tx_entry->iov[0].iov_len -= ep->msg_prefix_size;
	}

	tx_entry->total_len = ofi_total_iov_len(tx_entry->iov, tx_entry->iov_count);

	/* set flags */
	assert(ep->util_ep.tx_msg_flags == 0 ||
	       ep->util_ep.tx_msg_flags == FI_COMPLETION);
	tx_op_flags = ep->util_ep.tx_op_flags;
	if (ep->util_ep.tx_msg_flags == 0)
		tx_op_flags &= ~FI_COMPLETION;
	tx_entry->fi_flags = flags | tx_op_flags;

	/* cq_entry on completion */
	tx_entry->cq_entry.op_context = msg->context;
	tx_entry->cq_entry.len = ofi_total_iov_len(msg->msg_iov, msg->iov_count);
	if (OFI_LIKELY(tx_entry->cq_entry.len > 0))
		tx_entry->cq_entry.buf = msg->msg_iov[0].iov_base;
	else
		tx_entry->cq_entry.buf = NULL;

	tx_entry->cq_entry.data = msg->data;
	switch (op) {
	case ofi_op_tagged:
		tx_entry->cq_entry.flags = FI_TRANSMIT | FI_MSG | FI_TAGGED;
		break;
	case ofi_op_write:
		tx_entry->cq_entry.flags = FI_RMA | FI_WRITE;
		break;
	case ofi_op_read_req:
		tx_entry->cq_entry.flags = FI_RMA | FI_READ;
		break;
	case ofi_op_msg:
		tx_entry->cq_entry.flags = FI_TRANSMIT | FI_MSG;
		break;
	case ofi_op_atomic:
		tx_entry->cq_entry.flags = (FI_WRITE | FI_ATOMIC);
		break;
	case ofi_op_atomic_fetch:
	case ofi_op_atomic_compare:
		tx_entry->cq_entry.flags = (FI_READ | FI_ATOMIC);
		break;
	default:
		FI_WARN(&rxr_prov, FI_LOG_CQ, "invalid operation type\n");
		assert(0);
	}
}

/* create a new tx entry */
struct rxr_tx_entry *rxr_ep_alloc_tx_entry(struct rxr_ep *rxr_ep,
					   const struct fi_msg *msg,
					   uint32_t op,
					   uint64_t tag,
					   uint64_t flags)
{
	struct rxr_tx_entry *tx_entry;

	tx_entry = ofi_buf_alloc(rxr_ep->tx_entry_pool);
	if (OFI_UNLIKELY(!tx_entry)) {
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "TX entries exhausted.\n");
		return NULL;
	}

	rxr_tx_entry_init(rxr_ep, tx_entry, msg, op, flags);
	if (op == ofi_op_tagged) {
		tx_entry->cq_entry.tag = tag;
		tx_entry->tag = tag;
	}

	dlist_insert_tail(&tx_entry->ep_entry, &rxr_ep->tx_entry_list);
	return tx_entry;
}

void rxr_release_tx_entry(struct rxr_ep *ep, struct rxr_tx_entry *tx_entry)
{
	int i, err = 0;
	struct dlist_entry *tmp;
	struct rxr_pkt_entry *pkt_entry;

	assert(tx_entry->peer);
	dlist_remove(&tx_entry->peer_entry);

	for (i = 0; i < tx_entry->iov_count; i++) {
		if (tx_entry->mr[i]) {
			err = fi_close((struct fid *)tx_entry->mr[i]);
			if (OFI_UNLIKELY(err)) {
				FI_WARN(&rxr_prov, FI_LOG_CQ, "mr dereg failed. err=%d\n", err);
				efa_eq_write_error(&ep->util_ep, err, -err);
			}

			tx_entry->mr[i] = NULL;
		}
	}

	dlist_remove(&tx_entry->ep_entry);

	dlist_foreach_container_safe(&tx_entry->queued_pkts,
				     struct rxr_pkt_entry,
				     pkt_entry, entry, tmp) {
		rxr_pkt_entry_release_tx(ep, pkt_entry);
	}

	if (tx_entry->rxr_flags & RXR_TX_ENTRY_QUEUED_RNR)
		dlist_remove(&tx_entry->queued_rnr_entry);

	if (tx_entry->state == RXR_TX_QUEUED_CTRL)
		dlist_remove(&tx_entry->queued_ctrl_entry);

#ifdef ENABLE_EFA_POISONING
	rxr_poison_mem_region((uint32_t *)tx_entry,
			      sizeof(struct rxr_tx_entry));
#endif
	tx_entry->state = RXR_TX_FREE;
	ofi_buf_free(tx_entry);
}

int rxr_ep_tx_init_mr_desc(struct rxr_domain *rxr_domain,
			   struct rxr_tx_entry *tx_entry,
			   int mr_iov_start, uint64_t access)
{
	int i, err, ret;

	ret = 0;
	for (i = mr_iov_start; i < tx_entry->iov_count; ++i) {
		if (tx_entry->desc[i]) {
			assert(!tx_entry->mr[i]);
			continue;
		}

		if (tx_entry->iov[i].iov_len <= rxr_env.max_memcpy_size) {
			assert(!tx_entry->mr[i]);
			continue;
		}

		err = fi_mr_reg(rxr_domain->rdm_domain,
				tx_entry->iov[i].iov_base,
				tx_entry->iov[i].iov_len,
				access, 0, 0, 0,
				&tx_entry->mr[i], NULL);
		if (err) {
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"fi_mr_reg failed! buf: %p len: %ld access: %lx",
				tx_entry->iov[i].iov_base, tx_entry->iov[i].iov_len,
				access);

			tx_entry->mr[i] = NULL;
			ret = err;
		} else {
			tx_entry->desc[i] = fi_mr_desc(tx_entry->mr[i]);
		}
	}

	return ret;
}

/**
 * @brief convert EFA descriptors to shm descriptors.
 *
 * Each provider defines its descriptors format. The descriptor for
 * EFA provider is of struct efa_mr *, which shm provider cannot
 * understand. This function convert EFA descriptors to descriptors
 * shm can use.
 *
 * @param numdesc[in]       number of descriptors in the array
 * @param desc[in,out]      descriptors input is EFA descriptor, output
 *                          is shm descriptor.
 */
void rxr_convert_desc_for_shm(int numdesc, void **desc)
{
	int i;
	struct efa_mr *efa_mr;

	for (i = 0; i < numdesc; ++i) {
		efa_mr = desc[i];
		if (efa_mr)
			desc[i] = fi_mr_desc(efa_mr->shm_mr);
	}
}

void rxr_prepare_desc_send(struct rxr_domain *rxr_domain,
			   struct rxr_tx_entry *tx_entry)
{
	size_t offset;
	int index;

	/* Set the iov index and iov offset from bytes sent */
	offset = tx_entry->bytes_sent;
	for (index = 0; index < tx_entry->iov_count; ++index) {
		if (offset >= tx_entry->iov[index].iov_len) {
			offset -= tx_entry->iov[index].iov_len;
		} else {
			tx_entry->iov_index = index;
			tx_entry->iov_offset = offset;
			break;
		}
	}

	tx_entry->iov_mr_start = index;
	/* the return value of rxr_ep_tx_init_mr_desc() is not checked
	 * because the long message protocol would work with or without
	 * memory registration and descriptor.
	 */
	rxr_ep_tx_init_mr_desc(rxr_domain, tx_entry, index, FI_SEND);
}

/* Generic send */
int rxr_ep_set_tx_credit_request(struct rxr_ep *rxr_ep, struct rxr_tx_entry *tx_entry)
{
	struct rdm_peer *peer;
	int outstanding;

	peer = rxr_ep_get_peer(rxr_ep, tx_entry->addr);
	assert(peer);

	/*
	 * Divy up available credits to outstanding transfers and request the
	 * minimum of that and the amount required to finish the current long
	 * message.
	 */
	outstanding = peer->efa_outstanding_tx_ops + 1;
	tx_entry->credit_request = MIN(ofi_div_ceil(peer->tx_credits, outstanding),
				       ofi_div_ceil(tx_entry->total_len,
						    rxr_ep->max_data_payload_size));
	tx_entry->credit_request = MAX(tx_entry->credit_request,
				       rxr_env.tx_min_credits);
	if (peer->tx_credits >= tx_entry->credit_request)
		peer->tx_credits -= tx_entry->credit_request;

	/* Queue this REQ for later if there are too many outstanding packets */
	if (!tx_entry->credit_request)
		return -FI_EAGAIN;

	return 0;
}

static void rxr_ep_free_res(struct rxr_ep *rxr_ep)
{
	struct dlist_entry *entry, *tmp;
	struct rxr_rx_entry *rx_entry;
	struct rxr_tx_entry *tx_entry;
#if ENABLE_DEBUG
	struct rxr_pkt_entry *pkt;
#endif

	dlist_foreach_safe(&rxr_ep->rx_unexp_list, entry, tmp) {
		rx_entry = container_of(entry, struct rxr_rx_entry, entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with unmatched unexpected rx_entry: %p pkt_entry %p\n",
			rx_entry, rx_entry->unexp_pkt);
		rxr_pkt_entry_release_rx(rxr_ep, rx_entry->unexp_pkt);
		rxr_release_rx_entry(rxr_ep, rx_entry);
	}

	dlist_foreach_safe(&rxr_ep->rx_unexp_tagged_list, entry, tmp) {
		rx_entry = container_of(entry, struct rxr_rx_entry, entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with unmatched unexpected tagged rx_entry: %p pkt_entry %p\n",
			rx_entry, rx_entry->unexp_pkt);
		rxr_pkt_entry_release_rx(rxr_ep, rx_entry->unexp_pkt);
		rxr_release_rx_entry(rxr_ep, rx_entry);
	}

	dlist_foreach_safe(&rxr_ep->rx_entry_queued_rnr_list, entry, tmp) {
		rx_entry = container_of(entry, struct rxr_rx_entry,
					queued_rnr_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with queued rnr rx_entry: %p\n",
			rx_entry);
		rxr_release_rx_entry(rxr_ep, rx_entry);
	}

	dlist_foreach_safe(&rxr_ep->rx_entry_queued_ctrl_list, entry, tmp) {
		rx_entry = container_of(entry, struct rxr_rx_entry,
					queued_ctrl_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with queued ctrl rx_entry: %p\n",
			rx_entry);
		rxr_release_rx_entry(rxr_ep, rx_entry);
	}

	dlist_foreach_safe(&rxr_ep->tx_entry_queued_rnr_list, entry, tmp) {
		tx_entry = container_of(entry, struct rxr_tx_entry,
					queued_rnr_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with queued rnr tx_entry: %p\n",
			tx_entry);
		rxr_release_tx_entry(rxr_ep, tx_entry);
	}

	dlist_foreach_safe(&rxr_ep->tx_entry_queued_ctrl_list, entry, tmp) {
		tx_entry = container_of(entry, struct rxr_tx_entry,
					queued_ctrl_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with queued ctrl tx_entry: %p\n",
			tx_entry);
		rxr_release_tx_entry(rxr_ep, tx_entry);
	}

#if ENABLE_DEBUG
	dlist_foreach_safe(&rxr_ep->rx_posted_buf_list, entry, tmp) {
		pkt = container_of(entry, struct rxr_pkt_entry, dbg_entry);
		ofi_buf_free(pkt);
	}

	if (rxr_ep->use_shm) {
		dlist_foreach_safe(&rxr_ep->rx_posted_buf_shm_list, entry, tmp) {
			pkt = container_of(entry, struct rxr_pkt_entry, dbg_entry);
			ofi_buf_free(pkt);
		}
	}

	dlist_foreach_safe(&rxr_ep->rx_pkt_list, entry, tmp) {
		pkt = container_of(entry, struct rxr_pkt_entry, dbg_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with unreleased RX pkt_entry: %p\n",
			pkt);
		rxr_pkt_entry_release_rx(rxr_ep, pkt);
	}

	dlist_foreach_safe(&rxr_ep->tx_pkt_list, entry, tmp) {
		pkt = container_of(entry, struct rxr_pkt_entry, dbg_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with unreleased TX pkt_entry: %p\n",
			pkt);
		rxr_pkt_entry_release_tx(rxr_ep, pkt);
	}
#endif

	dlist_foreach_safe(&rxr_ep->rx_entry_list, entry, tmp) {
		rx_entry = container_of(entry, struct rxr_rx_entry,
					ep_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with unreleased rx_entry: %p\n",
			rx_entry);
		rxr_release_rx_entry(rxr_ep, rx_entry);
	}

	dlist_foreach_safe(&rxr_ep->tx_entry_list, entry, tmp) {
		tx_entry = container_of(entry, struct rxr_tx_entry,
					ep_entry);
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Closing ep with unreleased tx_entry: %p\n",
			tx_entry);
		rxr_release_tx_entry(rxr_ep, tx_entry);
	}

	if (rxr_ep->rx_entry_pool)
		ofi_bufpool_destroy(rxr_ep->rx_entry_pool);

	if (rxr_ep->tx_entry_pool)
		ofi_bufpool_destroy(rxr_ep->tx_entry_pool);

	if (rxr_ep->map_entry_pool)
		ofi_bufpool_destroy(rxr_ep->map_entry_pool);

	if (rxr_ep->read_entry_pool)
		ofi_bufpool_destroy(rxr_ep->read_entry_pool);

	if (rxr_ep->readrsp_tx_entry_pool)
		ofi_bufpool_destroy(rxr_ep->readrsp_tx_entry_pool);

	if (rxr_ep->rx_readcopy_pkt_pool) {
		FI_INFO(&rxr_prov, FI_LOG_EP_CTRL, "current usage of read copy packet pool is %d\n",
			rxr_ep->rx_readcopy_pkt_pool_used);
		FI_INFO(&rxr_prov, FI_LOG_EP_CTRL, "maximum usage of read copy packet pool is %d\n",
			rxr_ep->rx_readcopy_pkt_pool_max_used);
		assert(!rxr_ep->rx_readcopy_pkt_pool_used);
		ofi_bufpool_destroy(rxr_ep->rx_readcopy_pkt_pool);
	}

	if (rxr_ep->rx_ooo_pkt_pool)
		ofi_bufpool_destroy(rxr_ep->rx_ooo_pkt_pool);

	if (rxr_ep->rx_unexp_pkt_pool)
		ofi_bufpool_destroy(rxr_ep->rx_unexp_pkt_pool);

	if (rxr_ep->efa_rx_pkt_pool)
		ofi_bufpool_destroy(rxr_ep->efa_rx_pkt_pool);

	if (rxr_ep->efa_tx_pkt_pool)
		ofi_bufpool_destroy(rxr_ep->efa_tx_pkt_pool);

	if (rxr_ep->pkt_sendv_pool)
		ofi_bufpool_destroy(rxr_ep->pkt_sendv_pool);

	if (rxr_ep->use_shm) {
		if (rxr_ep->shm_rx_pkt_pool)
			ofi_bufpool_destroy(rxr_ep->shm_rx_pkt_pool);

		if (rxr_ep->shm_tx_pkt_pool)
			ofi_bufpool_destroy(rxr_ep->shm_tx_pkt_pool);
	}
}

static int rxr_ep_close(struct fid *fid)
{
	int ret, retv = 0;
	struct rxr_ep *rxr_ep;

	rxr_ep = container_of(fid, struct rxr_ep, util_ep.ep_fid.fid);

	ret = fi_close(&rxr_ep->rdm_ep->fid);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close EP\n");
		retv = ret;
	}

	ret = fi_close(&rxr_ep->rdm_cq->fid);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close msg CQ\n");
		retv = ret;
	}

	/* Close shm provider's endpoint and cq */
	if (rxr_ep->use_shm) {
		ret = fi_close(&rxr_ep->shm_ep->fid);
		if (ret) {
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close shm EP\n");
			retv = ret;
		}

		ret = fi_close(&rxr_ep->shm_cq->fid);
		if (ret) {
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close shm CQ\n");
			retv = ret;
		}
	}


	ret = ofi_endpoint_close(&rxr_ep->util_ep);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close util EP\n");
		retv = ret;
	}
	rxr_ep_free_res(rxr_ep);
	free(rxr_ep);
	return retv;
}

static int rxr_ep_bind(struct fid *ep_fid, struct fid *bfid, uint64_t flags)
{
	struct rxr_ep *rxr_ep =
		container_of(ep_fid, struct rxr_ep, util_ep.ep_fid.fid);
	struct util_cq *cq;
	struct efa_av *av;
	struct util_cntr *cntr;
	struct util_eq *eq;
	int ret = 0;

	switch (bfid->fclass) {
	case FI_CLASS_AV:
		av = container_of(bfid, struct efa_av, util_av.av_fid.fid);
		/*
		 * Binding multiple endpoints to a single AV is currently not
		 * supported.
		 */
		if (av->ep) {
			EFA_WARN(FI_LOG_EP_CTRL,
				 "Address vector already has endpoint bound to it.\n");
			return -FI_ENOSYS;
		}

		/* Bind util provider endpoint and av */
		ret = ofi_ep_bind_av(&rxr_ep->util_ep, &av->util_av);
		if (ret)
			return ret;

		ret = fi_ep_bind(rxr_ep->rdm_ep, &av->util_av.av_fid.fid, flags);
		if (ret)
			return ret;

		/* Bind shm provider endpoint & shm av */
		if (rxr_ep->use_shm) {
			ret = fi_ep_bind(rxr_ep->shm_ep, &av->shm_rdm_av->fid, flags);
			if (ret)
				return ret;
		}
		break;
	case FI_CLASS_CQ:
		cq = container_of(bfid, struct util_cq, cq_fid.fid);

		ret = ofi_ep_bind_cq(&rxr_ep->util_ep, cq, flags);
		if (ret)
			return ret;
		break;
	case FI_CLASS_CNTR:
		cntr = container_of(bfid, struct util_cntr, cntr_fid.fid);

		ret = ofi_ep_bind_cntr(&rxr_ep->util_ep, cntr, flags);
		if (ret)
			return ret;
		break;
	case FI_CLASS_EQ:
		eq = container_of(bfid, struct util_eq, eq_fid.fid);

		ret = ofi_ep_bind_eq(&rxr_ep->util_ep, eq);
		if (ret)
			return ret;
		break;
	default:
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "invalid fid class\n");
		ret = -FI_EINVAL;
		break;
	}
	return ret;
}

static
void rxr_ep_set_extra_info(struct rxr_ep *ep)
{
	memset(ep->extra_info, 0, sizeof(ep->extra_info));

	/* RDMA read is an extra feature defined in protocol version 4 (the base version) */
	if (efa_ep_support_rdma_read(ep->rdm_ep))
		ep->extra_info[0] |= RXR_EXTRA_FEATURE_RDMA_READ;

	ep->extra_info[0] |= RXR_EXTRA_FEATURE_DELIVERY_COMPLETE;

	if (ep->use_zcpy_rx) {
		/*
		 * zero copy receive requires the packet header length remains
		 * constant, so the application receive buffer is match with
		 * incoming application data.
		 */
		ep->extra_info[0] |= RXR_EXTRA_REQUEST_CONSTANT_HEADER_LENGTH;
	}
}

static int rxr_ep_ctrl(struct fid *fid, int command, void *arg)
{
	ssize_t ret;
	struct rxr_ep *ep;
	char shm_ep_name[SMR_NAME_MAX];

	switch (command) {
	case FI_ENABLE:
		/* Enable core endpoints & post recv buff */
		ep = container_of(fid, struct rxr_ep, util_ep.ep_fid.fid);

		ret = fi_enable(ep->rdm_ep);
		if (ret)
			return ret;

		fastlock_acquire(&ep->util_ep.lock);

		rxr_ep_set_extra_info(ep);

		ep->core_addrlen = RXR_MAX_NAME_LENGTH;
		ret = fi_getname(&ep->rdm_ep->fid,
				 ep->core_addr,
				 &ep->core_addrlen);
		assert(ret != -FI_ETOOSMALL);
		FI_DBG(&rxr_prov, FI_LOG_EP_CTRL, "core_addrlen = %ld\n",
		       ep->core_addrlen);

		/* Enable shm provider endpoint & post recv buff.
		 * Once core ep enabled, 18 bytes efa_addr (16 bytes raw + 2 bytes qpn) is set.
		 * We convert the address to 'gid_qpn' format, and set it as shm ep name, so
		 * that shm ep can create shared memory region with it when enabling.
		 * In this way, each peer is able to open and map to other local peers'
		 * shared memory region.
		 */
		if (ep->use_shm) {
			ret = rxr_raw_addr_to_smr_name(ep->core_addr, shm_ep_name);
			if (ret < 0)
				goto out;
			fi_setname(&ep->shm_ep->fid, shm_ep_name, sizeof(shm_ep_name));
			ret = fi_enable(ep->shm_ep);
			if (ret)
				goto out;
		}

out:
		fastlock_release(&ep->util_ep.lock);
		break;
	default:
		ret = -FI_ENOSYS;
		break;
	}

	return ret;
}

static struct fi_ops rxr_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = rxr_ep_close,
	.bind = rxr_ep_bind,
	.control = rxr_ep_ctrl,
	.ops_open = fi_no_ops_open,
};

static int rxr_ep_cancel_match_recv(struct dlist_entry *item,
				    const void *context)
{
	struct rxr_rx_entry *rx_entry = container_of(item,
						     struct rxr_rx_entry,
						     entry);
	return rx_entry->cq_entry.op_context == context;
}

static ssize_t rxr_ep_cancel_recv(struct rxr_ep *ep,
				  struct dlist_entry *recv_list,
				  void *context)
{
	struct rxr_domain *domain;
	struct dlist_entry *entry;
	struct rxr_rx_entry *rx_entry;
	struct fi_cq_err_entry err_entry;
	uint32_t api_version;

	fastlock_acquire(&ep->util_ep.lock);
	entry = dlist_remove_first_match(recv_list,
					 &rxr_ep_cancel_match_recv,
					 context);
	if (!entry) {
		fastlock_release(&ep->util_ep.lock);
		return 0;
	}

	rx_entry = container_of(entry, struct rxr_rx_entry, entry);
	rx_entry->rxr_flags |= RXR_RECV_CANCEL;
	if (rx_entry->fi_flags & FI_MULTI_RECV &&
	    rx_entry->rxr_flags & RXR_MULTI_RECV_POSTED) {
		if (dlist_empty(&rx_entry->multi_recv_consumers)) {
			/*
			 * No pending messages for the buffer,
			 * release it back to the app.
			 */
			rx_entry->cq_entry.flags |= FI_MULTI_RECV;
		} else {
			rx_entry = container_of(rx_entry->multi_recv_consumers.next,
						struct rxr_rx_entry,
						multi_recv_entry);
			rxr_msg_multi_recv_handle_completion(ep, rx_entry);
		}
	} else if (rx_entry->fi_flags & FI_MULTI_RECV &&
		   rx_entry->rxr_flags & RXR_MULTI_RECV_CONSUMER) {
		rxr_msg_multi_recv_handle_completion(ep, rx_entry);
	}
	fastlock_release(&ep->util_ep.lock);
	memset(&err_entry, 0, sizeof(err_entry));
	err_entry.op_context = rx_entry->cq_entry.op_context;
	err_entry.flags |= rx_entry->cq_entry.flags;
	err_entry.tag = rx_entry->tag;
	err_entry.err = FI_ECANCELED;
	err_entry.prov_errno = -FI_ECANCELED;

	domain = rxr_ep_domain(ep);
	api_version =
		 domain->util_domain.fabric->fabric_fid.api_version;
	if (FI_VERSION_GE(api_version, FI_VERSION(1, 5)))
		err_entry.err_data_size = 0;
	/*
	 * Other states are currently receiving data. Subsequent messages will
	 * be sunk (via RXR_RECV_CANCEL flag) and the completion suppressed.
	 */
	if (rx_entry->state & (RXR_RX_INIT | RXR_RX_UNEXP | RXR_RX_MATCHED))
		rxr_release_rx_entry(ep, rx_entry);
	return ofi_cq_write_error(ep->util_ep.rx_cq, &err_entry);
}

static ssize_t rxr_ep_cancel(fid_t fid_ep, void *context)
{
	struct rxr_ep *ep;
	int ret;

	ep = container_of(fid_ep, struct rxr_ep, util_ep.ep_fid.fid);

	ret = rxr_ep_cancel_recv(ep, &ep->rx_list, context);
	if (ret)
		return ret;

	ret = rxr_ep_cancel_recv(ep, &ep->rx_tagged_list, context);
	return ret;
}

static int rxr_ep_getopt(fid_t fid, int level, int optname, void *optval,
			 size_t *optlen)
{
	struct rxr_ep *rxr_ep = container_of(fid, struct rxr_ep,
					     util_ep.ep_fid.fid);

	if (level != FI_OPT_ENDPOINT || optname != FI_OPT_MIN_MULTI_RECV)
		return -FI_ENOPROTOOPT;

	*(size_t *)optval = rxr_ep->min_multi_recv_size;
	*optlen = sizeof(size_t);

	return FI_SUCCESS;
}

static int rxr_ep_setopt(fid_t fid, int level, int optname,
			 const void *optval, size_t optlen)
{
	struct rxr_ep *rxr_ep = container_of(fid, struct rxr_ep,
					     util_ep.ep_fid.fid);

	if (level != FI_OPT_ENDPOINT || optname != FI_OPT_MIN_MULTI_RECV)
		return -FI_ENOPROTOOPT;

	if (optlen < sizeof(size_t))
		return -FI_EINVAL;

	rxr_ep->min_multi_recv_size = *(size_t *)optval;

	return FI_SUCCESS;
}

static struct fi_ops_ep rxr_ops_ep = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = rxr_ep_cancel,
	.getopt = rxr_ep_getopt,
	.setopt = rxr_ep_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

static int rxr_buf_region_alloc_hndlr(struct ofi_bufpool_region *region)
{
	size_t ret;
	struct fid_mr *mr;
	struct rxr_domain *domain = region->pool->attr.context;

	ret = fi_mr_reg(domain->rdm_domain, region->alloc_region,
			region->pool->alloc_size,
			FI_SEND | FI_RECV, 0, 0, 0, &mr, NULL);

	region->context = mr;
	return ret;
}

static void rxr_buf_region_free_hndlr(struct ofi_bufpool_region *region)
{
	ssize_t ret;

	ret = fi_close((struct fid *)region->context);
	if (ret)
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Unable to deregister memory in a buf pool: %s\n",
			fi_strerror(-ret));
}

/*
 * rxr_create_pkt_pool create a packet pool. The size of pool is fixed
 * and the memory is registered with device.
 *
 * Important arguments:
 *      size: packet entry size
 *      flags: caller can specify OFI_BUFPOOL_HUGEPAGES so the pool
 *             will be backed by huge pages.
 */
static int rxr_create_pkt_pool(struct rxr_ep *ep, size_t size,
			       size_t chunk_count,
			       size_t flags,
			       struct ofi_bufpool **buf_pool)
{
	struct ofi_bufpool_attr attr = {
		.size		= size,
		.alignment	= RXR_BUF_POOL_ALIGNMENT,
		.max_cnt	= chunk_count,
		.chunk_cnt	= chunk_count,
		.alloc_fn	= rxr_buf_region_alloc_hndlr,
		.free_fn	= rxr_buf_region_free_hndlr,
		.init_fn	= NULL,
		.context	= rxr_ep_domain(ep),
		.flags		= flags,
	};

	return ofi_bufpool_create_attr(&attr, buf_pool);
}

/** @brief Initializes the endpoint.
 *
 * This function allocates the various buffer pools for the EFA and SHM
 * provider and does other endpoint initialization.
 *
 * @param ep rxr_ep struct to initialize.
 * @return 0 on success, fi_errno on error.
 */
int rxr_ep_init(struct rxr_ep *ep)
{
	size_t entry_sz, sendv_pool_size;
	int hp_pool_flag;
	int ret;

	entry_sz = ep->mtu_size + sizeof(struct rxr_pkt_entry);
#ifdef ENABLE_EFA_POISONING
	ep->tx_pkt_pool_entry_sz = entry_sz;
	ep->rx_pkt_pool_entry_sz = entry_sz;
#endif

	if (efa_fork_status == EFA_FORK_SUPPORT_ON)
		hp_pool_flag = 0;
	else
		hp_pool_flag = OFI_BUFPOOL_HUGEPAGES;

	ret = rxr_create_pkt_pool(ep, entry_sz, rxr_get_tx_pool_chunk_cnt(ep),
				  hp_pool_flag,
				  &ep->efa_tx_pkt_pool);
	if (ret)
		goto err_free;

	ret = rxr_create_pkt_pool(ep, entry_sz, rxr_get_rx_pool_chunk_cnt(ep),
				  hp_pool_flag,
				  &ep->efa_rx_pkt_pool);
	if (ret)
		goto err_free;

	if (rxr_env.rx_copy_unexp) {
		ret = ofi_bufpool_create(&ep->rx_unexp_pkt_pool, entry_sz,
					 RXR_BUF_POOL_ALIGNMENT, 0,
					 rxr_env.unexp_pool_chunk_size, 0);

		if (ret)
			goto err_free;
	}

	if (rxr_env.rx_copy_ooo) {
		ret = ofi_bufpool_create(&ep->rx_ooo_pkt_pool, entry_sz,
					 RXR_BUF_POOL_ALIGNMENT, 0,
					 rxr_env.ooo_pool_chunk_size, 0);

		if (ret)
			goto err_free;

	}

	if ((rxr_env.rx_copy_unexp || rxr_env.rx_copy_ooo) &&
	    (rxr_ep_domain(ep)->util_domain.mr_mode & FI_MR_HMEM)) {
		/* this pool is only needed when application requested FI_HMEM
		 * capability
		 */
		ret = rxr_create_pkt_pool(ep, entry_sz,
					  rxr_env.readcopy_pool_size,
					  0, &ep->rx_readcopy_pkt_pool);

		if (ret)
			goto err_free;
		ep->rx_readcopy_pkt_pool_used = 0;
		ep->rx_readcopy_pkt_pool_max_used = 0;
	}

	ret = ofi_bufpool_create(&ep->tx_entry_pool,
				 sizeof(struct rxr_tx_entry),
				 RXR_BUF_POOL_ALIGNMENT,
				 ep->tx_size, ep->tx_size, 0);
	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->read_entry_pool,
				 sizeof(struct rxr_read_entry),
				 RXR_BUF_POOL_ALIGNMENT,
				 ep->tx_size + RXR_MAX_RX_QUEUE_SIZE,
				 ep->tx_size + ep->rx_size, 0);
	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->readrsp_tx_entry_pool,
				 sizeof(struct rxr_tx_entry),
				 RXR_BUF_POOL_ALIGNMENT,
				 RXR_MAX_RX_QUEUE_SIZE,
				 ep->rx_size, 0);
	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->rx_entry_pool,
				 sizeof(struct rxr_rx_entry),
				 RXR_BUF_POOL_ALIGNMENT,
				 RXR_MAX_RX_QUEUE_SIZE,
				 ep->rx_size, 0);
	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->map_entry_pool,
				 sizeof(struct rxr_pkt_rx_map),
				 RXR_BUF_POOL_ALIGNMENT,
				 RXR_MAX_RX_QUEUE_SIZE,
				 ep->rx_size, 0);

	if (ret)
		goto err_free;

	ret = ofi_bufpool_create(&ep->rx_atomrsp_pool,
				 ep->mtu_size,
				 RXR_BUF_POOL_ALIGNMENT,
				 RXR_MAX_RX_QUEUE_SIZE,
				 rxr_env.atomrsp_pool_size, 0);
	if (ret)
		goto err_free;

	sendv_pool_size = rxr_get_tx_pool_chunk_cnt(ep);
	if (ep->use_shm)
		sendv_pool_size += shm_info->tx_attr->size;
	ret = ofi_bufpool_create(&ep->pkt_sendv_pool,
				 sizeof(struct rxr_pkt_sendv),
				 RXR_BUF_POOL_ALIGNMENT,
				 sendv_pool_size,
				 sendv_pool_size, 0);
	if (ret)
		goto err_free;

	/* create pkt pool for shm */
	if (ep->use_shm) {
		ret = ofi_bufpool_create(&ep->shm_tx_pkt_pool,
					 entry_sz,
					 RXR_BUF_POOL_ALIGNMENT,
					 shm_info->tx_attr->size,
					 shm_info->tx_attr->size, 0);
		if (ret)
			goto err_free;

		ret = ofi_bufpool_create(&ep->shm_rx_pkt_pool,
					 entry_sz,
					 RXR_BUF_POOL_ALIGNMENT,
					 shm_info->rx_attr->size,
					 shm_info->rx_attr->size, 0);
		if (ret)
			goto err_free;

		dlist_init(&ep->rx_posted_buf_shm_list);
	}

	/* Initialize entry list */
	dlist_init(&ep->rx_list);
	dlist_init(&ep->rx_unexp_list);
	dlist_init(&ep->rx_tagged_list);
	dlist_init(&ep->rx_unexp_tagged_list);
	dlist_init(&ep->rx_posted_buf_list);
	dlist_init(&ep->rx_entry_queued_rnr_list);
	dlist_init(&ep->rx_entry_queued_ctrl_list);
	dlist_init(&ep->tx_entry_queued_rnr_list);
	dlist_init(&ep->tx_entry_queued_ctrl_list);
	dlist_init(&ep->tx_pending_list);
	dlist_init(&ep->read_pending_list);
	dlist_init(&ep->peer_backoff_list);
	dlist_init(&ep->handshake_queued_peer_list);
#if ENABLE_DEBUG
	dlist_init(&ep->rx_pending_list);
	dlist_init(&ep->rx_pkt_list);
	dlist_init(&ep->tx_pkt_list);
#endif
	dlist_init(&ep->rx_entry_list);
	dlist_init(&ep->tx_entry_list);

	/* Initialize pkt to rx map */
	ep->pkt_rx_map = NULL;
	return 0;

err_free:
	if (ep->shm_tx_pkt_pool)
		ofi_bufpool_destroy(ep->shm_tx_pkt_pool);

	if (ep->pkt_sendv_pool)
		ofi_bufpool_destroy(ep->pkt_sendv_pool);

	if (ep->rx_atomrsp_pool)
		ofi_bufpool_destroy(ep->rx_atomrsp_pool);

	if (ep->map_entry_pool)
		ofi_bufpool_destroy(ep->map_entry_pool);

	if (ep->rx_entry_pool)
		ofi_bufpool_destroy(ep->rx_entry_pool);

	if (ep->readrsp_tx_entry_pool)
		ofi_bufpool_destroy(ep->readrsp_tx_entry_pool);

	if (ep->read_entry_pool)
		ofi_bufpool_destroy(ep->read_entry_pool);

	if (ep->tx_entry_pool)
		ofi_bufpool_destroy(ep->tx_entry_pool);

	if (ep->rx_readcopy_pkt_pool)
		ofi_bufpool_destroy(ep->rx_readcopy_pkt_pool);

	if (rxr_env.rx_copy_ooo && ep->rx_ooo_pkt_pool)
		ofi_bufpool_destroy(ep->rx_ooo_pkt_pool);

	if (rxr_env.rx_copy_unexp && ep->rx_unexp_pkt_pool)
		ofi_bufpool_destroy(ep->rx_unexp_pkt_pool);

	if (ep->efa_rx_pkt_pool)
		ofi_bufpool_destroy(ep->efa_rx_pkt_pool);

	if (ep->efa_tx_pkt_pool)
		ofi_bufpool_destroy(ep->efa_tx_pkt_pool);

	return ret;
}

static int rxr_ep_rdm_setname(fid_t fid, void *addr, size_t addrlen)
{
	struct rxr_ep *ep;

	ep = container_of(fid, struct rxr_ep, util_ep.ep_fid.fid);
	return fi_setname(&ep->rdm_ep->fid, addr, addrlen);
}

static int rxr_ep_rdm_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct rxr_ep *ep;

	ep = container_of(fid, struct rxr_ep, util_ep.ep_fid.fid);
	return fi_getname(&ep->rdm_ep->fid, addr, addrlen);
}

struct fi_ops_cm rxr_ep_cm = {
	.size = sizeof(struct fi_ops_cm),
	.setname = rxr_ep_rdm_setname,
	.getname = rxr_ep_rdm_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
	.join = fi_no_join,
};

/*
 * @brief explicitly allocate a chunk of memory for 5 packet pools on RX side:
 *     efa's receive packet pool (efa_rx_pkt_pool)
 *     shm's receive packet pool (shm_rx_pkt_pool)
 *     unexpected packet pool (rx_unexp_pkt_pool),
 *     out-of-order packet pool (rx_ooo_pkt_pool), and
 *     local read-copy packet pool (rx_readcopy_pkt_pool).
 *
 * @param ep[in]	endpoint
 * @return		On success, return 0
 * 			On failure, return a negative error code.
 */
int rxr_ep_grow_rx_pkt_pools(struct rxr_ep *ep)
{
	int err;

	assert(ep->efa_rx_pkt_pool);
	err = ofi_bufpool_grow(ep->efa_rx_pkt_pool);
	if (err) {
		FI_WARN(&rxr_prov, FI_LOG_CQ,
			"cannot allocate memory for EFA's RX packet pool. error: %s\n",
			strerror(-err));
		return err;
	}

	if (ep->use_shm) {
		assert(ep->shm_rx_pkt_pool);
		err = ofi_bufpool_grow(ep->shm_rx_pkt_pool);
		if (err) {
			FI_WARN(&rxr_prov, FI_LOG_CQ,
				"cannot allocate memory for SHM's RX packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	assert(ep->rx_unexp_pkt_pool);
	err = ofi_bufpool_grow(ep->rx_unexp_pkt_pool);
	if (err) {
		FI_WARN(&rxr_prov, FI_LOG_CQ,
			"cannot allocate memory for unexpected packet pool. error: %s\n",
			strerror(-err));
		return err;
	}

	assert(ep->rx_ooo_pkt_pool);
	err = ofi_bufpool_grow(ep->rx_ooo_pkt_pool);
	if (err) {
		FI_WARN(&rxr_prov, FI_LOG_CQ,
			"cannot allocate memory for out-of-order packet pool. error: %s\n",
			strerror(-err));
		return err;
	}

	if (ep->rx_readcopy_pkt_pool) {
		err = ofi_bufpool_grow(ep->rx_readcopy_pkt_pool);
		if (err) {
			FI_WARN(&rxr_prov, FI_LOG_CQ,
				"cannot allocate and register memory for readcopy packet pool. error: %s\n",
				strerror(-err));
			return err;
		}
	}

	return 0;
}

/**
 * @brief post internal receive buffers for progress engine.
 *
 * It is more efficient to post multiple receive buffers
 * to the device at once than to post each receive buffer
 * individually.
 *
 * Therefore, after an internal receive buffer (a packet
 * entry) was processed, it is not posted to the device
 * right away.
 *
 * Instead, we increase counter
 *      ep->efa/shm_rx_pkts_to_post
 * by one.
 *
 * Later, progress engine calls this function to
 * bulk post internal receive buffers (according to
 * the counter).
 *
 * This function also control number of internal
 * buffers posted to the device in zero copy receive
 * mode.
 *
 * param[in]	ep	endpoint
 */
static inline
void rxr_ep_progress_post_internal_rx_pkts(struct rxr_ep *ep)
{
	int err;

	if (ep->use_zcpy_rx) {
		/*
		 * In zero copy receive mode,
		 *
		 * If application did not post any receive buffer,
		 * we post one internal buffer so endpoint can
		 * receive RxR control packets such as handshake.
		 *
		 * If buffers have posted to the device, we do NOT
		 * repost internal buffers to maximize the chance
		 * user buffer is used to receive data.
		 */
		if (ep->efa_rx_pkts_posted == 0 && ep->efa_rx_pkts_to_post == 0) {
			ep->efa_rx_pkts_to_post = 1;
		} else if (ep->efa_rx_pkts_posted > 0 && ep->efa_rx_pkts_to_post > 0){
			ep->efa_rx_pkts_to_post = 0;
		}
	} else {
		if (ep->efa_rx_pkts_posted == 0 && ep->efa_rx_pkts_to_post == 0) {
			/* Both efa_rx_pkts_posted and efa_rx_pkts_to_post equal to 0 means
			 * this is the first call of the progress engine on this endpoint.
			 *
			 * In this case, we explictly allocate the 1st chunk of memory
			 * for unexp/ooo/readcopy RX packet pool.
			 *
			 * The reason to explicitly allocate the memory for RX packet
			 * pool is to improve efficiency.
			 *
			 * Without explicit memory allocation, a pkt pools's memory
			 * is allocated when 1st packet is allocated from it.
			 * During the computation, different processes got their 1st
			 * unexp/ooo/read-copy packet at different time. Therefore,
			 * if we do not explicitly allocate memory at the beginning,
			 * memory will be allocated at different time.
			 *
			 * When one process is allocating memory, other processes
			 * have to wait. When each process allocate memory at different
			 * time, the accumulated waiting time became significant.
			 *
			 * By explicitly allocating memory at 1st call to progress
			 * engine, the memory allocation is parallelized.
			 * (This assumes the 1st call to the progress engine on
			 * all processes happen at roughly the same time, which
			 * is a valid assumption according to our knowledge of
			 * the workflow of most application)
			 *
			 * The memory was not allocated during endpoint initialization
			 * because some applications will initialize some endpoints
			 * but never uses it, thus allocating memory initialization
			 * causes waste.
			 */
			err = rxr_ep_grow_rx_pkt_pools(ep);
			if (err)
				goto err_exit;

			ep->efa_rx_pkts_to_post = rxr_get_rx_pool_chunk_cnt(ep);
			ep->available_data_bufs = rxr_get_rx_pool_chunk_cnt(ep);

			if (ep->use_shm) {
				assert(ep->shm_rx_pkts_posted == 0 && ep->shm_rx_pkts_to_post == 0);
				ep->shm_rx_pkts_to_post = shm_info->rx_attr->size;
			}
		}
	}

	err = rxr_ep_bulk_post_internal_rx_pkts(ep, ep->efa_rx_pkts_to_post, EFA_EP);
	if (err)
		goto err_exit;

	ep->efa_rx_pkts_to_post = 0;

	if (ep->use_shm) {
		err = rxr_ep_bulk_post_internal_rx_pkts(ep, ep->shm_rx_pkts_to_post, SHM_EP);
		if (err)
			goto err_exit;

		ep->shm_rx_pkts_to_post = 0;
	}

	return;

err_exit:

	efa_eq_write_error(&ep->util_ep, err, err);
}

static inline int rxr_ep_send_queued_pkts(struct rxr_ep *ep,
					  struct dlist_entry *pkts)
{
	struct dlist_entry *tmp;
	struct rxr_pkt_entry *pkt_entry;
	int ret;

	dlist_foreach_container_safe(pkts, struct rxr_pkt_entry,
				     pkt_entry, entry, tmp) {
		if (ep->use_shm && rxr_ep_get_peer(ep, pkt_entry->addr)->is_local) {
			dlist_remove(&pkt_entry->entry);
			continue;
		}

		/* If send succeeded, pkt_entry->entry will be added
		 * to peer->outstanding_tx_pkts. Therefore, it must
		 * be removed from the list before send.
		 */
		dlist_remove(&pkt_entry->entry);

		ret = rxr_pkt_entry_send(ep, pkt_entry, 0);
		if (ret) {
			if (ret == -FI_EAGAIN) {
				/* add the pkt back to pkts, so it can be resent again */
				dlist_insert_tail(&pkt_entry->entry, pkts);
			}

			return ret;
		}
	}
	return 0;
}

static inline void rxr_ep_check_available_data_bufs_timer(struct rxr_ep *ep)
{
	if (OFI_LIKELY(ep->available_data_bufs != 0))
		return;

	if (ofi_gettime_us() - ep->available_data_bufs_ts >=
	    RXR_AVAILABLE_DATA_BUFS_TIMEOUT) {
		ep->available_data_bufs = rxr_get_rx_pool_chunk_cnt(ep);
		ep->available_data_bufs_ts = 0;
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Reset available buffers for large message receives\n");
	}
}

static inline void rxr_ep_check_peer_backoff_timer(struct rxr_ep *ep)
{
	struct rdm_peer *peer;
	struct dlist_entry *tmp;

	if (OFI_LIKELY(dlist_empty(&ep->peer_backoff_list)))
		return;

	dlist_foreach_container_safe(&ep->peer_backoff_list, struct rdm_peer,
				     peer, rnr_backoff_entry, tmp) {
		if (ofi_gettime_us() >= peer->rnr_backoff_begin_ts +
					peer->rnr_backoff_wait_time) {
			peer->flags &= ~RXR_PEER_IN_BACKOFF;
			dlist_remove(&peer->rnr_backoff_entry);
		}
	}
}

/**
 * @brief poll rdma-core cq and process the cq entry
 *
 * @param[in]	ep		end point
 * @param[in]	cqe_to_process	max number of cq entry to poll and process
 */
static inline void rdm_ep_poll_ibv_cq(struct rxr_ep *ep,
				      size_t cqe_to_process)
{
	struct ibv_wc ibv_wc;
	struct efa_cq *efa_cq;
	struct efa_av *efa_av;
	struct efa_ep *efa_ep;
	struct rdm_peer *peer;
	struct rxr_pkt_entry *pkt_entry;
	ssize_t ret;
	int i, err, prov_errno;

	efa_ep = container_of(ep->rdm_ep, struct efa_ep, util_ep.ep_fid);
	efa_av = efa_ep->av;
	efa_cq = container_of(ep->rdm_cq, struct efa_cq, util_cq.cq_fid);
	for (i = 0; i < cqe_to_process; i++) {
		ret = ibv_poll_cq(efa_cq->ibv_cq, 1, &ibv_wc);

		if (ret == 0)
			return;

		if (OFI_UNLIKELY(ret < 0 || ibv_wc.status)) {
			if (ret < 0) {
				efa_eq_write_error(&ep->util_ep, -ret, -ret);
				return;
			}

			pkt_entry = (void *)(uintptr_t)ibv_wc.wr_id;
			err = ibv_wc.status;
			prov_errno = ibv_wc.status;
			if (ibv_wc.opcode == IBV_WC_SEND) {
#if ENABLE_DEBUG
				ep->failed_send_comps++;
#endif
				rxr_pkt_handle_send_error(ep, pkt_entry, err, prov_errno);
			} else {
				assert(ibv_wc.opcode == IBV_WC_RECV);
				rxr_pkt_handle_recv_error(ep, pkt_entry, err, prov_errno);
			}

			return;
		}

		pkt_entry = (void *)(uintptr_t)ibv_wc.wr_id;

		switch (ibv_wc.opcode) {
		case IBV_WC_SEND:
#if ENABLE_DEBUG
			ep->send_comps++;
#endif
			rxr_pkt_handle_send_completion(ep, pkt_entry);
			break;
		case IBV_WC_RECV:
			peer = efa_ahn_qpn_to_peer(efa_av, ibv_wc.slid, ibv_wc.src_qp);
			pkt_entry->addr = peer ? peer->efa_fiaddr : FI_ADDR_NOTAVAIL;
			pkt_entry->pkt_size = ibv_wc.byte_len;
			assert(pkt_entry->pkt_size > 0);
			rxr_pkt_handle_recv_completion(ep, pkt_entry);
#if ENABLE_DEBUG
			ep->recv_comps++;
#endif
			break;
		default:
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"Unhandled cq type\n");
			assert(0 && "Unhandled cq type");
		}
	}
}

static inline
void rdm_ep_poll_shm_err_cq(struct fid_cq *shm_cq, struct fi_cq_err_entry *cq_err_entry)
{
	int ret;

	ret = fi_cq_readerr(shm_cq, cq_err_entry, 0);
	if (ret == 1)
		return;

	if (ret < 0) {
		FI_WARN(&rxr_prov, FI_LOG_CQ, "encountered error when fi_cq_readerr: %s\n",
			fi_strerror(-ret));
		cq_err_entry->err = -ret;
		cq_err_entry->prov_errno = -ret;
		return;
	}

	FI_WARN(&rxr_prov, FI_LOG_CQ, "fi_cq_readerr got expected return: %d\n", ret);
	cq_err_entry->err = FI_EIO;
	cq_err_entry->prov_errno = FI_EIO;
}

static inline void rdm_ep_poll_shm_cq(struct rxr_ep *ep,
				      size_t cqe_to_process)
{
	struct fi_cq_data_entry cq_entry;
	struct fi_cq_err_entry cq_err_entry = { 0 };
	struct rxr_pkt_entry *pkt_entry;
	fi_addr_t src_addr;
	ssize_t ret;
	struct efa_ep *efa_ep;
	struct efa_av *efa_av;
	int i;

	VALGRIND_MAKE_MEM_DEFINED(&cq_entry, sizeof(struct fi_cq_data_entry));

	efa_ep = container_of(ep->rdm_ep, struct efa_ep, util_ep.ep_fid);
	efa_av = efa_ep->av;
	for (i = 0; i < cqe_to_process; i++) {
		ret = fi_cq_readfrom(ep->shm_cq, &cq_entry, 1, &src_addr);

		if (ret == -FI_EAGAIN)
			return;

		if (OFI_UNLIKELY(ret < 0)) {
			if (ret != -FI_EAVAIL) {
				efa_eq_write_error(&ep->util_ep, -ret, -ret);
				return;
			}

			rdm_ep_poll_shm_err_cq(ep->shm_cq, &cq_err_entry);
			if (cq_err_entry.flags & (FI_SEND | FI_READ | FI_WRITE)) {
				assert(cq_entry.op_context);
				rxr_pkt_handle_send_error(ep, cq_entry.op_context, cq_err_entry.err, cq_err_entry.prov_errno);
			} else if (cq_err_entry.flags & FI_RECV) {
				assert(cq_entry.op_context);
				rxr_pkt_handle_recv_error(ep, cq_entry.op_context, cq_err_entry.err, cq_err_entry.prov_errno);
			} else {
				efa_eq_write_error(&ep->util_ep, cq_err_entry.err, cq_err_entry.prov_errno);
			}

			return;
		}

		if (OFI_UNLIKELY(ret == 0))
			return;

		pkt_entry = cq_entry.op_context;
		if (src_addr != FI_ADDR_UNSPEC) {
			/* convert SHM address to EFA address */
			assert(src_addr < EFA_SHM_MAX_AV_COUNT);
			src_addr = efa_av->shm_rdm_addr_map[src_addr];
		}

		if (cq_entry.flags & (FI_ATOMIC | FI_REMOTE_CQ_DATA)) {
			rxr_cq_handle_shm_completion(ep, &cq_entry, src_addr);
		} else if (cq_entry.flags & (FI_SEND | FI_READ | FI_WRITE)) {
			rxr_pkt_handle_send_completion(ep, pkt_entry);
		} else if (cq_entry.flags & (FI_RECV | FI_REMOTE_CQ_DATA)) {
			pkt_entry->addr = src_addr;
			pkt_entry->pkt_size = cq_entry.len;
			assert(pkt_entry->pkt_size > 0);
			rxr_pkt_handle_recv_completion(ep, pkt_entry);
		} else {
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"Unhandled cq type\n");
			assert(0 && "Unhandled cq type");
		}
	}
}

void rxr_ep_progress_internal(struct rxr_ep *ep)
{
	struct ibv_send_wr *bad_wr;
	struct efa_ep *efa_ep;
	struct rxr_rx_entry *rx_entry;
	struct rxr_tx_entry *tx_entry;
	struct rxr_read_entry *read_entry;
	struct rdm_peer *peer;
	struct dlist_entry *tmp;
	ssize_t ret;

	if (!ep->use_zcpy_rx)
		rxr_ep_check_available_data_bufs_timer(ep);

	// Poll the EFA completion queue
	rdm_ep_poll_ibv_cq(ep, rxr_env.efa_cq_read_size);

	// Poll the SHM completion queue if enabled
	if (ep->use_shm)
		rdm_ep_poll_shm_cq(ep, rxr_env.shm_cq_read_size);

	rxr_ep_progress_post_internal_rx_pkts(ep);

	rxr_ep_check_peer_backoff_timer(ep);

	/*
	 * Resend handshake packet for any peers where the first
	 * handshake send failed.
	 */
	dlist_foreach_container_safe(&ep->handshake_queued_peer_list,
				     struct rdm_peer, peer,
				     handshake_queued_entry, tmp) {
		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;

		ret = rxr_pkt_post_handshake(ep, peer);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
				"Failed to post HANDSHAKE to peer %ld: %s\n",
				peer->efa_fiaddr, fi_strerror(-ret));
			efa_eq_write_error(&ep->util_ep, FI_EIO, -ret);
			return;
		}

		dlist_remove(&peer->handshake_queued_entry);
		peer->flags &= ~RXR_PEER_HANDSHAKE_QUEUED;
		peer->flags |= RXR_PEER_HANDSHAKE_SENT;
	}

	/*
	 * Send any queued ctrl packets.
	 */
	dlist_foreach_container_safe(&ep->rx_entry_queued_rnr_list,
				     struct rxr_rx_entry,
				     rx_entry, queued_rnr_entry, tmp) {
		peer = rxr_ep_get_peer(ep, rx_entry->addr);
		assert(peer);

		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;

		assert(rx_entry->rxr_flags & RXR_RX_ENTRY_QUEUED_RNR);
		assert(!dlist_empty(&rx_entry->queued_pkts));
		ret = rxr_ep_send_queued_pkts(ep, &rx_entry->queued_pkts);

		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			rxr_cq_write_rx_error(ep, rx_entry, -ret, -ret);
			return;
		}

		dlist_remove(&rx_entry->queued_rnr_entry);
		rx_entry->rxr_flags &= ~RXR_RX_ENTRY_QUEUED_RNR;
	}

	dlist_foreach_container_safe(&ep->rx_entry_queued_ctrl_list,
				     struct rxr_rx_entry,
				     rx_entry, queued_ctrl_entry, tmp) {
		peer = rxr_ep_get_peer(ep, rx_entry->addr);
		assert(peer);

		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;
		/*
		 * rx_entry only send one ctrl packet at a time. The
		 * ctrl packet can be CTS, EOR, RECEIPT.
		 */
		assert(rx_entry->state == RXR_RX_QUEUED_CTRL);
		ret = rxr_pkt_post_ctrl(ep, RXR_RX_ENTRY, rx_entry,
					rx_entry->queued_ctrl.type,
					rx_entry->queued_ctrl.inject);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			rxr_cq_write_rx_error(ep, rx_entry, -ret, -ret);
			return;
		}

		/* it can happen that rxr_pkt_post_ctrl() released rx_entry
		 * (if the packet type is EOR and inject is used). In
		 * that case rx_entry's state has been set to RXR_RX_FREE and
		 * it has been removed from ep->rx_queued_entry_list, so nothing
		 * is left to do.
		 */
		if (rx_entry->state == RXR_RX_FREE)
			continue;

		dlist_remove(&rx_entry->queued_ctrl_entry);
		/*
		 * For CTS packet, the state need to be RXR_RX_RECV.
		 * For EOR/RECEIPT, all data has been received, so any state
		 * other than RXR_RX_QUEUED_CTRL should work.
		 * In all, we set the state to RXR_RX_RECV
		 */
		rx_entry->state = RXR_RX_RECV;
	}

	dlist_foreach_container_safe(&ep->tx_entry_queued_rnr_list,
				     struct rxr_tx_entry,
				     tx_entry, queued_rnr_entry, tmp) {
		peer = rxr_ep_get_peer(ep, tx_entry->addr);
		assert(peer);

		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;

		assert(tx_entry->rxr_flags & RXR_TX_ENTRY_QUEUED_RNR);
		ret = rxr_ep_send_queued_pkts(ep, &tx_entry->queued_pkts);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			rxr_cq_write_tx_error(ep, tx_entry, -ret, -ret);
			return;
		}

		dlist_remove(&tx_entry->queued_rnr_entry);
		tx_entry->rxr_flags &= ~RXR_TX_ENTRY_QUEUED_RNR;
	}

	dlist_foreach_container_safe(&ep->tx_entry_queued_ctrl_list,
				     struct rxr_tx_entry,
				     tx_entry, queued_ctrl_entry, tmp) {
		peer = rxr_ep_get_peer(ep, tx_entry->addr);
		assert(peer);

		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;

		assert(tx_entry->state == RXR_TX_QUEUED_CTRL);

		ret = rxr_pkt_post_ctrl(ep, RXR_TX_ENTRY, tx_entry,
					tx_entry->queued_ctrl.type,
					tx_entry->queued_ctrl.inject);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			rxr_cq_write_tx_error(ep, tx_entry, -ret, -ret);
			return;
		}

		dlist_remove(&tx_entry->queued_ctrl_entry);
		if (tx_entry->state == RXR_TX_QUEUED_CTRL)
			tx_entry->state = RXR_TX_REQ;
	}

	/*
	 * Send data packets until window or tx queue is exhausted.
	 */
	dlist_foreach_container(&ep->tx_pending_list, struct rxr_tx_entry,
				tx_entry, entry) {
		peer = rxr_ep_get_peer(ep, tx_entry->addr);
		assert(peer);

		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;

		if (tx_entry->window > 0)
			tx_entry->send_flags |= FI_MORE;
		else
			continue;

		while (tx_entry->window > 0) {
			if (ep->efa_max_outstanding_tx_ops - ep->efa_outstanding_tx_ops <= 1 ||
			    tx_entry->window <= ep->max_data_payload_size)
				tx_entry->send_flags &= ~FI_MORE;
			/*
			 * The core's TX queue is full so we can't do any
			 * additional work.
			 */
			if (ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
				goto out;

			if (peer->flags & RXR_PEER_IN_BACKOFF)
				break;

			ret = rxr_pkt_post_data(ep, tx_entry);
			if (OFI_UNLIKELY(ret)) {
				tx_entry->send_flags &= ~FI_MORE;
				if (ret == -FI_EAGAIN)
					goto out;

				rxr_cq_write_tx_error(ep, tx_entry, -ret, -ret);
				return;
			}
		}
	}

	/*
	 * Send read requests until finish or error encoutered
	 */
	dlist_foreach_container_safe(&ep->read_pending_list, struct rxr_read_entry,
				     read_entry, pending_entry, tmp) {
		peer = rxr_ep_get_peer(ep, read_entry->addr);
		assert(peer);

		if (peer->flags & RXR_PEER_IN_BACKOFF)
			continue;

		/*
		 * The core's TX queue is full so we can't do any
		 * additional work.
		 */
		if (ep->efa_outstanding_tx_ops == ep->efa_max_outstanding_tx_ops)
			goto out;

		ret = rxr_read_post(ep, read_entry);
		if (ret == -FI_EAGAIN)
			break;

		if (OFI_UNLIKELY(ret)) {
			rxr_read_write_error(ep, read_entry, -ret, -ret);
			return;
		}

		read_entry->state = RXR_RDMA_ENTRY_SUBMITTED;
		dlist_remove(&read_entry->pending_entry);
	}

out:
	efa_ep = container_of(ep->rdm_ep, struct efa_ep, util_ep.ep_fid);
	if (efa_ep->xmit_more_wr_tail != &efa_ep->xmit_more_wr_head) {
		ret = efa_post_flush(efa_ep, &bad_wr);
		if (OFI_UNLIKELY(ret))
			efa_eq_write_error(&ep->util_ep, -ret, -ret);
	}

	return;
}

void rxr_ep_progress(struct util_ep *util_ep)
{
	struct rxr_ep *ep;

	ep = container_of(util_ep, struct rxr_ep, util_ep);

	fastlock_acquire(&ep->util_ep.lock);
	rxr_ep_progress_internal(ep);
	fastlock_release(&ep->util_ep.lock);
}

static
bool rxr_ep_use_shm(struct fi_info *info)
{
	/* App provided hints supercede environmental variables.
	 *
	 * Using the shm provider comes with some overheads, particularly in the
	 * progress engine when polling an empty completion queue, so avoid
	 * initializing the provider if the app provides a hint that it does not
	 * require node-local communication. We can still loopback over the EFA
	 * device in cases where the app violates the hint and continues
	 * communicating with node-local peers.
	 */
	if (info
	    /* If the app requires explicitly remote communication */
	    && (info->caps & FI_REMOTE_COMM)
	    /* but not local communication */
	    && !(info->caps & FI_LOCAL_COMM))
		return 0;

	/*
	 * Currently, shm provider uses the SAR protocol for cuda
	 * memory buffer, whose performance is worse than using EFA device.
	 *
	 * To address this issue, shm usage is disabled if application
	 * requested the FI_HMEM capablity.
	 *
	 * This is not ideal, because host memory commuications are
	 * also going through device.
	 *
	 * The long term fix is make shm provider to support cuda
	 * buffers through cuda IPC. Once that is implemented, the
	 * following two lines need to be removed.
	 */
	if (info && (info->caps & FI_HMEM))
		return 0;

	return rxr_env.enable_shm_transfer;
}

int rxr_endpoint(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep, void *context)
{
	struct fi_info *rdm_info;
	struct rxr_domain *rxr_domain;
	struct efa_domain *efa_domain;
	struct rxr_ep *rxr_ep;
	struct fi_cq_attr cq_attr;
	int ret, retv;

	rxr_ep = calloc(1, sizeof(*rxr_ep));
	if (!rxr_ep)
		return -FI_ENOMEM;

	rxr_domain = container_of(domain, struct rxr_domain,
				  util_domain.domain_fid);
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_DATA;
	cq_attr.wait_obj = FI_WAIT_NONE;

	ret = ofi_endpoint_init(domain, &rxr_util_prov, info, &rxr_ep->util_ep,
				context, rxr_ep_progress);
	if (ret)
		goto err_free_ep;

	ret = rxr_get_lower_rdm_info(rxr_domain->util_domain.fabric->
				     fabric_fid.api_version, NULL, NULL, 0,
				     &rxr_util_prov, info, &rdm_info);
	if (ret)
		goto err_close_ofi_ep;

	rxr_reset_rx_tx_to_core(info, rdm_info);

	ret = fi_endpoint(rxr_domain->rdm_domain, rdm_info,
			  &rxr_ep->rdm_ep, rxr_ep);
	if (ret)
		goto err_free_rdm_info;

	efa_domain = container_of(rxr_domain->rdm_domain, struct efa_domain,
				  util_domain.domain_fid);

	rxr_ep->use_shm = rxr_ep_use_shm(info);
	if (rxr_ep->use_shm) {
		/* Open shm provider's endpoint */
		assert(!strcmp(shm_info->fabric_attr->name, "shm"));
		ret = fi_endpoint(efa_domain->shm_domain, shm_info,
				  &rxr_ep->shm_ep, rxr_ep);
		if (ret)
			goto err_close_core_ep;
	}

	rxr_ep->rx_size = info->rx_attr->size;
	rxr_ep->tx_size = info->tx_attr->size;
	rxr_ep->rx_iov_limit = info->rx_attr->iov_limit;
	rxr_ep->tx_iov_limit = info->tx_attr->iov_limit;
	rxr_ep->inject_size = info->tx_attr->inject_size;
	rxr_ep->efa_max_outstanding_tx_ops = rdm_info->tx_attr->size;
	rxr_ep->core_rx_size = rdm_info->rx_attr->size;
	rxr_ep->core_iov_limit = rdm_info->tx_attr->iov_limit;
	rxr_ep->core_caps = rdm_info->caps;

	cq_attr.size = MAX(rxr_ep->rx_size + rxr_ep->tx_size,
			   rxr_env.cq_size);

	if (info->tx_attr->op_flags & FI_DELIVERY_COMPLETE)
		FI_INFO(&rxr_prov, FI_LOG_CQ, "FI_DELIVERY_COMPLETE unsupported\n");

	assert(info->tx_attr->msg_order == info->rx_attr->msg_order);
	rxr_ep->msg_order = info->rx_attr->msg_order;
	rxr_ep->core_msg_order = rdm_info->rx_attr->msg_order;
	rxr_ep->core_inject_size = rdm_info->tx_attr->inject_size;
	rxr_ep->max_msg_size = info->ep_attr->max_msg_size;
	rxr_ep->msg_prefix_size = info->ep_attr->msg_prefix_size;
	rxr_ep->max_proto_hdr_size = rxr_pkt_max_header_size();
	rxr_ep->mtu_size = rdm_info->ep_attr->max_msg_size;
	fi_freeinfo(rdm_info);

	if (rxr_env.mtu_size > 0 && rxr_env.mtu_size < rxr_ep->mtu_size)
		rxr_ep->mtu_size = rxr_env.mtu_size;

	if (rxr_ep->mtu_size > RXR_MTU_MAX_LIMIT)
		rxr_ep->mtu_size = RXR_MTU_MAX_LIMIT;

	rxr_ep->max_data_payload_size = rxr_ep->mtu_size - sizeof(struct rxr_data_hdr);
	rxr_ep->min_multi_recv_size = rxr_ep->mtu_size - rxr_ep->max_proto_hdr_size;

	if (rxr_env.tx_queue_size > 0 &&
	    rxr_env.tx_queue_size < rxr_ep->efa_max_outstanding_tx_ops)
		rxr_ep->efa_max_outstanding_tx_ops = rxr_env.tx_queue_size;


	rxr_ep->use_zcpy_rx = rxr_ep_use_zcpy_rx(rxr_ep, info);
	FI_INFO(&rxr_prov, FI_LOG_EP_CTRL, "rxr_ep->use_zcpy_rx = %d\n", rxr_ep->use_zcpy_rx);

	rxr_ep->handle_resource_management = info->domain_attr->resource_mgmt;
	FI_INFO(&rxr_prov, FI_LOG_EP_CTRL,
		"rxr_ep->handle_resource_management = %d\n",
		rxr_ep->handle_resource_management);

#if ENABLE_DEBUG
	rxr_ep->efa_total_posted_tx_ops = 0;
	rxr_ep->shm_total_posted_tx_ops = 0;
	rxr_ep->send_comps = 0;
	rxr_ep->failed_send_comps = 0;
	rxr_ep->recv_comps = 0;
#endif

	rxr_ep->shm_rx_pkts_posted = 0;
	rxr_ep->shm_rx_pkts_to_post = 0;
	rxr_ep->efa_rx_pkts_posted = 0;
	rxr_ep->efa_rx_pkts_to_post = 0;
	rxr_ep->efa_outstanding_tx_ops = 0;
	rxr_ep->shm_outstanding_tx_ops = 0;
	rxr_ep->available_data_bufs_ts = 0;

	ret = fi_cq_open(rxr_domain->rdm_domain, &cq_attr,
			 &rxr_ep->rdm_cq, rxr_ep);
	if (ret)
		goto err_close_shm_ep;

	ret = fi_ep_bind(rxr_ep->rdm_ep, &rxr_ep->rdm_cq->fid,
			 FI_TRANSMIT | FI_RECV);
	if (ret)
		goto err_close_core_cq;

	/* Bind ep with shm provider's cq */
	if (rxr_ep->use_shm) {
		ret = fi_cq_open(efa_domain->shm_domain, &cq_attr,
				 &rxr_ep->shm_cq, rxr_ep);
		if (ret)
			goto err_close_core_cq;

		ret = fi_ep_bind(rxr_ep->shm_ep, &rxr_ep->shm_cq->fid,
				 FI_TRANSMIT | FI_RECV);
		if (ret)
			goto err_close_shm_cq;
	}

	ret = rxr_ep_init(rxr_ep);
	if (ret)
		goto err_close_shm_cq;

	*ep = &rxr_ep->util_ep.ep_fid;
	(*ep)->msg = &rxr_ops_msg;
	(*ep)->rma = &rxr_ops_rma;
	(*ep)->atomic = &rxr_ops_atomic;
	(*ep)->tagged = &rxr_ops_tagged;
	(*ep)->fid.ops = &rxr_ep_fi_ops;
	(*ep)->ops = &rxr_ops_ep;
	(*ep)->cm = &rxr_ep_cm;
	return 0;

err_close_shm_cq:
	if (rxr_ep->use_shm && rxr_ep->shm_cq) {
		retv = fi_close(&rxr_ep->shm_cq->fid);
		if (retv)
			FI_WARN(&rxr_prov, FI_LOG_CQ, "Unable to close shm cq: %s\n",
				fi_strerror(-retv));
	}
err_close_core_cq:
	retv = fi_close(&rxr_ep->rdm_cq->fid);
	if (retv)
		FI_WARN(&rxr_prov, FI_LOG_CQ, "Unable to close cq: %s\n",
			fi_strerror(-retv));
err_close_shm_ep:
	if (rxr_ep->use_shm && rxr_ep->shm_ep) {
		retv = fi_close(&rxr_ep->shm_ep->fid);
		if (retv)
			FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close shm EP: %s\n",
				fi_strerror(-retv));
	}
err_close_core_ep:
	retv = fi_close(&rxr_ep->rdm_ep->fid);
	if (retv)
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL, "Unable to close EP: %s\n",
			fi_strerror(-retv));
err_free_rdm_info:
	fi_freeinfo(rdm_info);
err_close_ofi_ep:
	retv = ofi_endpoint_close(&rxr_ep->util_ep);
	if (retv)
		FI_WARN(&rxr_prov, FI_LOG_EP_CTRL,
			"Unable to close util EP: %s\n",
			fi_strerror(-retv));
err_free_ep:
	free(rxr_ep);
	return ret;
}

/**
 * @brief record the event that a TX op has been submitted
 *
 * This function is called after a TX operation has been posted
 * successfully. It will:
 *
 *  1. increase the outstanding tx_op counter in endpoint and
 *     in the peer structure.
 *
 *  2. add the TX packet to peer's outstanding TX packet list.
 *
 * Both send and read are considered TX operation.
 *
 * The tx_op counters used to prevent over posting the device
 * and used in flow control. They are also usefull for debugging.
 *
 * Peer's outstanding TX packet list is used when removing a peer
 * to invalidate address of these packets, so that the completion
 * of these packet is ignored.
 *
 * @param[in,out]	ep		endpoint
 * @param[in]		pkt_entry	TX pkt_entry, which contains
 * 					the info of the TX op.
 */
void rxr_ep_record_tx_op_submitted(struct rxr_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct rdm_peer *peer;

	/*
	 * peer can be NULL when the pkt_entry is a RMA_CONTEXT_PKT,
	 * and the RMA is a local read toward the endpoint itself
	 */
	peer = rxr_ep_get_peer(ep, pkt_entry->addr);
	if (peer)
		dlist_insert_tail(&pkt_entry->entry, &peer->outstanding_tx_pkts);

	if (pkt_entry->alloc_type == RXR_PKT_FROM_EFA_TX_POOL) {
		ep->efa_outstanding_tx_ops++;
		if (peer)
			peer->efa_outstanding_tx_ops++;
#if ENABLE_DEBUG
		ep->efa_total_posted_tx_ops++;
#endif
	} else {
		assert(pkt_entry->alloc_type == RXR_PKT_FROM_SHM_TX_POOL);
		ep->shm_outstanding_tx_ops++;
		if (peer)
			peer->shm_outstanding_tx_ops++;
#if ENABLE_DEBUG
		ep->shm_total_posted_tx_ops++;
#endif
	}
}

/**
 * @brief record the event that an TX op is completed
 *
 * This function is called when the completion of
 * a TX operation is received. It will
 *
 * 1. decrease the outstanding tx_op counter in the endpoint
 *    and in the peer.
 *
 * 2. remove the TX packet from peer's outstanding
 *    TX packet list.
 *
 * Both send and read are considered TX operation.
 *
 * One may ask why this function is not integrated
 * into rxr_pkt_entry_relase_tx()?
 *
 * The reason is the action of decrease tx_op counter
 * is not tied to releasing a TX pkt_entry.
 *
 * Sometimes we need to decreate the tx_op counter
 * without releasing a TX pkt_entry. For example,
 * we handle a TX pkt_entry encountered RNR. We need
 * to decrease the tx_op counter and queue the packet.
 *
 * Sometimes we need release TX pkt_entry without
 * decreasing the tx_op counter. For example, when
 * rxr_pkt_post_ctrl() failed to post a pkt entry.
 *
 * @param[in,out]	ep		endpoint
 * @param[in]		pkt_entry	TX pkt_entry, which contains
 * 					the info of the TX op
 */
void rxr_ep_record_tx_op_completed(struct rxr_ep *ep, struct rxr_pkt_entry *pkt_entry)
{
	struct rdm_peer *peer;

	/*
	 * peer can be NULL when:
	 *
	 * 1. the pkt_entry is a RMA_CONTEXT_PKT, and the RMA op is a local read
	 *    toward the endpoint itself.
	 * 2. peer's address has been removed from address vector. Either because
	 *    a new peer has the same GID+QPN was inserted to address, or because
	 *    application removed the peer from address vector.
	 */
	peer = rxr_ep_get_peer(ep, pkt_entry->addr);
	if (peer)
		dlist_remove(&pkt_entry->entry);

	if (pkt_entry->alloc_type == RXR_PKT_FROM_EFA_TX_POOL) {
		ep->efa_outstanding_tx_ops--;
		if (peer)
			peer->efa_outstanding_tx_ops--;
	} else {
		assert(pkt_entry->alloc_type == RXR_PKT_FROM_SHM_TX_POOL);
		ep->shm_outstanding_tx_ops--;
		if (peer)
			peer->shm_outstanding_tx_ops--;
	}
}

