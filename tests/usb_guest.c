/* SPDX-License-Identifier: GPL-2.0-only */
/* Runs as PID 1 in the disposable QEMU guest launched by check_usb_guest.py. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/usb/raw_gadget.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint8_t report[130];
static const uint8_t device[] = {
	18, 1, 0, 2, 0, 0, 0, 64, 0x4f, 0x04, 0x77, 0xb6, 0, 1, 0, 0, 0, 1
};
static const uint8_t config[] = {
	9, 2, 41, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 2, 3, 0, 0, 0,
	9, 0x21, 0x11, 1, 0, 1, 0x22, sizeof(report), 0,
	7, 5, 1, 3, 32, 0, 4, 7, 5, 0x82, 3, 16, 0, 2,
};
struct io {
	struct usb_raw_ep_io header;
	uint8_t data[512];
};

static int gadget, ep_in, ep_out;

static void poweroff(void)
{
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}

static void require(int valid, const char *what)
{
	if (!valid) {
		perror(what);
		poweroff();
	}
}

static void load_module(const char *path)
{
	int fd = open(path, O_RDONLY);
	require(fd >= 0 && syscall(SYS_finit_module, fd, "", 0) == 0, path);
	close(fd);
}

static void *reports(void *unused)
{
	struct io io = {
		.header = { .ep = ep_in, .length = 15 },
		.data = { 7, 0, 0x80, 0xff, 3, 0xff, 3, 0xff, 3, 0, 0, 0, 0, 0, 8 },
	};
	for (;;) {
		usleep(2000);
		ioctl(gadget, USB_RAW_IOCTL_EP_WRITE, &io);
	}
}

static void *packets(void *unused)
{
	struct io io = { .header = { .ep = ep_out, .length = 32 } };
	for (;;) {
		int len = ioctl(gadget, USB_RAW_IOCTL_EP_READ, &io);
		if (len > 0) {
			printf("OUT");
			for (int i = 0; i < len; ++i)
				printf(" %02x", io.data[i]);
			puts("");
		}
	}
}

static void reply_in(struct usb_ctrlrequest req)
{
	struct io io = {0};
	const uint8_t firmware[] = {0, 35};
	const uint8_t *data = NULL;
	size_t size = 0;

	if (req.bRequest == USB_REQ_GET_DESCRIPTOR) {
		switch (req.wValue >> 8) {
		case USB_DT_DEVICE: data = device; size = sizeof(device); break;
		case USB_DT_CONFIG: data = config; size = sizeof(config); break;
		case 0x22: data = report; size = sizeof(report); break;
		}
	} else if (req.bRequest == 86) {
		data = firmware;
		size = sizeof(firmware);
	}
	if (!data) {
		ioctl(gadget, USB_RAW_IOCTL_EP0_STALL, 0);
		return;
	}
	io.header.length = size > req.wLength ? req.wLength : size;
	memcpy(io.data, data, io.header.length);
	ioctl(gadget, USB_RAW_IOCTL_EP0_WRITE, &io);
}

static void emulate(void)
{
	struct usb_raw_init init = { "dummy_udc", "dummy_udc.0", USB_SPEED_FULL };
	int fd = open("/report", O_RDONLY);

	require(fd >= 0 && read(fd, report, sizeof(report)) == sizeof(report), "read report fixture");
	close(fd);
	gadget = open("/dev/raw-gadget", O_RDWR);
	require(gadget >= 0 && ioctl(gadget, USB_RAW_IOCTL_INIT, &init) == 0 &&
		ioctl(gadget, USB_RAW_IOCTL_RUN, 0) == 0, "start raw gadget");
	for (;;) {
		struct { struct usb_raw_event header; struct usb_ctrlrequest request; } event = {
			.header.length = sizeof(event.request),
		};
		require(ioctl(gadget, USB_RAW_IOCTL_EVENT_FETCH, &event) == 0, "gadget event");
		if (event.header.type != USB_RAW_EVENT_CONTROL)
			continue;
		if (event.request.bRequestType & USB_DIR_IN) {
			reply_in(event.request);
			continue;
		}
		if (event.request.bRequestType == 0 && event.request.bRequest == USB_REQ_SET_CONFIGURATION) {
			pthread_t reader, writer;
			ep_out = ioctl(gadget, USB_RAW_IOCTL_EP_ENABLE, config + 27);
			ep_in = ioctl(gadget, USB_RAW_IOCTL_EP_ENABLE, config + 34);
			require(ep_out >= 0 && ep_in >= 0 &&
				ioctl(gadget, USB_RAW_IOCTL_CONFIGURE, 0) == 0, "configure endpoints");
			require(pthread_create(&reader, NULL, packets, NULL) == 0 &&
				pthread_create(&writer, NULL, reports, NULL) == 0, "gadget endpoint threads");
		}
		struct io io = { .header.length = event.request.wLength };
		ioctl(gadget, USB_RAW_IOCTL_EP0_READ, &io);
	}
}

static void write_sysfs(const char *path, const char *id)
{
	int fd = open(path, O_WRONLY);
	require(fd >= 0 && write(fd, id, strlen(id)) == strlen(id), path);
	close(fd);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	require(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0 &&
		mount("sysfs", "/sys", "sysfs", 0, NULL) == 0, "guest mount");
	load_module("/udc-core.ko");
	load_module("/dummy_hcd.ko");
	load_module("/raw_gadget.ko");
	load_module("/hid-t150.ko");
	pid_t child = fork();
	require(child >= 0, "fork emulator");
	if (!child)
		emulate();

	glob_t paths = {0};
	int status = GLOB_NOMATCH;
	const char *prefix = "/sys/bus/hid/drivers/hid-t150";
	for (int i = 0; status && i < 50; ++i) {
		usleep(100000);
		status = glob("/sys/bus/hid/drivers/hid-t150/0003:044F:B677.*", 0, NULL, &paths);
		if (status && !glob("/sys/bus/hid/drivers/hid-generic/0003:044F:B677.*", 0, NULL, &paths)) {
			const char *id = strrchr(paths.gl_pathv[0], '/') + 1;
			write_sysfs("/sys/bus/hid/drivers/hid-generic/unbind", id);
			write_sysfs("/sys/bus/hid/drivers/hid-t150/bind", id);
			globfree(&paths);
		}
	}
	require(!status, "probe dummy wheel");
	char id[64];
	snprintf(id, sizeof(id), "%s", strrchr(paths.gl_pathv[0], '/') + 1);
	globfree(&paths);
	char path[256];
	snprintf(path, sizeof(path), "%s/%s/firmware_version", prefix, id);
	int firmware = open(path, O_RDONLY);
	char version[3];
	require(firmware >= 0 && read(firmware, version, sizeof(version)) == 3 &&
		memcmp(version, "35\n", 3) == 0, "short firmware reply");
	close(firmware);
	snprintf(path, sizeof(path), "%s/%s/input/input*/event*", prefix, id);
	require(!glob(path, 0, NULL, &paths), "find dummy evdev");
	snprintf(path, sizeof(path), "/dev/input%s", strrchr(paths.gl_pathv[0], '/'));
	int wheel = open(path, O_RDWR);
	require(wheel >= 0, "open dummy evdev");
	snprintf(path, sizeof(path), "%s/%s/combine_pedals", prefix, id);
	write_sysfs(path, "1");
	usleep(40000);
	struct input_absinfo pedal;
	require(ioctl(wheel, EVIOCGABS(ABS_RZ), &pedal) == 0 && pedal.value == 511,
		"combined pedal center");
	write_sysfs(path, "0");
	usleep(40000);
	require(ioctl(wheel, EVIOCGABS(ABS_RZ), &pedal) == 0 && pedal.value == 1023,
		"separate pedal rest");

	int count = 0;
	require(ioctl(wheel, EVIOCGEFFECTS, &count) == 0 && count == 9, "nine effects");
	for (int i = 0; i < count; ++i) {
		struct ff_effect ff = { .type = FF_CONSTANT, .id = -1, .direction = 0x4000,
			.replay = { .length = 500 }, .u.constant = { .level = 2048 } };
		require(ioctl(wheel, EVIOCSFF, &ff) == 0 && ff.id == i, "upload constant");
	}
	struct input_event play = { .type = EV_FF, .code = 0, .value = 1 };
	require(write(wheel, &play, sizeof(play)) == sizeof(play), "start effect");
	usleep(40000);
	play.value = 0;
	require(write(wheel, &play, sizeof(play)) == sizeof(play), "stop effect");
	usleep(40000);
	require(ioctl(wheel, EVIOCRMFF, count - 1) == 0, "release final slot");
	struct ff_effect sine = { .type = FF_PERIODIC, .id = -1, .direction = 0x4000,
		.replay = { .length = 500 },
		.u.periodic = { .waveform = FF_SINE, .period = 125, .magnitude = 2048 } };
	require(ioctl(wheel, EVIOCSFF, &sine) == 0 && sine.id == count - 1, "upload sine");
	close(wheel);
	write_sysfs("/sys/bus/hid/drivers/hid-t150/unbind", id);
	puts("T150_GUEST_USB_OK");
	poweroff();
}
