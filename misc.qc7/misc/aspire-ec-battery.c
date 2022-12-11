// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>
#include <linux/input.h>

#include <linux/usb/typec_mux.h>
#include <drm/drm_bridge.h>

#define ACER_FG_STATIC  0x08
#define ACER_FG_DYNAMIC 0x07

#define ACER_FG_FLAG_PRESENT		BIT(0)
#define ACER_FG_FLAG_FULL		BIT(1)
#define ACER_FG_FLAG_DISCHARGING	BIT(2)
#define ACER_FG_FLAG_CHARGING		BIT(3)

struct aspire_battery {
	struct i2c_client	*client;
	struct power_supply	*psy;
	//struct input_dev	*idev;

	bool bridge_configured;
	struct drm_bridge bridge;
};

struct fg_static_data {
	u8 unk1;
	u8 flags;
	__le16 unk2;
	__le16 voltage_design;
	__le16 capacity_full;
	__le16 unk3;
	__le16 serial;
	u8 model_id;
	u8 vendor_id;
};

struct fg_dynamic_data {
	u8 unk1;
	u8 flags;
	u8 unk2;
	__be16 capacity_now;
	__be16 voltage_now;
	__be16 current_now;
	__be16 unk3;
	__be16 unk4;
};

static int acpi_gsb_i2c_read_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[2];
	int ret;
	u8 *buffer;

	buffer = kzalloc(data_len, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = data_len;
	msgs[1].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		dev_err(&client->adapter->dev, "i2c read failed\n");
	else
		memcpy(data, buffer, data_len);

	kfree(buffer);
	return ret;
}

static int acpi_gsb_i2c_write_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[1];
	u8 *buffer;
	int ret = AE_OK;

	buffer = kzalloc(data_len + 1, GFP_KERNEL);
	if (!buffer)
		return AE_NO_MEMORY;

	buffer[0] = cmd;
	memcpy(buffer + 1, data, data_len);

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = data_len + 1;
	msgs[0].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));

	kfree(buffer);

	if (ret < 0) {
		dev_err(&client->adapter->dev, "i2c write failed: %d\n", ret);
		return ret;
	}

	/* 1 transfer must have completed successfully */
	return (ret == 1) ? 0 : -EIO;
}

static int aspire_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct aspire_battery *battery = power_supply_get_drvdata(psy);
	struct fg_static_data sdat = {};
	struct fg_dynamic_data ddat = {};

	acpi_gsb_i2c_read_bytes(battery->client, ACER_FG_STATIC, (u8*)&sdat, sizeof(sdat));
	acpi_gsb_i2c_read_bytes(battery->client, ACER_FG_DYNAMIC, (u8*)&ddat, sizeof(ddat));

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		if (ddat.flags & ACER_FG_FLAG_CHARGING)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (ddat.flags & ACER_FG_FLAG_DISCHARGING)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (ddat.flags & ACER_FG_FLAG_FULL)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = be16_to_cpu(ddat.voltage_now) * 1000;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = le16_to_cpu(sdat.voltage_design) * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = be16_to_cpu(ddat.capacity_now) * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = le16_to_cpu(sdat.capacity_full) * 1000;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = be16_to_cpu(ddat.capacity_now) * 100 / le16_to_cpu(sdat.capacity_full);
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property aspire_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_PRESENT,
};

static const struct power_supply_desc aspire_battery_desc = {
	.name		= "aspire-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.get_property	= aspire_battery_get_property,
	.properties	= aspire_battery_props,
	.num_properties	= ARRAY_SIZE(aspire_battery_props),
};

static int aspire_battery_ram_read(struct i2c_client *client,
		u8 off, u8 *data, u8 data_len)
{
	u8 tmp[1] = {off};
	/* set addr */
	acpi_gsb_i2c_write_bytes(client, 0x20, tmp, sizeof(tmp));
	/* read bytes out */
	acpi_gsb_i2c_read_bytes(client, 0x20, data, data_len);
	return 0;
}

static int aspire_battery_ram_write(struct i2c_client *client,
		u8 off, u8 data)
{
	u8 tmp[2] = {off, data};
	/* set addr */
	acpi_gsb_i2c_write_bytes(client, 0x21, tmp, sizeof(tmp));
	return 0;
}

static irqreturn_t aspire_battery_irq_handler(int irq, void *aspire_handler)
{
	struct aspire_battery *battery = aspire_handler;
	u8 status = 0;

	acpi_gsb_i2c_read_bytes(battery->client, 0x05, &status, sizeof(status));

	switch (status) {
		case 0x0:
			break;
		case 0x20:
			/*
			 * here acpi responds to the event and clears some bit or smh
			 * Notify (\_SB.I2C3.BAT1, 0x81) // Information Change
			 * Notify (\_SB.I2C3.ADP1, 0x80) // Status Change
			 */
			/*
			aspire_battery_ram_read(battery->client, 0x19, &status, sizeof(status));
			dev_info(&battery->client->dev, "charger irq id=0x20 val=0x%x\n", status);
			aspire_battery_ram_write(battery->client, 0x19, status & 0xbf, sizeof(status));
			power_supply_changed(battery->psy);
			*/
			break;
		case 0x9b: /* close */
			/* Notify (\_SB.LID0, 0x80) // Status Change */
			//input_report_switch(battery->idev, SW_LID, 1);
			//input_sync(battery->idev);
			//dev_info(&battery->client->dev, "lid cl irq id=0x%x\n", status);
			break;
		case 0x9c: /* open */
			/* Notify (\_SB.LID0, 0x80) // Status Change */
			//input_report_switch(battery->idev, SW_LID, 0);
			//input_sync(battery->idev);
			//pm_wakeup_event(&battery->client->dev, 0);
			//dev_info(&battery->client->dev, "lid op irq id=0x%x\n", status);
			break;
		case 0x85:
			/* Notify (\_SB.I2C3.BAT1, 0x81) // Information Change */
		case 0xc6:
			/* Notify (\_SB.I2C3.BAT1, 0x80) // Status Change */
			//dev_info(&battery->client->dev, "bat irq id=0x%x\n", status);
			power_supply_changed(battery->psy);
			break;
		case 0xa3:
			dev_info(&battery->client->dev, "hpd dis irq id=0x%x\n", status);
			if (battery->bridge_configured)
				drm_bridge_hpd_notify(&battery->bridge, connector_status_disconnected);
			break;
		case 0xa4:
			dev_info(&battery->client->dev, "hpd con irq id=0x%x\n", status);
			if (battery->bridge_configured)
				drm_bridge_hpd_notify(&battery->bridge, connector_status_connected);
			break;
		default:
			dev_warn(&battery->client->dev, "unknown irq id=0x%x\n", status);
	}

	return IRQ_HANDLED;
}

static int yoga_c630_ec_attach(struct drm_bridge *bridge,
			    enum drm_bridge_attach_flags flags)
{
	return flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR ? 0 : -EINVAL;
}

static const struct drm_bridge_funcs yoga_c630_ec_bridge_funcs = {
	.attach = yoga_c630_ec_attach,
};

static int aspire_battery_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev = &client->dev;
	struct aspire_battery *battery;
	struct fwnode_handle *fwnode;
	u8 tmp;
	u32 port;
	int ret;

	battery = devm_kzalloc(&client->dev, sizeof(*battery), GFP_KERNEL);
	if (!battery)
		return -ENOMEM;

	battery->client = client;

	i2c_set_clientdata(client, battery);
	psy_cfg.drv_data = battery;

	/* batery status reports */
	battery->psy = power_supply_register(&client->dev,
					     &aspire_battery_desc, &psy_cfg);
	if (IS_ERR(battery->psy)) {
		dev_err(&client->dev, "Failed to register power supply\n");
		return PTR_ERR(battery->psy);
	}

	/* Lid switch */
	//battery->idev = devm_input_allocate_device(&client->dev);
	//if (!battery->idev)
	//	return -ENOMEM;

	//battery->idev->name = "aspire-ec";
	//battery->idev->phys = "aspire-ec/input0";
	//input_set_capability(battery->idev, EV_SW, SW_LID);

	//ret = input_register_device(battery->idev);
	//if (ret) {
	//	dev_err(&client->dev, "Input register failed: %d\n", ret);
	//	return ret;
	//}

	//device_init_wakeup(&client->dev, true);

	/* Also enable the keyboard fn keys */
	aspire_battery_ram_write(client, 0x43, 0x61);

	/* Prepare for external display attach reports */
	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_u32(fwnode, "reg", &port);
		if (ret < 0)
			continue;

		if (port != 0)
			continue;

		battery->bridge.funcs = &yoga_c630_ec_bridge_funcs;
		battery->bridge.of_node = to_of_node(fwnode);
		battery->bridge.ops = DRM_BRIDGE_OP_HPD;
		battery->bridge.type = DRM_MODE_CONNECTOR_USB;

		/*ret = devm_*/drm_bridge_add(/*dev, */&battery->bridge);
		if (ret) {
			dev_err(dev, "failed to register drm bridge\n");
			fwnode_handle_put(fwnode);
			return ret;
		}

		battery->bridge_configured = true;

		/* Report initial state */
		ret = aspire_battery_ram_read(client, 0xf4, &tmp, sizeof(tmp));
		if (tmp == 0x03)
			drm_bridge_hpd_notify(&battery->bridge, connector_status_connected);
	}

	ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
					aspire_battery_irq_handler, IRQF_ONESHOT,
					dev_name(&client->dev), battery);
	if (ret) {
		dev_err(&client->dev, "Failed to request irq: %d\n", ret);
	}

	return 0;
}

static int aspire_battery_remove(struct i2c_client *client)
{
	struct aspire_battery *battery = i2c_get_clientdata(client);

	drm_bridge_remove(&battery->bridge);
	power_supply_unregister(battery->psy);

	return 0;
}

static const struct i2c_device_id aspire_battery_id[] = {
	{ "aspire1-battery", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aspire_battery_id);

static const struct of_device_id aspire_battery_of_match[] = {
	{ .compatible = "acer,aspire1-battery", },
	{ }
};
MODULE_DEVICE_TABLE(of, aspire_battery_of_match);

static struct i2c_driver aspire_battery_driver = {
	.driver = {
		.name = "aspire-battery",
		.of_match_table = aspire_battery_of_match,
	},
	.probe = aspire_battery_probe,
	.remove = aspire_battery_remove,
	.id_table = aspire_battery_id,
};
module_i2c_driver(aspire_battery_driver);

MODULE_DESCRIPTION("Some other fuel gauge driver");
MODULE_AUTHOR("Nikita Travkin <nikita@trvn.ru>");
MODULE_LICENSE("GPL");
