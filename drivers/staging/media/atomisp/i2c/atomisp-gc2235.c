// SPDX-License-Identifier: GPL-2.0
/*
 * Support for GalaxyCore GC2235 2M camera sensor.
 *
 * Copyright (c) 2014 Intel Corporation. All Rights Reserved.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/moduleparam.h>
#include <media/v4l2-device.h>
#include "../include/linux/atomisp_gmin_platform.h"
#include <linux/acpi.h>
#include <linux/io.h>

#include "gc2235.h"

/* i2c read/write stuff */
static int gc2235_read_reg(struct i2c_client *client,
			   u16 data_length, u16 reg, u16 *val)
{
	int err;
	struct i2c_msg msg[2];
	unsigned char data[6];

	if (!client->adapter) {
		dev_err(&client->dev, "%s error, no client->adapter\n",
			__func__);
		return -ENODEV;
	}

	if (data_length != GC2235_8BIT) {
		dev_err(&client->dev, "%s error, invalid data length\n",
			__func__);
		return -EINVAL;
	}

	memset(msg, 0, sizeof(msg));

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = data;

	/* high byte goes out first */
	data[0] = (u8)(reg & 0xff);

	msg[1].addr = client->addr;
	msg[1].len = data_length;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = data;

	err = i2c_transfer(client->adapter, msg, 2);
	if (err != 2) {
		if (err >= 0)
			err = -EIO;
		dev_err(&client->dev,
			"read from offset 0x%x error %d", reg, err);
		return err;
	}

	*val = 0;
	/* high byte comes first */
	if (data_length == GC2235_8BIT)
		*val = (u8)data[0];

	return 0;
}

static int gc2235_i2c_write(struct i2c_client *client, u16 len, u8 *data)
{
	struct i2c_msg msg;
	const int num_msg = 1;
	int ret;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.len = len;
	msg.buf = data;
	ret = i2c_transfer(client->adapter, &msg, 1);

	return ret == num_msg ? 0 : -EIO;
}

static int gc2235_write_reg(struct i2c_client *client, u16 data_length,
			    u8 reg, u8 val)
{
	int ret;
	unsigned char data[4] = {0};
	const u16 len = data_length + sizeof(u8); /* 16-bit address + data */

	if (data_length != GC2235_8BIT) {
		dev_err(&client->dev,
			"%s error, invalid data_length\n", __func__);
		return -EINVAL;
	}

	/* high byte goes out first */
	data[0] = reg;
	data[1] = val;

	ret = gc2235_i2c_write(client, len, data);
	if (ret)
		dev_err(&client->dev,
			"write error: wrote 0x%x to offset 0x%x error %d",
			val, reg, ret);

	return ret;
}

static int __gc2235_flush_reg_array(struct i2c_client *client,
				    struct gc2235_write_ctrl *ctrl)
{
	u16 size;

	if (ctrl->index == 0)
		return 0;

	size = sizeof(u8) + ctrl->index; /* 8-bit address + data */
	ctrl->index = 0;

	return gc2235_i2c_write(client, size, (u8 *)&ctrl->buffer);
}

static int __gc2235_buf_reg_array(struct i2c_client *client,
				  struct gc2235_write_ctrl *ctrl,
				  const struct gc2235_reg *next)
{
	int size;

	if (next->type != GC2235_8BIT)
		return -EINVAL;

	size = 1;
	ctrl->buffer.data[ctrl->index] = (u8)next->val;

	/* When first item is added, we need to store its starting address */
	if (ctrl->index == 0)
		ctrl->buffer.addr = next->reg;

	ctrl->index += size;

	/*
	 * Buffer cannot guarantee free space for u32? Better flush it to avoid
	 * possible lack of memory for next item.
	 */
	if (ctrl->index + sizeof(u8) >= GC2235_MAX_WRITE_BUF_SIZE)
		return __gc2235_flush_reg_array(client, ctrl);

	return 0;
}

static int __gc2235_write_reg_is_consecutive(struct i2c_client *client,
					     struct gc2235_write_ctrl *ctrl,
					     const struct gc2235_reg *next)
{
	if (ctrl->index == 0)
		return 1;

	return ctrl->buffer.addr + ctrl->index == next->reg;
}

static int gc2235_write_reg_array(struct i2c_client *client,
				  const struct gc2235_reg *reglist)
{
	const struct gc2235_reg *next = reglist;
	struct gc2235_write_ctrl ctrl;
	int err;

	ctrl.index = 0;
	for (; next->type != GC2235_TOK_TERM; next++) {
		switch (next->type & GC2235_TOK_MASK) {
		case GC2235_TOK_DELAY:
			err = __gc2235_flush_reg_array(client, &ctrl);
			if (err)
				return err;
			msleep(next->val);
			break;
		default:
			/*
			 * If next address is not consecutive, data needs to be
			 * flushed before proceed.
			 */
			if (!__gc2235_write_reg_is_consecutive(client, &ctrl,
							       next)) {
				err = __gc2235_flush_reg_array(client, &ctrl);
				if (err)
					return err;
			}
			err = __gc2235_buf_reg_array(client, &ctrl, next);
			if (err) {
				dev_err(&client->dev, "%s: write error, aborted\n",
					__func__);
				return err;
			}
			break;
		}
	}

	return __gc2235_flush_reg_array(client, &ctrl);
}

static long __gc2235_set_exposure(struct v4l2_subdev *sd, int coarse_itg,
				  int gain, int digitgain)

{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 coarse_integration = (u16)coarse_itg;
	int ret = 0;
	u16 expo_coarse_h, expo_coarse_l, gain_val = 0xF0, gain_val2 = 0xF0;

	expo_coarse_h = coarse_integration >> 8;
	expo_coarse_l = coarse_integration & 0xff;

	ret = gc2235_write_reg(client, GC2235_8BIT,
			       GC2235_EXPOSURE_H, expo_coarse_h);
	ret = gc2235_write_reg(client, GC2235_8BIT,
			       GC2235_EXPOSURE_L, expo_coarse_l);

	if (gain <= 0x58) {
		gain_val = 0x40;
		gain_val2 = 0x58;
	} else if (gain < 256) {
		gain_val = 0x40;
		gain_val2 = gain;
	} else {
		gain_val2 = 64 * gain / 256;
		gain_val = 0xff;
	}

	ret = gc2235_write_reg(client, GC2235_8BIT,
			       GC2235_GLOBAL_GAIN, (u8)gain_val);
	ret = gc2235_write_reg(client, GC2235_8BIT,
			       GC2235_PRE_GAIN, (u8)gain_val2);

	return ret;
}

static int gc2235_set_exposure(struct v4l2_subdev *sd, int exposure,
			       int gain, int digitgain)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	int ret;

	mutex_lock(&dev->input_lock);
	ret = __gc2235_set_exposure(sd, exposure, gain, digitgain);
	mutex_unlock(&dev->input_lock);

	return ret;
}

static long gc2235_s_exposure(struct v4l2_subdev *sd,
			      struct atomisp_exposure *exposure)
{
	int exp = exposure->integration_time[0];
	int gain = exposure->gain[0];
	int digitgain = exposure->gain[1];

	/* we should not accept the invalid value below. */
	if (gain == 0) {
		struct i2c_client *client = v4l2_get_subdevdata(sd);

		v4l2_err(client, "%s: invalid value\n", __func__);
		return -EINVAL;
	}

	return gc2235_set_exposure(sd, exp, gain, digitgain);
}

static long gc2235_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	switch (cmd) {
	case ATOMISP_IOC_S_EXPOSURE:
		return gc2235_s_exposure(sd, arg);
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * This returns the exposure time being used. This should only be used
 * for filling in EXIF data, not for actual image processing.
 */
static int gc2235_q_exposure(struct v4l2_subdev *sd, s32 *value)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u16 reg_v, reg_v2;
	int ret;

	/* get exposure */
	ret = gc2235_read_reg(client, GC2235_8BIT,
			      GC2235_EXPOSURE_L,
			      &reg_v);
	if (ret)
		goto err;

	ret = gc2235_read_reg(client, GC2235_8BIT,
			      GC2235_EXPOSURE_H,
			      &reg_v2);
	if (ret)
		goto err;

	reg_v += reg_v2 << 8;

	*value = reg_v;
err:
	return ret;
}

static int gc2235_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc2235_device *dev =
	    container_of(ctrl->handler, struct gc2235_device, ctrl_handler);
	int ret = 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_ABSOLUTE:
		ret = gc2235_q_exposure(&dev->sd, &ctrl->val);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ctrl_ops = {
	.g_volatile_ctrl = gc2235_g_volatile_ctrl
};

static struct v4l2_ctrl_config gc2235_controls[] = {
	{
		.ops = &ctrl_ops,
		.id = V4L2_CID_EXPOSURE_ABSOLUTE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "exposure",
		.min = 0x0,
		.max = 0xffff,
		.step = 0x01,
		.def = 0x00,
		.flags = 0,
	},
};

static int __gc2235_init(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	/* restore settings */
	gc2235_res = gc2235_res_preview;
	N_RES = N_RES_PREVIEW;

	return gc2235_write_reg_array(client, gc2235_init_settings);
}

static int is_init;

static int power_ctrl(struct v4l2_subdev *sd, bool flag)
{
	int ret = -1;
	struct gc2235_device *dev = to_gc2235_sensor(sd);

	if (!dev || !dev->platform_data)
		return -ENODEV;

	if (flag) {
		ret = dev->platform_data->v1p8_ctrl(sd, 1);
		usleep_range(60, 90);
		if (ret == 0)
			ret |= dev->platform_data->v2p8_ctrl(sd, 1);
	} else {
		ret = dev->platform_data->v1p8_ctrl(sd, 0);
		ret |= dev->platform_data->v2p8_ctrl(sd, 0);
	}
	return ret;
}

static int gpio_ctrl(struct v4l2_subdev *sd, bool flag)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	int ret;

	if (!dev || !dev->platform_data)
		return -ENODEV;

	ret = dev->platform_data->gpio1_ctrl(sd, !flag);
	usleep_range(60, 90);
	ret |= dev->platform_data->gpio0_ctrl(sd, flag);

	return ret;
}

static int power_up(struct v4l2_subdev *sd)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	if (!dev->platform_data) {
		dev_err(&client->dev,
			"no camera_sensor_platform_data");
		return -ENODEV;
	}
	/* power control */
	ret = power_ctrl(sd, 1);
	if (ret)
		goto fail_power;

	/* according to DS, at least 5ms is needed between DOVDD and PWDN */
	usleep_range(5000, 6000);

	ret = dev->platform_data->flisclk_ctrl(sd, 1);
	if (ret)
		goto fail_clk;
	usleep_range(5000, 6000);

	/* gpio ctrl */
	ret = gpio_ctrl(sd, 1);
	if (ret) {
		ret = gpio_ctrl(sd, 1);
		if (ret)
			goto fail_power;
	}

	msleep(5);
	return 0;

fail_clk:
	gpio_ctrl(sd, 0);
fail_power:
	power_ctrl(sd, 0);
	dev_err(&client->dev, "sensor power-up failed\n");

	return ret;
}

static int power_down(struct v4l2_subdev *sd)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (!dev->platform_data) {
		dev_err(&client->dev,
			"no camera_sensor_platform_data");
		return -ENODEV;
	}
	/* gpio ctrl */
	ret = gpio_ctrl(sd, 0);
	if (ret) {
		ret = gpio_ctrl(sd, 0);
		if (ret)
			dev_err(&client->dev, "gpio failed 2\n");
	}

	ret = dev->platform_data->flisclk_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "flisclk failed\n");

	/* power control */
	ret = power_ctrl(sd, 0);
	if (ret)
		dev_err(&client->dev, "vprog failed.\n");

	return ret;
}

static int gc2235_s_power(struct v4l2_subdev *sd, int on)
{
	int ret;

	if (on == 0) {
		ret = power_down(sd);
	} else {
		ret = power_up(sd);
		if (!ret)
			ret = __gc2235_init(sd);
		is_init = 1;
	}
	return ret;
}

static int startup(struct v4l2_subdev *sd)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (is_init == 0) {
		/*
		 * force gc2235 to do a reset in res change, otherwise it
		 * can not output normal after switching res. and it is not
		 * necessary for first time run up after power on, for the sack
		 * of performance
		 */
		power_down(sd);
		power_up(sd);
		gc2235_write_reg_array(client, gc2235_init_settings);
	}

	ret = gc2235_write_reg_array(client, dev->res->regs);
	if (ret) {
		dev_err(&client->dev, "gc2235 write register err.\n");
		return ret;
	}
	is_init = 0;

	return ret;
}

static int gc2235_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct camera_mipi_info *gc2235_info = NULL;
	struct gc2235_resolution *res;
	int ret = 0;

	gc2235_info = v4l2_get_subdev_hostdata(sd);
	if (!gc2235_info)
		return -EINVAL;
	if (format->pad)
		return -EINVAL;
	if (!fmt)
		return -EINVAL;

	mutex_lock(&dev->input_lock);
	res = v4l2_find_nearest_size(gc2235_res_preview,
				     ARRAY_SIZE(gc2235_res_preview), width,
				     height, fmt->width, fmt->height);
	if (!res)
		res = &gc2235_res_preview[N_RES - 1];

	fmt->width = res->width;
	fmt->height = res->height;
	dev->res = res;

	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(sd_state, 0) = *fmt;
		mutex_unlock(&dev->input_lock);
		return 0;
	}

	ret = startup(sd);
	if (ret) {
		dev_err(&client->dev, "gc2235 startup err\n");
		goto err;
	}

err:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int gc2235_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct gc2235_device *dev = to_gc2235_sensor(sd);

	if (format->pad)
		return -EINVAL;

	if (!fmt)
		return -EINVAL;

	fmt->width = dev->res->width;
	fmt->height = dev->res->height;
	fmt->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int gc2235_detect(struct i2c_client *client)
{
	struct i2c_adapter *adapter = client->adapter;
	u16 high = 0, low = 0;
	u16 id;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -ENODEV;

	gc2235_read_reg(client, GC2235_8BIT, GC2235_SENSOR_ID_H, &high);
	gc2235_read_reg(client, GC2235_8BIT, GC2235_SENSOR_ID_L, &low);
	id = ((high << 8) | low);

	if (id != GC2235_ID) {
		dev_err(&client->dev, "sensor ID error, 0x%x\n", id);
		return -ENODEV;
	}

	dev_info(&client->dev, "detect gc2235 success\n");
	return 0;
}

static int gc2235_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	mutex_lock(&dev->input_lock);

	if (enable)
		ret = gc2235_write_reg_array(client, gc2235_stream_on);
	else
		ret = gc2235_write_reg_array(client, gc2235_stream_off);

	mutex_unlock(&dev->input_lock);
	return ret;
}

static int gc2235_s_config(struct v4l2_subdev *sd,
			   int irq, void *platform_data)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	if (!platform_data)
		return -ENODEV;

	dev->platform_data =
	    (struct camera_sensor_platform_data *)platform_data;

	mutex_lock(&dev->input_lock);
	/*
	 * power off the module, then power on it in future
	 * as first power on by board may not fulfill the
	 * power on sequqence needed by the module
	 */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "gc2235 power-off err.\n");
		goto fail_power_off;
	}

	ret = power_up(sd);
	if (ret) {
		dev_err(&client->dev, "gc2235 power-up err.\n");
		goto fail_power_on;
	}

	ret = dev->platform_data->csi_cfg(sd, 1);
	if (ret)
		goto fail_csi_cfg;

	/* config & detect sensor */
	ret = gc2235_detect(client);
	if (ret) {
		dev_err(&client->dev, "gc2235_detect err s_config.\n");
		goto fail_csi_cfg;
	}

	/* turn off sensor, after probed */
	ret = power_down(sd);
	if (ret) {
		dev_err(&client->dev, "gc2235 power-off err.\n");
		goto fail_csi_cfg;
	}
	mutex_unlock(&dev->input_lock);

	return 0;

fail_csi_cfg:
	dev->platform_data->csi_cfg(sd, 0);
fail_power_on:
	power_down(sd);
	dev_err(&client->dev, "sensor power-gating failed\n");
fail_power_off:
	mutex_unlock(&dev->input_lock);
	return ret;
}

static int gc2235_get_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_frame_interval *interval)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);

	/*
	 * FIXME: Implement support for V4L2_SUBDEV_FORMAT_TRY, using the V4L2
	 * subdev active state API.
	 */
	if (interval->which != V4L2_SUBDEV_FORMAT_ACTIVE)
		return -EINVAL;

	interval->interval.numerator = 1;
	interval->interval.denominator = dev->res->fps;

	return 0;
}

static int gc2235_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= MAX_FMTS)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int gc2235_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	int index = fse->index;

	if (index >= N_RES)
		return -EINVAL;

	fse->min_width = gc2235_res[index].width;
	fse->min_height = gc2235_res[index].height;
	fse->max_width = gc2235_res[index].width;
	fse->max_height = gc2235_res[index].height;

	return 0;
}

static int gc2235_g_skip_frames(struct v4l2_subdev *sd, u32 *frames)
{
	struct gc2235_device *dev = to_gc2235_sensor(sd);

	mutex_lock(&dev->input_lock);
	*frames = dev->res->skip_frames;
	mutex_unlock(&dev->input_lock);

	return 0;
}

static const struct v4l2_subdev_sensor_ops gc2235_sensor_ops = {
	.g_skip_frames	= gc2235_g_skip_frames,
};

static const struct v4l2_subdev_video_ops gc2235_video_ops = {
	.s_stream = gc2235_s_stream,
};

static const struct v4l2_subdev_core_ops gc2235_core_ops = {
	.s_power = gc2235_s_power,
	.ioctl = gc2235_ioctl,
};

static const struct v4l2_subdev_pad_ops gc2235_pad_ops = {
	.enum_mbus_code = gc2235_enum_mbus_code,
	.enum_frame_size = gc2235_enum_frame_size,
	.get_fmt = gc2235_get_fmt,
	.set_fmt = gc2235_set_fmt,
	.get_frame_interval = gc2235_get_frame_interval,
};

static const struct v4l2_subdev_ops gc2235_ops = {
	.core = &gc2235_core_ops,
	.video = &gc2235_video_ops,
	.pad = &gc2235_pad_ops,
	.sensor = &gc2235_sensor_ops,
};

static void gc2235_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2235_device *dev = to_gc2235_sensor(sd);

	dev->platform_data->csi_cfg(sd, 0);

	v4l2_device_unregister_subdev(sd);
	media_entity_cleanup(&dev->sd.entity);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	kfree(dev);
}

static int gc2235_probe(struct i2c_client *client)
{
	struct gc2235_device *dev;
	void *gcpdev;
	int ret;
	unsigned int i;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->input_lock);

	dev->res = &gc2235_res_preview[0];
	v4l2_i2c_subdev_init(&dev->sd, client, &gc2235_ops);

	gcpdev = gmin_camera_platform_data(&dev->sd,
					   ATOMISP_INPUT_FORMAT_RAW_10,
					   atomisp_bayer_order_grbg);

	ret = gc2235_s_config(&dev->sd, client->irq, gcpdev);
	if (ret)
		goto out_free;

	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret =
	    v4l2_ctrl_handler_init(&dev->ctrl_handler,
				   ARRAY_SIZE(gc2235_controls));
	if (ret) {
		gc2235_remove(client);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(gc2235_controls); i++)
		v4l2_ctrl_new_custom(&dev->ctrl_handler, &gc2235_controls[i],
				     NULL);

	if (dev->ctrl_handler.error) {
		gc2235_remove(client);
		return dev->ctrl_handler.error;
	}

	/* Use same lock for controls as for everything else. */
	dev->ctrl_handler.lock = &dev->input_lock;
	dev->sd.ctrl_handler = &dev->ctrl_handler;

	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret)
		gc2235_remove(client);

	return atomisp_register_i2c_module(&dev->sd, gcpdev);

out_free:
	v4l2_device_unregister_subdev(&dev->sd);
	kfree(dev);

	return ret;
}

static const struct acpi_device_id gc2235_acpi_match[] = {
	{ "INT33F8" },
	{},
};
MODULE_DEVICE_TABLE(acpi, gc2235_acpi_match);

static struct i2c_driver gc2235_driver = {
	.driver = {
		.name = "gc2235",
		.acpi_match_table = gc2235_acpi_match,
	},
	.probe = gc2235_probe,
	.remove = gc2235_remove,
};
module_i2c_driver(gc2235_driver);

MODULE_AUTHOR("Shuguang Gong <Shuguang.Gong@intel.com>");
MODULE_DESCRIPTION("A low-level driver for GC2235 sensors");
MODULE_LICENSE("GPL");
