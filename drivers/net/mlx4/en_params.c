/*
 * Copyright (c) 2007 Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
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
 *
 */

#include <linux/kernel.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>

#include "mlx4_en.h"
#include "en_port.h"

#define MLX4_EN_PARM_INT(X, def_val, desc) \
	static unsigned int X = def_val;\
	module_param(X , uint, 0444); \
	MODULE_PARM_DESC(X, desc);


/*
 * Device scope module parameters
 */


/* Use a XOR rathern than Toeplitz hash function for RSS */
MLX4_EN_PARM_INT(rss_xor, 0, "Use XOR hash function for RSS");

/* RSS hash type mask - default to <saddr, daddr, sport, dport> */
MLX4_EN_PARM_INT(rss_mask, 0xf, "RSS hash type bitmask");

/* Number of LRO sessions per Rx ring (rounded up to a power of two) */
MLX4_EN_PARM_INT(num_lro, MLX4_EN_MAX_LRO_DESCRIPTORS,
		 "Number of LRO sessions per ring or disabled (0)");

/* Priority pausing */
MLX4_EN_PARM_INT(pfctx, 0, "Priority based Flow Control policy on TX[7:0]."
			   " Per priority bit mask");
MLX4_EN_PARM_INT(pfcrx, 0, "Priority based Flow Control policy on RX[7:0]."
			   " Per priority bit mask");

MLX4_EN_PARM_INT(rx_ring_num1, 0, "Number or Rx rings for port 1 (0 = #cores)");
MLX4_EN_PARM_INT(rx_ring_num2, 0, "Number or Rx rings for port 2 (0 = #cores)");

MLX4_EN_PARM_INT(tx_ring_size1, MLX4_EN_AUTO_CONF, "Tx ring size for port 1");
MLX4_EN_PARM_INT(tx_ring_size2, MLX4_EN_AUTO_CONF, "Tx ring size for port 2");
MLX4_EN_PARM_INT(rx_ring_size1, MLX4_EN_AUTO_CONF, "Rx ring size for port 1");
MLX4_EN_PARM_INT(rx_ring_size2, MLX4_EN_AUTO_CONF, "Rx ring size for port 2");


int mlx4_en_get_profile(struct mlx4_en_dev *mdev)
{
	struct mlx4_en_profile *params = &mdev->profile;
	int i;

	params->rss_xor = (rss_xor != 0);
	params->rss_mask = rss_mask & 0x1f;
	params->num_lro = min_t(int, num_lro , MLX4_EN_MAX_LRO_DESCRIPTORS);
	for (i = 1; i <= MLX4_MAX_PORTS; i++) {
		params->prof[i].rx_pause = 1;
		params->prof[i].rx_ppp = pfcrx;
		params->prof[i].tx_pause = 1;
		params->prof[i].tx_ppp = pfctx;
	}
	if (pfcrx || pfctx) {
		params->prof[1].tx_ring_num = MLX4_EN_TX_RING_NUM;
		params->prof[2].tx_ring_num = MLX4_EN_TX_RING_NUM;
	} else {
		params->prof[1].tx_ring_num = 1;
		params->prof[2].tx_ring_num = 1;
	}
	params->prof[1].rx_ring_num = min_t(int, rx_ring_num1, MAX_RX_RINGS);
	params->prof[2].rx_ring_num = min_t(int, rx_ring_num2, MAX_RX_RINGS);

	if (tx_ring_size1 == MLX4_EN_AUTO_CONF)
		tx_ring_size1 = MLX4_EN_DEF_TX_RING_SIZE;
	params->prof[1].tx_ring_size =
		(tx_ring_size1 < MLX4_EN_MIN_TX_SIZE) ?
		 MLX4_EN_MIN_TX_SIZE : roundup_pow_of_two(tx_ring_size1);

	if (tx_ring_size2 == MLX4_EN_AUTO_CONF)
		tx_ring_size2 = MLX4_EN_DEF_TX_RING_SIZE;
	params->prof[2].tx_ring_size =
		(tx_ring_size2 < MLX4_EN_MIN_TX_SIZE) ?
		 MLX4_EN_MIN_TX_SIZE : roundup_pow_of_two(tx_ring_size2);

	if (rx_ring_size1 == MLX4_EN_AUTO_CONF)
		rx_ring_size1 = MLX4_EN_DEF_RX_RING_SIZE;
	params->prof[1].rx_ring_size =
		(rx_ring_size1 < MLX4_EN_MIN_RX_SIZE) ?
		 MLX4_EN_MIN_RX_SIZE : roundup_pow_of_two(rx_ring_size1);

	if (rx_ring_size2 == MLX4_EN_AUTO_CONF)
		rx_ring_size2 = MLX4_EN_DEF_RX_RING_SIZE;
	params->prof[2].rx_ring_size =
		(rx_ring_size2 < MLX4_EN_MIN_RX_SIZE) ?
		 MLX4_EN_MIN_RX_SIZE : roundup_pow_of_two(rx_ring_size2);
	return 0;
}


/*
 * Ethtool support
 */

static void mlx4_en_update_lro_stats(struct mlx4_en_priv *priv)
{
	int i;

	priv->port_stats.lro_aggregated = 0;
	priv->port_stats.lro_flushed = 0;
	priv->port_stats.lro_no_desc = 0;

	for (i = 0; i < priv->rx_ring_num; i++) {
		priv->port_stats.lro_aggregated += priv->rx_ring[i].lro.stats.aggregated;
		priv->port_stats.lro_flushed += priv->rx_ring[i].lro.stats.flushed;
		priv->port_stats.lro_no_desc += priv->rx_ring[i].lro.stats.no_desc;
	}
}

static void
mlx4_en_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *drvinfo)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;

	sprintf(drvinfo->driver, DRV_NAME " (%s)", mdev->dev->board_id);
	strncpy(drvinfo->version, DRV_VERSION " (" DRV_RELDATE ")", 32);
	sprintf(drvinfo->fw_version, "%d.%d.%d",
		(u16) (mdev->dev->caps.fw_ver >> 32),
		(u16) ((mdev->dev->caps.fw_ver >> 16) & 0xffff),
		(u16) (mdev->dev->caps.fw_ver & 0xffff));
	strncpy(drvinfo->bus_info, pci_name(mdev->dev->pdev), 32);
	drvinfo->n_stats = 0;
	drvinfo->regdump_len = 0;
	drvinfo->eedump_len = 0;
}

static u32 mlx4_en_get_tso(struct net_device *dev)
{
	return (dev->features & NETIF_F_TSO) != 0;
}

static int mlx4_en_set_tso(struct net_device *dev, u32 data)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);

	if (data) {
		if (!priv->mdev->LSO_support)
			return -EPERM;
		dev->features |= (NETIF_F_TSO | NETIF_F_TSO6);
	} else
		dev->features &= ~(NETIF_F_TSO | NETIF_F_TSO6);
	return 0;
}

static u32 mlx4_en_get_rx_csum(struct net_device *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	return priv->rx_csum;
}

static int mlx4_en_set_rx_csum(struct net_device *dev, u32 data)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	priv->rx_csum = (data != 0);
	return 0;
}

static const char main_strings[][ETH_GSTRING_LEN] = {
	"rx_packets", "tx_packets", "rx_bytes", "tx_bytes", "rx_errors",
	"tx_errors", "rx_dropped", "tx_dropped", "multicast", "collisions",
	"rx_length_errors", "rx_over_errors", "rx_crc_errors",
	"rx_frame_errors", "rx_fifo_errors", "rx_missed_errors",
	"tx_aborted_errors", "tx_carrier_errors", "tx_fifo_errors",
	"tx_heartbeat_errors", "tx_window_errors",

	/* port statistics */
	"lro_aggregated", "lro_flushed", "lro_no_desc", "tso_packets",
	"queue_stopped", "wake_queue", "tx_timeout", "rx_alloc_failed",
	"rx_csum_good", "rx_csum_none", "tx_chksum_offload",

	/* packet statistics */
	"broadcast", "rx_prio_0", "rx_prio_1", "rx_prio_2", "rx_prio_3",
	"rx_prio_4", "rx_prio_5", "rx_prio_6", "rx_prio_7", "tx_prio_0",
	"tx_prio_1", "tx_prio_2", "tx_prio_3", "tx_prio_4", "tx_prio_5",
	"tx_prio_6", "tx_prio_7",
};
#define NUM_MAIN_STATS	21
#define NUM_ALL_STATS	(NUM_MAIN_STATS + NUM_PORT_STATS + NUM_PKT_STATS + NUM_PERF_STATS)

static u32 mlx4_en_get_msglevel(struct net_device *dev)
{
	return ((struct mlx4_en_priv *) netdev_priv(dev))->msg_enable;
}

static void mlx4_en_set_msglevel(struct net_device *dev, u32 val)
{
	((struct mlx4_en_priv *) netdev_priv(dev))->msg_enable = val;
}

static void mlx4_en_get_wol(struct net_device *netdev,
			    struct ethtool_wolinfo *wol)
{
	wol->supported = 0;
	wol->wolopts = 0;

	return;
}

static int mlx4_en_get_sset_count(struct net_device *dev, int sset)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	return NUM_ALL_STATS + (priv->tx_ring_num + priv->rx_ring_num) * 2;
}

static void mlx4_en_get_ethtool_stats(struct net_device *dev,
		struct ethtool_stats *stats, uint64_t *data)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	int index = 0;
	int i;

	spin_lock_bh(&priv->stats_lock);

	mlx4_en_update_lro_stats(priv);

	for (i = 0; i < NUM_MAIN_STATS; i++)
		data[index++] = ((unsigned long *) &priv->stats)[i];
	for (i = 0; i < NUM_PORT_STATS; i++)
		data[index++] = ((unsigned long *) &priv->port_stats)[i];
	for (i = 0; i < priv->tx_ring_num; i++) {
		data[index++] = priv->tx_ring[i].packets;
		data[index++] = priv->tx_ring[i].bytes;
	}
	for (i = 0; i < priv->rx_ring_num; i++) {
		data[index++] = priv->rx_ring[i].packets;
		data[index++] = priv->rx_ring[i].bytes;
	}
	for (i = 0; i < NUM_PKT_STATS; i++)
		data[index++] = ((unsigned long *) &priv->pkstats)[i];
	spin_unlock_bh(&priv->stats_lock);

}

static void mlx4_en_get_strings(struct net_device *dev,
				uint32_t stringset, uint8_t *data)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	int index = 0;
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	/* Add main counters */
	for (i = 0; i < NUM_MAIN_STATS; i++)
		strcpy(data + (index++) * ETH_GSTRING_LEN, main_strings[i]);
	for (i = 0; i < NUM_PORT_STATS; i++)
		strcpy(data + (index++) * ETH_GSTRING_LEN,
			main_strings[i + NUM_MAIN_STATS]);
	for (i = 0; i < priv->tx_ring_num; i++) {
		sprintf(data + (index++) * ETH_GSTRING_LEN,
			"tx%d_packets", i);
		sprintf(data + (index++) * ETH_GSTRING_LEN,
			"tx%d_bytes", i);
	}
	for (i = 0; i < priv->rx_ring_num; i++) {
		sprintf(data + (index++) * ETH_GSTRING_LEN,
			"rx%d_packets", i);
		sprintf(data + (index++) * ETH_GSTRING_LEN,
			"rx%d_bytes", i);
	}
	for (i = 0; i < NUM_PKT_STATS; i++)
		strcpy(data + (index++) * ETH_GSTRING_LEN,
			main_strings[i + NUM_MAIN_STATS + NUM_PORT_STATS]);
}

static int mlx4_en_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	cmd->autoneg = AUTONEG_DISABLE;
	cmd->supported = SUPPORTED_10000baseT_Full;
	cmd->advertising = SUPPORTED_10000baseT_Full;
	if (netif_carrier_ok(dev)) {
		cmd->speed = SPEED_10000;
		cmd->duplex = DUPLEX_FULL;
	} else {
		cmd->speed = -1;
		cmd->duplex = -1;
	}
	return 0;
}

static int mlx4_en_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	if ((cmd->autoneg == AUTONEG_ENABLE) ||
	    (cmd->speed != SPEED_10000) || (cmd->duplex != DUPLEX_FULL))
		return -EINVAL;

	/* Nothing to change */
	return 0;
}

static int mlx4_en_get_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *coal)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);

	coal->tx_coalesce_usecs = 0;
	coal->tx_max_coalesced_frames = 0;
	coal->rx_coalesce_usecs = priv->rx_usecs;
	coal->rx_max_coalesced_frames = priv->rx_frames;

	coal->pkt_rate_low = priv->pkt_rate_low;
	coal->rx_coalesce_usecs_low = priv->rx_usecs_low;
	coal->pkt_rate_high = priv->pkt_rate_high;
	coal->rx_coalesce_usecs_high = priv->rx_usecs_high;
	coal->rate_sample_interval = priv->sample_interval;
	coal->use_adaptive_rx_coalesce = priv->adaptive_rx_coal;
	return 0;
}

static int mlx4_en_set_coalesce(struct net_device *dev,
			      struct ethtool_coalesce *coal)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	int err, i;

	priv->rx_frames = (coal->rx_max_coalesced_frames ==
			   MLX4_EN_AUTO_CONF) ?
				MLX4_EN_RX_COAL_TARGET /
				priv->dev->mtu + 1 :
				coal->rx_max_coalesced_frames;
	priv->rx_usecs = (coal->rx_coalesce_usecs ==
			  MLX4_EN_AUTO_CONF) ?
				MLX4_EN_RX_COAL_TIME :
				coal->rx_coalesce_usecs;

	/* Set adaptive coalescing params */
	priv->pkt_rate_low = coal->pkt_rate_low;
	priv->rx_usecs_low = coal->rx_coalesce_usecs_low;
	priv->pkt_rate_high = coal->pkt_rate_high;
	priv->rx_usecs_high = coal->rx_coalesce_usecs_high;
	priv->sample_interval = coal->rate_sample_interval;
	priv->adaptive_rx_coal = coal->use_adaptive_rx_coalesce;
	priv->last_moder_time = MLX4_EN_AUTO_CONF;
	if (priv->adaptive_rx_coal)
		return 0;

	for (i = 0; i < priv->rx_ring_num; i++) {
		priv->rx_cq[i].moder_cnt = priv->rx_frames;
		priv->rx_cq[i].moder_time = priv->rx_usecs;
		err = mlx4_en_set_cq_moder(priv, &priv->rx_cq[i]);
		if (err)
			return err;
	}
	return 0;
}

static int mlx4_en_set_pauseparam(struct net_device *dev,
				struct ethtool_pauseparam *pause)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	int err;

	priv->prof->tx_pause = pause->tx_pause != 0;
	priv->prof->rx_pause = pause->rx_pause != 0;
	err = mlx4_SET_PORT_general(mdev->dev, priv->port,
				    priv->rx_skb_size + ETH_FCS_LEN,
				    priv->prof->tx_pause,
				    priv->prof->tx_ppp,
				    priv->prof->rx_pause,
				    priv->prof->rx_ppp);
	if (err)
		mlx4_err(mdev, "Failed setting pause params to\n");

	return err;
}

static void mlx4_en_get_pauseparam(struct net_device *dev,
				 struct ethtool_pauseparam *pause)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);

	pause->tx_pause = priv->prof->tx_pause;
	pause->rx_pause = priv->prof->rx_pause;
}

static void mlx4_en_get_ringparam(struct net_device *dev,
				  struct ethtool_ringparam *param)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;

	memset(param, 0, sizeof(*param));
	param->rx_max_pending = mdev->dev->caps.max_rq_sg;
	param->tx_max_pending = mdev->dev->caps.max_sq_sg;
	param->rx_pending = mdev->profile.prof[priv->port].rx_ring_size;
	param->tx_pending = mdev->profile.prof[priv->port].tx_ring_size;
}

const struct ethtool_ops mlx4_en_ethtool_ops = {
	.get_drvinfo = mlx4_en_get_drvinfo,
	.get_settings = mlx4_en_get_settings,
	.set_settings = mlx4_en_set_settings,
#ifdef NETIF_F_TSO
	.get_tso = mlx4_en_get_tso,
	.set_tso = mlx4_en_set_tso,
#endif
	.get_sg = ethtool_op_get_sg,
	.set_sg = ethtool_op_set_sg,
	.get_link = ethtool_op_get_link,
	.get_rx_csum = mlx4_en_get_rx_csum,
	.set_rx_csum = mlx4_en_set_rx_csum,
	.get_tx_csum = ethtool_op_get_tx_csum,
	.set_tx_csum = ethtool_op_set_tx_ipv6_csum,
	.get_strings = mlx4_en_get_strings,
	.get_sset_count = mlx4_en_get_sset_count,
	.get_ethtool_stats = mlx4_en_get_ethtool_stats,
	.get_wol = mlx4_en_get_wol,
	.get_msglevel = mlx4_en_get_msglevel,
	.set_msglevel = mlx4_en_set_msglevel,
	.get_coalesce = mlx4_en_get_coalesce,
	.set_coalesce = mlx4_en_set_coalesce,
	.get_pauseparam = mlx4_en_get_pauseparam,
	.set_pauseparam = mlx4_en_set_pauseparam,
	.get_ringparam = mlx4_en_get_ringparam,
	.get_flags = ethtool_op_get_flags,
	.set_flags = ethtool_op_set_flags,
};





