/*
 * File: pep-gprs.c
 *
 * GPRS over Phonet pipe end point socket
 *
 * Copyright (C) 2008 Nokia Corporation.
 *
 * Author: Rémi Denis-Courmont <remi.denis-courmont@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <net/sock.h>

#include <linux/if_phonet.h>
#include <net/tcp_states.h>
#include <net/phonet/gprs.h>

#define GPRS_DEFAULT_MTU 1400

struct gprs_dev {
	struct sock		*sk;
	void			(*old_state_change)(struct sock *);
	void			(*old_data_ready)(struct sock *, int);
	void			(*old_write_space)(struct sock *);

	struct net_device	*dev;
};

static __be16 gprs_type_trans(struct sk_buff *skb)
{
	const u8 *pvfc;
	u8 buf;

	pvfc = skb_header_pointer(skb, 0, 1, &buf);
	if (!pvfc)
		return htons(0);
	/* Look at IP version field */
	switch (*pvfc >> 4) {
	case 4:
		return htons(ETH_P_IP);
	case 6:
		return htons(ETH_P_IPV6);
	}
	return htons(0);
}

static void gprs_writeable(struct gprs_dev *gp)
{
	struct net_device *dev = gp->dev;

	if (pep_writeable(gp->sk))
		netif_wake_queue(dev);
}

/*
 * Socket callbacks
 */

static void gprs_state_change(struct sock *sk)
{
	struct gprs_dev *gp = sk->sk_user_data;

	if (sk->sk_state == TCP_CLOSE_WAIT) {
		struct net_device *dev = gp->dev;

		netif_stop_queue(dev);
		netif_carrier_off(dev);
	}
}

static int gprs_recv(struct gprs_dev *gp, struct sk_buff *skb)
{
	struct net_device *dev = gp->dev;
	int err = 0;
	__be16 protocol = gprs_type_trans(skb);

	if (!protocol) {
		err = -EINVAL;
		goto drop;
	}

	if (likely(skb_headroom(skb) & 3)) {
		struct sk_buff *rskb, *fs;
		int flen = 0;

		/* Phonet Pipe data header is misaligned (3 bytes),
		 * so wrap the IP packet as a single fragment of an head-less
		 * socket buffer. The network stack will pull what it needs,
		 * but at least, the whole IP payload is not memcpy'd. */
		rskb = netdev_alloc_skb(dev, 0);
		if (!rskb) {
			err = -ENOBUFS;
			goto drop;
		}
		skb_shinfo(rskb)->frag_list = skb;
		rskb->len += skb->len;
		rskb->data_len += rskb->len;
		rskb->truesize += rskb->len;

		/* Avoid nested fragments */
		for (fs = skb_shinfo(skb)->frag_list; fs; fs = fs->next)
			flen += fs->len;
		skb->next = skb_shinfo(skb)->frag_list;
		skb_shinfo(skb)->frag_list = NULL;
		skb->len -= flen;
		skb->data_len -= flen;
		skb->truesize -= flen;

		skb = rskb;
	}

	skb->protocol = protocol;
	skb_reset_mac_header(skb);
	skb->dev = dev;

	if (likely(dev->flags & IFF_UP)) {
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;
		netif_rx(skb);
		skb = NULL;
	} else
		err = -ENODEV;

drop:
	if (skb) {
		dev_kfree_skb(skb);
		dev->stats.rx_dropped++;
	}
	return err;
}

static void gprs_data_ready(struct sock *sk, int len)
{
	struct gprs_dev *gp = sk->sk_user_data;
	struct sk_buff *skb;

	while ((skb = pep_read(sk)) != NULL) {
		skb_orphan(skb);
		gprs_recv(gp, skb);
	}
}

static void gprs_write_space(struct sock *sk)
{
	struct gprs_dev *gp = sk->sk_user_data;

	if (netif_running(gp->dev))
		gprs_writeable(gp);
}

/*
 * Network device callbacks
 */

static int gprs_open(struct net_device *dev)
{
	struct gprs_dev *gp = netdev_priv(dev);

	gprs_writeable(gp);
	return 0;
}

static int gprs_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static int gprs_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct gprs_dev *gp = netdev_priv(dev);
	struct sock *sk = gp->sk;
	int len, err;

	switch (skb->protocol) {
	case  htons(ETH_P_IP):
	case  htons(ETH_P_IPV6):
		break;
	default:
		dev_kfree_skb(skb);
		return 0;
	}

	skb_orphan(skb);
	skb_set_owner_w(skb, sk);
	len = skb->len;
	err = pep_write(sk, skb);
	if (err) {
		LIMIT_NETDEBUG(KERN_WARNING"%s: TX error (%d)\n",
				dev->name, err);
		dev->stats.tx_aborted_errors++;
		dev->stats.tx_errors++;
		dev_kfree_skb(skb);
	} else {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += len;
	}

	if (!pep_writeable(sk))
		netif_stop_queue(dev);
	return 0;
}

static int gprs_set_mtu(struct net_device *dev, int new_mtu)
{
	if ((new_mtu < 576) || (new_mtu > (PHONET_MAX_MTU - 11)))
		return -EINVAL;

	dev->mtu = new_mtu;
	return 0;
}

static void gprs_setup(struct net_device *dev)
{
	dev->features		= NETIF_F_FRAGLIST;
	dev->type		= ARPHRD_PHONET_PIPE;
	dev->flags		= IFF_POINTOPOINT | IFF_NOARP;
	dev->mtu		= GPRS_DEFAULT_MTU;
	dev->hard_header_len	= 0;
	dev->addr_len		= 0;
	dev->tx_queue_len	= 10;

	dev->destructor		= free_netdev;
	dev->open		= gprs_open;
	dev->stop		= gprs_close;
	dev->hard_start_xmit	= gprs_xmit; /* mandatory */
	dev->change_mtu		= gprs_set_mtu;
}

/*
 * External interface
 */

/*
 * Attach a GPRS interface to a datagram socket.
 * Returns the interface index on success, negative error code on error.
 */
int gprs_attach(struct sock *sk)
{
	static const char ifname[] = "gprs%d";
	struct gprs_dev *gp;
	struct net_device *dev;
	int err;

	if (unlikely(sk->sk_type == SOCK_STREAM))
		return -EINVAL; /* need packet boundaries */

	/* Create net device */
	dev = alloc_netdev(sizeof(*gp), ifname, gprs_setup);
	if (!dev)
		return -ENOMEM;
	gp = netdev_priv(dev);
	gp->sk = sk;
	gp->dev = dev;

	netif_stop_queue(dev);
	err = register_netdev(dev);
	if (err) {
		free_netdev(dev);
		return err;
	}

	lock_sock(sk);
	if (unlikely(sk->sk_user_data)) {
		err = -EBUSY;
		goto out_rel;
	}
	if (unlikely((1 << sk->sk_state & (TCPF_CLOSE|TCPF_LISTEN)) ||
			sock_flag(sk, SOCK_DEAD))) {
		err = -EINVAL;
		goto out_rel;
	}
	sk->sk_user_data	= gp;
	gp->old_state_change	= sk->sk_state_change;
	gp->old_data_ready	= sk->sk_data_ready;
	gp->old_write_space	= sk->sk_write_space;
	sk->sk_state_change	= gprs_state_change;
	sk->sk_data_ready	= gprs_data_ready;
	sk->sk_write_space	= gprs_write_space;
	release_sock(sk);
	sock_hold(sk);

	printk(KERN_DEBUG"%s: attached\n", dev->name);
	return dev->ifindex;

out_rel:
	release_sock(sk);
	unregister_netdev(dev);
	return err;
}

void gprs_detach(struct sock *sk)
{
	struct gprs_dev *gp = sk->sk_user_data;
	struct net_device *dev = gp->dev;

	lock_sock(sk);
	sk->sk_user_data	= NULL;
	sk->sk_state_change	= gp->old_state_change;
	sk->sk_data_ready	= gp->old_data_ready;
	sk->sk_write_space	= gp->old_write_space;
	release_sock(sk);

	printk(KERN_DEBUG"%s: detached\n", dev->name);
	unregister_netdev(dev);
	sock_put(sk);
}
