static int t150_input_configured(struct hid_device *hdev, struct hid_input *hidinput)
{
	struct t150 *t150 = hid_get_drvdata(hdev);
	struct input_dev *input = hidinput->input;
	int axis, errno;

	if (!t150)
		return -ENODEV;

	/* The wheel exposes a single application collection */
	if (t150->joystick)
		return 0;

	t150->combine_supported = hdev->product == USB_T150_PRODUCT_ID &&
		hdev->report_enum[HID_INPUT_REPORT].report_id_hash[STATE_PACKET_INPUT] &&
		hdev->report_enum[HID_INPUT_REPORT].report_id_hash[STATE_PACKET_INPUT]->size == 112;
	t150->joystick = input;
	input_set_drvdata(input, t150);
	input->open = t150_input_open;
	input->close = t150_input_close;

	if (input->absinfo) {
		for (axis = ABS_X; axis <= ABS_BRAKE; axis++) {
			if (!test_bit(axis, input->absbit))
				continue;
			input_abs_set_fuzz(input, axis, 0);
			input_abs_set_flat(input, axis, 0);
		}
	}

	errno = t150_init_ffb(t150);
	if (errno) {
		t150->joystick = NULL;
		return errno;
	}

	return 0;
}

static inline void t150_free_input(struct t150 *t150)
{
}

static int t150_input_open(struct input_dev *dev)
{
	struct t150 *t150 = input_get_drvdata(dev);
	__le16 packet = cpu_to_le16(T150_PKT_INPUT_OPEN);
	int ret;
	u16 gain;

	t150_get_gain(t150, &gain);
	if (READ_ONCE(t150->ff_paused)) {
		ret = t150_set_gain(t150, gain);
		if (ret)
			return ret;
	}

	ret = t150_send_int(t150, &packet, sizeof(packet));
	if (ret)
		return ret;

	ret = hid_hw_open(t150->hid_device);
	if (!ret)
		WRITE_ONCE(t150->ff_paused, false);
	return ret;
}

static void t150_input_close(struct input_dev *dev)
{
	struct t150 *t150 = input_get_drvdata(dev);
	__le16 what = cpu_to_le16(T150_PKT_INPUT_WHAT);
	__le16 close = cpu_to_le16(T150_PKT_INPUT_CLOSE);
	int i;

	t150_ff_quiesce(t150, true);
	hid_hw_close(t150->hid_device);

	for (i = 0; i < 2; i++)
		t150_send_int(t150, &what, sizeof(what));

	t150_send_int(t150, &close, sizeof(close));
}

/**
 * This function updates the current input status of the joystick
 * @hdev target hid device
 * @packet_raw raw interrupt payload
 */
static int t150_update_input(struct hid_device *hdev, struct hid_report *report,
			     uint8_t *packet_raw, int size)
{
	struct t150 *t150 = hid_get_drvdata(hdev);
	struct t150_state_packet *packet;
	u8 mode;

	if (!t150 || size < sizeof(struct t150_state_packet))
		return 0;

	packet = (struct t150_state_packet *)packet_raw;

	if (packet->type != STATE_PACKET_INPUT) {
		/* raw_event can run in interrupt context, so defer the USB
		 * control transfer used to refresh settings to process context. */
		t150_schedule_settings_refresh(t150);
		return 0;
	}

	mode = READ_ONCE(t150->combine_pedals);
	if (mode && t150->combine_supported && report->id == STATE_PACKET_INPUT && size >= 15) {
		u8 *throttle = packet_raw + 5;
		u8 *other = packet_raw + (mode == 1 ? 3 : 7);

		put_unaligned_le16(t150_combined_axis(get_unaligned_le16(throttle),
						     get_unaligned_le16(other)), throttle);
		put_unaligned_le16(1023, other);
	}

	return 0;
}
