/*
 * Copyright 2014 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libtsm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "dbus.h"
#include "dbus_interface.h"
#include "input.h"
#include "keysym.h"
#include "main.h"
#include "util.h"
#include "vt.h"

struct input_key_event {
	uint16_t code;
	unsigned char value;
};

/*
 * Pointer state of an input device, applied to the pointer on each SYN_REPORT:
 *  absolute - the device reports ABS_X/ABS_Y positions.
 *  touchpad - the positions are a finger moving the pointer relatively.
 *  abs_x, abs_y - range of the positions.
 *  x, y, abs_changed - latest position, and whether it changed.
 *  last_x, last_y, tracking - touchpad position at the previous report, if
 *    the same fingers were down.
 *  fingers - number of fingers on the touchpad.
 *  frac_x, frac_y, scroll - touchpad motion not yet applied.
 *  rel_x, rel_y, wheel - relative motion since the previous report.
 */
struct input_pointer {
	bool absolute;
	bool touchpad;
	struct input_absinfo abs_x, abs_y;
	int32_t x, y;
	bool abs_changed;
	int32_t last_x, last_y;
	bool tracking;
	unsigned int fingers;
	int64_t frac_x, frac_y;
	int32_t scroll;
	int32_t rel_x, rel_y, wheel;
};

struct input_dev {
	int fd;
	char* path;
	struct input_pointer pointer;
};

static void input_pointer_init(struct input_dev* dev);

struct keyboard_state {
	bool left_shift_state;
	bool right_shift_state;
	bool left_control_state;
	bool right_control_state;
	bool left_alt_state;
	bool right_alt_state;
	bool search_state;
};

/*
 * structure to keep input state:
 *  ndevs - number of input devices.
 *  devs - input devices to listen to.
 *  kbd_state - tracks modifier keys that are pressed.
 *  xkb_context, xkb_keymap, xkb_state - keyboard layout from
 *    xkeyboard-config, or NULL to use the built-in US layout.
 */
struct {
	unsigned int ndevs;
	struct input_dev* devs;
	struct keyboard_state kbd_state;
	struct xkb_context* xkb_context;
	struct xkb_keymap* xkb_keymap;
	struct xkb_state* xkb_state;
} input = {
	.ndevs = 0,
	.devs = NULL,
};

static bool is_shift_pressed(struct keyboard_state* k)
{
	return k->left_shift_state || k->right_shift_state;
}

static bool is_control_pressed(struct keyboard_state* k)
{
	return k->left_control_state || k->right_control_state;
}

static bool is_alt_pressed(struct keyboard_state* k)
{
	return k->left_alt_state || k->right_alt_state;
}

/* Return 1 if event is handled. */
static int input_special_key(struct input_key_event* ev)
{
	terminal_t* terminal;

	terminal = term_get_current_terminal();

	switch (ev->code) {
	case KEY_LEFTSHIFT:
		input.kbd_state.left_shift_state = ! !ev->value;
		return 1;
	case KEY_RIGHTSHIFT:
		input.kbd_state.right_shift_state = ! !ev->value;
		return 1;
	case KEY_LEFTCTRL:
		input.kbd_state.left_control_state = ! !ev->value;
		return 1;
	case KEY_RIGHTCTRL:
		input.kbd_state.right_control_state = ! !ev->value;
		return 1;
	case KEY_LEFTALT:
		input.kbd_state.left_alt_state = ! !ev->value;
		return 1;
	case KEY_RIGHTALT:
		input.kbd_state.right_alt_state = ! !ev->value;
		return 1;
	case KEY_LEFTMETA: // search key
		input.kbd_state.search_state = ! !ev->value;
		return 1;
	}

	if (term_is_active(terminal)) {
		/*
		 * Print Screen takes a screenshot to be read with the
		 * screenshot escape. Alt+Print Screen is the kernel's SysRq.
		 */
		if (command_flags.enable_osc &&
		    (ev->code == KEY_SYSRQ || ev->code == KEY_PRINT) &&
		    !is_alt_pressed(&input.kbd_state) && ev->value) {
			if (ev->value == 1)
				term_screenshot(terminal);
			return 1;
		}

		if (is_shift_pressed(&input.kbd_state) && ev->value) {
			switch (ev->code) {
			case KEY_PAGEUP:
				term_page_up(terminal);
				return 1;
			case KEY_PAGEDOWN:
				term_page_down(terminal);
				return 1;
			case KEY_UP:
				if (input.kbd_state.search_state)
					term_page_up(terminal);
				else
					term_line_up(terminal);
				return 1;
			case KEY_DOWN:
				if (input.kbd_state.search_state)
					term_page_down(terminal);
				else
					term_line_down(terminal);
				return 1;
			}
		}

		if (!is_alt_pressed(&input.kbd_state) &&
		    is_control_pressed(&input.kbd_state) &&
		    is_shift_pressed(&input.kbd_state) && ev->value) {
			switch (ev->code) {
			case KEY_MINUS:
				term_zoom(false);
				return 1;
			case KEY_EQUAL:
				term_zoom(true);
				return 1;
			}
		}

		if (!(input.kbd_state.search_state ||
		     is_alt_pressed(&input.kbd_state) ||
		     is_control_pressed(&input.kbd_state)) &&
		    ev->value) {
			switch (ev->code) {
				case KEY_F1:
				case KEY_F2:
				case KEY_F3:
				case KEY_F4:
				case KEY_F5:
					break;
				case KEY_F6:
				case KEY_F7:
					dbus_report_user_activity(USER_ACTIVITY_BRIGHTNESS_DOWN_KEY_PRESS -
								(ev->code - KEY_F6));
					return 1;
				case KEY_F8:
				case KEY_F9:
				case KEY_F10:
					break;
				case KEY_BRIGHTNESSDOWN:
					dbus_report_user_activity(USER_ACTIVITY_BRIGHTNESS_DOWN_KEY_PRESS);
					return 1;
				case KEY_BRIGHTNESSUP:
					dbus_report_user_activity(USER_ACTIVITY_BRIGHTNESS_UP_KEY_PRESS);
					return 1;
				case KEY_MUTE:
					dbus_report_user_activity(USER_ACTIVITY_VOLUME_MUTE_KEY_PRESS);
					return 1;
				case KEY_VOLUMEDOWN:
					dbus_report_user_activity(USER_ACTIVITY_VOLUME_DOWN_KEY_PRESS);
					return 1;
				case KEY_VOLUMEUP:
					dbus_report_user_activity(USER_ACTIVITY_VOLUME_MUTE_KEY_PRESS);
					return 1;
			}
		}
	}

	/*
	 * Kernel VT switching. The kernel keyboard is off on our VT, so we
	 * have to ask the kernel to switch on its behalf.
	 */
	if (vt_is_enabled() && vt_is_foreground() &&
	    is_alt_pressed(&input.kbd_state) &&
	    is_control_pressed(&input.kbd_state) &&
	    !is_shift_pressed(&input.kbd_state) &&
	    ev->value) {
		if ((ev->code >= KEY_F1) && (ev->code <= KEY_F10)) {
			vt_switch_to(ev->code - KEY_F1 + 1);
			return 1;
		}
		if (ev->code == KEY_F11) {
			vt_switch_to(11);
			return 1;
		}
		if (ev->code == KEY_F12) {
			vt_switch_to(12);
			return 1;
		}
	}

	/*
	 * Special case for key sequence that is used by Crouton.
	 * Just explicitly ignore here and do nothing.
	 * TODO(dbehr) remove it, when dnschneid is cool with it.
	 */
	if (command_flags.enable_vts &&
	    is_alt_pressed(&input.kbd_state) &&
	    is_control_pressed(&input.kbd_state) &&
	    is_shift_pressed(&input.kbd_state) &&
	    (ev->code >= KEY_F1) && (ev->code <= KEY_F10) &&
	    ev->value) {
		return 1;
	}

	/* Console switching. */
	if (command_flags.enable_vts &&
	    is_alt_pressed(&input.kbd_state) &&
	    is_control_pressed(&input.kbd_state) &&
	    !is_shift_pressed(&input.kbd_state) &&
	    ev->value) {

		if ((ev->code >= KEY_F1) && (ev->code < KEY_F1 + term_num_terminals)) {
			term_switch_to(ev->code - KEY_F1);
			return 1;
		}

		/* No F-keys on Vivaldi keyboards, use action codes that are
		 * guaranteed to be always there.
		 */
		switch (ev->code) {
			case KEY_BACK:
				term_switch_to(0);
				return 1;
			case KEY_FORWARD:
			case KEY_REFRESH:
				if (term_num_terminals >= 2) {
					term_switch_to(1);
					return 1;
				}
				break;
			case KEY_ZOOM:
				if (term_num_terminals >= 3) {
					term_switch_to(2);
					return 1;
				}
				break;
			case KEY_SCALE:
				if (term_num_terminals >= 4) {
					term_switch_to(3);
					return 1;
				}
				break;
		}
	}

	return 0;
}

static void input_get_keysym_and_unicode(struct input_key_event* event,
					 uint32_t* keysym, uint32_t* unicode)
{
	struct {
		uint32_t code;
		uint32_t keysym;
	} search_keys[] = {
		{ KEY_F1, KEYSYM_F1},
		{ KEY_F2, KEYSYM_F2},
		{ KEY_F3, KEYSYM_F3},
		{ KEY_F4, KEYSYM_F4},
		{ KEY_F5, KEYSYM_F5},
		{ KEY_F6, KEYSYM_F6},
		{ KEY_F7, KEYSYM_F7},
		{ KEY_F8, KEYSYM_F8},
		{ KEY_F9, KEYSYM_F8},
		{ KEY_F10, KEYSYM_F10},
		{ KEY_UP, KEYSYM_PAGEUP},
		{ KEY_DOWN, KEYSYM_PAGEDOWN},
		{ KEY_LEFT, KEYSYM_HOME},
		{ KEY_RIGHT, KEYSYM_END},
	};

	struct {
		uint32_t code;
		uint32_t keysym;
	} non_ascii_keys[] = {
		{ KEY_ESC, KEYSYM_ESC},
		{ KEY_HOME, KEYSYM_HOME},
		{ KEY_LEFT, KEYSYM_LEFT},
		{ KEY_UP, KEYSYM_UP},
		{ KEY_RIGHT, KEYSYM_RIGHT},
		{ KEY_DOWN, KEYSYM_DOWN},
		{ KEY_PAGEUP, KEYSYM_PAGEUP},
		{ KEY_PAGEDOWN, KEYSYM_PAGEDOWN},
		{ KEY_END, KEYSYM_END},
		{ KEY_INSERT, KEYSYM_INSERT},
		{ KEY_DELETE, KEYSYM_DELETE},
	};

	if (input.kbd_state.search_state) {
		for (unsigned i = 0; i < ARRAY_SIZE(search_keys); i++) {
			if (search_keys[i].code == event->code) {
				*keysym = search_keys[i].keysym;
				*unicode = -1;
				return;
			}
		}
	}

	if (input.xkb_state) {
		/* evdev keycodes are offset by 8 in xkb. */
		xkb_keycode_t keycode = event->code + 8;

		*keysym = xkb_state_key_get_one_sym(input.xkb_state, keycode);
		*unicode = xkb_state_key_get_utf32(input.xkb_state, keycode);
		if (!*unicode)
			*unicode = -1;
		return;
	}

	for (unsigned i = 0; i < ARRAY_SIZE(non_ascii_keys); i++) {
		if (non_ascii_keys[i].code == event->code) {
			*keysym = non_ascii_keys[i].keysym;
			*unicode = -1;
			return;
		}
	}

	if (event->code >= ARRAY_SIZE(keysym_table) / 2) {
		*keysym = '?';
	} else {
		*keysym = keysym_table[event->code * 2 + is_shift_pressed(&input.kbd_state)];
		if (is_control_pressed(&input.kbd_state) && isascii(*keysym))
			*keysym = tolower(*keysym) - 'a' + 1;
	}

	*unicode = *keysym;
}

int input_add(const char* devname)
{
	int ret = 0, fd = -1;

	/* for some reason every device has a null enumerations and notifications
	   of every device come with NULL string first */
	if (!devname) {
		ret = -EINVAL;
		goto errorret;
	}
	/* check for duplicates */
	for (unsigned int i = 0; i < input.ndevs; ++i) {
		if (strcmp(devname, input.devs[i].path) == 0) {
			LOG(INFO, "Skipping duplicate input device %s", devname);
			ret = -EINVAL;
			goto errorret;
		}
	}
	ret = fd = open(devname, O_RDONLY);
	if (fd < 0)
		goto errorret;

	ret = ioctl(fd, EVIOCGRAB, (void*) 1);
	if (!ret) {
		ret = ioctl(fd, EVIOCGRAB, (void*) 0);
		if (ret)
			LOG(ERROR,
				"EVIOCGRAB succeeded but the corresponding ungrab failed: %m");
	} else {
		LOG(ERROR, "Evdev device %s grabbed by another process",
			devname);
		ret = -EBUSY;
		goto closefd;
	}

	struct input_dev* newdevs =
	    realloc(input.devs, (input.ndevs + 1) * sizeof (struct input_dev));
	if (!newdevs) {
		ret = -ENOMEM;
		goto closefd;
	}
	input.devs = newdevs;
	memset(&input.devs[input.ndevs], 0, sizeof(input.devs[input.ndevs]));
	input.devs[input.ndevs].fd = fd;
	input.devs[input.ndevs].path = strdup(devname);
	if (!input.devs[input.ndevs].path) {
		ret = -ENOMEM;
		goto closefd;
	}
	input_pointer_init(&input.devs[input.ndevs]);
	input.ndevs++;

	return fd;

closefd:
	close(fd);
errorret:
	return ret;
}

void input_remove(const char* devname)
{
	unsigned int u;

	if (!devname)
		return;

	for (u = 0; u < input.ndevs; u++) {
		if (!strcmp(devname, input.devs[u].path)) {
			free(input.devs[u].path);
			close(input.devs[u].fd);
			input.ndevs--;
			if (u != input.ndevs) {
				input.devs[u] = input.devs[input.ndevs];
			}
			return;
		}
	}
}

/*
 * Switch to the xkb keymap for model, layout, variant and options from
 * xkeyboard-config.  NULL or empty names use libxkbcommon's defaults.  On
 * failure the current keymap is kept.
 */
bool input_set_keymap(const char* model, const char* layout,
		      const char* variant, const char* options)
{
	struct xkb_rule_names names = {
		.model = model,
		.layout = layout,
		.variant = variant,
		.options = options,
	};
	struct xkb_keymap* keymap;
	struct xkb_state* state;

	if (!input.xkb_context)
		return false;

	keymap = xkb_keymap_new_from_names(input.xkb_context, &names,
					   XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		LOG(WARNING, "Failed to compile xkb keymap");
		return false;
	}

	state = xkb_state_new(keymap);
	if (!state) {
		LOG(WARNING, "Failed to create xkb state");
		xkb_keymap_unref(keymap);
		return false;
	}

	xkb_state_unref(input.xkb_state);
	xkb_keymap_unref(input.xkb_keymap);
	input.xkb_keymap = keymap;
	input.xkb_state = state;
	return true;
}

/*
 * Load the system keyboard layout (localectl set-x11-keymap) from
 * xkeyboard-config.  On failure the built-in US layout is used.
 */
static void input_xkb_init(void)
{
	char* model = NULL;
	char* layout = NULL;
	char* variant = NULL;
	char* options = NULL;
	bool ok;

	input.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!input.xkb_context) {
		LOG(WARNING, "Failed to create xkb context, using built-in keymap");
		return;
	}

	dbus_get_x11_keymap(&model, &layout, &variant, &options);
	ok = input_set_keymap(model, layout, variant, options);
	free(model);
	free(layout);
	free(variant);
	free(options);
	if (!ok) {
		LOG(WARNING, "Using built-in keymap");
		xkb_context_unref(input.xkb_context);
		input.xkb_context = NULL;
	}
}

int input_init()
{
	if (!isatty(fileno(stdout)))
		setbuf(stdout, NULL);
	input_xkb_init();
	return 0;
}

void input_close()
{
	unsigned int u;

	xkb_state_unref(input.xkb_state);
	input.xkb_state = NULL;
	xkb_keymap_unref(input.xkb_keymap);
	input.xkb_keymap = NULL;
	xkb_context_unref(input.xkb_context);
	input.xkb_context = NULL;

	for (u = 0; u < input.ndevs; u++) {
		free(input.devs[u].path);
		close(input.devs[u].fd);
	}
	free(input.devs);
	input.devs = NULL;
	input.ndevs = 0;
}

void input_add_fds(fd_set* read_set, fd_set* exception_set, int *maxfd)
{
	unsigned int u;

	for (u = 0; u < input.ndevs; u++) {
		FD_SET(input.devs[u].fd, read_set);
		FD_SET(input.devs[u].fd, exception_set);
		if (input.devs[u].fd > *maxfd)
			*maxfd = input.devs[u].fd;
	}
}

static void input_key(struct input_key_event* event)
{
	terminal_t* terminal;

	if (!input_special_key(event) && event->value) {
		uint32_t keysym, unicode;
		// current_terminal can possibly change during
		// execution of input_special_key
		terminal = term_get_current_terminal();
		if (term_is_active(terminal)) {
			// Only report user activity when the terminal is active
			dbus_report_user_activity(USER_ACTIVITY_OTHER);
			input_get_keysym_and_unicode(
				event, &keysym, &unicode);
			term_key_event(terminal,
					keysym, unicode);
		}
	}
	/*
	 * Track modifiers and locks for every press and release,
	 * including keys consumed above, so the xkb state stays in
	 * sync. Autorepeat (value 2) does not change the state.
	 */
	if (input.xkb_state && event->value != 2)
		xkb_state_update_key(input.xkb_state, event->code + 8,
				     event->value ? XKB_KEY_DOWN : XKB_KEY_UP);
}

static bool is_pointer_key(uint16_t code)
{
	uint16_t pointer_keys[] = {
		BTN_TOUCH, // touchpad events
		BTN_TOOL_FINGER,
		BTN_TOOL_DOUBLETAP,
		BTN_TOOL_TRIPLETAP,
		BTN_TOOL_QUADTAP,
		BTN_TOOL_QUINTTAP,
		BTN_LEFT, // mouse buttons
		BTN_RIGHT,
		BTN_MIDDLE,
		BTN_SIDE,
		BTN_EXTRA,
		BTN_FORWARD,
		BTN_BACK,
		BTN_TASK
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(pointer_keys); i++)
		if (code == pointer_keys[i])
			return true;

	return false;
}

static unsigned int input_mouse_modifiers(void)
{
	unsigned int mods = 0;

	if (is_shift_pressed(&input.kbd_state))
		mods |= TSM_MOUSE_MODIFIER_SHIFT;
	if (is_alt_pressed(&input.kbd_state))
		mods |= TSM_MOUSE_MODIFIER_META;
	if (is_control_pressed(&input.kbd_state))
		mods |= TSM_MOUSE_MODIFIER_CTRL;

	return mods;
}

/*
 * Touchpad motion: one finger moves the pointer, with the width of the
 * touchpad scaled to the width of the screen, and two fingers scroll.
 */
static void input_touchpad_motion(struct input_pointer* p, fb_t* fb)
{
	int32_t range_x = p->abs_x.maximum - p->abs_x.minimum;
	int32_t range_y = p->abs_y.maximum - p->abs_y.minimum;
	/* Touchpad units to scroll one wheel notch. */
	int32_t scroll_step = MAX(range_y / 20, 1);

	if (p->tracking && p->fingers == 1) {
		p->frac_x += (int64_t)(p->x - p->last_x) * fb_getwidth(fb);
		p->frac_y += (int64_t)(p->y - p->last_y) * fb_getwidth(fb);
		p->rel_x += p->frac_x / range_x;
		p->rel_y += p->frac_y / range_x;
		p->frac_x %= range_x;
		p->frac_y %= range_x;
	} else if (p->tracking && p->fingers == 2) {
		/* Moving the fingers down scrolls down, like the wheel. */
		p->scroll += p->y - p->last_y;
		p->wheel -= p->scroll / scroll_step;
		p->scroll %= scroll_step;
	}

	if (!p->tracking) {
		p->frac_x = p->frac_y = 0;
		p->scroll = 0;
	}

	p->last_x = p->x;
	p->last_y = p->y;
	p->tracking = p->fingers > 0;
}

/* Apply the motion of a device since the previous report to the pointer. */
static void input_pointer_report(struct input_pointer* p)
{
	terminal_t* terminal = term_get_current_terminal();
	unsigned int mods = input_mouse_modifiers();

	if (!p->abs_changed && !p->rel_x && !p->rel_y && !p->wheel)
		return;

	if (term_is_active(terminal) && p->abs_changed) {
		fb_t* fb = term_getfb(terminal);

		if (p->touchpad) {
			input_touchpad_motion(p, fb);
		} else {
			int64_t x = p->x - p->abs_x.minimum;
			int64_t y = p->y - p->abs_y.minimum;

			term_mouse_move_to(terminal,
				x * (fb_getwidth(fb) - 1) /
				(p->abs_x.maximum - p->abs_x.minimum),
				y * (fb_getheight(fb) - 1) /
				(p->abs_y.maximum - p->abs_y.minimum),
				mods);
		}
	} else if (p->abs_changed) {
		p->tracking = false;
	}

	if (term_is_active(terminal)) {
		if (p->rel_x || p->rel_y)
			term_mouse_move(terminal, p->rel_x, p->rel_y, mods);
		if (p->wheel)
			term_mouse_wheel(terminal, p->wheel, mods);
	}

	p->abs_changed = false;
	p->rel_x = p->rel_y = p->wheel = 0;
}

static void input_pointer_button(struct input_pointer* p, uint16_t code,
				 int32_t value)
{
	terminal_t* terminal;
	unsigned int button;

	switch (code) {
	case BTN_TOOL_FINGER:
	case BTN_TOOL_DOUBLETAP:
	case BTN_TOOL_TRIPLETAP:
	case BTN_TOOL_QUADTAP:
	case BTN_TOOL_QUINTTAP: {
		unsigned int fingers = (code == BTN_TOOL_FINGER) ? 1 :
				       code - BTN_TOOL_DOUBLETAP + 2;
		if (value)
			p->fingers = fingers;
		else if (p->fingers == fingers)
			p->fingers = 0;
		/* The position jumps when the fingers change. */
		p->tracking = false;
		return;
	}
	case BTN_TOUCH:
		/* A touch on a touchscreen or tablet is a left click. */
		if (p->touchpad)
			return;
		button = TSM_MOUSE_BUTTON_LEFT;
		break;
	case BTN_LEFT:
		button = TSM_MOUSE_BUTTON_LEFT;
		break;
	case BTN_MIDDLE:
		button = TSM_MOUSE_BUTTON_MIDDLE;
		break;
	case BTN_RIGHT:
		button = TSM_MOUSE_BUTTON_RIGHT;
		break;
	default:
		return;
	}

	/* Press or release where the pointer is in this report. */
	input_pointer_report(p);

	terminal = term_get_current_terminal();
	if (term_is_active(terminal))
		term_mouse_button(terminal, button, value,
				  input_mouse_modifiers());
}

static void input_pointer_event(struct input_pointer* p,
				struct input_event* ev)
{
	switch (ev->type) {
	case EV_KEY:
		input_pointer_button(p, ev->code, ev->value);
		break;
	case EV_REL:
		if (ev->code == REL_X)
			p->rel_x += ev->value;
		else if (ev->code == REL_Y)
			p->rel_y += ev->value;
		else if (ev->code == REL_WHEEL)
			p->wheel += ev->value;
		break;
	case EV_ABS:
		if (!p->absolute)
			break;
		if (ev->code == ABS_X) {
			p->x = ev->value;
			p->abs_changed = true;
		} else if (ev->code == ABS_Y) {
			p->y = ev->value;
			p->abs_changed = true;
		}
		break;
	case EV_SYN:
		if (ev->code == SYN_REPORT) {
			input_pointer_report(p);
		} else if (ev->code == SYN_DROPPED) {
			p->abs_changed = false;
			p->tracking = false;
			p->rel_x = p->rel_y = p->wheel = 0;
		}
		break;
	}
}

static void input_event(struct input_dev* dev, struct input_event* ev)
{
	if (ev->type == EV_KEY && !is_pointer_key(ev->code)) {
		struct input_key_event event = {
			.code = ev->code,
			.value = ev->value,
		};
		input_key(&event);
	} else if (ev->type == EV_SW && ev->code == SW_LID) {
		/* TODO(dbehr), abstract this in input_key_event if we ever parse more than one */
		term_monitor_hotplug();
	} else {
		input_pointer_event(&dev->pointer, ev);
	}
}

void input_dispatch_io(fd_set* read_set, fd_set* exception_set)
{
	unsigned int u;
	struct input_event evs[64];
	int ret;

	for (u = 0; u < input.ndevs; u++) {
		if (FD_ISSET(input.devs[u].fd, read_set)
		    && !FD_ISSET(input.devs[u].fd, exception_set)) {
			ret = read(input.devs[u].fd, evs, sizeof(evs));
			if (ret < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				if (errno != ENODEV) {
					LOG(ERROR, "read: %s: %s", input.devs[u].path,
						strerror(errno));
				}
				input_remove(input.devs[u].path);
				return;
			} else if (ret < (int) sizeof (struct input_event)) {
				LOG(ERROR, "expected %d bytes, got %d",
				       (int) sizeof (struct input_event), ret);
				return;
			}

			for (int i = 0; i < ret / (int) sizeof(evs[0]); i++)
				input_event(&input.devs[u], &evs[i]);
		}
	}
}

#define BITS_PER_LONG (sizeof(long) * 8)
#define BITS_TO_LONGS(bits) (((bits) - 1) / BITS_PER_LONG + 1)
#define BITMASK_GET_BIT(bitmask, bit) \
    ((bitmask[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1)

static const int kMaxBit = MAX(MAX(EV_MAX, KEY_MAX), SW_MAX);

static bool has_event_bit(int fd, int event_type, int bit)
{
	unsigned long bitmask[BITS_TO_LONGS(kMaxBit+1)];
	memset(bitmask, 0, sizeof(bitmask));

	if (ioctl(fd, EVIOCGBIT(event_type, sizeof(bitmask)), bitmask) < 0)
		return false;

	return BITMASK_GET_BIT(bitmask, bit);
}

static int get_switch_bit(int fd, int bit) {
	unsigned long bitmask[BITS_TO_LONGS(SW_MAX+1)];
	memset(bitmask, 0, sizeof(bitmask));
	if (ioctl(fd, EVIOCGSW(sizeof(bitmask)), bitmask) < 0)
		return -1;

	return BITMASK_GET_BIT(bitmask, bit);
}

static bool is_lid_switch(int fd)
{
	return has_event_bit(fd, 0, EV_SW) && has_event_bit(fd, EV_SW, SW_LID);
}

static bool has_property(int fd, int prop)
{
	unsigned long bitmask[BITS_TO_LONGS(INPUT_PROP_MAX+1)];
	memset(bitmask, 0, sizeof(bitmask));

	if (ioctl(fd, EVIOCGPROP(sizeof(bitmask)), bitmask) < 0)
		return false;

	return BITMASK_GET_BIT(bitmask, prop);
}

/*
 * Find out whether the device reports absolute positions. Joysticks and
 * accelerometers also have ABS_X/ABS_Y, so require a touch or a button.
 */
static void input_pointer_init(struct input_dev* dev)
{
	struct input_pointer* p = &dev->pointer;
	int fd = dev->fd;

	if (!has_event_bit(fd, EV_ABS, ABS_X) ||
	    !has_event_bit(fd, EV_ABS, ABS_Y))
		return;
	if (!has_event_bit(fd, EV_KEY, BTN_TOUCH) &&
	    !has_event_bit(fd, EV_KEY, BTN_LEFT))
		return;
	if (ioctl(fd, EVIOCGABS(ABS_X), &p->abs_x) < 0 ||
	    ioctl(fd, EVIOCGABS(ABS_Y), &p->abs_y) < 0)
		return;
	if (p->abs_x.maximum <= p->abs_x.minimum ||
	    p->abs_y.maximum <= p->abs_y.minimum)
		return;

	p->absolute = true;
	p->touchpad = !has_property(fd, INPUT_PROP_DIRECT) &&
		      has_event_bit(fd, EV_KEY, BTN_TOOL_FINGER);
	p->x = p->abs_x.value;
	p->y = p->abs_y.value;
}

int input_check_lid_state(void)
{
	unsigned int u;

	for (u = 0; u < input.ndevs; u++) {
		if (is_lid_switch(input.devs[u].fd)) {
			return get_switch_bit(input.devs[u].fd, SW_LID);
		}
	}
	return -ENODEV;
}
