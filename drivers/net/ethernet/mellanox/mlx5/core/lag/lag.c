/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
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
 */

#include <linux/netdevice.h>
#include <net/bonding.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/eswitch.h>
#include <linux/mlx5/vport.h>
#include "lib/devcom.h"
#include "mlx5_core.h"
#include "eswitch.h"
#include "esw/acl/ofld.h"
#include "lag.h"
#include "mp.h"
#include "mpesw.h"


/* General purpose, use for short periods of time.
 * Beware of lock dependencies (preferably, no locks should be acquired
 * under it).
 */
static DEFINE_SPINLOCK(lag_lock);

static int get_port_sel_mode(enum mlx5_lag_mode mode, unsigned long flags)
{
	if (test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &flags))
		return MLX5_LAG_PORT_SELECT_MODE_PORT_SELECT_FT;

	if (mode == MLX5_LAG_MODE_MPESW)
		return MLX5_LAG_PORT_SELECT_MODE_PORT_SELECT_MPESW;

	return MLX5_LAG_PORT_SELECT_MODE_QUEUE_AFFINITY;
}

static u8 lag_active_port_bits(struct mlx5_lag *ldev)
{
	u8 enabled_ports[MLX5_MAX_PORTS] = {};
	u8 active_port = 0;
	int num_enabled;
	int idx;

	mlx5_infer_tx_enabled(&ldev->tracker, ldev, enabled_ports,
			      &num_enabled);
	for (idx = 0; idx < num_enabled; idx++)
		active_port |= BIT_MASK(enabled_ports[idx]);

	return active_port;
}

static int mlx5_cmd_create_lag(struct mlx5_core_dev *dev, struct mlx5_lag *ldev,
			       int mode, unsigned long flags)
{
	bool fdb_sel_mode = test_bit(MLX5_LAG_MODE_FLAG_FDB_SEL_MODE_NATIVE,
				     &flags);
	int port_sel_mode = get_port_sel_mode(mode, flags);
	u32 in[MLX5_ST_SZ_DW(create_lag_in)] = {};
	u8 *ports = ldev->v2p_map;
	int idx0, idx1;
	void *lag_ctx;

	lag_ctx = MLX5_ADDR_OF(create_lag_in, in, ctx);
	MLX5_SET(create_lag_in, in, opcode, MLX5_CMD_OP_CREATE_LAG);
	MLX5_SET(lagc, lag_ctx, fdb_selection_mode, fdb_sel_mode);
	idx0 = mlx5_lag_get_dev_index_by_seq(ldev, 0);
	idx1 = mlx5_lag_get_dev_index_by_seq(ldev, 1);

	if (idx0 < 0 || idx1 < 0)
		return -EINVAL;

	switch (port_sel_mode) {
	case MLX5_LAG_PORT_SELECT_MODE_QUEUE_AFFINITY:
		if (MLX5_CAP_GEN(dev, lag_queue_affinity_active_port)) {
			MLX5_SET(lagc, lag_ctx, active_port,
				 lag_active_port_bits(mlx5_lag_dev(dev)));
		} else {
			MLX5_SET(lagc, lag_ctx,
				 tx_remap_affinity_1, ports[idx0]);
			MLX5_SET(lagc, lag_ctx,
				 tx_remap_affinity_2, ports[idx1]);
		}
		break;
	case MLX5_LAG_PORT_SELECT_MODE_PORT_SELECT_FT:
		if (!MLX5_CAP_PORT_SELECTION(dev, port_select_flow_table_bypass))
			break;

		MLX5_SET(lagc, lag_ctx, active_port,
			 lag_active_port_bits(mlx5_lag_dev(dev)));
		break;
	default:
		break;
	}
	MLX5_SET(lagc, lag_ctx, port_select_mode, port_sel_mode);

	return mlx5_cmd_exec_in(dev, create_lag, in);
}

static int mlx5_cmd_modify_lag(struct mlx5_core_dev *dev, struct mlx5_lag *ldev,
			       u8 *ports)
{
	u32 in[MLX5_ST_SZ_DW(modify_lag_in)] = {};
	void *lag_ctx = MLX5_ADDR_OF(modify_lag_in, in, ctx);
	int idx0, idx1;

	idx0 = mlx5_lag_get_dev_index_by_seq(ldev, 0);
	idx1 = mlx5_lag_get_dev_index_by_seq(ldev, 1);
	if (idx0 < 0 || idx1 < 0)
		return -EINVAL;

	MLX5_SET(modify_lag_in, in, opcode, MLX5_CMD_OP_MODIFY_LAG);

	if (MLX5_CAP_GEN(dev, lag_queue_affinity_active_port)) {
		MLX5_SET(modify_lag_in, in, field_select, 0x2);
		MLX5_SET(lagc, lag_ctx, active_port,
			 lag_active_port_bits(mlx5_lag_dev(dev)));
	} else {
		MLX5_SET(modify_lag_in, in, field_select, 0x1);
		MLX5_SET(lagc, lag_ctx, tx_remap_affinity_1, ports[idx0]);
		MLX5_SET(lagc, lag_ctx, tx_remap_affinity_2, ports[idx1]);
	}

	return mlx5_cmd_exec_in(dev, modify_lag, in);
}

int mlx5_cmd_create_vport_lag(struct mlx5_core_dev *dev)
{
	u32 in[MLX5_ST_SZ_DW(create_vport_lag_in)] = {};

	MLX5_SET(create_vport_lag_in, in, opcode, MLX5_CMD_OP_CREATE_VPORT_LAG);

	return mlx5_cmd_exec_in(dev, create_vport_lag, in);
}
EXPORT_SYMBOL(mlx5_cmd_create_vport_lag);

int mlx5_cmd_destroy_vport_lag(struct mlx5_core_dev *dev)
{
	u32 in[MLX5_ST_SZ_DW(destroy_vport_lag_in)] = {};

	MLX5_SET(destroy_vport_lag_in, in, opcode, MLX5_CMD_OP_DESTROY_VPORT_LAG);

	return mlx5_cmd_exec_in(dev, destroy_vport_lag, in);
}
EXPORT_SYMBOL(mlx5_cmd_destroy_vport_lag);

static void mlx5_infer_tx_disabled(struct lag_tracker *tracker, struct mlx5_lag *ldev,
				   u8 *ports, int *num_disabled)
{
	int i;

	*num_disabled = 0;
	ldev_for_each(i, 0, ldev)
		if (!tracker->netdev_state[i].tx_enabled ||
		    !tracker->netdev_state[i].link_up)
			ports[(*num_disabled)++] = i;
}

void mlx5_infer_tx_enabled(struct lag_tracker *tracker, struct mlx5_lag *ldev,
			   u8 *ports, int *num_enabled)
{
	int i;

	*num_enabled = 0;
	ldev_for_each(i, 0, ldev)
		if (tracker->netdev_state[i].tx_enabled &&
		    tracker->netdev_state[i].link_up)
			ports[(*num_enabled)++] = i;

	if (*num_enabled == 0)
		mlx5_infer_tx_disabled(tracker, ldev, ports, num_enabled);
}

static void mlx5_lag_print_mapping(struct mlx5_core_dev *dev,
				   struct mlx5_lag *ldev,
				   struct lag_tracker *tracker,
				   unsigned long flags)
{
	char buf[MLX5_MAX_PORTS * 10 + 1] = {};
	u8 enabled_ports[MLX5_MAX_PORTS] = {};
	int written = 0;
	int num_enabled;
	int idx;
	int err;
	int i;
	int j;

	if (test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &flags)) {
		mlx5_infer_tx_enabled(tracker, ldev, enabled_ports,
				      &num_enabled);
		for (i = 0; i < num_enabled; i++) {
			err = scnprintf(buf + written, 4, "%d, ", enabled_ports[i] + 1);
			if (err != 3)
				return;
			written += err;
		}
		buf[written - 2] = 0;
		mlx5_core_info(dev, "lag map active ports: %s\n", buf);
	} else {
		ldev_for_each(i, 0, ldev) {
			for (j  = 0; j < ldev->buckets; j++) {
				idx = i * ldev->buckets + j;
				err = scnprintf(buf + written, 10,
						" port %d:%d", i + 1, ldev->v2p_map[idx]);
				if (err != 9)
					return;
				written += err;
			}
		}
		mlx5_core_info(dev, "lag map:%s\n", buf);
	}
}

static int mlx5_lag_netdev_event(struct notifier_block *this,
				 unsigned long event, void *ptr);
static void mlx5_do_bond_work(struct work_struct *work);

static void mlx5_ldev_free(struct kref *ref)
{
	struct mlx5_lag *ldev = container_of(ref, struct mlx5_lag, ref);

	if (ldev->nb.notifier_call)
		unregister_netdevice_notifier_net(&init_net, &ldev->nb);
	mlx5_lag_mp_cleanup(ldev);
	cancel_delayed_work_sync(&ldev->bond_work);
	destroy_workqueue(ldev->wq);
	mutex_destroy(&ldev->lock);
	kfree(ldev);
}

static void mlx5_ldev_put(struct mlx5_lag *ldev)
{
	kref_put(&ldev->ref, mlx5_ldev_free);
}

static void mlx5_ldev_get(struct mlx5_lag *ldev)
{
	kref_get(&ldev->ref);
}

static struct mlx5_lag *mlx5_lag_dev_alloc(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	int err;

	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	if (!ldev)
		return NULL;

	ldev->wq = create_singlethread_workqueue("mlx5_lag");
	if (!ldev->wq) {
		kfree(ldev);
		return NULL;
	}

	kref_init(&ldev->ref);
	mutex_init(&ldev->lock);
	INIT_DELAYED_WORK(&ldev->bond_work, mlx5_do_bond_work);

	ldev->nb.notifier_call = mlx5_lag_netdev_event;
	if (register_netdevice_notifier_net(&init_net, &ldev->nb)) {
		ldev->nb.notifier_call = NULL;
		mlx5_core_err(dev, "Failed to register LAG netdev notifier\n");
	}
	ldev->mode = MLX5_LAG_MODE_NONE;

	err = mlx5_lag_mp_init(ldev);
	if (err)
		mlx5_core_err(dev, "Failed to init multipath lag err=%d\n",
			      err);

	ldev->ports = MLX5_CAP_GEN(dev, num_lag_ports);
	ldev->buckets = 1;

	return ldev;
}

int mlx5_lag_dev_get_netdev_idx(struct mlx5_lag *ldev,
				struct net_device *ndev)
{
	int i;

	ldev_for_each(i, 0, ldev)
		if (ldev->pf[i].netdev == ndev)
			return i;

	return -ENOENT;
}

int mlx5_lag_get_dev_index_by_seq(struct mlx5_lag *ldev, int seq)
{
	int i, num = 0;

	if (!ldev)
		return -ENOENT;

	ldev_for_each(i, 0, ldev) {
		if (num == seq)
			return i;
		num++;
	}
	return -ENOENT;
}

int mlx5_lag_num_devs(struct mlx5_lag *ldev)
{
	int i, num = 0;

	if (!ldev)
		return 0;

	ldev_for_each(i, 0, ldev) {
		(void)i;
		num++;
	}
	return num;
}

int mlx5_lag_num_netdevs(struct mlx5_lag *ldev)
{
	int i, num = 0;

	if (!ldev)
		return 0;

	ldev_for_each(i, 0, ldev)
		if (ldev->pf[i].netdev)
			num++;
	return num;
}

static bool __mlx5_lag_is_roce(struct mlx5_lag *ldev)
{
	return ldev->mode == MLX5_LAG_MODE_ROCE;
}

static bool __mlx5_lag_is_sriov(struct mlx5_lag *ldev)
{
	return ldev->mode == MLX5_LAG_MODE_SRIOV;
}

/* Create a mapping between steering slots and active ports.
 * As we have ldev->buckets slots per port first assume the native
 * mapping should be used.
 * If there are ports that are disabled fill the relevant slots
 * with mapping that points to active ports.
 */
static void mlx5_infer_tx_affinity_mapping(struct lag_tracker *tracker,
					   struct mlx5_lag *ldev,
					   u8 buckets,
					   u8 *ports)
{
	int disabled[MLX5_MAX_PORTS] = {};
	int enabled[MLX5_MAX_PORTS] = {};
	int disabled_ports_num = 0;
	int enabled_ports_num = 0;
	int idx;
	u32 rand;
	int i;
	int j;

	ldev_for_each(i, 0, ldev) {
		if (tracker->netdev_state[i].tx_enabled &&
		    tracker->netdev_state[i].link_up)
			enabled[enabled_ports_num++] = i;
		else
			disabled[disabled_ports_num++] = i;
	}

	/* Use native mapping by default where each port's buckets
	 * point the native port: 1 1 1 .. 1 2 2 2 ... 2 3 3 3 ... 3 etc
	 */
	ldev_for_each(i, 0, ldev) {
		for (j = 0; j < buckets; j++) {
			idx = i * buckets + j;
			ports[idx] = i + 1;
		}
	}

	/* If all ports are disabled/enabled keep native mapping */
	if (enabled_ports_num == ldev->ports ||
	    disabled_ports_num == ldev->ports)
		return;

	/* Go over the disabled ports and for each assign a random active port */
	for (i = 0; i < disabled_ports_num; i++) {
		for (j = 0; j < buckets; j++) {
			get_random_bytes(&rand, 4);
			ports[disabled[i] * buckets + j] = enabled[rand % enabled_ports_num] + 1;
		}
	}
}

static bool mlx5_lag_has_drop_rule(struct mlx5_lag *ldev)
{
	int i;

	ldev_for_each(i, 0, ldev)
		if (ldev->pf[i].has_drop)
			return true;
	return false;
}

static void mlx5_lag_drop_rule_cleanup(struct mlx5_lag *ldev)
{
	int i;

	ldev_for_each(i, 0, ldev) {
		if (!ldev->pf[i].has_drop)
			continue;

		mlx5_esw_acl_ingress_vport_drop_rule_destroy(ldev->pf[i].dev->priv.eswitch,
							     MLX5_VPORT_UPLINK);
		ldev->pf[i].has_drop = false;
	}
}

static void mlx5_lag_drop_rule_setup(struct mlx5_lag *ldev,
				     struct lag_tracker *tracker)
{
	u8 disabled_ports[MLX5_MAX_PORTS] = {};
	struct mlx5_core_dev *dev;
	int disabled_index;
	int num_disabled;
	int err;
	int i;

	/* First delete the current drop rule so there won't be any dropped
	 * packets
	 */
	mlx5_lag_drop_rule_cleanup(ldev);

	if (!ldev->tracker.has_inactive)
		return;

	mlx5_infer_tx_disabled(tracker, ldev, disabled_ports, &num_disabled);

	for (i = 0; i < num_disabled; i++) {
		disabled_index = disabled_ports[i];
		dev = ldev->pf[disabled_index].dev;
		err = mlx5_esw_acl_ingress_vport_drop_rule_create(dev->priv.eswitch,
								  MLX5_VPORT_UPLINK);
		if (!err)
			ldev->pf[disabled_index].has_drop = true;
		else
			mlx5_core_err(dev,
				      "Failed to create lag drop rule, error: %d", err);
	}
}

static int mlx5_cmd_modify_active_port(struct mlx5_core_dev *dev, u8 ports)
{
	u32 in[MLX5_ST_SZ_DW(modify_lag_in)] = {};
	void *lag_ctx;

	lag_ctx = MLX5_ADDR_OF(modify_lag_in, in, ctx);

	MLX5_SET(modify_lag_in, in, opcode, MLX5_CMD_OP_MODIFY_LAG);
	MLX5_SET(modify_lag_in, in, field_select, 0x2);

	MLX5_SET(lagc, lag_ctx, active_port, ports);

	return mlx5_cmd_exec_in(dev, modify_lag, in);
}

static int _mlx5_modify_lag(struct mlx5_lag *ldev, u8 *ports)
{
	int idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_core_dev *dev0;
	u8 active_ports;
	int ret;

	if (idx < 0)
		return -EINVAL;

	dev0 = ldev->pf[idx].dev;
	if (test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &ldev->mode_flags)) {
		ret = mlx5_lag_port_sel_modify(ldev, ports);
		if (ret ||
		    !MLX5_CAP_PORT_SELECTION(dev0, port_select_flow_table_bypass))
			return ret;

		active_ports = lag_active_port_bits(ldev);

		return mlx5_cmd_modify_active_port(dev0, active_ports);
	}
	return mlx5_cmd_modify_lag(dev0, ldev, ports);
}

static struct net_device *mlx5_lag_active_backup_get_netdev(struct mlx5_core_dev *dev)
{
	struct net_device *ndev = NULL;
	struct mlx5_lag *ldev;
	unsigned long flags;
	int i, last_idx;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);

	if (!ldev)
		goto unlock;

	ldev_for_each(i, 0, ldev)
		if (ldev->tracker.netdev_state[i].tx_enabled)
			ndev = ldev->pf[i].netdev;
	if (!ndev) {
		last_idx = mlx5_lag_get_dev_index_by_seq(ldev, ldev->ports - 1);
		if (last_idx < 0)
			goto unlock;
		ndev = ldev->pf[last_idx].netdev;
	}

	if (ndev)
		dev_hold(ndev);

unlock:
	spin_unlock_irqrestore(&lag_lock, flags);

	return ndev;
}

void mlx5_modify_lag(struct mlx5_lag *ldev,
		     struct lag_tracker *tracker)
{
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	u8 ports[MLX5_MAX_PORTS * MLX5_LAG_MAX_HASH_BUCKETS] = {};
	struct mlx5_core_dev *dev0;
	int idx;
	int err;
	int i;
	int j;

	if (first_idx < 0)
		return;

	dev0 = ldev->pf[first_idx].dev;
	mlx5_infer_tx_affinity_mapping(tracker, ldev, ldev->buckets, ports);

	ldev_for_each(i, 0, ldev) {
		for (j = 0; j < ldev->buckets; j++) {
			idx = i * ldev->buckets + j;
			if (ports[idx] == ldev->v2p_map[idx])
				continue;
			err = _mlx5_modify_lag(ldev, ports);
			if (err) {
				mlx5_core_err(dev0,
					      "Failed to modify LAG (%d)\n",
					      err);
				return;
			}
			memcpy(ldev->v2p_map, ports, sizeof(ports));

			mlx5_lag_print_mapping(dev0, ldev, tracker,
					       ldev->mode_flags);
			break;
		}
	}

	if (tracker->tx_type == NETDEV_LAG_TX_TYPE_ACTIVEBACKUP) {
		struct net_device *ndev = mlx5_lag_active_backup_get_netdev(dev0);

		if(!(ldev->mode == MLX5_LAG_MODE_ROCE))
			mlx5_lag_drop_rule_setup(ldev, tracker);
		/** Only sriov and roce lag should have tracker->tx_type set so
		 *  no need to check the mode
		 */
		blocking_notifier_call_chain(&dev0->priv.lag_nh,
					     MLX5_DRIVER_EVENT_ACTIVE_BACKUP_LAG_CHANGE_LOWERSTATE,
					     ndev);
		dev_put(ndev);
	}
}

enum mlx5_lag_user_pref mlx5_lag_get_user_mode(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev = dev->priv.lag;
	int i;

	ldev_for_each(i, 0, ldev)
		if (ldev->pf[i].dev == dev)
			break;
	return ldev->pf[i].user_mode;
}

void mlx5_lag_set_user_mode(struct mlx5_core_dev *dev,
			    enum mlx5_lag_user_pref mode)
{
	struct mlx5_lag *ldev = dev->priv.lag;
	int i;

	ldev_for_each(i, 0, ldev)
		if (ldev->pf[i].dev == dev)
			break;
	ldev->pf[i].user_mode = mode;
}

static int mlx5_lag_set_port_sel_mode(struct mlx5_lag *ldev,
				      enum mlx5_lag_mode mode,
				      unsigned long *flags)
{
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_core_dev *dev0;

	if (first_idx < 0)
		return -EINVAL;

	if (mode == MLX5_LAG_MODE_MPESW ||
	    mode == MLX5_LAG_MODE_MULTIPATH)
		return 0;

	dev0 = ldev->pf[first_idx].dev;

	if (!MLX5_CAP_PORT_SELECTION(dev0, port_select_flow_table)) {
		if (ldev->ports > 2)
			return -EINVAL;
		return 0;
	}

	if (ldev->ports > 2)
		ldev->buckets = MLX5_LAG_MAX_HASH_BUCKETS;

	set_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, flags);

	return 0;
}

static int mlx5_lag_set_flags(struct mlx5_lag *ldev, enum mlx5_lag_mode mode,
			      struct lag_tracker *tracker, bool shared_fdb,
			      unsigned long *flags)
{
	struct lag_func *dev0, *dev1;
	int idx0, idx1;

	idx0 = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	idx1 = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P2);

	if (idx0 < 0 || idx1 < 0)
		return -EINVAL;

	dev0 = &ldev->pf[idx0];
	dev1 = &ldev->pf[idx1];

	*flags = 0;
	if (shared_fdb) {
		set_bit(MLX5_LAG_MODE_FLAG_SHARED_FDB, flags);
		set_bit(MLX5_LAG_MODE_FLAG_FDB_SEL_MODE_NATIVE, flags);
	}

	if (dev0->user_mode != dev1->user_mode) {
		mlx5_core_err(dev0->dev,
			      "LAG port selection mode must be the same for both devices\n");
		return -EINVAL;
	}

	if (dev0->user_mode == MLX5_LAG_USER_PREF_MODE_QUEUE_AFFINITY) {
		if (ldev->ports > 2) {
			mlx5_core_err(dev0->dev,
				      "LAG on NICs with more than 2 ports does not support queue affinity\n");
			return -EINVAL;
		}

		return 0;
	}

	if (mode == MLX5_LAG_MODE_MPESW)
		set_bit(MLX5_LAG_MODE_FLAG_FDB_SEL_MODE_NATIVE, flags);

	return mlx5_lag_set_port_sel_mode(ldev, mode, flags);
}

char *mlx5_get_str_port_sel_mode(enum mlx5_lag_mode mode, unsigned long flags)
{
	int port_sel_mode = get_port_sel_mode(mode, flags);

	switch (port_sel_mode) {
	case MLX5_LAG_PORT_SELECT_MODE_QUEUE_AFFINITY: return "queue_affinity";
	case MLX5_LAG_PORT_SELECT_MODE_PORT_SELECT_FT: return "hash";
	case MLX5_LAG_PORT_SELECT_MODE_PORT_SELECT_MPESW: return "mpesw";
	default: return "invalid";
	}
}

static int mlx5_lag_create_single_fdb(struct mlx5_lag *ldev)
{
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_eswitch *master_esw;
	struct mlx5_core_dev *dev0;
	int i, j;
	int err;

	if (first_idx < 0)
		return -EINVAL;

	dev0 = ldev->pf[first_idx].dev;
	master_esw = dev0->priv.eswitch;
	ldev_for_each(i, first_idx + 1, ldev) {
		struct mlx5_eswitch *slave_esw = ldev->pf[i].dev->priv.eswitch;

		err = mlx5_eswitch_offloads_single_fdb_add_one(master_esw,
							       slave_esw, ldev->ports);
		if (err)
			goto err;
	}
	return 0;
err:
	ldev_for_each_reverse(j, i, first_idx + 1, ldev)
		mlx5_eswitch_offloads_single_fdb_del_one(master_esw,
							 ldev->pf[j].dev->priv.eswitch);
	return err;
}

static int mlx5_create_lag(struct mlx5_lag *ldev,
			   struct lag_tracker *tracker,
			   enum mlx5_lag_mode mode,
			   unsigned long flags)
{
	bool shared_fdb = test_bit(MLX5_LAG_MODE_FLAG_SHARED_FDB, &flags);
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	u32 in[MLX5_ST_SZ_DW(destroy_lag_in)] = {};
	struct mlx5_core_dev *dev0;
	int err;

	if (first_idx < 0)
		return -EINVAL;

	dev0 = ldev->pf[first_idx].dev;
	if (tracker)
		mlx5_lag_print_mapping(dev0, ldev, tracker, flags);
	mlx5_core_info(dev0, "shared_fdb:%d mode:%s\n",
		       shared_fdb, mlx5_get_str_port_sel_mode(mode, flags));

	err = mlx5_cmd_create_lag(dev0, ldev, mode, flags);
	if (err) {
		mlx5_core_err(dev0,
			      "Failed to create LAG (%d)\n",
			      err);
		return err;
	}

	if (shared_fdb) {
		err = mlx5_lag_create_single_fdb(ldev);
		if (err)
			mlx5_core_err(dev0, "Can't enable single FDB mode\n");
		else
			mlx5_core_info(dev0, "Operation mode is single FDB\n");
	}

	if (err) {
		MLX5_SET(destroy_lag_in, in, opcode, MLX5_CMD_OP_DESTROY_LAG);
		if (mlx5_cmd_exec_in(dev0, destroy_lag, in))
			mlx5_core_err(dev0,
				      "Failed to deactivate RoCE LAG; driver restart required\n");
	}
	BLOCKING_INIT_NOTIFIER_HEAD(&dev0->priv.lag_nh);

	return err;
}

int mlx5_activate_lag(struct mlx5_lag *ldev,
		      struct lag_tracker *tracker,
		      enum mlx5_lag_mode mode,
		      bool shared_fdb)
{
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	bool roce_lag = mode == MLX5_LAG_MODE_ROCE;
	struct mlx5_core_dev *dev0;
	unsigned long flags = 0;
	int err;

	if (first_idx < 0)
		return -EINVAL;

	dev0 = ldev->pf[first_idx].dev;
	err = mlx5_lag_set_flags(ldev, mode, tracker, shared_fdb, &flags);
	if (err)
		return err;

	if (mode != MLX5_LAG_MODE_MPESW) {
		mlx5_infer_tx_affinity_mapping(tracker, ldev, ldev->buckets, ldev->v2p_map);
		if (test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &flags)) {
			err = mlx5_lag_port_sel_create(ldev, tracker->hash_type,
						       ldev->v2p_map);
			if (err) {
				mlx5_core_err(dev0,
					      "Failed to create LAG port selection(%d)\n",
					      err);
				return err;
			}
		}
	}

	err = mlx5_create_lag(ldev, tracker, mode, flags);
	if (err) {
		if (test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &flags))
			mlx5_lag_port_sel_destroy(ldev);
		if (roce_lag)
			mlx5_core_err(dev0,
				      "Failed to activate RoCE LAG\n");
		else
			mlx5_core_err(dev0,
				      "Failed to activate VF LAG\n"
				      "Make sure all VFs are unbound prior to VF LAG activation or deactivation\n");
		return err;
	}

	if (tracker && tracker->tx_type == NETDEV_LAG_TX_TYPE_ACTIVEBACKUP &&
	    !roce_lag)
		mlx5_lag_drop_rule_setup(ldev, tracker);

	ldev->mode = mode;
	ldev->mode_flags = flags;
	return 0;
}

int mlx5_deactivate_lag(struct mlx5_lag *ldev)
{
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	u32 in[MLX5_ST_SZ_DW(destroy_lag_in)] = {};
	bool roce_lag = __mlx5_lag_is_roce(ldev);
	unsigned long flags = ldev->mode_flags;
	struct mlx5_eswitch *master_esw;
	struct mlx5_core_dev *dev0;
	int err;
	int i;

	if (first_idx < 0)
		return -EINVAL;

	dev0 = ldev->pf[first_idx].dev;
	master_esw = dev0->priv.eswitch;
	ldev->mode = MLX5_LAG_MODE_NONE;
	ldev->mode_flags = 0;
	mlx5_lag_mp_reset(ldev);

	if (test_bit(MLX5_LAG_MODE_FLAG_SHARED_FDB, &flags)) {
		ldev_for_each(i, first_idx + 1, ldev)
			mlx5_eswitch_offloads_single_fdb_del_one(master_esw,
								 ldev->pf[i].dev->priv.eswitch);
		clear_bit(MLX5_LAG_MODE_FLAG_SHARED_FDB, &flags);
	}

	MLX5_SET(destroy_lag_in, in, opcode, MLX5_CMD_OP_DESTROY_LAG);
	err = mlx5_cmd_exec_in(dev0, destroy_lag, in);
	if (err) {
		if (roce_lag) {
			mlx5_core_err(dev0,
				      "Failed to deactivate RoCE LAG; driver restart required\n");
		} else {
			mlx5_core_err(dev0,
				      "Failed to deactivate VF LAG; driver restart required\n"
				      "Make sure all VFs are unbound prior to VF LAG activation or deactivation\n");
		}
		return err;
	}

	if (test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &flags)) {
		mlx5_lag_port_sel_destroy(ldev);
		ldev->buckets = 1;
	}
	if (mlx5_lag_has_drop_rule(ldev))
		mlx5_lag_drop_rule_cleanup(ldev);

	return 0;
}

bool mlx5_lag_check_prereq(struct mlx5_lag *ldev)
{
	int first_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
#ifdef CONFIG_MLX5_ESWITCH
	struct mlx5_core_dev *dev;
	u8 mode;
#endif
	bool roce_support;
	int i;

	if (first_idx < 0 || mlx5_lag_num_devs(ldev) != ldev->ports)
		return false;

#ifdef CONFIG_MLX5_ESWITCH
	ldev_for_each(i, 0, ldev) {
		dev = ldev->pf[i].dev;
		if (mlx5_eswitch_num_vfs(dev->priv.eswitch) && !is_mdev_switchdev_mode(dev))
			return false;
	}

	dev = ldev->pf[first_idx].dev;
	mode = mlx5_eswitch_mode(dev);
	ldev_for_each(i, 0, ldev)
		if (mlx5_eswitch_mode(ldev->pf[i].dev) != mode)
			return false;

#else
	ldev_for_each(i, 0, ldev)
		if (mlx5_sriov_is_enabled(ldev->pf[i].dev))
			return false;
#endif
	roce_support = mlx5_get_roce_state(ldev->pf[first_idx].dev);
	ldev_for_each(i, first_idx + 1, ldev)
		if (mlx5_get_roce_state(ldev->pf[i].dev) != roce_support)
			return false;

	return true;
}

void mlx5_lag_add_devices(struct mlx5_lag *ldev)
{
	int i;

	ldev_for_each(i, 0, ldev) {
		if (ldev->pf[i].dev->priv.flags &
		    MLX5_PRIV_FLAGS_DISABLE_ALL_ADEV)
			continue;

		ldev->pf[i].dev->priv.flags &= ~MLX5_PRIV_FLAGS_DISABLE_IB_ADEV;
		mlx5_rescan_drivers_locked(ldev->pf[i].dev);
	}
}

void mlx5_lag_remove_devices(struct mlx5_lag *ldev)
{
	int i;

	ldev_for_each(i, 0, ldev) {
		if (ldev->pf[i].dev->priv.flags &
		    MLX5_PRIV_FLAGS_DISABLE_ALL_ADEV)
			continue;

		ldev->pf[i].dev->priv.flags |= MLX5_PRIV_FLAGS_DISABLE_IB_ADEV;
		mlx5_rescan_drivers_locked(ldev->pf[i].dev);
	}
}

void mlx5_disable_lag(struct mlx5_lag *ldev)
{
	bool shared_fdb = test_bit(MLX5_LAG_MODE_FLAG_SHARED_FDB, &ldev->mode_flags);
	int idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_core_dev *dev0;
	bool roce_lag;
	int err;
	int i;

	if (idx < 0)
		return;

	dev0 = ldev->pf[idx].dev;
	roce_lag = __mlx5_lag_is_roce(ldev);

	if (shared_fdb) {
		mlx5_lag_remove_devices(ldev);
	} else if (roce_lag) {
		if (!(dev0->priv.flags & MLX5_PRIV_FLAGS_DISABLE_ALL_ADEV)) {
			dev0->priv.flags |= MLX5_PRIV_FLAGS_DISABLE_IB_ADEV;
			mlx5_rescan_drivers_locked(dev0);
		}
		ldev_for_each(i, idx + 1, ldev)
			mlx5_nic_vport_disable_roce(ldev->pf[i].dev);
	}

	err = mlx5_deactivate_lag(ldev);
	if (err)
		return;

	if (shared_fdb || roce_lag)
		mlx5_lag_add_devices(ldev);

	if (shared_fdb)
		ldev_for_each(i, 0, ldev)
			if (!(ldev->pf[i].dev->priv.flags & MLX5_PRIV_FLAGS_DISABLE_ALL_ADEV))
				mlx5_eswitch_reload_ib_reps(ldev->pf[i].dev->priv.eswitch);
}

bool mlx5_lag_shared_fdb_supported(struct mlx5_lag *ldev)
{
	int idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_core_dev *dev;
	int i;

	if (idx < 0)
		return false;

	ldev_for_each(i, idx + 1, ldev) {
		dev = ldev->pf[i].dev;
		if (is_mdev_switchdev_mode(dev) &&
		    mlx5_eswitch_vport_match_metadata_enabled(dev->priv.eswitch) &&
		    MLX5_CAP_GEN(dev, lag_native_fdb_selection) &&
		    MLX5_CAP_ESW(dev, root_ft_on_other_esw) &&
		    mlx5_eswitch_get_npeers(dev->priv.eswitch) ==
		    MLX5_CAP_GEN(dev, num_lag_ports) - 1)
			continue;
		return false;
	}

	dev = ldev->pf[idx].dev;
	if (is_mdev_switchdev_mode(dev) &&
	    mlx5_eswitch_vport_match_metadata_enabled(dev->priv.eswitch) &&
	    mlx5_esw_offloads_devcom_is_ready(dev->priv.eswitch) &&
	    MLX5_CAP_ESW(dev, esw_shared_ingress_acl) &&
	    mlx5_eswitch_get_npeers(dev->priv.eswitch) == MLX5_CAP_GEN(dev, num_lag_ports) - 1)
		return true;

	return false;
}

static bool mlx5_lag_is_roce_lag(struct mlx5_lag *ldev)
{
	bool roce_lag = true;
	int i;

	ldev_for_each(i, 0, ldev)
		roce_lag = roce_lag && !mlx5_sriov_is_enabled(ldev->pf[i].dev);

#ifdef CONFIG_MLX5_ESWITCH
	ldev_for_each(i, 0, ldev)
		roce_lag = roce_lag && is_mdev_legacy_mode(ldev->pf[i].dev);
#endif

	return roce_lag;
}

static bool mlx5_lag_should_modify_lag(struct mlx5_lag *ldev, bool do_bond)
{
	return do_bond && __mlx5_lag_is_active(ldev) &&
	       ldev->mode != MLX5_LAG_MODE_MPESW;
}

static bool mlx5_lag_should_disable_lag(struct mlx5_lag *ldev, bool do_bond)
{
	return !do_bond && __mlx5_lag_is_active(ldev) &&
	       ldev->mode != MLX5_LAG_MODE_MPESW;
}

static void mlx5_do_bond(struct mlx5_lag *ldev)
{
	int idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct lag_tracker tracker = { };
	struct mlx5_core_dev *dev0;
	struct net_device *ndev;
	bool do_bond, roce_lag;
	int err;
	int i;

	if (idx < 0)
		return;

	dev0 = ldev->pf[idx].dev;
	if (!mlx5_lag_is_ready(ldev)) {
		do_bond = false;
	} else {
		/* VF LAG is in multipath mode, ignore bond change requests */
		if (mlx5_lag_is_multipath(dev0))
			return;

		tracker = ldev->tracker;

		do_bond = tracker.is_bonded && mlx5_lag_check_prereq(ldev);
	}

	if (do_bond && !__mlx5_lag_is_active(ldev)) {
		bool shared_fdb = mlx5_lag_shared_fdb_supported(ldev);

		roce_lag = mlx5_lag_is_roce_lag(ldev);

		if (shared_fdb || roce_lag)
			mlx5_lag_remove_devices(ldev);

		err = mlx5_activate_lag(ldev, &tracker,
					roce_lag ? MLX5_LAG_MODE_ROCE :
						   MLX5_LAG_MODE_SRIOV,
					shared_fdb);
		if (err) {
			if (shared_fdb || roce_lag)
				mlx5_lag_add_devices(ldev);
			if (shared_fdb) {
				ldev_for_each(i, 0, ldev)
					mlx5_eswitch_reload_ib_reps(ldev->pf[i].dev->priv.eswitch);
			}

			return;
		} else if (roce_lag) {
			dev0->priv.flags &= ~MLX5_PRIV_FLAGS_DISABLE_IB_ADEV;
			mlx5_rescan_drivers_locked(dev0);
			ldev_for_each(i, idx + 1, ldev) {
				if (mlx5_get_roce_state(ldev->pf[i].dev))
					mlx5_nic_vport_enable_roce(ldev->pf[i].dev);
			}
		} else if (shared_fdb) {
			int i;

			dev0->priv.flags &= ~MLX5_PRIV_FLAGS_DISABLE_IB_ADEV;
			mlx5_rescan_drivers_locked(dev0);

			ldev_for_each(i, 0, ldev) {
				err = mlx5_eswitch_reload_ib_reps(ldev->pf[i].dev->priv.eswitch);
				if (err)
					break;
			}

			if (err) {
				dev0->priv.flags |= MLX5_PRIV_FLAGS_DISABLE_IB_ADEV;
				mlx5_rescan_drivers_locked(dev0);
				mlx5_deactivate_lag(ldev);
				mlx5_lag_add_devices(ldev);
				ldev_for_each(i, 0, ldev)
					mlx5_eswitch_reload_ib_reps(ldev->pf[i].dev->priv.eswitch);
				mlx5_core_err(dev0, "Failed to enable lag\n");
				return;
			}
		}
		if (tracker.tx_type == NETDEV_LAG_TX_TYPE_ACTIVEBACKUP) {
			ndev = mlx5_lag_active_backup_get_netdev(dev0);
			/** Only sriov and roce lag should have tracker->TX_type
			 *  set so no need to check the mode
			 */
			blocking_notifier_call_chain(&dev0->priv.lag_nh,
						     MLX5_DRIVER_EVENT_ACTIVE_BACKUP_LAG_CHANGE_LOWERSTATE,
						     ndev);
			dev_put(ndev);
		}
	} else if (mlx5_lag_should_modify_lag(ldev, do_bond)) {
		mlx5_modify_lag(ldev, &tracker);
	} else if (mlx5_lag_should_disable_lag(ldev, do_bond)) {
		mlx5_disable_lag(ldev);
	}
}

/* The last mdev to unregister will destroy the workqueue before removing the
 * devcom component, and as all the mdevs use the same devcom component we are
 * guaranteed that the devcom is valid while the calling work is running.
 */
struct mlx5_devcom_comp_dev *mlx5_lag_get_devcom_comp(struct mlx5_lag *ldev)
{
	struct mlx5_devcom_comp_dev *devcom = NULL;
	int i;

	mutex_lock(&ldev->lock);
	ldev_for_each(i, 0, ldev) {
		devcom = ldev->pf[i].dev->priv.hca_devcom_comp;
		break;
	}
	mutex_unlock(&ldev->lock);
	return devcom;
}

static void mlx5_queue_bond_work(struct mlx5_lag *ldev, unsigned long delay)
{
	queue_delayed_work(ldev->wq, &ldev->bond_work, delay);
}

static void mlx5_do_bond_work(struct work_struct *work)
{
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct mlx5_lag *ldev = container_of(delayed_work, struct mlx5_lag,
					     bond_work);
	struct mlx5_devcom_comp_dev *devcom;
	int status;

	devcom = mlx5_lag_get_devcom_comp(ldev);
	if (!devcom)
		return;

	status = mlx5_devcom_comp_trylock(devcom);
	if (!status) {
		mlx5_queue_bond_work(ldev, HZ);
		return;
	}

	mutex_lock(&ldev->lock);
	if (ldev->mode_changes_in_progress) {
		mutex_unlock(&ldev->lock);
		mlx5_devcom_comp_unlock(devcom);
		mlx5_queue_bond_work(ldev, HZ);
		return;
	}

	mlx5_do_bond(ldev);
	mutex_unlock(&ldev->lock);
	mlx5_devcom_comp_unlock(devcom);
}

static int mlx5_handle_changeupper_event(struct mlx5_lag *ldev,
					 struct lag_tracker *tracker,
					 struct netdev_notifier_changeupper_info *info)
{
	struct net_device *upper = info->upper_dev, *ndev_tmp;
	struct netdev_lag_upper_info *lag_upper_info = NULL;
	bool is_bonded, is_in_lag, mode_supported;
	bool has_inactive = 0;
	struct slave *slave;
	u8 bond_status = 0;
	int num_slaves = 0;
	int changed = 0;
	int i, idx = -1;

	if (!netif_is_lag_master(upper))
		return 0;

	if (info->linking)
		lag_upper_info = info->upper_info;

	/* The event may still be of interest if the slave does not belong to
	 * us, but is enslaved to a master which has one or more of our netdevs
	 * as slaves (e.g., if a new slave is added to a master that bonds two
	 * of our netdevs, we should unbond).
	 */

	rcu_read_lock();
	for_each_netdev_in_bond_rcu(upper, ndev_tmp) {
		ldev_for_each(i, 0, ldev) {
			if (ldev->pf[i].netdev == ndev_tmp) {
				idx++;
				break;
			}
		}
		if (i < MLX5_MAX_PORTS) {
			slave = bond_slave_get_rcu(ndev_tmp);
			if (slave)
				has_inactive |= bond_is_slave_inactive(slave);
			bond_status |= (1 << idx);
		}

		num_slaves++;
	}
	rcu_read_unlock();

	/* None of this lagdev's netdevs are slaves of this master. */
	if (!(bond_status & GENMASK(ldev->ports - 1, 0)))
		return 0;

	if (lag_upper_info) {
		tracker->tx_type = lag_upper_info->tx_type;
		tracker->hash_type = lag_upper_info->hash_type;
	}

	tracker->has_inactive = has_inactive;
	/* Determine bonding status:
	 * A device is considered bonded if both its physical ports are slaves
	 * of the same lag master, and only them.
	 */
	is_in_lag = num_slaves == ldev->ports &&
		bond_status == GENMASK(ldev->ports - 1, 0);

	/* Lag mode must be activebackup or hash. */
	mode_supported = tracker->tx_type == NETDEV_LAG_TX_TYPE_ACTIVEBACKUP ||
			 tracker->tx_type == NETDEV_LAG_TX_TYPE_HASH;

	is_bonded = is_in_lag && mode_supported;
	if (tracker->is_bonded != is_bonded) {
		tracker->is_bonded = is_bonded;
		changed = 1;
	}

	if (!is_in_lag)
		return changed;

	if (!mlx5_lag_is_ready(ldev))
		NL_SET_ERR_MSG_MOD(info->info.extack,
				   "Can't activate LAG offload, PF is configured with more than 64 VFs");
	else if (!mode_supported)
		NL_SET_ERR_MSG_MOD(info->info.extack,
				   "Can't activate LAG offload, TX type isn't supported");

	return changed;
}

static int mlx5_handle_changelowerstate_event(struct mlx5_lag *ldev,
					      struct lag_tracker *tracker,
					      struct net_device *ndev,
					      struct netdev_notifier_changelowerstate_info *info)
{
	struct netdev_lag_lower_state_info *lag_lower_info;
	int idx;

	if (!netif_is_lag_port(ndev))
		return 0;

	idx = mlx5_lag_dev_get_netdev_idx(ldev, ndev);
	if (idx < 0)
		return 0;

	/* This information is used to determine virtual to physical
	 * port mapping.
	 */
	lag_lower_info = info->lower_state_info;
	if (!lag_lower_info)
		return 0;

	tracker->netdev_state[idx] = *lag_lower_info;

	return 1;
}

static int mlx5_handle_changeinfodata_event(struct mlx5_lag *ldev,
					    struct lag_tracker *tracker,
					    struct net_device *ndev)
{
	struct net_device *ndev_tmp;
	struct slave *slave;
	bool has_inactive = 0;
	int idx;

	if (!netif_is_lag_master(ndev))
		return 0;

	rcu_read_lock();
	for_each_netdev_in_bond_rcu(ndev, ndev_tmp) {
		idx = mlx5_lag_dev_get_netdev_idx(ldev, ndev_tmp);
		if (idx < 0)
			continue;

		slave = bond_slave_get_rcu(ndev_tmp);
		if (slave)
			has_inactive |= bond_is_slave_inactive(slave);
	}
	rcu_read_unlock();

	if (tracker->has_inactive == has_inactive)
		return 0;

	tracker->has_inactive = has_inactive;

	return 1;
}

/* this handler is always registered to netdev events */
static int mlx5_lag_netdev_event(struct notifier_block *this,
				 unsigned long event, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct lag_tracker tracker;
	struct mlx5_lag *ldev;
	int changed = 0;

	if (event != NETDEV_CHANGEUPPER &&
	    event != NETDEV_CHANGELOWERSTATE &&
	    event != NETDEV_CHANGEINFODATA)
		return NOTIFY_DONE;

	ldev    = container_of(this, struct mlx5_lag, nb);

	tracker = ldev->tracker;

	switch (event) {
	case NETDEV_CHANGEUPPER:
		changed = mlx5_handle_changeupper_event(ldev, &tracker, ptr);
		break;
	case NETDEV_CHANGELOWERSTATE:
		changed = mlx5_handle_changelowerstate_event(ldev, &tracker,
							     ndev, ptr);
		break;
	case NETDEV_CHANGEINFODATA:
		changed = mlx5_handle_changeinfodata_event(ldev, &tracker, ndev);
		break;
	}

	ldev->tracker = tracker;

	if (changed)
		mlx5_queue_bond_work(ldev, 0);

	return NOTIFY_DONE;
}

static void mlx5_lag_set_default_port_sel_mode(struct mlx5_lag *ldev,
					       struct mlx5_core_dev *dev)
{
	unsigned int fn = mlx5_get_dev_index(dev);

	if (ldev->pf[fn].user_mode)
		return;

	if (MLX5_CAP_PORT_SELECTION(dev, port_select_flow_table))
		ldev->pf[fn].user_mode = MLX5_LAG_USER_PREF_MODE_HASH;
	else
		ldev->pf[fn].user_mode = MLX5_LAG_USER_PREF_MODE_QUEUE_AFFINITY;
}

static void mlx5_ldev_add_netdev(struct mlx5_lag *ldev,
				 struct mlx5_core_dev *dev,
				 struct net_device *netdev)
{
	unsigned int fn = mlx5_get_dev_index(dev);
	unsigned long flags;

	spin_lock_irqsave(&lag_lock, flags);
	mlx5_lag_set_default_port_sel_mode(ldev, dev);
	ldev->pf[fn].netdev = netdev;
	ldev->tracker.netdev_state[fn].link_up = 0;
	ldev->tracker.netdev_state[fn].tx_enabled = 0;
	spin_unlock_irqrestore(&lag_lock, flags);
}

static void mlx5_ldev_remove_netdev(struct mlx5_lag *ldev,
				    struct net_device *netdev)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&lag_lock, flags);
	ldev_for_each(i, 0, ldev) {
		if (ldev->pf[i].netdev == netdev) {
			ldev->pf[i].netdev = NULL;
			break;
		}
	}
	spin_unlock_irqrestore(&lag_lock, flags);
}

static void mlx5_ldev_add_mdev(struct mlx5_lag *ldev,
			      struct mlx5_core_dev *dev)
{
	unsigned int fn = mlx5_get_dev_index(dev);

	ldev->pf[fn].dev = dev;
	dev->priv.lag = ldev;
}

static void mlx5_ldev_remove_mdev(struct mlx5_lag *ldev,
				  struct mlx5_core_dev *dev)
{
	int fn;

	fn = mlx5_get_dev_index(dev);
	if (ldev->pf[fn].dev != dev)
		return;

	ldev->pf[fn].dev = NULL;
	dev->priv.lag = NULL;
}

/* Must be called with HCA devcom component lock held */
static int __mlx5_lag_dev_add_mdev(struct mlx5_core_dev *dev)
{
	struct mlx5_devcom_comp_dev *pos = NULL;
	struct mlx5_lag *ldev = NULL;
	struct mlx5_core_dev *tmp_dev;

	tmp_dev = mlx5_devcom_get_next_peer_data(dev->priv.hca_devcom_comp, &pos);
	if (tmp_dev)
		ldev = mlx5_lag_dev(tmp_dev);

	if (!ldev) {
		ldev = mlx5_lag_dev_alloc(dev);
		if (!ldev) {
			mlx5_core_err(dev, "Failed to alloc lag dev\n");
			return 0;
		}
		mlx5_ldev_add_mdev(ldev, dev);
		return 0;
	}

	mutex_lock(&ldev->lock);
	if (ldev->mode_changes_in_progress) {
		mutex_unlock(&ldev->lock);
		return -EAGAIN;
	}
	mlx5_ldev_get(ldev);
	mlx5_ldev_add_mdev(ldev, dev);
	mutex_unlock(&ldev->lock);

	return 0;
}

void mlx5_lag_remove_mdev(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;

	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		return;

	/* mdev is being removed, might as well remove debugfs
	 * as early as possible.
	 */
	mlx5_ldev_remove_debugfs(dev->priv.dbg.lag_debugfs);
recheck:
	mutex_lock(&ldev->lock);
	if (ldev->mode_changes_in_progress) {
		mutex_unlock(&ldev->lock);
		msleep(100);
		goto recheck;
	}
	mlx5_ldev_remove_mdev(ldev, dev);
	mutex_unlock(&ldev->lock);
	mlx5_ldev_put(ldev);
}

void mlx5_lag_add_mdev(struct mlx5_core_dev *dev)
{
	int err;

	if (!mlx5_lag_is_supported(dev))
		return;

	if (IS_ERR_OR_NULL(dev->priv.hca_devcom_comp))
		return;

recheck:
	mlx5_devcom_comp_lock(dev->priv.hca_devcom_comp);
	err = __mlx5_lag_dev_add_mdev(dev);
	mlx5_devcom_comp_unlock(dev->priv.hca_devcom_comp);

	if (err) {
		msleep(100);
		goto recheck;
	}
	mlx5_ldev_add_debugfs(dev);
}

void mlx5_lag_remove_netdev(struct mlx5_core_dev *dev,
			    struct net_device *netdev)
{
	struct mlx5_lag *ldev;
	bool lag_is_active;

	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		return;

	mutex_lock(&ldev->lock);
	mlx5_ldev_remove_netdev(ldev, netdev);
	clear_bit(MLX5_LAG_FLAG_NDEVS_READY, &ldev->state_flags);

	lag_is_active = __mlx5_lag_is_active(ldev);
	mutex_unlock(&ldev->lock);

	if (lag_is_active)
		mlx5_queue_bond_work(ldev, 0);
}

void mlx5_lag_add_netdev(struct mlx5_core_dev *dev,
			 struct net_device *netdev)
{
	struct mlx5_lag *ldev;
	int num = 0;

	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		return;

	mutex_lock(&ldev->lock);
	mlx5_ldev_add_netdev(ldev, dev, netdev);
	num = mlx5_lag_num_netdevs(ldev);
	if (num >= ldev->ports)
		set_bit(MLX5_LAG_FLAG_NDEVS_READY, &ldev->state_flags);
	mutex_unlock(&ldev->lock);
	mlx5_queue_bond_work(ldev, 0);
}

int get_pre_ldev_func(struct mlx5_lag *ldev, int start_idx, int end_idx)
{
	int i;

	for (i = start_idx; i >= end_idx; i--)
		if (ldev->pf[i].dev)
			return i;
	return -1;
}

int get_next_ldev_func(struct mlx5_lag *ldev, int start_idx)
{
	int i;

	for (i = start_idx; i < MLX5_MAX_PORTS; i++)
		if (ldev->pf[i].dev)
			return i;
	return MLX5_MAX_PORTS;
}

bool mlx5_lag_is_roce(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	bool res;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	res  = ldev && __mlx5_lag_is_roce(ldev);
	spin_unlock_irqrestore(&lag_lock, flags);

	return res;
}
EXPORT_SYMBOL(mlx5_lag_is_roce);

bool mlx5_lag_is_active(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	bool res = false;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	res  = ldev && __mlx5_lag_is_active(ldev);
	spin_unlock_irqrestore(&lag_lock, flags);

	return res;
}
EXPORT_SYMBOL(mlx5_lag_is_active);

bool mlx5_lag_mode_is_hash(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	bool res = 0;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	if (ldev)
		res = test_bit(MLX5_LAG_MODE_FLAG_HASH_BASED, &ldev->mode_flags);
	spin_unlock_irqrestore(&lag_lock, flags);

	return res;
}
EXPORT_SYMBOL(mlx5_lag_mode_is_hash);

bool mlx5_lag_is_master(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	bool res;
	int idx;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	res = ldev && __mlx5_lag_is_active(ldev) && idx >= 0 && dev == ldev->pf[idx].dev;
	spin_unlock_irqrestore(&lag_lock, flags);

	return res;
}
EXPORT_SYMBOL(mlx5_lag_is_master);

bool mlx5_lag_is_sriov(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	bool res;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	res  = ldev && __mlx5_lag_is_sriov(ldev);
	spin_unlock_irqrestore(&lag_lock, flags);

	return res;
}
EXPORT_SYMBOL(mlx5_lag_is_sriov);

bool mlx5_lag_is_shared_fdb(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	bool res;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	res = ldev && test_bit(MLX5_LAG_MODE_FLAG_SHARED_FDB, &ldev->mode_flags);
	spin_unlock_irqrestore(&lag_lock, flags);

	return res;
}
EXPORT_SYMBOL(mlx5_lag_is_shared_fdb);

void mlx5_lag_disable_change(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;

	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		return;

	mlx5_devcom_comp_lock(dev->priv.hca_devcom_comp);
	mutex_lock(&ldev->lock);

	ldev->mode_changes_in_progress++;
	if (__mlx5_lag_is_active(ldev))
		mlx5_disable_lag(ldev);

	mutex_unlock(&ldev->lock);
	mlx5_devcom_comp_unlock(dev->priv.hca_devcom_comp);
}

void mlx5_lag_enable_change(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;

	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		return;

	mutex_lock(&ldev->lock);
	ldev->mode_changes_in_progress--;
	mutex_unlock(&ldev->lock);
	mlx5_queue_bond_work(ldev, 0);
}

u8 mlx5_lag_get_slave_port(struct mlx5_core_dev *dev,
			   struct net_device *slave)
{
	struct mlx5_lag *ldev;
	unsigned long flags;
	u8 port = 0;
	int i;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	if (!(ldev && __mlx5_lag_is_roce(ldev)))
		goto unlock;

	ldev_for_each(i, 0, ldev) {
		if (ldev->pf[i].netdev == slave) {
			port = i;
			break;
		}
	}

	port = ldev->v2p_map[port * ldev->buckets];

unlock:
	spin_unlock_irqrestore(&lag_lock, flags);
	return port;
}
EXPORT_SYMBOL(mlx5_lag_get_slave_port);

u8 mlx5_lag_get_num_ports(struct mlx5_core_dev *dev)
{
	struct mlx5_lag *ldev;

	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		return 0;

	return ldev->ports;
}
EXPORT_SYMBOL(mlx5_lag_get_num_ports);

struct mlx5_core_dev *mlx5_lag_get_next_peer_mdev(struct mlx5_core_dev *dev, int *i)
{
	struct mlx5_core_dev *peer_dev = NULL;
	struct mlx5_lag *ldev;
	unsigned long flags;
	int idx;

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	if (!ldev)
		goto unlock;

	if (*i == MLX5_MAX_PORTS)
		goto unlock;
	ldev_for_each(idx, *i, ldev)
		if (ldev->pf[idx].dev != dev)
			break;

	if (idx == MLX5_MAX_PORTS) {
		*i = idx;
		goto unlock;
	}
	*i = idx + 1;

	peer_dev = ldev->pf[idx].dev;

unlock:
	spin_unlock_irqrestore(&lag_lock, flags);
	return peer_dev;
}
EXPORT_SYMBOL(mlx5_lag_get_next_peer_mdev);

int mlx5_lag_query_cong_counters(struct mlx5_core_dev *dev,
				 u64 *values,
				 int num_counters,
				 size_t *offsets)
{
	int outlen = MLX5_ST_SZ_BYTES(query_cong_statistics_out);
	struct mlx5_core_dev **mdev;
	int ret = 0, i, j, idx = 0;
	struct mlx5_lag *ldev;
	unsigned long flags;
	int num_ports;
	void *out;

	out = kvzalloc(outlen, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	mdev = kvzalloc(sizeof(mdev[0]) * MLX5_MAX_PORTS, GFP_KERNEL);
	if (!mdev) {
		ret = -ENOMEM;
		goto free_out;
	}

	memset(values, 0, sizeof(*values) * num_counters);

	spin_lock_irqsave(&lag_lock, flags);
	ldev = mlx5_lag_dev(dev);
	if (ldev && __mlx5_lag_is_active(ldev)) {
		num_ports = ldev->ports;
		ldev_for_each(i, 0, ldev)
			mdev[idx++] = ldev->pf[i].dev;
	} else {
		num_ports = 1;
		mdev[MLX5_LAG_P1] = dev;
	}
	spin_unlock_irqrestore(&lag_lock, flags);

	for (i = 0; i < num_ports; ++i) {
		u32 in[MLX5_ST_SZ_DW(query_cong_statistics_in)] = {};

		MLX5_SET(query_cong_statistics_in, in, opcode,
			 MLX5_CMD_OP_QUERY_CONG_STATISTICS);
		ret = mlx5_cmd_exec_inout(mdev[i], query_cong_statistics, in,
					  out);
		if (ret)
			goto free_mdev;

		for (j = 0; j < num_counters; ++j)
			values[j] += be64_to_cpup((__be64 *)(out + offsets[j]));
	}

free_mdev:
	kvfree(mdev);
free_out:
	kvfree(out);
	return ret;
}
EXPORT_SYMBOL(mlx5_lag_query_cong_counters);
