// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2016-2019 Mellanox Technologies. All rights reserved */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/types.h>

#include "core.h"
#include "core_env.h"
#include "i2c.h"

static const char mlxsw_m_driver_name[] = "mlxsw_minimal";

#define MLXSW_M_FWREV_MINOR	2000
#define MLXSW_M_FWREV_SUBMINOR	1886

static const struct mlxsw_fw_rev mlxsw_m_fw_rev = {
	.minor = MLXSW_M_FWREV_MINOR,
	.subminor = MLXSW_M_FWREV_SUBMINOR,
};

struct mlxsw_m_line_card;
struct mlxsw_m_port;

struct mlxsw_m {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	u8 base_mac[ETH_ALEN];
	u16 max_ports;
	u8 max_module_count; /* Maximum number of modules per-slot. */
	u8 num_of_slots; /* Including the main board. */
	struct mlxsw_m_line_card **line_cards;
};

struct mlxsw_m_port_mapping {
	struct mlxsw_m_port *port;
	int module_to_port;
	u8 module;
};

struct mlxsw_m_line_card {
	struct mlxsw_m *mlxsw_m;
	u8 max_ports;
	u8 module_offset;
	bool active;
	struct mlxsw_m_port_mapping port_mapping[];
};

struct mlxsw_m_port {
	struct net_device *dev;
	struct mlxsw_m *mlxsw_m;
	u16 local_port;
	u8 module;
	u8 slot_index;
	u8 module_offset;
};

static int mlxsw_m_base_mac_get(struct mlxsw_m *mlxsw_m)
{
	char spad_pl[MLXSW_REG_SPAD_LEN] = {0};
	int err;
#if 0
	err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(spad), spad_pl);
	if (err)
		return err;
	mlxsw_reg_spad_base_mac_memcpy_from(spad_pl, mlxsw_m->base_mac);
#endif
	return 0;
}

static int mlxsw_m_port_open(struct net_device *dev)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(dev);
	struct mlxsw_m *mlxsw_m = mlxsw_m_port->mlxsw_m;

	return mlxsw_env_module_port_up(mlxsw_m->core, 0,
					mlxsw_m_port->module);
}

static int mlxsw_m_port_stop(struct net_device *dev)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(dev);
	struct mlxsw_m *mlxsw_m = mlxsw_m_port->mlxsw_m;

	mlxsw_env_module_port_down(mlxsw_m->core, 0, mlxsw_m_port->module);
	return 0;
}

static struct devlink_port *
mlxsw_m_port_get_devlink_port(struct net_device *dev)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(dev);
	struct mlxsw_m *mlxsw_m = mlxsw_m_port->mlxsw_m;

	return mlxsw_core_port_devlink_port_get(mlxsw_m->core,
						mlxsw_m_port->local_port);
}

static const struct net_device_ops mlxsw_m_port_netdev_ops = {
	.ndo_open		= mlxsw_m_port_open,
	.ndo_stop		= mlxsw_m_port_stop,
	.ndo_get_devlink_port	= mlxsw_m_port_get_devlink_port,
};

static void mlxsw_m_module_get_drvinfo(struct net_device *dev,
				       struct ethtool_drvinfo *drvinfo)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(dev);
	struct mlxsw_m *mlxsw_m = mlxsw_m_port->mlxsw_m;

	strlcpy(drvinfo->driver, mlxsw_m->bus_info->device_kind,
		sizeof(drvinfo->driver));
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		 "%d.%d.%d",
		 mlxsw_m->bus_info->fw_rev.major,
		 mlxsw_m->bus_info->fw_rev.minor,
		 mlxsw_m->bus_info->fw_rev.subminor);
	strlcpy(drvinfo->bus_info, mlxsw_m->bus_info->device_name,
		sizeof(drvinfo->bus_info));
}

static int mlxsw_m_get_module_info(struct net_device *netdev,
				   struct ethtool_modinfo *modinfo)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_core *core = mlxsw_m_port->mlxsw_m->core;

	return mlxsw_env_get_module_info(core, mlxsw_m_port->slot_index,
					 mlxsw_m_port->module, modinfo);
}

static int
mlxsw_m_get_module_eeprom(struct net_device *netdev, struct ethtool_eeprom *ee,
			  u8 *data)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_core *core = mlxsw_m_port->mlxsw_m->core;

	return mlxsw_env_get_module_eeprom(netdev, core,
					   mlxsw_m_port->slot_index,
					   mlxsw_m_port->module, ee, data);
}

static int
mlxsw_m_get_module_eeprom_by_page(struct net_device *netdev,
				  const struct ethtool_module_eeprom *page,
				  struct netlink_ext_ack *extack)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_core *core = mlxsw_m_port->mlxsw_m->core;

	return mlxsw_env_get_module_eeprom_by_page(core,
						   mlxsw_m_port->slot_index,
						   mlxsw_m_port->module,
						   page, extack);
}

static int mlxsw_m_reset(struct net_device *netdev, u32 *flags)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_core *core = mlxsw_m_port->mlxsw_m->core;

	return mlxsw_env_reset_module(netdev, core, mlxsw_m_port->slot_index,
				      mlxsw_m_port->module,
				      flags);
}

static int
mlxsw_m_get_module_power_mode(struct net_device *netdev,
			      struct ethtool_module_power_mode_params *params,
			      struct netlink_ext_ack *extack)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_core *core = mlxsw_m_port->mlxsw_m->core;

	return mlxsw_env_get_module_power_mode(core, mlxsw_m_port->slot_index,
					       mlxsw_m_port->module,
					       params, extack);
}

static int
mlxsw_m_set_module_power_mode(struct net_device *netdev,
			      const struct ethtool_module_power_mode_params *params,
			      struct netlink_ext_ack *extack)
{
	struct mlxsw_m_port *mlxsw_m_port = netdev_priv(netdev);
	struct mlxsw_core *core = mlxsw_m_port->mlxsw_m->core;

	return mlxsw_env_set_module_power_mode(core, mlxsw_m_port->slot_index,
					       mlxsw_m_port->module,
					       params->policy, extack);
}

static const struct ethtool_ops mlxsw_m_port_ethtool_ops = {
	.get_drvinfo		= mlxsw_m_module_get_drvinfo,
	.get_module_info	= mlxsw_m_get_module_info,
	.get_module_eeprom	= mlxsw_m_get_module_eeprom,
	.get_module_eeprom_by_page = mlxsw_m_get_module_eeprom_by_page,
	.reset			= mlxsw_m_reset,
	.get_module_power_mode	= mlxsw_m_get_module_power_mode,
	.set_module_power_mode	= mlxsw_m_set_module_power_mode,
};

static int
mlxsw_m_port_dev_addr_get(struct mlxsw_m_port *mlxsw_m_port)
{
	struct mlxsw_m *mlxsw_m = mlxsw_m_port->mlxsw_m;
	struct net_device *dev = mlxsw_m_port->dev;
	char ppad_pl[MLXSW_REG_PPAD_LEN];
	int err;

	mlxsw_reg_ppad_pack(ppad_pl, false, 0);
	err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(ppad), ppad_pl);
	if (err)
		return err;
	mlxsw_reg_ppad_mac_memcpy_from(ppad_pl, dev->dev_addr);
	/* The last byte value in base mac address is guaranteed
	 * to be such it does not overflow when adding local_port
	 * value.
	 */
	dev->dev_addr[ETH_ALEN - 1] = mlxsw_m_port->module + 1 +
				      mlxsw_m_port->module_offset;
	return 0;
}

static struct
mlxsw_m_port_mapping *mlxsw_m_port_mapping_get(struct mlxsw_m *mlxsw_m,
					       u8 slot_index, u16 local_port)
{
	return &mlxsw_m->line_cards[slot_index]->port_mapping[local_port];
}

static int
mlxsw_m_port_create(struct mlxsw_m *mlxsw_m, u8 slot_index, u16 local_port,
		    u8 module)
{
	struct mlxsw_m_port_mapping *port_mapping;
	struct mlxsw_m_port *mlxsw_m_port;
	struct net_device *dev;
	u8 module_offset;
	int err;

	module_offset = mlxsw_m->line_cards[slot_index]->module_offset;
	err = mlxsw_core_port_init(mlxsw_m->core, local_port, slot_index,
				   module + 1 + module_offset, false, 0, false,
				   0, mlxsw_m->base_mac,
				   sizeof(mlxsw_m->base_mac));
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Port %d: Failed to init core port\n",
			local_port);
		return err;
	}

	dev = alloc_etherdev(sizeof(struct mlxsw_m_port));
	if (!dev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}

	SET_NETDEV_DEV(dev, mlxsw_m->bus_info->dev);
	dev_net_set(dev, mlxsw_core_net(mlxsw_m->core));
	mlxsw_m_port = netdev_priv(dev);
	mlxsw_m_port->dev = dev;
	mlxsw_m_port->mlxsw_m = mlxsw_m;
	mlxsw_m_port->local_port = local_port;
	mlxsw_m_port->module = module;
	mlxsw_m_port->slot_index = slot_index;
	/* Add module offset for line card. Offset for main board iz zero.
	 * For line card in slot #n offset is calculated as (#n - 1)
	 * multiplied by maximum modules number, which could be found on a line
	 * card.
	 */
	mlxsw_m_port->module_offset = module_offset;

	dev->netdev_ops = &mlxsw_m_port_netdev_ops;
	dev->ethtool_ops = &mlxsw_m_port_ethtool_ops;

	err = mlxsw_m_port_dev_addr_get(mlxsw_m_port);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Port %d: Unable to get port mac address\n",
			mlxsw_m_port->local_port);
		goto err_dev_addr_get;
	}

	netif_carrier_off(dev);
	port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
						local_port);
	port_mapping->port = mlxsw_m_port;
	err = register_netdev(dev);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Port %d: Failed to register netdev\n",
			mlxsw_m_port->local_port);
		goto err_register_netdev;
	}

	mlxsw_core_port_eth_set(mlxsw_m->core, mlxsw_m_port->local_port,
				mlxsw_m_port, dev);

	return 0;

err_register_netdev:
	port_mapping->port = NULL;
err_dev_addr_get:
	free_netdev(dev);
err_alloc_etherdev:
	mlxsw_core_port_fini(mlxsw_m->core, local_port);
	return err;
}

static void mlxsw_m_port_remove(struct mlxsw_m *mlxsw_m, u8 slot_index,
				u16 local_port)
{
	struct mlxsw_m_port_mapping *port_mapping;
	struct mlxsw_m_port *mlxsw_m_port;

	port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
						local_port);
	mlxsw_m_port = port_mapping->port;
	mlxsw_core_port_clear(mlxsw_m->core, local_port, mlxsw_m);
	unregister_netdev(mlxsw_m_port->dev); /* This calls ndo_stop */
	port_mapping->port = NULL;
	free_netdev(mlxsw_m_port->dev);
	mlxsw_core_port_fini(mlxsw_m->core, local_port);
}

static int mlxsw_m_port_module_map(struct mlxsw_m *mlxsw_m, u8 slot_index,
				   u16 local_port, u8 module)
{
	struct mlxsw_m_port_mapping *port_mapping;

	port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
						local_port);

	if (WARN_ON_ONCE(port_mapping->module_to_port >= mlxsw_m->max_ports))
		return -EINVAL;
	mlxsw_env_module_port_map(mlxsw_m->core, slot_index, module);
	port_mapping->module_to_port = local_port;
	port_mapping->module = module;

	return 0;
}

static void
mlxsw_m_port_module_unmap(struct mlxsw_m *mlxsw_m, u8 slot_index,
			  struct mlxsw_m_port_mapping *port_mapping)
{
	port_mapping->module_to_port = -1;
	mlxsw_env_module_port_unmap(mlxsw_m->core, slot_index,
				    port_mapping->module);
}

static int mlxsw_m_ports_create(struct mlxsw_m *mlxsw_m, u8 slot_index)
{
	struct mlxsw_m_port_mapping *port_mapping;
	struct mlxsw_m_line_card *line_card;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	int i, err;

	mlxsw_reg_mgpir_pack(mgpir_pl, slot_index);
	err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	line_card = mlxsw_m->line_cards[slot_index];
	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
			       &line_card->max_ports, NULL);
	if (!line_card->max_ports)
		return 0;

	line_card->max_ports += 1;
	line_card->module_offset = slot_index ? (slot_index - 1) *
				   mlxsw_m->max_module_count : 0;

	/* Fill out module to local port mapping array */
	for (i = 1; i < mlxsw_m->line_cards[slot_index]->max_ports; i++) {
		err = mlxsw_m_port_module_map(mlxsw_m, slot_index, i +
					      line_card->module_offset, i - 1);
		if (err)
			goto err_module_to_port_map;
	}

	/* Create port objects for each valid entry */
	for (i = 0; i < mlxsw_m->max_ports; i++) {
		port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
							i);
		if (port_mapping->module_to_port > 0) {
			err = mlxsw_m_port_create(mlxsw_m, slot_index,
						  port_mapping->module_to_port,
						  port_mapping->module);
			if (err)
				goto err_module_to_port_create;
		}
	}

	return 0;

err_module_to_port_create:
	for (i--; i >= 0; i--) {
		port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
							i);
		if (port_mapping->module_to_port > 0)
			mlxsw_m_port_remove(mlxsw_m, slot_index,
					    port_mapping->module_to_port);
	}
	i = mlxsw_m->max_ports;
err_module_to_port_map:
	for (i--; i > 0; i--) {
		port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
							i);
		if (port_mapping->module_to_port > 0)
			mlxsw_m_port_module_unmap(mlxsw_m, slot_index,
						  port_mapping);
	}
	return err;
}

static void mlxsw_m_ports_remove(struct mlxsw_m *mlxsw_m, u8 slot_index)
{
	struct mlxsw_m_port_mapping *port_mapping;
	u8 module;
	int i;

	for (i = 0; i < mlxsw_m->max_ports; i++) {
		port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, slot_index,
							i);
		if (port_mapping->module_to_port > 0) {
			module = port_mapping->port->module;
			mlxsw_m_port_remove(mlxsw_m, slot_index,
					    port_mapping->module_to_port);
			mlxsw_m_port_module_unmap(mlxsw_m, slot_index,
						  port_mapping);
		}
	}
}

static int mlxsw_m_fw_rev_validate(struct mlxsw_m *mlxsw_m)
{
	const struct mlxsw_fw_rev *rev = &mlxsw_m->bus_info->fw_rev;

	/* Validate driver and FW are compatible.
	 * Do not check major version, since it defines chip type, while
	 * driver is supposed to support any type.
	 */
	if (mlxsw_core_fw_rev_minor_subminor_validate(rev, &mlxsw_m_fw_rev))
		return 0;

	dev_err(mlxsw_m->bus_info->dev, "The firmware version %d.%d.%d is incompatible with the driver (required >= %d.%d.%d)\n",
		rev->major, rev->minor, rev->subminor, rev->major,
		mlxsw_m_fw_rev.minor, mlxsw_m_fw_rev.subminor);

	return -EINVAL;
}

static int mlxsw_m_get_peripheral_info(struct mlxsw_m *mlxsw_m)
{
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	u8 module_count;
	int err;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL, &module_count,
			       &mlxsw_m->num_of_slots);
	/* If the system is modular, get the maximum number of modules per-slot.
	 * Otherwise, get the maximum number of modules on the main board.
	 */
	mlxsw_m->max_module_count = mlxsw_m->num_of_slots ?
			   mlxsw_reg_mgpir_max_modules_per_slot_get(mgpir_pl) :
			   module_count;
	/* Add slot for main board. */
	mlxsw_m->num_of_slots += 1;

	return 0;
}

static int mlxsw_env_line_cards_alloc(struct mlxsw_m *mlxsw_m)
{
	unsigned int max_ports = mlxsw_core_max_ports(mlxsw_m->core);
	struct mlxsw_m_port_mapping *port_mapping;
	int i, j;

	mlxsw_m->line_cards = kcalloc(mlxsw_m->num_of_slots,
				      sizeof(*mlxsw_m->line_cards),
				      GFP_KERNEL);
	if (!mlxsw_m->line_cards)
		goto err_kcalloc;

	for (i = 0; i < mlxsw_m->num_of_slots; i++) {
		mlxsw_m->line_cards[i] = kzalloc(struct_size(mlxsw_m->line_cards[i],
							     port_mapping, max_ports),
						 GFP_KERNEL);
		if (!mlxsw_m->line_cards[i])
			goto kzalloc_err;

		/* Invalidate the entries of module to local port mapping array */
		for (j = 0; j < mlxsw_m->max_ports; j++) {
			port_mapping = mlxsw_m_port_mapping_get(mlxsw_m, i, j);
			port_mapping->module_to_port = -1;
		}
	}

	mlxsw_m->max_ports = max_ports;

	return 0;

kzalloc_err:
	for (i--; i >= 0; i--)
		kfree(mlxsw_m->line_cards[i]);
err_kcalloc:
	kfree(mlxsw_m->line_cards);
	return -ENOMEM;
}

static void mlxsw_m_line_cards_free(struct mlxsw_m *mlxsw_m)
{
	int i = mlxsw_m->num_of_slots;

	for (i--; i >= 0; i--)
		kfree(mlxsw_m->line_cards[i]);
	kfree(mlxsw_m->line_cards);
}

static void mlxsw_m_sys_event_handler(struct mlxsw_core *mlxsw_core)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);
	struct mlxsw_m *mlxsw_m = mlxsw_core_driver_priv(mlxsw_core);
	char mddq_pl[MLXSW_REG_MDDQ_LEN];
	int i, err;

	if (!linecards)
		return;

	/* Handle line cards, for which active status has been changed. */
	for (i = 1; i <= linecards->count; i++) {
		mlxsw_reg_mddq_slot_info_pack(mddq_pl, i, false);
		err = mlxsw_reg_query(mlxsw_m->core, MLXSW_REG(mddq), mddq_pl);
		if (err)
			dev_err(mlxsw_m->bus_info->dev, "Fail to query MDDQ register for slot %d\n",
				i);

		mlxsw_linecard_status_process(mlxsw_m->core, mddq_pl);
	}
}

static void
mlxsw_m_got_active(struct mlxsw_core *mlxsw_core, u8 slot_index,
		   const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_m *mlxsw_m = priv;
	int err;

	err = mlxsw_m_ports_create(mlxsw_m, slot_index);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to set line card at slot %d\n",
			slot_index);
		goto mlxsw_m_ports_create_fail;
	}
	mlxsw_m->line_cards[slot_index]->active = true;

mlxsw_m_ports_create_fail:
	return;
}

static void
mlxsw_m_got_inactive(struct mlxsw_core *mlxsw_core, u8 slot_index,
		     const struct mlxsw_linecard *linecard, void *priv)
{
	struct mlxsw_m *mlxsw_m = priv;

	mlxsw_m_ports_remove(mlxsw_m, slot_index);
	mlxsw_m->line_cards[slot_index]->active = false;
}

static struct mlxsw_linecards_event_ops mlxsw_m_event_ops = {
	.got_active = mlxsw_m_got_active,
	.got_inactive = mlxsw_m_got_inactive,
};

static int mlxsw_m_linecards_register(struct mlxsw_m *mlxsw_m)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_m->core);

	if (!linecards || !linecards->count)
		return 0;

	return mlxsw_linecards_event_ops_register(mlxsw_m->core,
						  &mlxsw_m_event_ops,
						  mlxsw_m);
}

static void mlxsw_m_linecards_unregister(struct mlxsw_m *mlxsw_m)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_m->core);
	int i;

	if (!linecards || !linecards->count)
		return;

	for (i = 1; i <= linecards->count; i++) {
		if (mlxsw_m->line_cards[i]->active)
			mlxsw_m_got_inactive(mlxsw_m->core, i, NULL, mlxsw_m);
	}

	mlxsw_linecards_event_ops_unregister(mlxsw_m->core,
					     &mlxsw_m_event_ops, mlxsw_m);
}

static int mlxsw_m_init(struct mlxsw_core *mlxsw_core,
			const struct mlxsw_bus_info *mlxsw_bus_info,
			struct netlink_ext_ack *extack)
{
	struct mlxsw_m *mlxsw_m = mlxsw_core_driver_priv(mlxsw_core);
	int err;

	mlxsw_m->core = mlxsw_core;
	mlxsw_m->bus_info = mlxsw_bus_info;

	err = mlxsw_m_fw_rev_validate(mlxsw_m);
	if (err)
		return err;

	err = mlxsw_m_get_peripheral_info(mlxsw_m);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to get peripheral info\n");
		return err;
	}

	err = mlxsw_m_base_mac_get(mlxsw_m);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to get base mac\n");
		return err;
	}

	err = mlxsw_env_line_cards_alloc(mlxsw_m);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to allocate memory\n");
		return err;
	}

	err = mlxsw_m_ports_create(mlxsw_m, 0);
	if (err) {
		dev_err(mlxsw_m->bus_info->dev, "Failed to create ports\n");
		goto err_mlxsw_m_ports_create;
	}

	err = mlxsw_m_linecards_register(mlxsw_m);
	if (err)
		goto err_linecards_register;

	return 0;

err_linecards_register:
	mlxsw_m_ports_remove(mlxsw_m, 0);
err_mlxsw_m_ports_create:
	mlxsw_m_line_cards_free(mlxsw_m);
	return err;
}

static void mlxsw_m_fini(struct mlxsw_core *mlxsw_core)
{
	struct mlxsw_m *mlxsw_m = mlxsw_core_driver_priv(mlxsw_core);

	mlxsw_m_linecards_unregister(mlxsw_m);
	mlxsw_m_ports_remove(mlxsw_m, 0);
	mlxsw_m_line_cards_free(mlxsw_m);
}

static const struct mlxsw_config_profile mlxsw_m_config_profile;

static struct mlxsw_driver mlxsw_m_driver = {
	.kind			= mlxsw_m_driver_name,
	.priv_size		= sizeof(struct mlxsw_m),
	.init			= mlxsw_m_init,
	.fini			= mlxsw_m_fini,
	.sys_event_handler	= mlxsw_m_sys_event_handler,
	.profile		= &mlxsw_m_config_profile,
	.res_query_enabled	= true,
};

static const struct i2c_device_id mlxsw_m_i2c_id[] = {
	{ "mlxsw_minimal", 0},
	{ },
};

static struct i2c_driver mlxsw_m_i2c_driver = {
	.driver.name = "mlxsw_minimal",
	.class = I2C_CLASS_HWMON,
	.id_table = mlxsw_m_i2c_id,
};

static int __init mlxsw_m_module_init(void)
{
	int err;

	err = mlxsw_core_driver_register(&mlxsw_m_driver);
	if (err)
		return err;

	err = mlxsw_i2c_driver_register(&mlxsw_m_i2c_driver);
	if (err)
		goto err_i2c_driver_register;

	return 0;

err_i2c_driver_register:
	mlxsw_core_driver_unregister(&mlxsw_m_driver);

	return err;
}

static void __exit mlxsw_m_module_exit(void)
{
	mlxsw_i2c_driver_unregister(&mlxsw_m_i2c_driver);
	mlxsw_core_driver_unregister(&mlxsw_m_driver);
}

module_init(mlxsw_m_module_init);
module_exit(mlxsw_m_module_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Vadim Pasternak <vadimp@mellanox.com>");
MODULE_DESCRIPTION("Mellanox minimal driver");
MODULE_DEVICE_TABLE(i2c, mlxsw_m_i2c_id);
