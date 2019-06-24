/*
 * ADC Sensor driver
 *
 * Copyright  (C)  2016 - 2017 Topband. Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the ADC sensor into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Author: shenhaibo
 * Version: 1.0.0
 * Release Date: 2019/6/17
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/signal.h>
#include <linux/pm.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/adc.h>
#include <linux/slab.h>
#include <linux/wakelock.h>

#include <linux/iio/iio.h>
#include <linux/iio/machine.h>
#include <linux/iio/driver.h>
#include <linux/iio/consumer.h>


#define ADC_SENSOR_NAME "adcsensor"

// ioctl cmd
#define ADC_SENSOR_IOC_MAGIC  'a'

#define ADC_SENSOR_IOC_ENABLE _IOW(ADC_SENSOR_IOC_MAGIC, 1, int)
#define ADC_SENSOR_IOC_SET_RATE _IOW(ADC_SENSOR_IOC_MAGIC, 2, int)

#define ADC_SENSOR_IOC_MAXNR 2

#define EMPTY_ADVALUE			950
#define DRIFT_ADVALUE			70
#define INVALID_ADVALUE			-1

#define POLL_INTERVAL_DEFAULT   1000

#define HZ_TO_PERIOD_NSEC(hz)		(1000 * 1000 * 1000 / ((u32)(hz)))
#define MS_TO_US(x)			({ typeof(x) _x = (x); ((_x) * \
							((typeof(x)) 1000));})
#define US_TO_NS(x)			(MS_TO_US(x))
#define MS_TO_NS(x)			(US_TO_NS(MS_TO_US(x)))
#define US_TO_MS(x)			({ typeof(x) _x = (x); ((_x) / \
							((typeof(x)) 1000));})
#define NS_TO_US(x)			(US_TO_MS(x))
#define NS_TO_MS(x)			(US_TO_MS(NS_TO_US(x)))

struct adc_sensor_data {
    struct platform_device *platform_dev;
    struct miscdevice adc_sensor_device;
    struct input_dev *input_dev;
    struct iio_channel *chan;

    bool enabled;
    unsigned int poll_interval;
    struct hrtimer hr_timer;
    struct work_struct input_work;
    ktime_t ktime;
};

struct adc_sensor_data *g_adc;

static struct workqueue_struct *adc_sensor_workqueue = 0;

static int adc_sensor_enable(struct adc_sensor_data *adc) {
    hrtimer_start(&adc->hr_timer, adc->ktime, HRTIMER_MODE_REL);
    adc->enabled = true;
    return 0;
}

static int adc_sensor_disable(struct adc_sensor_data *adc) {
    cancel_work_sync(&adc->input_work);
    hrtimer_cancel(&adc->hr_timer);
    adc->enabled = false;
    return 0;
}

static int adc_sensor_parse_dt(struct device *dev,
                               struct adc_sensor_data *adc)
{
    struct iio_channel *chan;

    chan = iio_channel_get(dev, NULL);
    if (IS_ERR(chan)) {
        dev_info(dev, "no io-channels defined\n");
        return -EINVAL;
    }
    adc->chan = chan;

    return 0;
}

static int adc_sensor_request_input_dev(struct adc_sensor_data *adc)
{
    int ret = -1;

    adc->input_dev = input_allocate_device();
    if (adc->input_dev == NULL) {
        dev_err(&adc->platform_dev->dev, "Failed to to allocate input device\n");
        return -ENOMEM;
    }

    adc->input_dev->name = ADC_SENSOR_NAME;
    input_set_drvdata(adc->input_dev, adc);

    input_set_capability(adc->input_dev, EV_REL, REL_MISC);

    ret = input_register_device(adc->input_dev);
    if (ret) {
        dev_err(&adc->platform_dev->dev, "Register %s input device failed\n",
                adc->input_dev->name);
        input_free_device(adc->input_dev);
        return -ENODEV;
    }

    return 0;
}

static int adc_sensor_dev_open(struct inode *inode, struct file *filp)
{
    int ret = 0;

    struct adc_sensor_data *adc = container_of(filp->private_data,
                                  struct adc_sensor_data,
                                  adc_sensor_device);
    filp->private_data = adc;
    dev_info(&adc->platform_dev->dev,
             "device node major=%d, minor=%d\n", imajor(inode), iminor(inode));

    return ret;
}

static long adc_sensor_dev_ioctl(struct file *pfile,
                                 unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    int data = 0;
    struct adc_sensor_data *adc = pfile->private_data;

    if (_IOC_TYPE(cmd) != ADC_SENSOR_IOC_MAGIC) {
        return -EINVAL;
    }
    if (_IOC_NR(cmd) > ADC_SENSOR_IOC_MAXNR) {
        return -EINVAL;
    }

    if (_IOC_DIR(cmd) & _IOC_READ) {
        ret = !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd));
    } else if (_IOC_DIR(cmd) & _IOC_WRITE) {
        ret = !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd));
    }
    if (ret) {
        return -EFAULT;
    }

    if (copy_from_user(&data, (int *)arg, sizeof(int))) {
        dev_err(&adc->platform_dev->dev,
                "%s, copy from user failed\n", __func__);
        return -EFAULT;
    }

    dev_info(&adc->platform_dev->dev,
             "%s, (%x, %lx): data=%d\n", __func__, cmd,
             arg, data);

    switch (cmd) {
        case ADC_SENSOR_IOC_ENABLE:
            if (data > 0) {
                adc_sensor_enable(adc);
            } else {
                adc_sensor_disable(adc);
            }
            break;

        case ADC_SENSOR_IOC_SET_RATE:
            break;

        default:
            return -EINVAL;
    }

    return ret;
}

static const struct file_operations adc_sensor_dev_fops = {
    .owner = THIS_MODULE,
    .open = adc_sensor_dev_open,
    .unlocked_ioctl = adc_sensor_dev_ioctl
};

static int adc_sensor_adc_iio_read(struct adc_sensor_data *adc)
{
    struct iio_channel *channel = adc->chan;
    int val, ret;

    if (!channel) {
        return INVALID_ADVALUE;
    }

    ret = iio_read_channel_raw(channel, &val);
    if (ret < 0) {
        dev_err(&adc->platform_dev->dev,
                "%s, read channel error: %d\n", __func__, ret);
        return ret;
    }

    return val;
}

static void adc_sensor_report_event(struct adc_sensor_data *adc, s32 data)
{
    struct input_dev *input = adc->input_dev;

    if (!adc->enabled) {
        return;
    }

    input_report_rel(input, REL_MISC, data);
    input_sync(input);
}

enum hrtimer_restart adc_sensor_poll_function_read(struct hrtimer *timer)
{
    struct adc_sensor_data *adc;

    adc = container_of((struct hrtimer *)timer, struct adc_sensor_data, hr_timer);

    queue_work(adc_sensor_workqueue, &adc->input_work);

    return HRTIMER_NORESTART;
}

static void poll_function_work(struct work_struct *input_work)
{
    int ret;
    struct adc_sensor_data *adc;

    adc = container_of((struct work_struct *)input_work,
                       struct adc_sensor_data, input_work);

    hrtimer_start(&adc->hr_timer, adc->ktime, HRTIMER_MODE_REL);

    ret = adc_sensor_adc_iio_read(adc);
    if (ret > INVALID_ADVALUE && ret < EMPTY_ADVALUE) {
        adc_sensor_report_event(adc, ret);
    }
}

static int adc_sensor_init(struct adc_sensor_data *adc)
{
    hrtimer_init(&adc->hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    adc->hr_timer.function = &adc_sensor_poll_function_read;

    adc->poll_interval = POLL_INTERVAL_DEFAULT;
    adc->ktime = ktime_set(0, MS_TO_NS(adc->poll_interval));
    INIT_WORK(&adc->input_work, poll_function_work);

    return 0;
}

static int adc_sensor_probe(struct platform_device *pdev)
{
    int ret = 0;
    struct adc_sensor_data *adc;

    adc = devm_kzalloc(&pdev->dev, sizeof(*adc), GFP_KERNEL);
    if (adc == NULL) {
        dev_err(&pdev->dev, "Failed to alloc ts memory");
        return -ENOMEM;
    }

    g_adc = adc;

    if (pdev->dev.of_node) {
        ret = adc_sensor_parse_dt(&pdev->dev, adc);
        if (ret) {
            dev_err(&pdev->dev, "Failed to parse dts\n");
            goto exit_free_data;
        }
    }

    adc->platform_dev = pdev;

    ret = adc_sensor_request_input_dev(adc);
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to register input device\n");
        goto exit_free_data;
    }

    platform_set_drvdata(pdev, adc);

    if(adc_sensor_workqueue == 0)
		adc_sensor_workqueue = create_workqueue("adc_sensor_workqueue");

    ret = adc_sensor_init(adc);
    if (ret < 0) {
        dev_err(&pdev->dev, "failed to init sensor\n");
		goto exit_unreg_input_dev;
	}

    adc->adc_sensor_device.minor = MISC_DYNAMIC_MINOR;
    adc->adc_sensor_device.name = ADC_SENSOR_NAME;
    adc->adc_sensor_device.fops = &adc_sensor_dev_fops;

    ret = misc_register(&adc->adc_sensor_device);
    if (ret) {
        dev_err(&pdev->dev, "Failed to misc_register\n");
        goto exit_unreg_input_dev;
    }

    dev_info(&pdev->dev, "%s, over\n", __func__);
    return 0;

exit_unreg_input_dev:
    input_unregister_device(adc->input_dev);

exit_free_data:
    devm_kfree(&pdev->dev, adc);

    return ret;
}

static int adc_sensor_remove(struct platform_device *pdev)
{
    struct adc_sensor_data *adc = platform_get_drvdata(pdev);
    
    input_unregister_device(adc->input_dev);
    kfree(adc);

    return 0;
}

static const struct of_device_id adc_sensor_of_match[] = {
    { .compatible =  "adcsensor"},
    {},
};

MODULE_DEVICE_TABLE(of, adc_sensor_of_match);

static struct platform_driver adc_sensor_driver = {
    .probe = adc_sensor_probe,
    .remove = adc_sensor_remove,
    .driver = {
        .name = ADC_SENSOR_NAME,
        .owner  = THIS_MODULE,
        .of_match_table = adc_sensor_of_match,
    },
};

module_platform_driver(adc_sensor_driver);

MODULE_AUTHOR("shenhb@topband.com.cn");
MODULE_DESCRIPTION("ADC Sensor for appcation to detect battery voltage");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
