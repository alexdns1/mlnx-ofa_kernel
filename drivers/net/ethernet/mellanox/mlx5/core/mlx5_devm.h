/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2021 Mellanox Technologies Ltd. */

#ifndef MLX5_DEVM_H
#define MLX5_DEVM_H

#if IS_ENABLED(CONFIG_MLXDEVM)
#include <net/mlxdevm.h>
#include <linux/rwsem.h>
#include <devl_internal.h>
#include "mlx5_devm_driver_params.h"

struct mlx5_devm_device {
	struct mlxdevm device;
	struct list_head port_list;
	struct mlx5_core_dev *dev;
	struct list_head list;
	struct rw_semaphore port_list_rwsem;
	struct xarray devm_sfs;
};

struct mlx5_devm_device *mlx5_devm_device_get(struct mlx5_core_dev *dev);
int mlx5_devm_register(struct mlx5_core_dev *dev);
void mlx5_devm_unregister(struct mlx5_core_dev *dev);
int mlx5_devm_affinity_get_param(struct mlx5_core_dev *dev, struct cpumask *mask);
int mlx5_devm_affinity_get_weight(struct mlx5_core_dev *dev);
void mlx5_devm_params_publish(struct mlx5_core_dev *dev);
void mlx5_devm_rate_nodes_destroy(struct mlx5_core_dev *dev);
bool mlx5_devm_is_devm_sf(struct mlx5_core_dev *dev, u32 sfnum);
void mlx5_devm_sfs_clean(struct mlx5_core_dev *dev);
void mlx5_devm_params_publish(struct mlx5_core_dev *dev);

#else
static inline int mlx5_devm_register(struct mlx5_core_dev *dev)
{
	return 0;
}

static inline void mlx5_devm_unregister(struct mlx5_core_dev *dev)
{
}
static inline bool
mlx5_devm_is_devm_sf(struct mlx5_core_dev *dev, u32 sfnum) { return false; }

static inline void mlx5_devm_params_publish(struct mlx5_core_dev *dev)
{
}

static inline void mlx5_devm_sfs_clean(struct mlx5_core_dev *dev)
{
}

static inline int
mlx5_devm_affinity_get_param(struct mlx5_core_dev *dev, struct cpumask *mask)
{
	return 0;
}

static inline int
mlx5_devm_affinity_get_weight(struct mlx5_core_dev *dev)
{
	return 0;
}

static inline void mlx5_devm_params_publish(struct mlx5_core_dev *dev)
{
}
#endif
#endif
