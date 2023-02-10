// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DECnet       An implementation of the DECnet protocol suite for the LINUX
 *              operating system.  DECnet is implemented using the  BSD Socket
 *              interface as the means of communication with the user level.
 *
 *              DECnet Network Services Protocol (Input)
 *
 * Author:      Eduardo Marcelo Serrat <emserrat@geocities.com>
 *
 * Changes:
 *
 *    Steve Whitehouse:  Split into dn_nsp_in.c and dn_nsp_out.c from
 *                       original dn_nsp.c.
 *    Steve Whitehouse:  Updated to work with my new routing architecture.
 *    Steve Whitehouse:  Add changes from Eduardo Serrat's patches.
 *    Steve Whitehouse:  Put all ack handling code in a common routine.
 *    Steve Whitehouse:  Put other common bits into dn_nsp_rx()
 *    Steve Whitehouse:  More checks on skb->len to catch bogus packets
 *                       Fixed various race conditions and possible nasties.
 *    Steve Whitehouse:  Now handles returned conninit frames.
 *     David S. Miller:  New socket locking
 *    Steve Whitehouse:  Fixed lockup when socket filtering was enabled.
 *         Paul Koning:  Fix to push CC sockets into RUN when acks are
 *                       received.
 *    Steve Whitehouse:
 *   Patrick Caulfield:  Checking conninits for correctness & sending of error
 *                       responses.
 *    Steve Whitehouse:  Added backlog congestion level return codes.
 *   Patrick Caulfield:
 *    Steve Whitehouse:  Added flow control support (outbound)
 *    Steve Whitehouse:  Prepare for nonlinear skbs
 */

/******************************************************************************
    (c) 1995-1998 E.M. Serrat           emserrat@geocities.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*******************************************************************************/

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/inet.h>
#include <linux/route.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/termios.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/netfilter_decnet.h>
#include <trace/events/sock.h>
#include <net/neighbour.h>
#include <net/dst.h>
#include <net/dn.h>
#include <net/dn_nsp.h>
#include <net/dn_dev.h>
#include <net/dn_route.h>

#define ACKDELAY        (3 * HZ)

extern int decnet_log_martians;
extern int decnet_segbufsize;

static void dn_log_martian(struct sk_buff *skb, const char *msg)
{
        if (decnet_log_martians) {
                char *devname = skb->dev ? skb->dev->name : "???";
                struct dn_skb_cb *cb = DN_SKB_CB(skb);
                net_info_ratelimited("DECnet: Martian packet (%s) dev=%s src=0x%04hx dst=0x%04hx srcport=0x%04hx dstport=0x%04hx\n",
                                     msg, devname,
                                     le16_to_cpu(cb->src),
                                     le16_to_cpu(cb->dst),
                                     le16_to_cpu(cb->src_port),
                                     le16_to_cpu(cb->dst_port));
        }
}

/*
 * For this function we've flipped the cross-subchannel bit
 * if the message is an otherdata or linkservice message. Thus
 * we can use it to work out what to update.
 */
static void dn_ack(struct sock *sk, struct sk_buff *skb, unsigned short ack)
{
        struct dn_scp *scp = DN_SK(sk);
        unsigned short type = ((ack >> 12) & 0x0003);
        int wakeup = 0;

        switch (type) {
        case 0: /* ACK - Data */
                if (dn_after(ack, scp->ackrcv_dat)) {
                        scp->ackrcv_dat = ack & NSP_SG_MASK;
                        wakeup |= dn_nsp_check_xmit_queue(sk, skb,
                                                          &scp->data_xmit_queue,
                                                          ack, 0);
                }
                break;
        case 1: /* NAK - Data */
                break;
        case 2: /* ACK - OtherData */
                if (dn_after(ack, scp->ackrcv_oth)) {
                        scp->ackrcv_oth = ack & NSP_SG_MASK;
                        wakeup |= dn_nsp_check_xmit_queue(sk, skb,
                                                          &scp->other_xmit_queue,
                                                          ack, 1);
                }
                break;
        case 3: /* NAK - OtherData */
                break;
        }

        if (wakeup && !sock_flag(sk, SOCK_DEAD))
                sk->sk_state_change(sk);
}

/*
 * This function is a universal ack processor.
 */
static int dn_process_ack(struct sock *sk, struct sk_buff *skb, int oth)
{
        __le16 *ptr = (__le16 *)skb->data;
        int len = 0;
        unsigned short ack;

        if (skb->len < 2)
                return len;

        if ((ack = le16_to_cpu(*ptr)) & 0x8000) {
                skb_pull(skb, 2);
                ptr++;
                len += 2;
                if ((ack & 0x4000) == 0) {
                        if (oth)
                                ack ^= 0x2000;
                        dn_ack(sk, skb, ack);
                }

        	if (skb->len < 2)
                	return len;

        	if ((ack = le16_to_cpu(*ptr)) & 0x8000) {
                	skb_pull(skb, 2);
                	len += 2;
                	if ((ack & 0x4000) == 0) {
                        	if (oth)
                                	ack ^= 0x2000;
                        	dn_ack(sk, skb, ack);
                	}
        	}
	}

        return len;
}


/**
 * dn_check_idf - Check an image data field format is correct.
 * @pptr: Pointer to pointer to image data
 * @len: Pointer to length of image data
 * @max: The maximum allowed length of the data in the image data field
 *
 * Returns: 0 if ok, -1 on error
 */
static inline int dn_check_idf(unsigned char **pptr, int *len, unsigned char max)
{
        unsigned char *ptr = *pptr;
        unsigned char flen = *ptr++;

        (*len)--;
        if (flen > max)
                return -1;
        if (flen > *len)
                return -1;

        *len -= flen;
        *pptr = ptr + flen;
        return 0;
}

/*
 * Table of reason codes to pass back to node which sent us a badly
 * formed message, plus text messages for the log. A zero entry in
 * the reason field means "don't reply" otherwise a disc init is sent with
 * the specified reason code.
 */
static struct {
        unsigned short reason;
        const char *text;
} ci_err_table[] = {
 { 0,             "CI: Truncated message" },
 { NSP_REASON_ID, "CI: Destination username error" },
 { NSP_REASON_ID, "CI: Destination username type" },
 { NSP_REASON_US, "CI: Source username error" },
 { 0,             "CI: Truncated at menuver" },
 { 0,             "CI: Truncated before access or user data" },
 { NSP_REASON_IO, "CI: Access data format error" },
 { NSP_REASON_IO, "CI: User data format error" }
};

/*
 * This function uses a slightly different lookup method
 * to find its sockets, since it searches on object name/number
 * rather than port numbers. Various tests are done to ensure that
 * the incoming data is in the correct format before it is queued to
 * a socket.
 */
static struct sock *dn_find_listener(struct sk_buff *skb, unsigned short *reason)
{
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        struct nsp_conn_init_msg *msg = (struct nsp_conn_init_msg *)skb->data;
        struct sockaddr_dn dstaddr;
        struct sockaddr_dn srcaddr;
        unsigned char type = 0;
        int dstlen;
        int srclen;
        unsigned char *ptr;
        int len;
        int err = 0;
        unsigned char menuver;

        memset(&dstaddr, 0, sizeof(struct sockaddr_dn));
        memset(&srcaddr, 0, sizeof(struct sockaddr_dn));

        /*
         * 1. Decode & remove message header
         */
        cb->src_port = msg->srcaddr;
        cb->dst_port = msg->dstaddr;
        cb->services = msg->services;
        cb->info     = msg->info;
        cb->segsize  = le16_to_cpu(msg->segsize);

        if (!pskb_may_pull(skb, sizeof(*msg)))
                goto err_out;

        skb_pull(skb, sizeof(*msg));

        len = skb->len;
        ptr = skb->data;

        /*
         * 2. Check destination end username format
         */
        dstlen = dn_username2sockaddr(ptr, len, &dstaddr, &type);
        err++;
        if (dstlen < 0)
                goto err_out;

        err++;
        if (type > 1)
                goto err_out;

        len -= dstlen;
        ptr += dstlen;

        /*
         * 3. Check source end username format
         */
        srclen = dn_username2sockaddr(ptr, len, &srcaddr, &type);
        err++;
        if (srclen < 0)
                goto err_out;

        len -= srclen;
        ptr += srclen;
        err++;
        if (len < 1)
                goto err_out;

        menuver = *ptr;
        ptr++;
        len--;

        /*
         * 4. Check that optional data actually exists if menuver says it does
         */
        err++;
        if ((menuver & (DN_MENUVER_ACC | DN_MENUVER_USR)) && (len < 1))
                goto err_out;

        /*
         * 5. Check optional access data format
         */
        err++;
        if (menuver & DN_MENUVER_ACC) {
                if (dn_check_idf(&ptr, &len, 39))
                        goto err_out;
                if (dn_check_idf(&ptr, &len, 39))
                        goto err_out;
                if (dn_check_idf(&ptr, &len, 39))
                        goto err_out;
        }

        /*
         * 6. Check optional user data format
         */
        err++;
        if (menuver & DN_MENUVER_USR) {
                if (dn_check_idf(&ptr, &len, 16))
                        goto err_out;
        }

        /*
         * 7. Look up socket based on destination end username
         */
        return dn_sklist_find_listener(&dstaddr);
err_out:
        dn_log_martian(skb, ci_err_table[err].text);
        *reason = ci_err_table[err].reason;
        return NULL;
}


static void dn_nsp_conn_init(struct sock *sk, struct sk_buff *skb)
{
        if (sk_acceptq_is_full(sk)) {
                kfree_skb(skb);
                return;
        }

        sk_acceptq_added(sk);
        skb_queue_tail(&sk->sk_receive_queue, skb);
        sk->sk_state_change(sk);
}

static void dn_nsp_conn_conf(struct sock *sk, struct sk_buff *skb)
{
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        struct dn_scp *scp = DN_SK(sk);
        unsigned char *ptr;

        if (skb->len < 4)
                goto out;

        ptr = skb->data;
        cb->services = *ptr++;
        cb->info = *ptr++;
        cb->segsize = le16_to_cpu(*(__le16 *)ptr);
        skb_pull(skb, 4);

        if ((scp->state == DN_CI) || (scp->state == DN_CD)) {
                scp->persist = 0;
		scp->conntimer = 0;
                scp->addrrem = cb->src_port;
                sk->sk_state = TCP_ESTABLISHED;
                scp->state = DN_RUN;
                scp->services_rem = cb->services;
                scp->info_rem = cb->info;
                scp->segsize_rem = cb->segsize;

                /*
                 * If the Connect Confirm message was received with
                 * a short routing header or with the Intra-Ethernet bit
                 * clear, revert to the "SEGMENT BUFFER SIZE" parameter since
                 * traffic will be going off ethernet.
                 */
                if (((cb->rt_flags & DN_RT_PKT_MSK) == DN_RT_PKT_SHORT) ||
                    ((cb->rt_flags & DN_RT_F_IE) == 0))
                        scp->segsize_rem =
                          decnet_segbufsize - (DN_MAX_NSP_DATA_HEADER + 6);

                if ((scp->services_rem & NSP_FC_MASK) == NSP_FC_NONE)
                        scp->max_window = decnet_no_fc_max_cwnd;

                if (skb->len > 0) {
                        u16 dlen = *skb->data;
                        if ((dlen <= 16) && (dlen <= skb->len)) {
                                scp->conndata_in.opt_optl = cpu_to_le16(dlen);
                                skb_copy_from_linear_data_offset(skb, 1,
                                              scp->conndata_in.opt_data, dlen);
                        }
                }
		dn_nsp_schedule_pending(sk, DN_PEND_IDLE);
                if (!sock_flag(sk, SOCK_DEAD))
                        sk->sk_state_change(sk);
        }

out:
        kfree_skb(skb);
}

static void dn_nsp_conn_ack(struct sock *sk, struct sk_buff *skb)
{
        struct dn_scp *scp = DN_SK(sk);

        if (scp->state == DN_CI) {
                scp->state = DN_CD;
                scp->persist = 0;
		scp->conntimer = decnet_outgoing_timer * HZ;
        }

        kfree_skb(skb);
}

static void dn_nsp_disc_init(struct sock *sk, struct sk_buff *skb)
{
        struct dn_scp *scp = DN_SK(sk);
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        unsigned short reason;

        if (skb->len < 2)
                goto out;

        reason = le16_to_cpu(*(__le16 *)skb->data);
        skb_pull(skb, 2);

        scp->discdata_in.opt_status = cpu_to_le16(reason);
        scp->discdata_in.opt_optl   = 0;
        memset(scp->discdata_in.opt_data, 0, 16);

        if (skb->len > 0) {
                u16 dlen = *skb->data;
                if ((dlen <= 16) && (dlen <= skb->len)) {
                        scp->discdata_in.opt_optl = cpu_to_le16(dlen);
                        skb_copy_from_linear_data_offset(skb, 1, scp->discdata_in.opt_data, dlen);
                }
        }

        scp->addrrem = cb->src_port;
        sk->sk_state = TCP_CLOSE;

        switch (scp->state) {
        case DN_CI:
        case DN_CD:
                scp->state = DN_RJ;
                sk->sk_err = ECONNREFUSED;
		scp->conntimer = 0;
                break;
        case DN_RUN:
                sk->sk_shutdown |= SHUTDOWN_MASK;
                scp->state = DN_DN;
                break;
        case DN_DI:
                scp->state = DN_DIC;
                break;
        }

        if (!sock_flag(sk, SOCK_DEAD)) {
                if (sk->sk_socket->state != SS_UNCONNECTED)
                        sk->sk_socket->state = SS_DISCONNECTING;
                sk->sk_state_change(sk);
        }

        /*
         * It appears that its possible for remote machines to send disc
         * init messages with no port identifier if we are in the CI and
         * possibly also the CD state. Obviously we shouldn't reply with
         * a message if we don't know what the end point is.
         */
        if (scp->addrrem) {
                dn_nsp_send_disc(sk, NSP_DISCCONF, NSP_REASON_DC, GFP_ATOMIC);
        }
        scp->persist_fxn = dn_destroy_timer;
        scp->persist = dn_nsp_persist(sk);

out:
        kfree_skb(skb);
}

/*
 * disc_conf messages are also called no_resources or no_link
 * messages depending upon the "reason" field.
 */
static void dn_nsp_disc_conf(struct sock *sk, struct sk_buff *skb)
{
        struct dn_scp *scp = DN_SK(sk);
        unsigned short reason;

        if (skb->len != 2)
                goto out;

        reason = le16_to_cpu(*(__le16 *)skb->data);

        sk->sk_state = TCP_CLOSE;

        switch (scp->state) {
        case DN_CI:
                scp->state = DN_NR;
                break;
        case DN_DR:
                if (reason == NSP_REASON_DC)
                        scp->state = DN_DRC;
                if (reason == NSP_REASON_NL)
                        scp->state = DN_CN;
                break;
        case DN_DI:
                scp->state = DN_DIC;
                break;
        case DN_RUN:
                sk->sk_shutdown |= SHUTDOWN_MASK;
		fallthrough;

        case DN_CC:
                scp->state = DN_CN;
        }

        if (!sock_flag(sk, SOCK_DEAD)) {
                if (sk->sk_socket->state != SS_UNCONNECTED)
                        sk->sk_socket->state = SS_DISCONNECTING;
                sk->sk_state_change(sk);
        }

        scp->persist_fxn = dn_destroy_timer;
        scp->persist = dn_nsp_persist(sk);

out:
        kfree_skb(skb);
}

static void dn_nsp_linkservice(struct sock *sk, struct sk_buff *skb)
{
        struct dn_scp *scp = DN_SK(sk);
        unsigned short segnum;
        unsigned char lsflags;
        signed char fcval;
        int wake_up = 0;
        char *ptr = skb->data;
        unsigned char fctype = scp->services_rem & NSP_FC_MASK;

        if (skb->len != 4)
                goto out;

        segnum = le16_to_cpu(*(__le16 *)ptr);
        ptr += 2;
        lsflags = *(unsigned char *)ptr++;
        fcval = *ptr;

        /*
         * Here we ignore erronous packets which should really
         * should cause a connection abort. It is not critical
         * for now though.
         */
        if (lsflags & 0xf8)
                goto out;

        if (seq_next(scp->numoth_rcv, segnum)) {
                seq_add(&scp->numoth_rcv, 1);
                switch(lsflags & 0x04) { /* FCVAL INT */
                case 0x00: /* Normal Request */
                        switch(lsflags & 0x03) { /* FCVAL MOD */
                        case DN_NOCHANGE: /* Request count */
                                if (fcval < 0) {
                                        unsigned char p_fcval = -fcval;
                                        if ((scp->flowrem_dat > p_fcval) &&
                                            (fctype == NSP_FC_SCMC)) {
                                                scp->flowrem_dat -= p_fcval;
                                        }
                                } else if (fcval > 0) {
                                        scp->flowrem_dat += fcval;
                                        wake_up = 1;
                                }
                                break;
                        case DN_DONTSEND: /* Stop outgoing data */
                                scp->flowrem_sw = DN_DONTSEND;
                                break;
                        case DN_SEND: /* Ok to start again */
                                scp->flowrem_sw = DN_SEND;
                                dn_nsp_output(sk);
                                wake_up = 1;
                        }
                        break;
                case 0x04: /* Interrupt Request */
                        if (fcval > 0) {
                                scp->flowrem_oth += fcval;
                                wake_up = 1;
                        }
                        break;
                }
                if (wake_up && !sock_flag(sk, SOCK_DEAD))
                        sk->sk_state_change(sk);
        }

        dn_nsp_send_oth_ack(sk);

out:
        kfree_skb(skb);
}

/*
 * NOTE:	Keep this in sync with the code in sock.c
 *
 * Copy of sock_queue_rcv_skb() (from sock.c) modified to accept a pointer
 * to the buffer queue.
 */
static __inline__ int dn_queue_skb(struct sock *sk, struct sk_buff *skb, struct sk_buff_head *queue)
{
	int err;
	unsigned long flags;

	err = sk_filter(sk, skb);
	if (err)
		return err;

	if (atomic_read(&sk->sk_rmem_alloc) >= sk->sk_rcvbuf) {
		atomic_inc(&sk->sk_drops);
		return -ENOMEM;
	}

	if (!sk_rmem_schedule(sk, skb, skb->truesize)) {
		atomic_inc(&sk->sk_drops);
		return -ENOBUFS;
	}

	skb->dev = NULL;
	skb_set_owner_r(skb, sk);

	/* we escape from rcu protected region, make sure we dont leak
	 * a norefcounted dst
	 */
	skb_dst_force(skb);

	spin_lock_irqsave(&queue->lock, flags);
	sock_skb_set_dropcount(sk, skb);
	__skb_queue_tail(queue, skb);
	spin_unlock_irqrestore(&queue->lock, flags);

	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_data_ready(sk);
	return 0;
}

static void dn_nsp_otherdata(struct sock *sk, struct sk_buff *skb)
{
        struct dn_scp *scp = DN_SK(sk);
        unsigned short segnum;
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        int queued = 0;

        if (skb->len < 2)
                goto out;

        cb->segnum = segnum = le16_to_cpu(*(__le16 *)skb->data);
        skb_pull(skb, 2);

        if (seq_next(scp->numoth_rcv, segnum)) {
		rcu_read_lock();
		if (dn_queue_skb(sk, skb, &scp->other_receive_queue) == 0) {
                        seq_add(&scp->numoth_rcv, 1);
                        scp->other_report = 0;
                        queued = 1;
                }
		rcu_read_unlock();
        }

        dn_nsp_send_oth_ack(sk);
out:
        if (!queued)
                kfree_skb(skb);
}

static void dn_nsp_data(struct sock *sk, struct sk_buff *skb)
{
        int queued = 0;
        unsigned short segnum;
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        struct dn_scp *scp = DN_SK(sk);

        if (skb->len < 2)
                goto out;

        cb->segnum = segnum = le16_to_cpu(*(__le16 *)skb->data);
        skb_pull(skb, 2);

        if (seq_next(scp->numdat_rcv, segnum)) {
		rcu_read_lock();
		if (dn_queue_skb(sk, skb, &sk->sk_receive_queue) == 0) {
                        seq_add(&scp->numdat_rcv, 1);
                        queued = 1;
                }
		rcu_read_unlock();

                if ((scp->flowloc_sw == DN_SEND) && dn_congested(sk)) {
                        scp->flowloc_sw = DN_DONTSEND;
			dn_nsp_schedule_pending(sk, DN_PEND_SW);
                }
        }

        if (queued && !sendack(segnum)) {
                if (scp->ackdelay != 0) {
                        scp->ackdelay = ACKDELAY;
                }
        } else
                dn_nsp_send_data_ack(sk);
out:
        if (!queued)
                kfree_skb(skb);
}

/*
 * If one of our conninit messages is returned, this function
 * deals with it. It puts the socket into the NO_COMMUNICATION
 * state.
 */
static void dn_returned_conn_init(struct sock *sk)
{
        struct dn_scp *scp = DN_SK(sk);

        if (scp->state == DN_CI) {
                scp->state = DN_NC;
                sk->sk_state = TCP_CLOSE;
		sk->sk_err = EHOSTUNREACH;
                if (!sock_flag(sk, SOCK_DEAD))
                        sk->sk_state_change(sk);
        }
}

static int dn_nsp_no_socket(struct sk_buff *skb, unsigned short reason)
{
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        int ret = NET_RX_DROP;

        /* Must not reply to returned packets */
        if (cb->rt_flags & DN_RT_F_RTS)
                goto out;

        if ((reason != NSP_REASON_OK) && ((cb->nsp_flags & 0x0c) == 0x08)) {
                switch (cb->nsp_flags & 0x70) {
                case 0x10:
                case 0x60: /* (Retransmitted) Connect Init */
                        dn_nsp_return_disc(skb, NSP_DISCINIT, reason);
                        ret = NET_RX_SUCCESS;
                        break;
                case 0x20: /* Connect Confirm */
                        dn_nsp_return_disc(skb, NSP_DISCCONF, reason);
                        ret = NET_RX_SUCCESS;
                        break;
                }
        }

out:
        kfree_skb(skb);
        return ret;
}

static int dn_nsp_rx_packet(struct net *net, struct sock *sk2,
                            struct sk_buff *skb)
{
        struct dn_skb_cb *cb = DN_SKB_CB(skb);
        struct sock *sk = NULL;
        unsigned char *ptr = (unsigned char *)skb->data;
        unsigned short reason = NSP_REASON_NL;

        if (!pskb_may_pull(skb, 2))
                goto free_out;

        skb_reset_transport_header(skb);
        cb->nsp_flags = *ptr++;

        if (decnet_debug_level & DN_DBG_RX_NSP)
                printk(KERN_DEBUG "dn_nsp_rx: Message type 0x%02x\n", (int)cb->nsp_flags);

        if (cb->nsp_flags & 0x83)
                goto free_out;

        /*
         * Filter out conninits and useless packet types
         */
        if ((cb->nsp_flags & 0x0c) == 0x08) {
                switch (cb->nsp_flags & 0x70) {
                case 0x00: /* NOP */
                case 0x70: /* Reserved */
                case 0x50: /* Reserved, Phase II node init */
                        goto free_out;
                case 0x10:
                case 0x60:
			if (cb->rt_flags & DN_RT_F_RTS) {
				if (pskb_may_pull(skb, 4)) {
					cb->dst_port = *(__le16 *)ptr;
					ptr += 2;
					cb->src_port = *(__le16 *)ptr;
					if ((sk = dn_check_returned_conn(skb)) != NULL) {
						dn_returned_conn_init(sk);
						sock_put(sk);
					}
				}
				kfree_skb(skb);
				return NET_RX_SUCCESS;
			}
                        sk = dn_find_listener(skb, &reason);
                        goto got_it;
                }
        }

	/*
	 * We've already handled all packet types which can be returned
	 * to sender (CI and retransmitted CI). Discard all other packet types.
	 */
        if (unlikely(cb->rt_flags & DN_RT_F_RTS))
		goto free_out;

        if (!pskb_may_pull(skb, 3))
                goto free_out;

        /*
         * Grab the destination address.
         */
        cb->dst_port = *(__le16 *)ptr;
        cb->src_port = 0;
        ptr += 2;

        /*
         * If not a connack, grab the source address too.
         */
        if (pskb_may_pull(skb, 5)) {
                cb->src_port = *(__le16 *)ptr;
                ptr += 2;
                skb_pull(skb, 5);
        }

        /*
         * Find the socket to which this skb is destined.
         */
        sk = dn_find_by_skb(skb);
got_it:
        if (sk != NULL) {
                struct dn_scp *scp = DN_SK(sk);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,0,0)
        	if (skb_dst(skb) != rcu_dereference_check(sk->sk_dst_cache, 1)) {
#else
        	if (skb_dst(skb) != rcu_dereference_protected(sk->sk_dst_cache, 1)) {
#endif
                        /*
                         * We may have a newer path to the remote system
                         * which takes the Intra-Ethernet bit into
                         * consideration. Switch the socket to this new
                         * path but only if we are in the RUN state - we want
                         * to avoid messing with sockets which are listening.
                         */
                        if (scp->state == DN_RUN)
                                __sk_dst_set(sk, dst_clone(skb_dst(skb)));
                }

                /* Reset backoff */
                scp->nsp_rxtshift = 0;

		/*
		 * Remember when we last received a message.
		 */
		scp->stamp = jiffies;

                /*
                 * We linearize everything except data segments here.
                 */
                if (cb->nsp_flags & ~0x60) {
                        if (unlikely(skb_linearize(skb)))
                                goto free_out;
                }

                return sk_receive_skb(sk, skb, 0);
        }

        return dn_nsp_no_socket(skb, reason);

free_out:
        kfree_skb(skb);
        return NET_RX_DROP;
}

int dn_nsp_rx(struct sk_buff *skb)
{
        return NF_HOOK(NFPROTO_DECNET, NF_DN_LOCAL_IN,
                       &init_net, NULL, skb, skb->dev, NULL,
                       dn_nsp_rx_packet);
}

/*
 * This is the main receive routine for sockets. It is called
 * from the above when the socket is not busy, and also from
 * sock_release() when there is a backlog queued up.
 */
int dn_nsp_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
        struct dn_scp *scp = DN_SK(sk);
        struct dn_skb_cb *cb = DN_SKB_CB(skb);

        if (cb->rt_flags & DN_RT_F_RTS) {
                kfree_skb(skb);
                return NET_RX_SUCCESS;
        }

        /*
         * Control packet.
         */
        if ((cb->nsp_flags & 0x0c) == 0x08) {
                switch (cb->nsp_flags & 0x70) {
                case 0x10:
                case 0x60:
                        dn_nsp_conn_init(sk, skb);
                        break;
                case 0x20:
                        dn_nsp_conn_conf(sk, skb);
                        break;
                case 0x30:
                        dn_nsp_disc_init(sk, skb);
                        break;
                case 0x40:
                        dn_nsp_disc_conf(sk, skb);
                        break;
                }

        } else if (cb->nsp_flags == 0x24) {
                /*
                 * Special for connacks, 'cos they don't have
                 * ack data or ack otherdata info.
                 */
                dn_nsp_conn_ack(sk, skb);
        } else {
                int other = 1;

                /* both data and ack frames can kick a CC socket into RUN */
                if ((scp->state == DN_CC) && !sock_flag(sk, SOCK_DEAD)) {
                        scp->state = DN_RUN;
                        sk->sk_state = TCP_ESTABLISHED;
                        sk->sk_state_change(sk);

                        /*
                         * If the data or ack frame was received with a short
                         * routing header or with the Intra-Ethernet bit
                         * clear, revert to the "SEGMENT BUFFER SIZE"
                         * parameter since traffic will be going off ethernet.
                         */
                        if (((cb->rt_flags & DN_RT_PKT_MSK) == DN_RT_PKT_SHORT) ||
                            ((cb->rt_flags & DN_RT_F_IE) == 0))
                                scp->segsize_rem =
                                  decnet_segbufsize - (DN_MAX_NSP_DATA_HEADER + 6);
                }

                if ((cb->nsp_flags & 0x1c) == 0)
                        other = 0;
                if (cb->nsp_flags == 0x04)
                        other = 0;

                /*
                 * Read out ack data here, this applies equally
                 * to data, other data, link serivce and both
                 * ack data and ack otherdata.
                 */
                dn_process_ack(sk, skb, other);

                /*
                 * If we've some sort of data here then call a
                 * suitable routine for dealing with it, otherwise
                 * the packet is an ack and can be discarded.
                 */
                if ((cb->nsp_flags & 0x0c) == 0) {

                        if (scp->state != DN_RUN)
                                goto free_out;

                        switch (cb->nsp_flags) {
                        case 0x10: /* LS */
                                dn_nsp_linkservice(sk, skb);
                                break;
                        case 0x30: /* OD */
                                dn_nsp_otherdata(sk, skb);
                                break;
                        default:
                                dn_nsp_data(sk, skb);
                        }

                } else { /* Ack, chuck it out here */
free_out:
                        kfree_skb(skb);
                }
        }

        return NET_RX_SUCCESS;
}
