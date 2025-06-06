// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2021 Mellanox Technologies Ltd. */

#include <linux/mlx5/driver.h>
#include <net/mlxdevm.h>
#include "eswitch.h"
#include "mlx5_esw_devm.h"

void mlx5_devm_sf_port_type_eth_set(struct mlx5_core_dev *dev, u16 vport_num,
				    struct net_device *ndev)
{
	struct mlx5_devm_device *devm_dev;
	struct mlx5_devm_port *port;

	devm_dev = mlx5_devm_device_get(dev);
	if (!devm_dev)
		return;

	down_read(&devm_dev->port_list_rwsem);
	list_for_each_entry(port, &devm_dev->port_list, list) {
		if (port->vport_num != vport_num)
			continue;
		/* found the port */
		rtnl_lock();
		mlxdevm_port_type_eth_set(&port->port, ndev);
		rtnl_unlock();
		up_read(&devm_dev->port_list_rwsem);
		return;
	}
	up_read(&devm_dev->port_list_rwsem);
}

void mlx5_devm_sf_port_type_eth_unset(struct mlx5_core_dev *dev, u16 vport_num,
				      struct net_device *ndev)
{
	struct mlx5_devm_device *devm_dev;
	struct mlx5_devm_port *port;

	devm_dev = mlx5_devm_device_get(dev);
	if (!devm_dev)
		return;

	down_read(&devm_dev->port_list_rwsem);
	list_for_each_entry(port, &devm_dev->port_list, list) {
		if (port->vport_num != vport_num)
			continue;
		/* found the port */
		mlxdevm_port_type_clear(&port->port);
		up_read(&devm_dev->port_list_rwsem);
		return;
	}
	up_read(&devm_dev->port_list_rwsem);
}

u32 mlx5_devm_sf_vport_to_sfnum(struct mlx5_core_dev *dev, u16 vport_num)
{
	struct mlx5_devm_device *devm_dev;
	struct mlx5_devm_port *port;
	u32 sfnum = 0;

	devm_dev = mlx5_devm_device_get(dev);
	if (!devm_dev)
		return -EOPNOTSUPP;

	down_read(&devm_dev->port_list_rwsem);
	list_for_each_entry(port, &devm_dev->port_list, list) {
		if (port->vport_num == vport_num) {
			/* found the port */
			sfnum = port->sfnum;
			break;
		}
	}
	up_read(&devm_dev->port_list_rwsem);
	return sfnum;
}

u32 mlx5_devm_sf_vport_to_controller(struct mlx5_core_dev *dev, u16 vport_num)
{
	struct mlx5_devm_device *devm_dev;
	struct mlx5_devm_port *port;
	u32 controller = 0;

	devm_dev = mlx5_devm_device_get(dev);
	if (!devm_dev)
		return 0;

	down_read(&devm_dev->port_list_rwsem);
	list_for_each_entry(port, &devm_dev->port_list, list) {
		if (port->vport_num == vport_num) {
			/* found the port */
			controller = port->port.attrs.pci_sf.controller;
			break;
		}
	}
	up_read(&devm_dev->port_list_rwsem);
	return controller;
}
