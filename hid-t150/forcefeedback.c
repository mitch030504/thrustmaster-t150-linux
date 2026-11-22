static void t150_ff_schedule(struct t150 *t150)
{
	if (!READ_ONCE(t150->gone) && !READ_ONCE(t150->ff_paused))
		schedule_work(&t150->ff_work);
}

static void t150_ff_rewrite_rumble(struct ff_effect *effect)
{
	int magnitude;

	if (effect->type != FF_RUMBLE)
		return;

	/* Same weighting as hid-tmff2 / input ff-core, east-facing so the
	 * T150 direction scaling does not collapse the force to zero. */
	magnitude = effect->u.rumble.strong_magnitude / 3
		  + effect->u.rumble.weak_magnitude / 6;

	effect->type = FF_PERIODIC;
	effect->direction = 16384;
	effect->u.periodic.waveform = FF_SINE;
	effect->u.periodic.period = 50;
	effect->u.periodic.magnitude = magnitude;
	effect->u.periodic.offset = 0;
	effect->u.periodic.phase = 0;
	effect->u.periodic.envelope.attack_length = 0;
	effect->u.periodic.envelope.attack_level = 0;
	effect->u.periodic.envelope.fade_length = 0;
	effect->u.periodic.envelope.fade_level = 0;
}

static uint8_t t150_scale_envelope_level(uint16_t level)
{
	if (level > 0x7fff)
		level = 0x7fff;
	return DIV_ROUND_CLOSEST((uint32_t)level * 0xff, 0x7fff);
}

static uint8_t t150_scale_phase(uint16_t phase)
{
	uint32_t p = phase;

	if (p >= 36000)
		p %= 36000;
	return DIV_ROUND_CLOSEST(p * 0xff, 36000);
}

static int8_t t150_scale_coeff(int16_t coeff, uint8_t level)
{
	int32_t v = coeff / 0x147;

	v = v * level / 100;
	if (v > 100)
		v = 100;
	if (v < -100)
		v = -100;
	return (int8_t)v;
}

static uint8_t t150_scale_sat(uint16_t sat, uint16_t divisor, uint8_t level)
{
	uint32_t v;

	if (!divisor)
		return 0;
	v = sat / divisor;
	v = v * level / 100;
	if (v > 0xff)
		v = 0xff;
	return (uint8_t)v;
}

static inline int t150_init_ffb(struct t150 *t150)
{
	int errno, i;

	t150->ff_effects = kcalloc(T150_FF_MAX_EFFECTS, sizeof(*t150->ff_effects), GFP_KERNEL);
	if (!t150->ff_effects)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(t150_ffb_effects); i++)
		set_bit(t150_ffb_effects[i], t150->joystick->ffbit);

	errno = input_ff_create(t150->joystick, T150_FF_MAX_EFFECTS);
	if (errno) {
		hid_err(t150->hid_device, "error create ff :(. errno=%i\n", errno);
		kfree(t150->ff_effects);
		t150->ff_effects = NULL;
		return errno;
	}

	t150->joystick->ff->upload = t150_ff_upload;
	t150->joystick->ff->erase = t150_ff_erase;
	t150->joystick->ff->playback = t150_ff_play;
	t150->joystick->ff->set_gain = t150_ff_set_gain;
	t150->joystick->ff->set_autocenter = t150_ff_set_autocenter;

	return 0;
}

static inline void t150_free_ffb(struct t150 *t150)
{
	cancel_work_sync(&t150->ff_work);
	kfree(t150->ff_effects);
	t150->ff_effects = NULL;
}

static void t150_ff_prepare_first(struct ff_first *ff_first, struct ff_effect *effect)
{
	struct ff_envelope *ff_envelope;

	switch (effect->type) {
	case FF_CONSTANT:
		ff_envelope = &effect->u.constant.envelope;
		ff_first->f0 = T150_FF_FIRST_CODE_CONSTANT;
		break;
	case FF_PERIODIC:
		ff_envelope = &effect->u.periodic.envelope;
		ff_first->f0 = T150_FF_FIRST_CODE_PERIODIC;
		break;
	case FF_DAMPER:
	case FF_SPRING:
		ff_envelope = NULL;
		ff_first->f0 = T150_FF_FIRST_CODE_CONDITION;
		break;
	default:
		ff_envelope = NULL;
		break;
	}

	ff_first->pk_id0 = effect->id * 0x1c + 0x1c;
	ff_first->f1 = 0;
	ff_first->f2 = 0x46;
	ff_first->f3 = 0x54;

	if (ff_envelope) {
		ff_first->attack_length = cpu_to_le16(ff_envelope->attack_length);
		ff_first->attack_level = t150_scale_envelope_level(ff_envelope->attack_level);
		ff_first->fade_length = cpu_to_le16(ff_envelope->fade_length);
		ff_first->fade_level = t150_scale_envelope_level(ff_envelope->fade_level);
	}
}

static void t150_ff_prepare_update(struct t150 *t150, struct ff_update *ff_update,
				   struct ff_effect *effect)
{
	int32_t level = 0;
	uint8_t cond_level;

	ff_update->pk_id1 = effect->id * 0x1c + 0x0e;
	ff_update->f1 = 0x00;

	switch (effect->type) {
	case FF_PERIODIC:
		ff_update->effect_class = T150_FF_UPDATE_CODE_PERIODIC;
		ff_update->effect.periodic.magnitude = word_high(t150_scale_force(
			effect->u.periodic.magnitude, READ_ONCE(t150->periodic_level),
			READ_ONCE(t150->boost)));
		ff_update->effect.periodic.offset = word_high(t150_scale_force(
			effect->u.periodic.offset, READ_ONCE(t150->periodic_level), false));
		ff_update->effect.periodic.phase = t150_scale_phase(effect->u.periodic.phase);
		ff_update->effect.periodic.period = cpu_to_le16(effect->u.periodic.period);
		break;
	case FF_CONSTANT:
		ff_update->effect_class = T150_FF_UPDATE_CODE_CONSTANT;
		level = effect->u.constant.level * fixp_sin16(effect->direction / (0xFFFF / 360)) * +1;
		level >>= 15;
		level = t150_scale_force(level, READ_ONCE(t150->constant_level),
					 READ_ONCE(t150->boost));
		ff_update->effect.constant.level = (level / 0x01ff);
		break;
	case FF_SPRING:
		cond_level = READ_ONCE(t150->spring_level);
		ff_update->effect_class = T150_FF_UPDATE_CODE_CONDITION;
		ff_update->effect.condition.right_coeff =
			t150_scale_coeff(effect->u.condition[0].right_coeff, cond_level);
		ff_update->effect.condition.left_coeff =
			t150_scale_coeff(effect->u.condition[0].left_coeff, cond_level);
		ff_update->effect.condition.center = cpu_to_le16(
			effect->u.condition[0].center / (0x7fff / 0x01f4));
		ff_update->effect.condition.deadband = cpu_to_le16(
			effect->u.condition[0].deadband / (0xffff / 0x03e8));
		ff_update->effect.condition.right_sat =
			t150_scale_sat(effect->u.condition[0].right_saturation, 0x030c, cond_level);
		ff_update->effect.condition.left_sat =
			t150_scale_sat(effect->u.condition[0].left_saturation, 0x030c, cond_level);
		break;
	case FF_DAMPER:
		cond_level = READ_ONCE(t150->damper_level);
		ff_update->effect_class = T150_FF_UPDATE_CODE_CONDITION;
		ff_update->effect.condition.right_coeff =
			t150_scale_coeff(effect->u.condition[0].right_coeff, cond_level);
		ff_update->effect.condition.left_coeff =
			t150_scale_coeff(effect->u.condition[0].left_coeff, cond_level);
		ff_update->effect.condition.center = cpu_to_le16(
			effect->u.condition[0].center / (0x7fff / 0x01f4));
		ff_update->effect.condition.deadband = cpu_to_le16(
			effect->u.condition[0].deadband / (0xffff / 0x03e8));
		ff_update->effect.condition.right_sat =
			t150_scale_sat(effect->u.condition[0].right_saturation, 0x028f, cond_level);
		ff_update->effect.condition.left_sat =
			t150_scale_sat(effect->u.condition[0].left_saturation, 0x028f, cond_level);
		break;
	default:
		ff_update->effect_class = T150_FF_UPDATE_CODE_PERIODIC;
		break;
	}
}

static void t150_ff_prepare_commit(struct ff_commit *ff_commit, struct ff_effect *effect)
{
	ff_commit->f0 = 0x01;
	ff_commit->id = effect->id;
	if (effect->replay.length)
		ff_commit->length = cpu_to_le16(effect->replay.length);
	else
		ff_commit->length = cpu_to_le16(0xffff);
	ff_commit->f1 = 0;
	ff_commit->f2 = 0;
	ff_commit->pk_id1 = effect->id * 0x1c + 0x0e;
	ff_commit->f3 = 0;
	ff_commit->pk_id0 = effect->id * 0x1c + 0x1c;
	ff_commit->f4 = 0;
	ff_commit->delay = min_t(uint16_t, effect->replay.delay, 0xff);
	ff_commit->f5 = 0;

	switch (effect->type) {
	case FF_PERIODIC:
		switch (effect->u.periodic.waveform) {
		case FF_SAW_UP:
			ff_commit->effect_type = cpu_to_le16(T150_FF_COMMIT_CODE_SAW_UP);
			break;
		case FF_SAW_DOWN:
			ff_commit->effect_type = cpu_to_le16(T150_FF_COMMIT_CODE_SAW_DOWN);
			break;
		case FF_SINE:
		default:
			ff_commit->effect_type = cpu_to_le16(T150_FF_COMMIT_CODE_SINE);
			break;
		}
		break;
	case FF_CONSTANT:
		ff_commit->effect_type = cpu_to_le16(T150_FF_COMMIT_CODE_CONSTANT);
		break;
	case FF_SPRING:
		ff_commit->effect_type = cpu_to_le16(T150_FF_COMMIT_CODE_SPRING);
		break;
	case FF_DAMPER:
		ff_commit->effect_type = cpu_to_le16(T150_FF_COMMIT_CODE_DAMPER);
		break;
	default:
		pr_err("t150: unknown effect type: %i\n", effect->type);
		break;
	}
}

static int t150_ff_send_upload(struct t150 *t150, struct t150_ff_effect *slot)
{
	struct ff_first first_old = { 0 }, first_new = { 0 };
	struct ff_update update_old = { 0 }, update_new = { 0 };
	struct ff_commit commit_old = { 0 }, commit_new = { 0 };
	size_t first_len = sizeof(first_new);
	size_t update_len = sizeof(update_new);
	int errno = 0;

	t150_ff_prepare_first(&first_new, &slot->effect);
	t150_ff_prepare_update(t150, &update_new, &slot->effect);
	t150_ff_prepare_commit(&commit_new, &slot->effect);

	/* Constant and periodic transfers use their own sizes in T150 captures. */
	if (t150->hid_device->product == USB_T150_PRODUCT_ID) {
		switch (update_new.effect_class) {
		case T150_FF_UPDATE_CODE_CONSTANT:
			first_len = offsetof(struct ff_first, f2);
			update_len = offsetofend(struct ff_update, effect.constant);
			break;
		case T150_FF_UPDATE_CODE_PERIODIC:
			first_len = offsetof(struct ff_first, f2);
			update_len = offsetofend(struct ff_update, effect.periodic);
			break;
		}
	}

	if (slot->has_old) {
		t150_ff_prepare_first(&first_old, &slot->old);
		t150_ff_prepare_update(t150, &update_old, &slot->old);
		t150_ff_prepare_commit(&commit_old, &slot->old);
	}

	mutex_lock(&t150->lock);
	if (T150_FF_BLIND_UPLOAD || !slot->has_old ||
	    memcmp(&first_old, &first_new, sizeof(first_new))) {
		errno = t150_send_int_locked(t150, &first_new, first_len);
		if (errno) {
			hid_err(t150->hid_device, "ffb first packet of effect %d failed: %d\n",
				slot->effect.id, errno);
			goto out;
		}
	}

	if (T150_FF_BLIND_UPLOAD || !slot->has_old ||
	    memcmp(&update_old, &update_new, sizeof(update_new))) {
		errno = t150_send_int_locked(t150, &update_new, update_len);
		if (errno) {
			hid_err(t150->hid_device, "ffb update packet of effect %d failed: %d\n",
				slot->effect.id, errno);
			goto out;
		}
	}

	if (T150_FF_BLIND_UPLOAD || !slot->has_old ||
	    memcmp(&commit_old, &commit_new, sizeof(commit_new))) {
		errno = t150_send_int_locked(t150, &commit_new, sizeof(commit_new));
		if (errno) {
			hid_err(t150->hid_device, "ffb commit packet of effect %d failed: %d\n",
				slot->effect.id, errno);
			goto out;
		}
	}

out:
	mutex_unlock(&t150->lock);
	return errno;
}

static int t150_ff_send_play(struct t150 *t150, int effect_id, int times)
{
	struct ff_change_effect_status pkt;
	int errno;

	pkt.f0 = 0x41;
	pkt.id = effect_id;
	pkt.mode = times ? 0x41 : 0x00;
	pkt.times = times ? min_t(int, times, 0xff) : 0x01;

	errno = t150_send_int(t150, &pkt, sizeof(pkt));
	if (errno)
		hid_err(t150->hid_device, "unable to play effect %d, errno %d\n",
			effect_id, errno);
	return errno;
}

static int t150_ff_stop_effect(struct t150 *t150, int id)
{
	int ret = t150_ff_send_play(t150, id, 0);

	if (ret && ret != -ENODEV && ret != -ESHUTDOWN) {
		t150->ff_fault = true;
		t150_send_gain(t150, 0, 0);
		hid_err(t150->hid_device, "force stop failed, reconnect the wheel\n");
	}
	return ret;
}

/* ff_io_lock orders playback, erase and synchronous evdev uploads. */
static void t150_ff_work(struct work_struct *work)
{
	struct t150 *t150 = container_of(work, struct t150, ff_work);
	unsigned long flags;
	int id, value;
	u16 gain, ac, limit;
	bool gain_pending, ac_pending, upload;
	struct t150_ff_effect *slot;

	mutex_lock(&t150->ff_io_lock);
	if (READ_ONCE(t150->gone) || READ_ONCE(t150->ff_paused))
		goto out;

	for (id = 0; id < T150_FF_MAX_EFFECTS; id++) {
		slot = &t150->ff_effects[id];
		spin_lock_irqsave(&t150->ff_lock, flags);
		value = slot->stop_pending;
		slot->stop_pending = false;
		spin_unlock_irqrestore(&t150->ff_lock, flags);
		if (value && slot->valid)
			t150_ff_stop_effect(t150, id);
	}

	if (t150->ff_fault)
		goto out;

	spin_lock_irqsave(&t150->ff_lock, flags);
	gain_pending = t150->gain_pending;
	gain = t150->pending_gain;
	t150->gain_pending = false;
	ac_pending = t150->autocenter_pending;
	ac = t150->pending_autocenter;
	t150->autocenter_pending = false;
	spin_unlock_irqrestore(&t150->ff_lock, flags);

	if (gain_pending) {
		t150_get_gain(t150, &limit);
		if (!t150_send_gain(t150, limit, gain))
			t150->game_gain = gain;
	}

	if (ac_pending) {
		bool wheel_centering;
		t150_get_enable_autocenter(t150, &wheel_centering);
		if (!wheel_centering &&
		    (!ac || !t150_settings_set40(t150, SET40_RETURN_FORCE,
						 DIV_ROUND_CLOSEST((u32)ac * 100, U16_MAX))))
			t150_settings_set40(t150, SET40_USE_RETURN_FORCE, !!ac);
	}

	for (id = 0; id < T150_FF_MAX_EFFECTS; id++) {
		slot = &t150->ff_effects[id];
		spin_lock_irqsave(&t150->ff_lock, flags);
		value = slot->play_pending && !slot->stop_pending ? slot->play_value : -1;
		if (value >= 0)
			slot->play_pending = false;
		spin_unlock_irqrestore(&t150->ff_lock, flags);

		upload = slot->upload_pending;
		slot->upload_pending = false;
		if (slot->valid && upload && t150_ff_send_upload(t150, slot)) {
			if (t150_ff_stop_effect(t150, id))
				goto out;
			slot->valid = false;
		}
		if (slot->valid && value >= 0 && !READ_ONCE(t150->ff_paused)) {
			if (t150_ff_send_play(t150, id, value))
				t150_ff_stop_effect(t150, id);
			if (t150->ff_fault)
				goto out;
		}
	}
out:
	mutex_unlock(&t150->ff_io_lock);
}

static void t150_ff_resubmit_condition_effects(struct t150 *t150, uint16_t type)
{
	int id;

	mutex_lock(&t150->ff_io_lock);
	if (!t150->ff_effects)
		goto out;
	for (id = 0; id < T150_FF_MAX_EFFECTS; id++) {
		struct t150_ff_effect *slot = &t150->ff_effects[id];

		if (slot->valid && slot->effect.type == type) {
			slot->has_old = false;
			slot->upload_pending = true;
		}
	}
	t150_ff_schedule(t150);
out:
	mutex_unlock(&t150->ff_io_lock);
}

static int t150_ff_upload(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	struct t150 *t150 = input_get_drvdata(dev);
	struct t150_ff_effect next = { .effect = *effect };
	struct t150_ff_effect *slot;
	int ret;

	if (effect->id < 0 || effect->id >= T150_FF_MAX_EFFECTS)
		return -EINVAL;
	if (effect->type == FF_PERIODIC) {
		u16 waveform = effect->u.periodic.waveform;

		if (!effect->u.periodic.period)
			return -EINVAL;
		if (waveform != FF_SINE && waveform != FF_SAW_UP && waveform != FF_SAW_DOWN)
			return -EOPNOTSUPP;
	}
	t150_ff_rewrite_rumble(&next.effect);

	mutex_lock(&t150->ff_io_lock);
	if (READ_ONCE(t150->gone) || READ_ONCE(t150->ff_paused)) {
		ret = -ENODEV;
		goto out;
	}

	if (t150->ff_fault) {
		ret = -EIO;
		goto out;
	}

	slot = &t150->ff_effects[effect->id];
	next.has_old = slot->valid && !slot->upload_pending;
	next.old = slot->effect;
	ret = t150_ff_send_upload(t150, &next);
	if (ret) {
		if (!t150_ff_stop_effect(t150, effect->id))
			slot->valid = false;
	} else {
		slot->effect = next.effect;
		slot->upload_pending = false;
		slot->has_old = false;
		slot->valid = true;
	}
out:
	mutex_unlock(&t150->ff_io_lock);
	return ret;
}

static int t150_ff_erase(struct input_dev *dev, int effect_id)
{
	struct t150 *t150 = input_get_drvdata(dev);
	struct t150_ff_effect *slot = &t150->ff_effects[effect_id];
	unsigned long flags;
	int ret = 0;

	mutex_lock(&t150->ff_io_lock);
	spin_lock_irqsave(&t150->ff_lock, flags);
	slot->play_pending = false;
	slot->stop_pending = false;
	spin_unlock_irqrestore(&t150->ff_lock, flags);

	if (slot->valid && !READ_ONCE(t150->gone) && !READ_ONCE(t150->ff_paused))
		ret = t150_ff_stop_effect(t150, effect_id);
	if (!ret)
		slot->valid = false;
	slot->upload_pending = false;
	mutex_unlock(&t150->ff_io_lock);
	return ret;
}

static int t150_ff_quiesce(struct t150 *t150, bool forget)
{
	unsigned long flags;
	int id, ret = 0;

	spin_lock_irqsave(&t150->ff_lock, flags);
	if (t150->ff_paused || t150->gone || !t150->ff_effects) {
		spin_unlock_irqrestore(&t150->ff_lock, flags);
		return 0;
	}
	WRITE_ONCE(t150->ff_paused, true);
	t150->gain_pending = false;
	t150->autocenter_pending = false;
	for (id = 0; id < T150_FF_MAX_EFFECTS; id++) {
		t150->ff_effects[id].play_pending = false;
		t150->ff_effects[id].stop_pending = false;
	}
	spin_unlock_irqrestore(&t150->ff_lock, flags);

	cancel_work_sync(&t150->ff_work);
	mutex_lock(&t150->ff_io_lock);
	for (id = 0; id < T150_FF_MAX_EFFECTS; id++) {
		struct t150_ff_effect *slot = &t150->ff_effects[id];
		int error;

		if (slot->valid) {
			error = t150_ff_stop_effect(t150, id);
			if (error && !ret)
				ret = error;
		}
		slot->valid = slot->valid && !forget;
		slot->upload_pending = slot->valid;
		slot->has_old = false;
	}
	t150->game_gain = U16_MAX;
	if (forget && !t150->ff_fault) {
		bool enable;
		u16 autocenter;

		t150_get_enable_autocenter(t150, &enable);
		t150_get_autocenter(t150, &autocenter);
		t150_set_autocenter(t150, autocenter);
		t150_set_enable_autocenter(t150, enable);
	}
	mutex_unlock(&t150->ff_io_lock);
	return ret;
}

static int t150_ff_play(struct input_dev *dev, int effect_id, int value)
{
	struct t150 *t150 = input_get_drvdata(dev);
	unsigned long flags;

	if (effect_id < 0 || effect_id >= T150_FF_MAX_EFFECTS || value < 0)
		return -EINVAL;

	spin_lock_irqsave(&t150->ff_lock, flags);
	if (!t150->ff_paused && !t150->gone) {
		struct t150_ff_effect *slot = &t150->ff_effects[effect_id];

		if (!value)
			slot->stop_pending = true;
		slot->play_value = value;
		slot->play_pending = !!value;
		t150_ff_schedule(t150);
	}
	spin_unlock_irqrestore(&t150->ff_lock, flags);
	return 0;
}

static void t150_ff_set_gain(struct input_dev *dev, uint16_t gain)
{
	struct t150 *t150 = input_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&t150->ff_lock, flags);
	if (!t150->ff_paused && !t150->gone) {
		t150->pending_gain = gain;
		t150->gain_pending = true;
		t150_ff_schedule(t150);
	}
	spin_unlock_irqrestore(&t150->ff_lock, flags);
}

static void t150_ff_set_autocenter(struct input_dev *dev, uint16_t value)
{
	struct t150 *t150 = input_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&t150->ff_lock, flags);
	if (!t150->ff_paused && !t150->gone) {
		t150->pending_autocenter = value;
		t150->autocenter_pending = true;
		t150_ff_schedule(t150);
	}
	spin_unlock_irqrestore(&t150->ff_lock, flags);
}
