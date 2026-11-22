#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/completion.h>
#include <linux/input.h>
#include <linux/usb/input.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/fixp-arith.h>
#include <linux/spinlock.h>
#include <linux/hid.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hid-t150.h"
#include "../common/t150_presets.h"
#include "../common/t150_controls.h"
#include "input.h"
#include "attributes.h"
#include "settings.h"
#include "forcefeedback.h"
#include "packet.h"

static char *default_preset = "sim";
module_param_named(preset, default_preset, charp, 0644);
MODULE_PARM_DESC(preset,
		 "Default wheel-feel preset: sim, arcade, street, offroad, rally, drift, truck");

static int t150_find_endpoints(struct t150 *t150)
{
	struct usb_interface *interface = to_usb_interface(t150->hid_device->dev.parent);
	struct usb_endpoint_descriptor *ep, *ep_irq_in = NULL, *ep_irq_out = NULL;
	int i;

	t150->usb_device = interface_to_usbdev(interface);

	for (i = 0; i < interface->cur_altsetting->desc.bNumEndpoints; i++) {
		ep = &interface->cur_altsetting->endpoint[i].desc;
		if (!usb_endpoint_xfer_int(ep))
			continue;
		if (usb_endpoint_dir_in(ep))
			ep_irq_in = ep;
		else
			ep_irq_out = ep;
	}

	if (!ep_irq_in || !ep_irq_out)
		return -ENODEV;

	t150->pipe_in = usb_rcvintpipe(t150->usb_device, ep_irq_in->bEndpointAddress);
	t150->pipe_out = usb_sndintpipe(t150->usb_device, ep_irq_out->bEndpointAddress);
	t150->bInterval_in = ep_irq_in->bInterval;
	t150->bInterval_out = ep_irq_out->bInterval;
	return 0;
}

static int t150_constructor(struct t150 *t150, struct hid_device *hid_device)
{
	int error_code;
	int n;

	t150->hid_device = hid_device;
	hid_set_drvdata(hid_device, t150);

	mutex_init(&t150->lock);
	mutex_init(&t150->ff_io_lock);
	t150->game_gain = U16_MAX;
	t150->constant_level = 100;
	t150->periodic_level = 100;
	spin_lock_init(&t150->settings.access_lock);
	spin_lock_init(&t150->ff_lock);
	INIT_WORK(&t150->settings_refresh_work, t150_settings_refresh_work);
	INIT_WORK(&t150->ff_work, t150_ff_work);
	t150->spring_level = 100;
	t150->damper_level = 100;

	error_code = hid_parse(hid_device);
	if (error_code) {
		hid_err(hid_device, "hid_parse() failed\n");
		return error_code;
	}

	error_code = t150_find_endpoints(t150);
	if (error_code)
		return error_code;

	n = usb_make_path(t150->usb_device, t150->dev_path, sizeof(t150->dev_path));
	if (n > 0 && n < sizeof(t150->dev_path))
		strscpy(t150->dev_path + n, "/input0", sizeof(t150->dev_path) - n);

	t150->usb_buf_size = T150_USB_BUF_SIZE;
	t150->usb_buf = kzalloc(t150->usb_buf_size, GFP_KERNEL);
	if (!t150->usb_buf)
		return -ENOMEM;

	error_code = t150_setup_task(t150);
	if (error_code)
		goto err_buf;

	error_code = hid_hw_start(hid_device, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (error_code) {
		hid_err(hid_device, "hid_hw_start() failed\n");
		goto err_hw_start;
	}

	if (!t150->joystick) {
		error_code = -ENODEV;
		goto err_hw;
	}

	error_code = t150_init_attributes(t150);
	if (error_code)
		goto err_hw;

	return 0;

err_hw:
	WRITE_ONCE(t150->gone, true);
	hid_hw_stop(hid_device);
	t150_cancel_settings_refresh(t150);
	t150_free_ffb(t150);
	goto err_buf;
err_hw_start:
	t150_free_ffb(t150);
err_buf:
	kfree(t150->usb_buf);
	t150->usb_buf = NULL;
	return error_code;
}

static int t150_probe(struct hid_device *hid_device, const struct hid_device_id *id)
{
	int error_code = 0;
	struct t150 *t150;

	t150 = kzalloc(sizeof(*t150), GFP_KERNEL);
	if (!t150)
		return -ENOMEM;

	error_code = t150_constructor(t150, hid_device);
	if (error_code)
		goto error0;

	hid_info(hid_device, "T150/TMX wheel ready\n");
	return 0;

error0:
	kfree(t150);
	return error_code;
}

static void t150_remove(struct hid_device *hid_device)
{
	struct t150 *t150 = hid_get_drvdata(hid_device);

	if (!t150)
		return;

	hid_info(t150->hid_device, "T150RS Wheel removed. Bye\n");

	t150_free_attributes(t150);
	t150_ff_quiesce(t150, true);
	WRITE_ONCE(t150->gone, true);
	hid_hw_stop(hid_device);
	t150_cancel_settings_refresh(t150);
	t150_free_ffb(t150);

	mutex_lock(&t150->lock);
	mutex_unlock(&t150->lock);

	kfree(t150->usb_buf);
	kfree(t150);
}

#include "attributes.c"
#include "input.c"
#include "settings.c"
#include "forcefeedback.c"

static int t150_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct t150 *t150 = hid_get_drvdata(hdev);

	t150_ff_quiesce(t150, false);
	WRITE_ONCE(t150->suspended, true);
	t150_cancel_settings_refresh(t150);
	return 0;
}

static int t150_resume(struct hid_device *hdev)
{
	struct t150 *t150 = hid_get_drvdata(hdev);
	u16 range, gain, autocenter;
	bool enable;
	int ret;

	t150_get_range(t150, &range);
	t150_get_gain(t150, &gain);
	t150_get_autocenter(t150, &autocenter);
	t150_get_enable_autocenter(t150, &enable);
	WRITE_ONCE(t150->suspended, false);
	ret = t150_set_range(t150, range);
	if (!ret)
		ret = t150_set_gain(t150, gain);
	if (!ret)
		ret = t150_set_autocenter(t150, autocenter);
	if (!ret)
		ret = t150_set_enable_autocenter(t150, enable);
	if (!ret)
		WRITE_ONCE(t150->ff_paused, false);
	else
		WRITE_ONCE(t150->suspended, true);
	return ret;
}

static const struct hid_device_id t150_table[] = {
	{ HID_USB_DEVICE(USB_THRUSTMASTER_VENDOR_ID, USB_T150_PRODUCT_ID) },
	{ HID_USB_DEVICE(USB_THRUSTMASTER_VENDOR_ID, USB_TMX_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, t150_table);

static struct hid_driver t150_driver = {
	.name = "hid-t150",
	.id_table = t150_table,
	.probe = t150_probe,
	.remove = t150_remove,
	.raw_event = t150_update_input,
	.input_configured = t150_input_configured,
	.suspend = t150_suspend,
	.resume = t150_resume,
	.reset_resume = t150_resume,
};
module_hid_driver(t150_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dario Pagani <>");
MODULE_DESCRIPTION("ThrustMaster T150 steering wheel driver");
MODULE_VERSION("1.2.0");
