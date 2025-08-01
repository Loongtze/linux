// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/net/sunrpc/socklib.c
 *
 * Common socket helper routines for RPC client and server
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#include <linux/compiler.h>
#include <linux/netdevice.h>
#include <linux/gfp.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/pagemap.h>
#include <linux/udp.h>
#include <linux/sunrpc/msg_prot.h>
#include <linux/sunrpc/sched.h>
#include <linux/sunrpc/xdr.h>
#include <linux/export.h>

#include "socklib.h"

/*
 * Helper structure for copying from an sk_buff.
 */
struct xdr_skb_reader {
	struct sk_buff	*skb;
	unsigned int	offset;
	bool		need_checksum;
	size_t		count;
	__wsum		csum;
};

/**
 * xdr_skb_read_bits - copy some data bits from skb to internal buffer
 * @desc: sk_buff copy helper
 * @to: copy destination
 * @len: number of bytes to copy
 *
 * Possibly called several times to iterate over an sk_buff and copy data out of
 * it.
 */
static size_t
xdr_skb_read_bits(struct xdr_skb_reader *desc, void *to, size_t len)
{
	len = min(len, desc->count);

	if (desc->need_checksum) {
		__wsum csum;

		csum = skb_copy_and_csum_bits(desc->skb, desc->offset, to, len);
		desc->csum = csum_block_add(desc->csum, csum, desc->offset);
	} else {
		if (unlikely(skb_copy_bits(desc->skb, desc->offset, to, len)))
			return 0;
	}

	desc->count -= len;
	desc->offset += len;
	return len;
}

static ssize_t
xdr_partial_copy_from_skb(struct xdr_buf *xdr, struct xdr_skb_reader *desc)
{
	struct page **ppage = xdr->pages + (xdr->page_base >> PAGE_SHIFT);
	unsigned int poff = xdr->page_base & ~PAGE_MASK;
	unsigned int pglen = xdr->page_len;
	ssize_t copied = 0;
	size_t ret;

	if (xdr->head[0].iov_len == 0)
		return 0;

	ret = xdr_skb_read_bits(desc, xdr->head[0].iov_base,
				xdr->head[0].iov_len);
	if (ret != xdr->head[0].iov_len || !desc->count)
		return ret;
	copied += ret;

	while (pglen) {
		unsigned int len = min(PAGE_SIZE - poff, pglen);
		char *kaddr;

		/* ACL likes to be lazy in allocating pages - ACLs
		 * are small by default but can get huge. */
		if ((xdr->flags & XDRBUF_SPARSE_PAGES) && *ppage == NULL) {
			*ppage = alloc_page(GFP_NOWAIT | __GFP_NOWARN);
			if (unlikely(*ppage == NULL)) {
				if (copied == 0)
					return -ENOMEM;
				return copied;
			}
		}

		kaddr = kmap_atomic(*ppage);
		ret = xdr_skb_read_bits(desc, kaddr + poff, len);
		flush_dcache_page(*ppage);
		kunmap_atomic(kaddr);

		copied += ret;
		if (ret != len || !desc->count)
			return copied;
		ppage++;
		pglen -= len;
		poff = 0;
	}

	if (xdr->tail[0].iov_len) {
		copied += xdr_skb_read_bits(desc, xdr->tail[0].iov_base,
					xdr->tail[0].iov_len);
	}

	return copied;
}

/**
 * csum_partial_copy_to_xdr - checksum and copy data
 * @xdr: target XDR buffer
 * @skb: source skb
 *
 * We have set things up such that we perform the checksum of the UDP
 * packet in parallel with the copies into the RPC client iovec.  -DaveM
 */
int csum_partial_copy_to_xdr(struct xdr_buf *xdr, struct sk_buff *skb)
{
	struct xdr_skb_reader desc = {
		.skb		= skb,
		.count		= skb->len - desc.offset,
	};

	if (skb_csum_unnecessary(skb)) {
		if (xdr_partial_copy_from_skb(xdr, &desc) < 0)
			return -1;
		if (desc.count)
			return -1;
		return 0;
	}

	desc.need_checksum = true;
	desc.csum = csum_partial(skb->data, desc.offset, skb->csum);
	if (xdr_partial_copy_from_skb(xdr, &desc) < 0)
		return -1;
	if (desc.offset != skb->len) {
		__wsum csum2;
		csum2 = skb_checksum(skb, desc.offset, skb->len - desc.offset, 0);
		desc.csum = csum_block_add(desc.csum, csum2, desc.offset);
	}
	if (desc.count)
		return -1;
	if (csum_fold(desc.csum))
		return -1;
	if (unlikely(skb->ip_summed == CHECKSUM_COMPLETE) &&
	    !skb->csum_complete_sw)
		netdev_rx_csum_fault(skb->dev, skb);
	return 0;
}

static inline int xprt_sendmsg(struct socket *sock, struct msghdr *msg,
			       size_t seek)
{
	if (seek)
		iov_iter_advance(&msg->msg_iter, seek);
	return sock_sendmsg(sock, msg);
}

static int xprt_send_kvec(struct socket *sock, struct msghdr *msg,
			  struct kvec *vec, size_t seek)
{
	iov_iter_kvec(&msg->msg_iter, ITER_SOURCE, vec, 1, vec->iov_len);
	return xprt_sendmsg(sock, msg, seek);
}

static int xprt_send_pagedata(struct socket *sock, struct msghdr *msg,
			      struct xdr_buf *xdr, size_t base)
{
	iov_iter_bvec(&msg->msg_iter, ITER_SOURCE, xdr->bvec, xdr_buf_pagecount(xdr),
		      xdr->page_len + xdr->page_base);
	return xprt_sendmsg(sock, msg, base + xdr->page_base);
}

/* Common case:
 *  - stream transport
 *  - sending from byte 0 of the message
 *  - the message is wholly contained in @xdr's head iovec
 */
static int xprt_send_rm_and_kvec(struct socket *sock, struct msghdr *msg,
				 rpc_fraghdr marker, struct kvec *vec,
				 size_t base)
{
	struct kvec iov[2] = {
		[0] = {
			.iov_base	= &marker,
			.iov_len	= sizeof(marker)
		},
		[1] = *vec,
	};
	size_t len = iov[0].iov_len + iov[1].iov_len;

	iov_iter_kvec(&msg->msg_iter, ITER_SOURCE, iov, 2, len);
	return xprt_sendmsg(sock, msg, base);
}

/**
 * xprt_sock_sendmsg - write an xdr_buf directly to a socket
 * @sock: open socket to send on
 * @msg: socket message metadata
 * @xdr: xdr_buf containing this request
 * @base: starting position in the buffer
 * @marker: stream record marker field
 * @sent_p: return the total number of bytes successfully queued for sending
 *
 * Return values:
 *   On success, returns zero and fills in @sent_p.
 *   %-ENOTSOCK if  @sock is not a struct socket.
 */
int xprt_sock_sendmsg(struct socket *sock, struct msghdr *msg,
		      struct xdr_buf *xdr, unsigned int base,
		      rpc_fraghdr marker, unsigned int *sent_p)
{
	unsigned int rmsize = marker ? sizeof(marker) : 0;
	unsigned int remainder = rmsize + xdr->len - base;
	unsigned int want;
	int err = 0;

	*sent_p = 0;

	if (unlikely(!sock))
		return -ENOTSOCK;

	msg->msg_flags |= MSG_MORE;
	want = xdr->head[0].iov_len + rmsize;
	if (base < want) {
		unsigned int len = want - base;

		remainder -= len;
		if (remainder == 0)
			msg->msg_flags &= ~MSG_MORE;
		if (rmsize)
			err = xprt_send_rm_and_kvec(sock, msg, marker,
						    &xdr->head[0], base);
		else
			err = xprt_send_kvec(sock, msg, &xdr->head[0], base);
		if (remainder == 0 || err != len)
			goto out;
		*sent_p += err;
		base = 0;
	} else {
		base -= want;
	}

	if (base < xdr->page_len) {
		unsigned int len = xdr->page_len - base;

		remainder -= len;
		if (remainder == 0)
			msg->msg_flags &= ~MSG_MORE;
		err = xprt_send_pagedata(sock, msg, xdr, base);
		if (remainder == 0 || err != len)
			goto out;
		*sent_p += err;
		base = 0;
	} else {
		base -= xdr->page_len;
	}

	if (base >= xdr->tail[0].iov_len)
		return 0;
	msg->msg_flags &= ~MSG_MORE;
	err = xprt_send_kvec(sock, msg, &xdr->tail[0], base);
out:
	if (err > 0) {
		*sent_p += err;
		err = 0;
	}
	return err;
}
