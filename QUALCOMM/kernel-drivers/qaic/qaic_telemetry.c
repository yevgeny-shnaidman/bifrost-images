// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2020-2021, The Linux Foundation. All rights reserved. */
/* Copyright (c) 2021-2024 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <asm/byteorder.h>
#include <linux/completion.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/mutex.h>
#include <linux/srcu.h>
#include <linux/workqueue.h>

#include "qaic.h"
#include "qaic_telemetry.h"

#define MAGIC				0x55AA
#define VERSION				0x1

enum fw_errno {
	ERR_READ_ONLY		= -1,
	ERR_NOT_SUPPORTED	= -2,
	ERR_WHILE_PROCESSING	= -3,
	ERR_INVALID_VALUE	= -4,
	ERR_INVALID_HEADER	= -5,
};

enum cmds {
	CMD_THERMAL_SOC_TEMP,
	CMD_THERMAL_SOC_MAX_TEMP,
	CMD_THERMAL_BOARD_TEMP,
	CMD_THERMAL_BOARD_MAX_TEMP,
	CMD_THERMAL_DDR_TEMP,
	CMD_THERMAL_WARNING_TEMP,
	CMD_THERMAL_SHUTDOWN_TEMP,
	CMD_BOARD_TDP,
	CMD_BOARD_POWER,
	CMD_POWER_STATE,
	CMD_POWER_MAX,
	CMD_THROTTLE_PERCENT,
	CMD_THROTTLE_TIME,
	CMD_UPTIME,
	CMD_THERMAL_SOC_FLOOR_TEMP,
	CMD_THERMAL_SOC_CEILING_TEMP,
	CMD_VOLTAGE_CX,
	CMD_VOLTAGE_MX,
	CMD_VOLTAGE_NSP_CX,
	CMD_VOLTAGE_NSP_MX,
	CMD_CFG_TOPS,
	CMD_THERMAL_PMIC_TEMP_A,
	CMD_THERMAL_PMIC_TEMP_C,
	CMD_THERMAL_PMIC_TEMP_E,
	CMD_POWER_ACTION,
	CMD_SOC_TDP,
	CMD_SOC_POWER,
};

enum cmd_type {
	TYPE_READ,  /* read value from device */
	TYPE_WRITE, /* write value to device */
};

enum msg_type {
	MSG_PUSH, /* async push from device */
	MSG_REQ,  /* sync request to device */
	MSG_RESP, /* sync response from device */
};

struct telemetry_data {
	u8	cmd;
	u8	cmd_type;
	u8	status;
	__le64	val; /* signed */
} __packed;

struct telemetry_header {
	__le16	magic;
	__le16	ver;
	__le32	seq_num;
	u8	type;
	u8	id;
	__le16	len;
} __packed;

struct telemetry_msg { /* little endian encoded */
	struct telemetry_header hdr;
	struct telemetry_data data;
} __packed;

struct wrapper_msg {
	struct kref ref_count;
	struct telemetry_msg msg;
};

struct xfer_queue_elem {
	/*
	 * Node in list of ongoing transfer request on telemetry channel.
	 * Maintained by root device struct
	 */
	struct list_head list;
	/* Sequence number of this transfer request */
	u32 seq_num;
	/* This is used to wait on until completion of transfer request */
	struct completion xfer_done;
	/* Received data from device */
	void *buf;
};

struct resp_work {
	/* Work struct to schedule work coming on QAIC_TELEMETRY channel */
	struct work_struct work;
	/* Root struct of device, used to access device resources */
	struct qaic_device *qdev;
	/* Buffer used by MHI for transfer requests */
	void *buf;
};

static void free_wrapper(struct kref *ref)
{
	struct wrapper_msg *wrapper = container_of(ref, struct wrapper_msg, ref_count);

	kfree(wrapper);
}

static int telemetry_request(struct qaic_device *qdev, u8 cmd, u8 cmd_type, s64 *val)
{
	struct wrapper_msg *wrapper;
	struct xfer_queue_elem elem;
	struct telemetry_msg *resp;
	struct telemetry_msg *req;
	long ret = 0;

	wrapper = kzalloc(sizeof(*wrapper), GFP_KERNEL);
	if (!wrapper)
		return -ENOMEM;

	kref_init(&wrapper->ref_count);
	req = &wrapper->msg;

	ret = mutex_lock_interruptible(&qdev->tele_mutex);
	if (ret)
		goto free_req;

	req->hdr.magic = cpu_to_le16(MAGIC);
	req->hdr.ver = cpu_to_le16(VERSION);
	req->hdr.seq_num = cpu_to_le32(qdev->tele_next_seq_num++);
	req->hdr.type = MSG_REQ;
	req->hdr.id = 0;
	req->hdr.len = cpu_to_le16(sizeof(req->data));

	req->data.cmd = cmd;
	req->data.cmd_type = cmd_type;
	req->data.status = 0;
	if (cmd_type == TYPE_READ)
		req->data.val = cpu_to_le64(0);
	else
		req->data.val = cpu_to_le64(*val);

	elem.seq_num = qdev->tele_next_seq_num - 1;
	elem.buf = NULL;
	init_completion(&elem.xfer_done);
	if (likely(!qdev->tele_lost_buf)) {
		resp = kzalloc(sizeof(*resp), GFP_KERNEL);
		if (!resp) {
			mutex_unlock(&qdev->tele_mutex);
			ret = -ENOMEM;
			goto free_req;
		}

		ret = mhi_queue_buf(qdev->tele_ch, DMA_FROM_DEVICE, resp, sizeof(*resp), MHI_EOT);
		if (ret) {
			mutex_unlock(&qdev->tele_mutex);
			goto free_resp;
		}
	} else {
		/*
		 * we lost a buffer because we queued a recv buf, but then
		 * queuing the corresponding tx buf failed. To try to avoid
		 * a memory leak, lets reclaim it and use it for this
		 * transaction.
		 */
		qdev->tele_lost_buf = false;
	}

	kref_get(&wrapper->ref_count);
	ret = mhi_queue_buf(qdev->tele_ch, DMA_TO_DEVICE, req, sizeof(*req), MHI_EOT);
	if (ret) {
		qdev->tele_lost_buf = true;
		kref_put(&wrapper->ref_count, free_wrapper);
		mutex_unlock(&qdev->tele_mutex);
		goto free_req;
	}

	list_add_tail(&elem.list, &qdev->tele_xfer_list);
	mutex_unlock(&qdev->tele_mutex);

	ret = wait_for_completion_interruptible_timeout(&elem.xfer_done, HZ);
	/*
	 * not using _interruptable because we have to cleanup or we'll
	 * likely cause memory corruption
	 */
	mutex_lock(&qdev->tele_mutex);
	if (!list_empty(&elem.list))
		list_del(&elem.list);
	if (!ret && !elem.buf)
		ret = -ETIMEDOUT;
	else if (ret > 0 && !elem.buf)
		ret = -EIO;
	mutex_unlock(&qdev->tele_mutex);

	resp = elem.buf;

	if (ret < 0)
		goto free_resp;

	if (le16_to_cpu(resp->hdr.magic) != MAGIC || le16_to_cpu(resp->hdr.ver) != VERSION ||
	    resp->hdr.type != MSG_RESP || resp->hdr.id != 0 ||
	    le16_to_cpu(resp->hdr.len) != sizeof(resp->data) || resp->data.cmd != cmd ||
	    resp->data.cmd_type != cmd_type) {
		ret = -EINVAL;
		goto free_resp;
	}

	if (resp->data.status) {
		switch ((int8_t) resp->data.status) {
		case ERR_READ_ONLY:
			ret = -EROFS;
			break;
		case ERR_NOT_SUPPORTED:
			ret = -EOPNOTSUPP;
			break;
		case ERR_WHILE_PROCESSING:
			ret = -EREMOTEIO;
			break;
		case ERR_INVALID_VALUE:
			ret = -EINVAL;
			break;
		case ERR_INVALID_HEADER:
			ret = -EBADR;
			break;
		default:
			ret = -EPROTO;
		}
		goto free_resp;
	}

	if (cmd_type == TYPE_READ)
		*val = le64_to_cpu(resp->data.val);

	ret = 0;

free_resp:
	kfree(resp);
free_req:
	kref_put(&wrapper->ref_count, free_wrapper);

	return ret;
}

static ssize_t throttle_percent_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	s64 val = 0;
	int rcu_id;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	ret = telemetry_request(qdev, CMD_THROTTLE_PERCENT, TYPE_READ, &val);
	if (ret) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return ret;
	}

	/*
	 * The percent the device performance is being throttled to meet
	 * the limits. IE performance is throttled 20% to meet power/thermal/
	 * etc limits.
	 */
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return sprintf(buf, "%lld\n", val);
}

static SENSOR_DEVICE_ATTR_RO(throttle_percent, throttle_percent, 0);

static ssize_t throttle_time_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	s64 val = 0;
	int rcu_id;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	ret = telemetry_request(qdev, CMD_THROTTLE_TIME, TYPE_READ, &val);
	if (ret) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return ret;
	}

	/* The time, in seconds, the device has been in a throttled state */
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return sprintf(buf, "%lld\n", val);
}

static SENSOR_DEVICE_ATTR_RO(throttle_time, throttle_time, 0);

static ssize_t power_level_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	s64 val = 0;
	int rcu_id;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	ret = telemetry_request(qdev, CMD_POWER_STATE, TYPE_READ, &val);
	if (ret) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return ret;
	}

	/*
	 * Power level the device is operating at. What is the upper limit
	 * it is allowed to consume.
	 * 1 - full power
	 * 2 - reduced power
	 * 3 - minimal power
	 */
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return sprintf(buf, "%lld\n", val);
}

static ssize_t power_level_store(struct device *dev, struct device_attribute *a, const char *buf,
				 size_t count)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	s64 val;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	if (kstrtol(buf, 10, (long *)&val)) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -EINVAL;
	}

	ret = telemetry_request(qdev, CMD_POWER_STATE, TYPE_WRITE, &val);
	if (ret) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return ret;
	}

	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return count;
}

static SENSOR_DEVICE_ATTR_RW(power_level, power_level, 0);

static struct attribute *power_attrs[] = {
	&sensor_dev_attr_power_level.dev_attr.attr,
	&sensor_dev_attr_throttle_percent.dev_attr.attr,
	&sensor_dev_attr_throttle_time.dev_attr.attr,
	NULL,
};

static const struct attribute_group power_group = {
	.attrs = power_attrs,
};

static ssize_t uptime_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	s64 val = 0;
	int rcu_id;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	ret = telemetry_request(qdev, CMD_UPTIME, TYPE_READ, &val);
	if (ret) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return ret;
	}

	/* The time, in seconds, the device has been up */
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return sprintf(buf, "%lld\n", val);
}

static SENSOR_DEVICE_ATTR_RO(uptime, uptime, 0);

static struct attribute *uptime_attrs[] = {
	&sensor_dev_attr_uptime.dev_attr.attr,
	NULL,
};

static const struct attribute_group uptime_group = {
	.attrs = uptime_attrs,
};

static ssize_t soc_temp_floor_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	int ret;
	s64 val;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto exit;
	}

	ret = telemetry_request(qdev, CMD_THERMAL_SOC_FLOOR_TEMP, TYPE_READ, &val);
	if (ret)
		goto exit;

	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return sprintf(buf, "%lld\n", val * 1000);

exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static SENSOR_DEVICE_ATTR_RO(temp2_floor, soc_temp_floor, 0);

static ssize_t soc_temp_ceiling_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	int ret;
	s64 val;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto exit;
	}

	ret = telemetry_request(qdev, CMD_THERMAL_SOC_CEILING_TEMP, TYPE_READ, &val);
	if (ret)
		goto exit;

	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return sprintf(buf, "%lld\n", val * 1000);

exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static SENSOR_DEVICE_ATTR_RO(temp2_ceiling, soc_temp_ceiling, 0);

static struct attribute *temp2_attrs[] = {
	&sensor_dev_attr_temp2_floor.dev_attr.attr,
	&sensor_dev_attr_temp2_ceiling.dev_attr.attr,
	NULL,
};

static const struct attribute_group temp2_group = {
	.attrs = temp2_attrs,
};

static ssize_t soc_tops_store(struct device *dev, struct device_attribute *a, const char *buf,
			      size_t count)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	s64 val;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto exit;
	}

	if (kstrtol(buf, 10, (long *)&val)) {
		ret = -EINVAL;
		goto exit;
	}

	ret = telemetry_request(qdev, CMD_CFG_TOPS, TYPE_WRITE, &val);
	if (ret)
		goto exit;

	ret = count;
exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static ssize_t soc_tops_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	s64 val;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto exit;
	}

	ret = telemetry_request(qdev, CMD_CFG_TOPS, TYPE_READ, &val);

exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	if (ret)
		return ret;

	return sprintf(buf, "%lld\n", val);
}

static SENSOR_DEVICE_ATTR_RW(tops, soc_tops, 0);

static struct attribute *tops_attrs[] = {
	&sensor_dev_attr_tops.dev_attr.attr,
	NULL,
};

static const struct attribute_group tops_group = {
	.attrs = tops_attrs,
};

static ssize_t soc_pmic_temp_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct sensor_device_attribute *sensor = to_sensor_dev_attr(a);
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	int ret;
	s64 val;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto exit;
	}

	ret = telemetry_request(qdev, CMD_THERMAL_PMIC_TEMP_A + sensor->index, TYPE_READ, &val);

exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	if (ret)
		return ret;

	return sprintf(buf, "%lld\n", val * 1000);
}

static SENSOR_DEVICE_ATTR_RO(temp_pmicA_input, soc_pmic_temp, 0);
static SENSOR_DEVICE_ATTR_RO(temp_pmicC_input, soc_pmic_temp, 1);
static SENSOR_DEVICE_ATTR_RO(temp_pmicE_input, soc_pmic_temp, 2);

static struct attribute *pmic_attrs[] = {
	&sensor_dev_attr_temp_pmicA_input.dev_attr.attr,
	&sensor_dev_attr_temp_pmicC_input.dev_attr.attr,
	&sensor_dev_attr_temp_pmicE_input.dev_attr.attr,
	NULL,
};

static const struct attribute_group pmic_group = {
	.attrs = pmic_attrs,
};

static ssize_t power_action_store(struct device *dev, struct device_attribute *a, const char *buf,
				  size_t count)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int rcu_id;
	s64 val;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto exit;
	}

	if (kstrtol(buf, 10, (long *)&val)) {
		ret = -EINVAL;
		goto exit;
	}

	ret = telemetry_request(qdev, CMD_POWER_ACTION, TYPE_WRITE, &val);
	if (ret)
		goto exit;

	ret = count;
exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static SENSOR_DEVICE_ATTR_WO(power_action, power_action, 0);

static struct attribute *reset_attrs[] = {
	&sensor_dev_attr_power_action.dev_attr.attr,
	NULL,
};

static const struct attribute_group reset_group = {
	.attrs = reset_attrs,
};

static umode_t qaic_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
			       int channel)
{
	switch (type) {
	case hwmon_power:
		switch (attr) {
		case hwmon_power_max:
			return 0644;
		default:
			return 0444;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			fallthrough;
		case hwmon_temp_highest:
			fallthrough;
		case hwmon_temp_alarm:
			return 0444;
		case hwmon_temp_crit:
			fallthrough;
		case hwmon_temp_emergency:
			return 0644;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			return 0444;
		}
		break;
	default:
		return 0;
	}
	return 0;
}

static int qaic_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		     long *vall)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;
	s64 val = 0;
	int rcu_id;
	u8 cmd;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	switch (type) {
	case hwmon_power:
		switch (attr) {
		case hwmon_power_max:
			if (channel == 0)
				cmd = CMD_BOARD_TDP;
			else if (channel == 1)
				cmd = CMD_SOC_TDP;
			else
				goto exit;
			ret = telemetry_request(qdev, cmd, TYPE_READ, &val);
			val *= 1000000;
			goto exit;
		case hwmon_power_input:
			if (channel == 0)
				cmd = CMD_BOARD_POWER;
			else if (channel == 1)
				cmd = CMD_SOC_POWER;
			else
				goto exit;
			ret = telemetry_request(qdev, cmd, TYPE_READ, &val);
			val *= 1000000;
			goto exit;
		default:
			goto exit;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_crit:
			ret = telemetry_request(qdev, CMD_THERMAL_WARNING_TEMP, TYPE_READ, &val);
			val *= 1000;
			goto exit;
		case hwmon_temp_emergency:
			ret = telemetry_request(qdev, CMD_THERMAL_SHUTDOWN_TEMP, TYPE_READ, &val);
			val *= 1000;
			goto exit;
		case hwmon_temp_alarm:
			ret = telemetry_request(qdev, CMD_THERMAL_DDR_TEMP, TYPE_READ, &val);
			goto exit;
		case hwmon_temp_input:
			if (channel == 0)
				cmd = CMD_THERMAL_BOARD_TEMP;
			else if (channel == 1)
				cmd = CMD_THERMAL_SOC_TEMP;
			else
				goto exit;
			ret = telemetry_request(qdev, cmd, TYPE_READ, &val);
			val *= 1000;
			goto exit;
		case hwmon_temp_highest:
			if (channel == 0)
				cmd = CMD_THERMAL_BOARD_MAX_TEMP;
			else if (channel == 1)
				cmd = CMD_THERMAL_SOC_MAX_TEMP;
			else
				goto exit;
			ret = telemetry_request(qdev, cmd, TYPE_READ, &val);
			val *= 1000;
			goto exit;
		default:
			goto exit;
		}
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			if (channel == 0)
				cmd = CMD_VOLTAGE_CX;
			else if (channel == 1)
				cmd = CMD_VOLTAGE_MX;
			else if (channel == 2)
				cmd = CMD_VOLTAGE_NSP_CX;
			else if (channel == 3)
				cmd = CMD_VOLTAGE_NSP_MX;
			else
				goto exit;
			ret = telemetry_request(qdev, cmd, TYPE_READ, &val);
			val *= 1000;
			goto exit;
		default:
			goto exit;
		}
	default:
		goto exit;
	}

exit:
	*vall = (long)val;
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static int qaic_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		      long vall)
{
	struct qaic_device *qdev = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;
	int rcu_id;
	s64 val;
	u8 cmd;

	val = vall;
	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		srcu_read_unlock(&qdev->dev_lock, rcu_id);
		return -ENODEV;
	}

	switch (type) {
	case hwmon_power:
		switch (attr) {
		case hwmon_power_max:
			if (channel == 0)
				cmd = CMD_BOARD_TDP;
			else if (channel == 1)
				cmd = CMD_SOC_TDP;
			else
				goto exit;
			val /= 1000000;
			ret = telemetry_request(qdev, cmd, TYPE_WRITE, &val);
			goto exit;
		default:
			goto exit;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_crit:
			val /= 1000;
			ret = telemetry_request(qdev, CMD_THERMAL_WARNING_TEMP, TYPE_WRITE, &val);
			goto exit;
		case hwmon_temp_emergency:
			val /= 1000;
			ret = telemetry_request(qdev, CMD_THERMAL_SHUTDOWN_TEMP, TYPE_WRITE, &val);
			goto exit;
		default:
			goto exit;
		}
	default:
		goto exit;
	}

exit:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static const struct attribute_group *special_groups[] = {
	&power_group,
	&uptime_group,
	&temp2_group,
	&tops_group,
	&pmic_group,
	&reset_group,
	NULL,
};

static const struct hwmon_ops qaic_ops = {
	.is_visible = qaic_is_visible,
	.read = qaic_read,
	.write = qaic_write,
};

static const u32 qaic_config_temp[] = {
	/* board level */
	HWMON_T_INPUT | HWMON_T_HIGHEST,
	/* SoC level */
	HWMON_T_INPUT | HWMON_T_HIGHEST | HWMON_T_CRIT | HWMON_T_EMERGENCY,
#ifdef _QBP_NEED_HWMON_TEMP_ALARM
	/* DDR level */
	HWMON_T_ALARM,
#endif /* end _QBP_NEED_HWMON_TEMP_ALARM */
	0
};

static const struct hwmon_channel_info qaic_temp = {
	.type = hwmon_temp,
	.config = qaic_config_temp,
};

static const u32 qaic_config_power[] = {
	HWMON_P_INPUT | HWMON_P_MAX, /* board level */
	HWMON_P_INPUT | HWMON_P_MAX, /* soc level */
	0
};

static const struct hwmon_channel_info qaic_power = {
	.type = hwmon_power,
	.config = qaic_config_power,
};

static const u32 qaic_config_volts[] = {
	HWMON_I_INPUT, /* cx */
	HWMON_I_INPUT, /* mx */
	HWMON_I_INPUT, /* cx nsp */
	HWMON_I_INPUT, /* mx nsp */
	0
};

static const struct hwmon_channel_info qaic_volts = {
	.type = hwmon_in,
	.config = qaic_config_volts,
};

static const struct hwmon_channel_info *qaic_info[] = {
	&qaic_power,
	&qaic_temp,
	&qaic_volts,
	NULL
};

static const struct hwmon_chip_info qaic_chip_info = {
	.ops = &qaic_ops,
	.info = qaic_info
};

static int qaic_telemetry_mhi_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id)
{
	struct qaic_device *qdev = pci_get_drvdata(to_pci_dev(mhi_dev->mhi_cntrl->cntrl_dev));
	int ret;

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret)
		return ret;

	qdev->hwmon = hwmon_device_register_with_info(&qdev->pdev->dev, "qaic", qdev,
						      &qaic_chip_info, special_groups);
	if (!qdev->hwmon) {
		mhi_unprepare_from_transfer(mhi_dev);
		return -ENODEV;
	}

	dev_set_drvdata(&mhi_dev->dev, qdev);
	qdev->tele_ch = mhi_dev;
	qdev->tele_lost_buf = false;

	return 0;
}

static void qaic_telemetry_mhi_remove(struct mhi_device *mhi_dev)
{
	struct qaic_device *qdev = dev_get_drvdata(&mhi_dev->dev);

	hwmon_device_unregister(qdev->hwmon);
	mhi_unprepare_from_transfer(qdev->tele_ch);
	qdev->tele_ch = NULL;
	qdev->hwmon = NULL;
}

static void resp_worker(struct work_struct *work)
{
	struct resp_work *resp = container_of(work, struct resp_work, work);
	struct qaic_device *qdev = resp->qdev;
	struct telemetry_msg *msg = resp->buf;
	struct xfer_queue_elem *elem;
	struct xfer_queue_elem *i;
	bool found = false;

	if (le16_to_cpu(msg->hdr.magic) != MAGIC) {
		kfree(msg);
		kfree(resp);
		return;
	}

	mutex_lock(&qdev->tele_mutex);
	list_for_each_entry_safe(elem, i, &qdev->tele_xfer_list, list) {
		if (elem->seq_num == le32_to_cpu(msg->hdr.seq_num)) {
			found = true;
			list_del_init(&elem->list);
			elem->buf = msg;
			complete_all(&elem->xfer_done);
			break;
		}
	}
	mutex_unlock(&qdev->tele_mutex);

	if (!found)
		/* request must have timed out, drop packet */
		kfree(msg);

	kfree(resp);
}

static void qaic_telemetry_mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
					  struct mhi_result *mhi_result)
{
	struct telemetry_msg *msg = mhi_result->buf_addr;
	struct wrapper_msg *wrapper;

	wrapper = container_of(msg, struct wrapper_msg, msg);
	kref_put(&wrapper->ref_count, free_wrapper);
}

static void qaic_telemetry_mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
					  struct mhi_result *mhi_result)
{
	struct qaic_device *qdev = dev_get_drvdata(&mhi_dev->dev);
	struct telemetry_msg *msg = mhi_result->buf_addr;
	struct resp_work *resp;

	if (mhi_result->transaction_status) {
		kfree(msg);
		return;
	}

	resp = kmalloc(sizeof(*resp), GFP_ATOMIC);
	if (!resp) {
		pci_err(qdev->pdev, "dl_xfer_cb alloc fail, dropping message\n");
		kfree(msg);
		return;
	}

	INIT_WORK(&resp->work, resp_worker);
	resp->qdev = qdev;
	resp->buf = msg;
	queue_work(qdev->tele_wq, &resp->work);
}

static const struct mhi_device_id qaic_telemetry_mhi_match_table[] = {
	{ .chan = "QAIC_TELEMETRY", },
	{},
};

static struct mhi_driver qaic_telemetry_mhi_driver = {
	.id_table = qaic_telemetry_mhi_match_table,
	.remove = qaic_telemetry_mhi_remove,
	.probe = qaic_telemetry_mhi_probe,
	.ul_xfer_cb = qaic_telemetry_mhi_ul_xfer_cb,
	.dl_xfer_cb = qaic_telemetry_mhi_dl_xfer_cb,
	.driver = {
		.name = "qaic_telemetry",
		.owner = THIS_MODULE,
	},
};

int qaic_telemetry_register(void)
{
	return mhi_driver_register(&qaic_telemetry_mhi_driver);
}

void qaic_telemetry_unregister(void)
{
	mhi_driver_unregister(&qaic_telemetry_mhi_driver);
}

void wake_all_telemetry(struct qaic_device *qdev)
{
	struct xfer_queue_elem *elem;
	struct xfer_queue_elem *i;

	mutex_lock(&qdev->tele_mutex);
	list_for_each_entry_safe(elem, i, &qdev->tele_xfer_list, list) {
		list_del_init(&elem->list);
		complete_all(&elem->xfer_done);
	}
	qdev->tele_lost_buf = false;
	mutex_unlock(&qdev->tele_mutex);
}
