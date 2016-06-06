/*
 * Copyright © 2016 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "litest.h"
#include "litest-int.h"

#define SYSFS_LED_BASE "/tmp/wacom-intuos5-leds"

static inline void
init_sysfs(void)
{
	int fd;
	int rc;
	char buf[] = "2\n";

	rc = mkdir(SYSFS_LED_BASE, 0755);
	if (rc != 0 && errno != EEXIST)
		litest_assert_int_eq(rc, 0);
	rc = mkdir(SYSFS_LED_BASE "/wacom_led", 0755);
	if (rc != 0 && errno != EEXIST)
		litest_assert_int_eq(rc, 0);

	fd = creat(SYSFS_LED_BASE "/wacom_led/status_led0_select", O_WRONLY);
	litest_assert_int_ge(fd, 0);
	rc = write(fd, buf, sizeof(buf));
	ck_assert_int_eq(rc, sizeof(buf));
	close(fd);

	fd = creat(SYSFS_LED_BASE "/wacom_led/status0_luminance", O_WRONLY);
	litest_assert_int_ge(fd, 0);
	rc = write(fd, buf, sizeof(buf));
	ck_assert_int_eq(rc, sizeof(buf));
	close(fd);
}

static void
litest_wacom_intuos5_pad_setup(void)
{
	struct litest_device *d;

	init_sysfs();

	d = litest_create_device(LITEST_WACOM_INTUOS5_PAD);

	litest_set_current_device(d);
}

static void
litest_wacom_intuos5_pad_teardown(void)
{
	unlink(SYSFS_LED_BASE "wacom_led/status_led0_select");
	unlink(SYSFS_LED_BASE "wacom_led/status0_luminance");
	rmdir(SYSFS_LED_BASE "wacom_led");
	rmdir(SYSFS_LED_BASE);

	litest_generic_device_teardown();
}

static struct input_event down[] = {
	{ .type = -1, .code = -1 },
};

static struct input_event move[] = {
	{ .type = -1, .code = -1 },
};

static struct input_event ring_start[] = {
	{ .type = EV_ABS, .code = ABS_WHEEL, .value = LITEST_AUTO_ASSIGN },
	{ .type = EV_ABS, .code = ABS_MISC, .value = 15 },
	{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
	{ .type = -1, .code = -1 },
} ;

static struct input_event ring_change[] = {
	{ .type = EV_ABS, .code = ABS_WHEEL, .value = LITEST_AUTO_ASSIGN },
	{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
	{ .type = -1, .code = -1 },
} ;

static struct input_event ring_end[] = {
	{ .type = EV_ABS, .code = ABS_WHEEL, .value = 0 },
	{ .type = EV_ABS, .code = ABS_MISC, .value = 0 },
	{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
	{ .type = -1, .code = -1 },
} ;

static struct litest_device_interface interface = {
	.touch_down_events = down,
	.touch_move_events = move,
	.pad_ring_start_events = ring_start,
	.pad_ring_change_events = ring_change,
	.pad_ring_end_events = ring_end,
};

static struct input_absinfo absinfo[] = {
	{ ABS_X, 0, 1, 0, 0, 0 },
	{ ABS_Y, 0, 1, 0, 0, 0 },
	{ ABS_WHEEL, 0, 71, 0, 0, 0 },
	{ ABS_MISC, 0, 0, 0, 0, 10 },
	{ .value = -1 },
};

static struct input_id input_id = {
	.bustype = 0x3,
	.vendor = 0x56a,
	.product = 0x27,
};

static int events[] = {
	EV_KEY, BTN_0,
	EV_KEY, BTN_1,
	EV_KEY, BTN_2,
	EV_KEY, BTN_3,
	EV_KEY, BTN_4,
	EV_KEY, BTN_5,
	EV_KEY, BTN_6,
	EV_KEY, BTN_7,
	EV_KEY, BTN_8,
	EV_KEY, BTN_STYLUS,
	-1, -1,
};

static const char udev_rule[] =
"ACTION==\"remove\", GOTO=\"pad_end\"\n"
"KERNEL!=\"event*\", GOTO=\"pad_end\"\n"
"\n"
"ATTRS{name}==\"litest Wacom Intuos5 touch M Pad*\",\\\n"
"    ENV{ID_INPUT_TABLET_PAD}=\"1\",\\\n"
"    ENV{LIBINPUT_TEST_TABLET_PAD_SYSFS_PATH}=\"" SYSFS_LED_BASE "\"\n"
"\n"
"LABEL=\"pad_end\"";

struct litest_test_device litest_wacom_intuos5_pad_device = {
	.type = LITEST_WACOM_INTUOS5_PAD,
	.features = LITEST_TABLET_PAD | LITEST_RING | LITEST_MODES,
	.shortname = "wacom-pad",
	.setup = litest_wacom_intuos5_pad_setup,
	.teardown = litest_wacom_intuos5_pad_teardown,
	.interface = &interface,

	.name = "Wacom Intuos5 touch M Pad",
	.id = &input_id,
	.events = events,
	.absinfo = absinfo,
	.udev_rule = udev_rule,
};
