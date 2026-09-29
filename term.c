/*
 * Copyright 2014 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <ctype.h>
#include <fcntl.h>
#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "dbus.h"
#include "fb.h"
#include "font.h"
#include "image.h"
#include "input.h"
#include "main.h"
#include "shl_pty.h"
#include "term.h"
#include "util.h"
#include "vt.h"

unsigned int term_num_terminals = 4;
static terminal_t* terminals[TERM_MAX_TERMINALS];
static uint32_t current_terminal = 0;

struct term {
	struct tsm_screen* screen;
	struct tsm_vte* vte;
	struct shl_pty* pty;
	int pty_bridge;
	int pid;
	tsm_age_t age;
	int w_in_char, h_in_char;
};

/*
 * xterm mouse modes, set by the program with DECSET. libtsm keeps the tracking
 * mode and the encoding in one field, so "\e[?1006;1000h" leaves it with the
 * legacy encoding. frecon tracks them separately and sends the reports itself.
 */
#define MOUSE_TRACK_X10		9	/* presses only */
#define MOUSE_TRACK_NORMAL	1000	/* presses and releases */
#define MOUSE_TRACK_BUTTON	1002	/* and motion with a button held */
#define MOUSE_TRACK_ANY		1003	/* and all motion */
#define MOUSE_ENCODING_SGR	1006
#define MOUSE_ENCODING_PIXELS	1016	/* SGR with pixel positions */

#define MOUSE_SCAN_MAX_PARAMS	16

/*
 * State of the scan of the program's output for the mouse modes:
 *  state - position in an escape sequence.
 *  bang - the CSI sequence has a '!' intermediate, for DECSTR.
 *  params, nparams - parameters of a private CSI sequence.
 */
struct mouse_scan {
	enum {
		MOUSE_SCAN_GROUND,
		MOUSE_SCAN_ESC,
		MOUSE_SCAN_CSI,
		MOUSE_SCAN_PRIVATE,
		MOUSE_SCAN_OTHER,
	} state;
	bool bang;
	unsigned int params[MOUSE_SCAN_MAX_PARAMS];
	unsigned int nparams;
};

/*
 * Per-terminal mouse state:
 *  enable - mouse support is on for this terminal.
 *  track, encoding - mouse modes set by the program, MOUSE_TRACK_* or 0 and
 *    MOUSE_ENCODING_* or 0 for the legacy encoding.
 *  scan - scan of the program's output for the mouse modes.
 *  cell_x, cell_y - cell the pointer was last in, or -1.
 *  buttons - bitmask of held TSM_MOUSE_BUTTON_* buttons.
 *  reporting - the held buttons are reported to the application.
 *  selecting - the left button is held to make a selection from the anchor.
 *  sel_started - the selection was started, it is not just a click.
 *  clicks, click_x, click_y, click_time - multi-click detection.
 */
struct mouse {
	bool enable;
	unsigned int track;
	unsigned int encoding;
	struct mouse_scan scan;
	int cell_x, cell_y;
	unsigned int buttons;
	bool reporting;
	bool selecting;
	bool sel_started;
	unsigned int anchor_x, anchor_y;
	unsigned int clicks;
	unsigned int click_x, click_y;
	struct timespec click_time;
};

struct _terminal_t {
	unsigned vt;
	bool active;
	bool input_enable;
	uint32_t background;
	bool background_valid;
	fb_t* fb;
	struct term* term;
	char** exec;
	struct mouse mouse;
};


static char* interactive_cmd_line[] = {
	"/sbin/agetty",
	"-",
	"9600",
	"xterm",
	NULL
};

static bool in_background = false;
static bool hotplug_occured = false;

/*
 * The mouse pointer position in pixels, shared by all terminals. The pointer
 * is shown once the mouse has been used, and hidden while it is idle.
 */
static struct {
	int32_t x, y;
	bool visible;
	bool idle;
} pointer;

/* Fires when the mouse has been idle for POINTER_IDLE_SEC, or -1. */
static int pointer_timer_fd = -1;

#define POINTER_IDLE_SEC	5

/* Text of the last selection, shared by all terminals. */
static char* clipboard;

#define MOUSE_MULTI_CLICK_MS	400
#define MOUSE_WHEEL_LINES	3

static const struct {
	const char *name;
	uint8_t colors[TSM_COLOR_NUM][3];
} palettes[] = {
	{
		.name = "wildcherry",
		.colors = {
			[TSM_COLOR_BLACK]         = { 0x00, 0x05, 0x06 },
			[TSM_COLOR_RED]           = { 0xd9, 0x40, 0x85 },
			[TSM_COLOR_GREEN]         = { 0x2a, 0xb2, 0x50 },
			[TSM_COLOR_YELLOW]        = { 0xff, 0xd0, 0x6e },
			[TSM_COLOR_BLUE]          = { 0x87, 0x3b, 0xdb },
			[TSM_COLOR_MAGENTA]       = { 0xec, 0xec, 0xec },
			[TSM_COLOR_CYAN]          = { 0xc1, 0xb8, 0xb6 },
			[TSM_COLOR_LIGHT_GREY]    = { 0xff, 0xf8, 0xdd },
			[TSM_COLOR_DARK_GREY]     = { 0x00, 0x9c, 0xc9 },
			[TSM_COLOR_LIGHT_RED]     = { 0xda, 0x6b, 0xab },
			[TSM_COLOR_LIGHT_GREEN]   = { 0xf4, 0xdb, 0xa5 },
			[TSM_COLOR_LIGHT_YELLOW]  = { 0xea, 0xc0, 0x66 },
			[TSM_COLOR_LIGHT_BLUE]    = { 0x2f, 0x8b, 0xb9 },
			[TSM_COLOR_LIGHT_MAGENTA] = { 0xae, 0x63, 0x6b },
			[TSM_COLOR_LIGHT_CYAN]    = { 0xff, 0x91, 0x9d },
			[TSM_COLOR_WHITE]         = { 0xe4, 0x83, 0x8d },

			[TSM_COLOR_FOREGROUND]    = { 0xd9, 0xfa, 0xff },
			[TSM_COLOR_BACKGROUND]    = { 0x1f, 0x16, 0x26 },
		},
	},
};

static int term_find_palette(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(palettes); i++) {
		if (!strcmp(name, palettes[i].name))
			return i;
	}

	return -1;
}

bool term_palette_is_valid(const char *name)
{
	return term_find_palette(name) >= 0;
}

static void term_set_palette(terminal_t *terminal, const char *name)
{
	struct tsm_vte *vte = terminal->term->vte;
	struct tsm_screen_attr attr;
	int i = term_find_palette(name);

	if (i < 0)
		return;

	/* libtsm copies the palette, so casting away const is safe. */
	tsm_vte_set_custom_palette(vte, (uint8_t (*)[3])palettes[i].colors);
	tsm_vte_set_palette(vte, "custom");

	/* Fill the screen border with the palette's background. */
	tsm_vte_get_def_attr(vte, &attr);
	terminal->background = (attr.br << 16) | (attr.bg << 8) | attr.bb;
}


static void __attribute__ ((noreturn)) term_run_child(terminal_t* terminal)
{
	/* XXX figure out how to fix "top" for xterm-256color */
	setenv("TERM", "xterm", 1);
	if (terminal->exec) {
		execve(terminal->exec[0], terminal->exec, environ);
		exit(1);
	} else {
		while (1)
			sleep(1000000);
	}
}

static int term_draw_cell(struct tsm_screen* screen, uint64_t id,
			  const uint32_t* ch, size_t len,
			  unsigned int cwidth, unsigned int posx,
			  unsigned int posy,
			  const struct tsm_screen_attr* attr,
			  tsm_age_t age, void* data)
{
	terminal_t* terminal = (terminal_t*)data;
	uint32_t front_color, back_color;
	uint8_t br, bb, bg;
	uint32_t luminance;

	if (age && terminal->term->age && age <= terminal->term->age)
		return 0;

	if (terminal->background_valid) {
		br = (terminal->background >> 16) & 0xFF;
		bg = (terminal->background >> 8) & 0xFF;
		bb = (terminal->background) & 0xFF;
		luminance = (3 * br + bb + 4 * bg) >> 3;

		/*
		 * FIXME: black is chosen on a dark background, but it uses the
		 * default color for light backgrounds
		 */
		if (luminance > 128) {
			front_color = 0;
			back_color = terminal->background;
		} else {
			front_color = (attr->fr << 16) | (attr->fg << 8) | attr->fb;
			back_color = terminal->background;
		}
	} else {
			front_color = (attr->fr << 16) | (attr->fg << 8) | attr->fb;
			back_color = (attr->br << 16) | (attr->bg << 8) | attr->bb;
	}

	if (attr->inverse) {
		uint32_t tmp = front_color;
		front_color = back_color;
		back_color = tmp;
	}

	if (len)
		font_render(terminal->fb, posx, posy, *ch,
					front_color, back_color);
	else
		font_fillchar(terminal->fb, posx, posy,
						front_color, back_color);

	return 0;
}

/* Get the cell under the pointer. Returns false if the pointer is hidden. */
static bool term_pointer_cell(terminal_t* terminal, unsigned int* x,
			      unsigned int* y)
{
	uint32_t char_width, char_height;

	if (!pointer.visible || !terminal->mouse.enable)
		return false;

	font_get_size(&char_width, &char_height);
	*x = MIN((uint32_t)pointer.x / char_width,
		 (uint32_t)terminal->term->w_in_char - 1);
	*y = MIN((uint32_t)pointer.y / char_height,
		 (uint32_t)terminal->term->h_in_char - 1);
	return true;
}

/* Show the pointer on the screen of an active terminal, or hide it. */
static void term_pointer_update(terminal_t* terminal)
{
	if (pointer.visible && !pointer.idle && terminal->mouse.enable)
		fb_pointer_show(terminal->fb, pointer.x, pointer.y);
	else
		fb_pointer_hide(terminal->fb);
}

/* Show the pointer after mouse activity and restart the idle timer. */
static void term_pointer_wake(terminal_t* terminal)
{
	struct itimerspec timeout = {
		.it_value = { .tv_sec = POINTER_IDLE_SEC },
	};

	if (pointer_timer_fd < 0) {
		pointer_timer_fd = timerfd_create(CLOCK_MONOTONIC,
						  TFD_NONBLOCK | TFD_CLOEXEC);
		if (pointer_timer_fd < 0)
			LOG(ERROR, "Failed to create the pointer idle timer: %m");
	}
	if (pointer_timer_fd >= 0)
		timerfd_settime(pointer_timer_fd, 0, &timeout, NULL);

	if (pointer.idle) {
		pointer.idle = false;
		term_pointer_update(terminal);
	}
}

void term_pointer_add_fds(fd_set* read_set, fd_set* exception_set, int* maxfd)
{
	if (pointer_timer_fd < 0)
		return;

	FD_SET(pointer_timer_fd, read_set);
	*maxfd = MAX(*maxfd, pointer_timer_fd);
}

void term_pointer_dispatch_io(fd_set* read_set)
{
	terminal_t* terminal = term_get_current_terminal();
	uint64_t expirations;

	if (pointer_timer_fd < 0 || !FD_ISSET(pointer_timer_fd, read_set))
		return;
	if (read(pointer_timer_fd, &expirations, sizeof(expirations)) !=
	    sizeof(expirations))
		return;

	/* Keep the pointer while a button is held, as in a slow selection. */
	if (term_is_active(terminal) && terminal->mouse.buttons) {
		term_pointer_wake(terminal);
		return;
	}

	pointer.idle = true;
	if (term_is_active(terminal))
		term_pointer_update(terminal);
}

static void term_redraw(terminal_t* terminal)
{
	if (fb_lock(terminal->fb)) {
		terminal->term->age =
			tsm_screen_draw(terminal->term->screen, term_draw_cell, terminal);
		fb_unlock(terminal->fb);
	}
}

void term_key_event(terminal_t* terminal, uint32_t keysym, int32_t unicode)
{
	if (!terminal->input_enable)
		return;

	if (tsm_vte_handle_keyboard(terminal->term->vte, keysym, 0, 0, unicode))
		tsm_screen_sb_reset(terminal->term->screen);

	term_redraw(terminal);
}

/* Like xterm, resetting any tracking mode or encoding turns it off. */
static void term_mouse_set_mode(struct mouse* mouse, unsigned int mode,
				bool set)
{
	switch (mode) {
	case MOUSE_TRACK_X10:
	case MOUSE_TRACK_NORMAL:
	case MOUSE_TRACK_BUTTON:
	case MOUSE_TRACK_ANY:
		mouse->track = set ? mode : 0;
		break;
	case MOUSE_ENCODING_SGR:
	case MOUSE_ENCODING_PIXELS:
		mouse->encoding = set ? mode : 0;
		break;
	}
}

/*
 * Scan the program's output for mouse mode changes: DECSET and DECRST
 * ("\e[?...h" and "\e[?...l"), and the resets RIS ("\ec") and DECSTR
 * ("\e[!p"). Sequences may be split across reads.
 */
static void term_mouse_scan(struct mouse* mouse, const char* u8, size_t len)
{
	struct mouse_scan* scan = &mouse->scan;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = u8[i];

		/* ESC starts a new sequence, and CAN and SUB cancel one. */
		if (c == 0x1b) {
			scan->state = MOUSE_SCAN_ESC;
			continue;
		}
		if (c == 0x18 || c == 0x1a) {
			scan->state = MOUSE_SCAN_GROUND;
			continue;
		}

		switch (scan->state) {
		case MOUSE_SCAN_GROUND:
			break;
		case MOUSE_SCAN_ESC:
			if (c == '[') {
				scan->state = MOUSE_SCAN_CSI;
				scan->bang = false;
				scan->nparams = 1;
				scan->params[0] = 0;
				break;
			}
			if (c == 'c')
				mouse->track = mouse->encoding = 0;
			scan->state = MOUSE_SCAN_GROUND;
			break;
		case MOUSE_SCAN_CSI:
			if (c == '?') {
				scan->state = MOUSE_SCAN_PRIVATE;
				break;
			}
			scan->state = MOUSE_SCAN_OTHER;
			/* fall through */
		case MOUSE_SCAN_OTHER:
			if (c == '!') {
				scan->bang = true;
			} else if (c >= 0x40 && c <= 0x7e) {
				if (c == 'p' && scan->bang)
					mouse->track = mouse->encoding = 0;
				scan->state = MOUSE_SCAN_GROUND;
			}
			break;
		case MOUSE_SCAN_PRIVATE:
			/* nparams is past the maximum once there are too many. */
			if (c >= '0' && c <= '9') {
				unsigned int* param;

				if (scan->nparams > MOUSE_SCAN_MAX_PARAMS)
					break;
				param = &scan->params[scan->nparams - 1];
				if (*param < 100000)
					*param = *param * 10 + (c - '0');
			} else if (c == ';') {
				if (scan->nparams < MOUSE_SCAN_MAX_PARAMS)
					scan->params[scan->nparams] = 0;
				if (scan->nparams <= MOUSE_SCAN_MAX_PARAMS)
					scan->nparams++;
			} else if (c >= 0x40 && c <= 0x7e) {
				if (c == 'h' || c == 'l') {
					unsigned int n = MIN(scan->nparams,
							     MOUSE_SCAN_MAX_PARAMS);

					for (unsigned int p = 0; p < n; p++)
						term_mouse_set_mode(mouse,
								    scan->params[p],
								    c == 'h');
				}
				scan->state = MOUSE_SCAN_GROUND;
			}
			break;
		}
	}
}

static void term_read_cb(struct shl_pty* pty, char* u8, size_t len, void* data)
{
	terminal_t* terminal = (terminal_t*)data;

	term_mouse_scan(&terminal->mouse, u8, len);
	tsm_vte_input(terminal->term->vte, u8, len);

	term_redraw(terminal);
}

static void term_write_cb(struct tsm_vte* vte, const char* u8, size_t len,
				void* data)
{
	struct term* term = data;
	int r;

	r = shl_pty_write(term->pty, u8, len);
	if (r < 0)
		LOG(ERROR, "OOM in pty-write (%d)", r);

	shl_pty_dispatch(term->pty);
}

static void term_esc_show_image(terminal_t* terminal, char* params)
{
	char* tok;
	image_t* image;
	int status;

	image = image_create();
	if (!image) {
		LOG(ERROR, "Out of memory when creating an image.\n");
		return;
	}
	for (tok = strtok(params, ";"); tok; tok = strtok(NULL, ";")) {
		if (strncmp("file=", tok, 5) == 0) {
			image_set_filename(image, tok + 5);
		} else if (strncmp("location=", tok, 9) == 0) {
			uint32_t x, y;
			if (sscanf(tok + 9, "%u,%u", &x, &y) != 2) {
				LOG(ERROR, "Error parsing image location.\n");
				goto done;
			}
			image_set_location(image, x, y);
		} else if (strncmp("offset=", tok, 7) == 0) {
			int32_t x, y;
			if (sscanf(tok + 7, "%d,%d", &x, &y) != 2) {
				LOG(ERROR, "Error parsing image offset.\n");
				goto done;
			}
			image_set_offset(image, x, y);
		} else if (strncmp("scale=", tok, 6) == 0) {
			uint32_t s;
			if (sscanf(tok + 6, "%u", &s) != 1) {
				LOG(ERROR, "Error parsing image scale.\n");
				goto done;
			}
			if (s == 0)
				s = image_get_auto_scale(term_getfb(terminal));
			image_set_scale(image, s);
		}
	}

	status = image_load_image_from_file(image);
	if (status != 0) {
		LOG(WARNING, "Term ESC image_load_image_from_file %s failed: %d:%s.",
	        image_get_filename(image), status, strerror(status));
	} else {
		term_show_image(terminal, image);
	}
done:
	image_destroy(image);
}

static void term_esc_draw_box(terminal_t* terminal, char* params)
{
	char* tok;
	uint32_t color = 0;
	uint32_t w = 1;
	uint32_t h = 1;
	uint32_t locx, locy;
	bool use_location = false;
	int32_t offx, offy;
	bool use_offset = false;
	uint32_t scale = 1;
	fb_stepper_t s;
	int32_t startx, starty;

	for (tok = strtok(params, ";"); tok; tok = strtok(NULL, ";")) {
		if (strncmp("color=", tok, 6) == 0) {
			color = strtoul(tok + 6, NULL, 0);
		} else if (strncmp("size=", tok, 5) == 0) {
			if (sscanf(tok + 5, "%u,%u", &w, &h) != 2) {
				LOG(ERROR, "Error parsing box size.\n");
				goto done;
			}
		} else if (strncmp("location=", tok, 9) == 0) {
			if (sscanf(tok + 9, "%u,%u", &locx, &locy) != 2) {
				LOG(ERROR, "Error parsing box location.\n");
				goto done;
			}
			use_location = true;
		} else if (strncmp("offset=", tok, 7) == 0) {
			if (sscanf(tok + 7, "%d,%d", &offx, &offy) != 2) {
				LOG(ERROR, "Error parsing box offset.\n");
				goto done;
			}
			use_offset = true;
		} else if (strncmp("scale=", tok, 6) == 0) {
			if (sscanf(tok + 6, "%u", &scale) != 1) {
				LOG(ERROR, "Error parsing box scale.\n");
				goto done;
			}
			if (scale == 0)
				scale = image_get_auto_scale(term_getfb(terminal));
		}
	}

	w *= scale;
	h *= scale;
	offx *= scale;
	offy *= scale;

	if (!fb_lock(terminal->fb))
		goto done;

	if (use_offset && use_location) {
		LOG(WARNING, "Box offset and location set, using location.");
		use_offset = false;
	}

	if (use_location) {
		startx = locx;
		starty = locy;
	} else {
		startx = (fb_getwidth(terminal->fb) - w)/2;
		starty = (fb_getheight(terminal->fb) - h)/2;
	}

	if (use_offset) {
		startx += offx;
		starty += offy;
	}

	if (!fb_stepper_init(&s, terminal->fb, startx, starty, w, h))
		goto done_fb;

	do {
		do {
		} while (fb_stepper_step_x(&s, color));
	} while (fb_stepper_step_y(&s));

done_fb:
	fb_unlock(terminal->fb);
done:
	;
}

/* Returns 1 for on, 0 for off and -1 if params is neither. */
static int term_esc_parse_onoff(const char* params)
{
	if (strcmp(params, "1") == 0 ||
	    strcasecmp(params, "on") == 0 ||
	    strcasecmp(params, "true") == 0)
		return 1;
	if (strcmp(params, "0") == 0 ||
	    strcasecmp(params, "off") == 0 ||
	    strcasecmp(params, "false") == 0)
		return 0;
	return -1;
}

static void term_esc_input(terminal_t* terminal, char* params)
{
	int onoff = term_esc_parse_onoff(params);

	if (onoff < 0)
		LOG(ERROR, "Invalid parameter for input escape.\n");
	else
		term_input_enable(terminal, onoff);
}

static void term_esc_mouse(terminal_t* terminal, char* params)
{
	int onoff = term_esc_parse_onoff(params);

	if (onoff < 0)
		LOG(ERROR, "Invalid parameter for mouse escape.");
	else
		term_mouse_enable(terminal, onoff);
}

static void term_esc_switchvt(terminal_t* terminal, char* params)
{
	uint32_t vt = (uint32_t)strtoul(params, NULL, 0);
	if (vt >= term_num_terminals || vt >= TERM_MAX_TERMINALS) {
		LOG(ERROR, "Invalid parameter for switchvt escape.");
		return;
	}
	term_switch_to(vt);
}

static void term_esc_drmdropmaster(terminal_t* terminal, char* params)
{
	term_background(true);
}

/*
 * Assume all one or two digit sequences followed by ; are xterm OSC escapes.
 */
static bool is_xterm_osc(char *osc)
{
	if (isdigit(osc[0])) {
		if (osc[1] == ';')
			return true;
		if (isdigit(osc[1]) && osc[2] == ';')
			return true;
	}
	return false;
}

static void term_osc_cb(struct tsm_vte *vte, const char *osc_string,
			size_t osc_len, void *data)
{
	terminal_t* terminal = (terminal_t*)data;
	size_t i;
	char *osc;

	for (i = 0; i < osc_len; i++)
		if ((unsigned char)osc_string[i] >= 128)
			return; /* we only want to deal with ASCII */

	osc = malloc(osc_len + 1);
	if (!osc) {
		LOG(WARNING, "Out of memory when processing OSC.\n");
		return;
	}

	for (i = 0; i < osc_len; i++)
		osc[i] = (char)osc_string[i];
	osc[i] = '\0';

	/*
	 * Mouse support can be toggled by anything writing to the terminal.
	 * The other escapes are only processed with --enable-osc.
	 */
	if (strncmp(osc, "mouse:", 6) == 0)
		term_esc_mouse(terminal, osc + 6);
	else if (!command_flags.enable_osc)
		; /* Ignore it. */
	else if (strncmp(osc, "image:", 6) == 0)
		term_esc_show_image(terminal, osc + 6);
	else if (strncmp(osc, "box:", 4) == 0)
		term_esc_draw_box(terminal, osc + 4);
	else if (strncmp(osc, "input:", 6) == 0)
		term_esc_input(terminal, osc + 6);
	else if (strncmp(osc, "switchvt:", 9) == 0)
		term_esc_switchvt(terminal, osc + 9);
	else if (strncmp(osc, "drmdropmaster", 13) == 0)
		term_esc_drmdropmaster(terminal, osc + 13);
	else if (is_xterm_osc(osc))
		; /* Ignore it. */
	else
		LOG(WARNING, "Unknown OSC escape sequence \"%s\", ignoring.", osc);

	free(osc);
}

#ifdef __clang__
__attribute__((__format__ (__printf__, 7, 0)))
#endif
static void log_tsm(void* data, const char* file, int line, const char* fn,
		    const char* subs, unsigned int sev, const char* format,
		    va_list args)
{
	char buffer[KMSG_LINE_MAX];
	int len = snprintf(buffer, KMSG_LINE_MAX, "<%i>frecon[%d]: %s: ", sev,
	                   getpid(), subs);
	if (len < 0)
		return;
	if (len < KMSG_LINE_MAX - 1)
		vsnprintf(buffer+len, KMSG_LINE_MAX - len, format, args);
	fprintf(stderr, "%s\n", buffer);
}

static int term_resize(terminal_t* term, int scaling)
{
	uint32_t char_width, char_height;
	int status;

	if (!scaling)
		scaling = fb_getscaling(term->fb);

	font_init(scaling);
	font_get_size(&char_width, &char_height);

	term->term->w_in_char = fb_getwidth(term->fb) / char_width;
	term->term->h_in_char = fb_getheight(term->fb) / char_height;

	status = tsm_screen_resize(term->term->screen,
				   term->term->w_in_char, term->term->h_in_char);
	if (status < 0) {
		font_free();
		return -1;
	}

	status = shl_pty_resize(term->term->pty, term->term->w_in_char,
				term->term->h_in_char);
	if (status < 0) {
		font_free();
		return -1;
	}

	return 0;
}

void term_set_num_terminals(unsigned new_num)
{
	if (new_num < 1)
		term_num_terminals = 1;
	else if (new_num > TERM_MAX_TERMINALS)
		term_num_terminals = TERM_MAX_TERMINALS;
	else
		term_num_terminals = new_num;
}

static bool term_is_interactive(unsigned int vt)
{
	if (command_flags.no_login)
		return false;

	if (vt == TERM_SPLASH_TERMINAL)
		return command_flags.enable_vt1;

	return true;
}

/*
 * Set the area not covered by any characters, possibly existing on the right
 * side and bottom of the screen, to the background color.
 */
static void term_clear_border(terminal_t* terminal)
{
	fb_stepper_t s;
	uint32_t char_width, char_height;
	font_get_size(&char_width, &char_height);

	if (!fb_lock(terminal->fb))
		return;

	if (fb_stepper_init(&s, terminal->fb,
			    terminal->term->w_in_char * char_width, 0,
			    fb_getwidth(terminal->fb) - terminal->term->w_in_char * char_width, terminal->term->h_in_char * char_height)) {
		do {
			do {
			} while (fb_stepper_step_x(&s, terminal->background));
		} while (fb_stepper_step_y(&s));
	}

	if (fb_stepper_init(&s, terminal->fb,
			    0, terminal->term->h_in_char * char_height,
			    fb_getwidth(terminal->fb), fb_getheight(terminal->fb) - terminal->term->h_in_char * char_height)) {
		do {
			do {
			} while (fb_stepper_step_x(&s, terminal->background));
		} while (fb_stepper_step_y(&s));
	}

	fb_unlock(terminal->fb);
}

static void term_hide_cursor(terminal_t* terminal)
{
	tsm_screen_set_flags(terminal->term->screen, TSM_SCREEN_HIDE_CURSOR);
}

__attribute__ ((unused))
static void term_show_cursor(terminal_t* terminal)
{
	term_write_message(terminal, "\033[?25h");
}

terminal_t* term_init(unsigned vt, int pts_fd)
{
	const int scrollback_size = 200;
	int status;
	terminal_t* new_terminal;
	bool interactive = term_is_interactive(vt);

	new_terminal = (terminal_t*)calloc(1, sizeof(*new_terminal));
	if (!new_terminal)
		return NULL;

	new_terminal->vt = vt;
	new_terminal->background_valid = false;
	new_terminal->input_enable = true;
	new_terminal->mouse.enable = command_flags.enable_mouse;
	new_terminal->mouse.cell_x = new_terminal->mouse.cell_y = -1;

	new_terminal->fb = fb_init();

	if (!new_terminal->fb) {
		LOG(ERROR, "Failed to create fb on VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	}

	new_terminal->term = (struct term*)calloc(1, sizeof(*new_terminal->term));
	if (!new_terminal->term) {
		term_close(new_terminal);
		return NULL;
	}

	if (interactive)
		new_terminal->exec = interactive_cmd_line;
	else
		new_terminal->exec = NULL;

	status = tsm_screen_new(&new_terminal->term->screen,
			log_tsm, new_terminal->term);
	if (status < 0) {
		LOG(ERROR, "Failed to create new screen on VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	}

	tsm_screen_set_max_sb(new_terminal->term->screen, scrollback_size);

	status = tsm_vte_new(&new_terminal->term->vte, new_terminal->term->screen,
			term_write_cb, new_terminal->term, log_tsm, new_terminal->term);

	if (status < 0) {
		LOG(ERROR, "Failed to create new VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	}

	if (command_flags.palette)
		term_set_palette(new_terminal, command_flags.palette);

	tsm_vte_set_osc_cb(new_terminal->term->vte, term_osc_cb, (void *)new_terminal);

	new_terminal->term->pty_bridge = shl_pty_bridge_new();
	if (new_terminal->term->pty_bridge < 0) {
		LOG(ERROR, "Failed to create pty bridge on VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	}

	status = shl_pty_open(&new_terminal->term->pty,
			term_read_cb, new_terminal, 1, 1, pts_fd);

	if (status < 0) {
		LOG(ERROR, "Failed to open pty on VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	} else if (status == 0) {
		term_run_child(new_terminal);
		exit(1);
	}

	status = mkdir(FRECON_RUN_DIR, S_IRWXU);
	if (status == 0 || (status < 0 && errno == EEXIST)) {
		char path[32];
		snprintf(path, sizeof(path), FRECON_VT_PATH, vt);
		unlink(path); /* In case it already exists. Ignore return codes. */
		if (symlink(ptsname(shl_pty_get_fd(new_terminal->term->pty)), path) < 0)
			LOG(ERROR, "Failed to symlink pts name %s to %s, %d:%s",
			    path,
			    ptsname(shl_pty_get_fd(new_terminal->term->pty)),
			    errno, strerror(errno));
	}

	status = shl_pty_bridge_add(new_terminal->term->pty_bridge, new_terminal->term->pty);
	if (status) {
		LOG(ERROR, "Failed to add pty bridge on VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	}

	new_terminal->term->pid = shl_pty_get_child(new_terminal->term->pty);

	status = term_resize(new_terminal, 0);

	if (status < 0) {
		LOG(ERROR, "Failed to resize VT%u.", vt);
		term_close(new_terminal);
		return NULL;
	}

	if (!interactive) {
		term_hide_cursor(new_terminal);
		term_input_enable(new_terminal, false);
	}

	return new_terminal;
}

void term_activate(terminal_t* terminal)
{
	term_set_current_to(terminal);
	/* While another VT is shown, vt_acquire() activates it later. */
	if (vt_is_enabled() && !vt_is_foreground())
		return;
	terminal->active = true;
	fb_setmode(terminal->fb);
	term_redraw(terminal);
	/* The mode set hid the hardware cursor. */
	term_pointer_update(terminal);
}

void term_deactivate(terminal_t* terminal)
{
	if (!terminal->active)
		return;

	/*
	 * Don't leave the hardware cursor for the next terminal or DRM master.
	 * term_activate() shows the pointer again.
	 */
	fb_pointer_hide(terminal->fb);
	terminal->active = false;
}

void term_close(terminal_t* term)
{
	char path[32];
	if (!term)
		return;

	snprintf(path, sizeof(path), FRECON_VT_PATH, term->vt);
	unlink(path);
	if (term->vt == term_get_current())
		unlink(FRECON_CURRENT_VT);

	if (term->fb) {
		fb_close(term->fb);
		term->fb = NULL;
	}

	if (term->term) {
		if (term->term->pty) {
			if (term->term->pty_bridge >= 0) {
				shl_pty_bridge_remove(term->term->pty_bridge, term->term->pty);
				shl_pty_bridge_free(term->term->pty_bridge);
				term->term->pty_bridge = -1;
			}
			shl_pty_close(term->term->pty);
			term->term->pty = NULL;
		}
		free(term->term);
		term->term = NULL;
	}

	font_free();
	free(term);
}

bool term_is_child_done(terminal_t* terminal)
{
	int status;
	int ret;
	ret = waitpid(terminal->term->pid, &status, WNOHANG);

	if ((ret == -1) && (errno == ECHILD)) {
		return false;
	}
	return ret != 0;
}

void term_page_up(terminal_t* terminal)
{
	tsm_screen_sb_page_up(terminal->term->screen, 1);
	term_redraw(terminal);
}

void term_page_down(terminal_t* terminal)
{
	tsm_screen_sb_page_down(terminal->term->screen, 1);
	term_redraw(terminal);
}

void term_line_up(terminal_t* terminal)
{
	tsm_screen_sb_up(terminal->term->screen, 1);
	term_redraw(terminal);
}

void term_line_down(terminal_t* terminal)
{
	tsm_screen_sb_down(terminal->term->screen, 1);
	term_redraw(terminal);
}

bool term_is_valid(terminal_t* terminal)
{
	return ((terminal != NULL) && (terminal->term != NULL));
}

int term_fd(terminal_t* terminal)
{
	if (term_is_valid(terminal))
		return terminal->term->pty_bridge;
	else
		return -1;
}

void term_dispatch_io(terminal_t* terminal, fd_set* read_set)
{
	if (term_is_valid(terminal))
		if (FD_ISSET(terminal->term->pty_bridge, read_set))
			shl_pty_bridge_dispatch(terminal->term->pty_bridge, 0);
}

bool term_exception(terminal_t* terminal, fd_set* exception_set)
{
	if (term_is_valid(terminal)) {
		if (terminal->term->pty_bridge >= 0) {
			return FD_ISSET(terminal->term->pty_bridge,
					exception_set);
		}
	}

	return false;
}

bool term_is_active(terminal_t* terminal)
{
	if (term_is_valid(terminal))
		return terminal->active;

	return false;
}

void term_add_fds(terminal_t* terminal, fd_set* read_set, fd_set* exception_set, int* maxfd)
{
	if (term_is_valid(terminal)) {
		if (terminal->term->pty_bridge >= 0) {
			*maxfd = MAX(*maxfd, terminal->term->pty_bridge);
			FD_SET(terminal->term->pty_bridge, read_set);
			FD_SET(terminal->term->pty_bridge, exception_set);
		}
	}
}

const char* term_get_ptsname(terminal_t* terminal)
{
	return ptsname(shl_pty_get_fd(terminal->term->pty));
}

void term_set_background(terminal_t* terminal, uint32_t bg)
{
	terminal->background = bg;
	terminal->background_valid = true;
}

int term_show_image(terminal_t* terminal, image_t* image)
{
	return image_show(image, terminal->fb);
}

void term_write_message(terminal_t* terminal, char* message)
{
	FILE* fp;

	fp = fopen(term_get_ptsname(terminal), "w");
	if (fp) {
		fputs(message, fp);
		fclose(fp);
	}
}

fb_t* term_getfb(terminal_t* terminal)
{
	return terminal->fb;
}

terminal_t* term_get_terminal(int num)
{
	return terminals[num];
}

void term_set_terminal(int num, terminal_t* terminal)
{
	terminals[num] = terminal;
}

int term_create_splash_term(int pts_fd)
{
	terminal_t* terminal = term_init(TERM_SPLASH_TERMINAL, pts_fd);
	if (!terminal) {
		LOG(ERROR, "Could not create splash term.");
		return -1;
	}
	term_set_terminal(TERM_SPLASH_TERMINAL, terminal);

	return 0;
}

void term_destroy_splash_term(void)
{
	terminal_t *terminal;
	if (command_flags.enable_vt1) {
		return;
	}
	terminal = term_get_terminal(TERM_SPLASH_TERMINAL);
	term_set_terminal(TERM_SPLASH_TERMINAL, NULL);
	term_close(terminal);
}

void term_update_current_link(void)
{
	char path[32];
	unlink(FRECON_CURRENT_VT);
	if (TERM_SPLASH_TERMINAL != current_terminal ||
	    command_flags.enable_vt1) {
		snprintf(path, sizeof(path), FRECON_VT_PATH, current_terminal);
		if (symlink(path, FRECON_CURRENT_VT) < 0)
			LOG(ERROR, "set_current: failed to create current symlink.");
	}
}

void term_set_current(uint32_t t)
{
	if (t >= TERM_MAX_TERMINALS)
		LOG(ERROR, "set_current: larger than array size");
	else
	if (t >= term_num_terminals)
		LOG(ERROR, "set_current: larger than num terminals");
	else {
		current_terminal = t;
		term_update_current_link();
	}
}

uint32_t term_get_current(void)
{
	return current_terminal;
}

terminal_t *term_get_current_terminal(void)
{
	return terminals[current_terminal];
}

void term_set_current_terminal(terminal_t* terminal)
{
	terminals[current_terminal] = terminal;
}

void term_set_current_to(terminal_t* terminal)
{
	if (!terminal) {
		if (terminals[current_terminal])
			term_close(terminals[current_terminal]);
		terminals[current_terminal] = NULL;
		current_terminal = 0;
		return;
	}

	for (unsigned i = 0; i < term_num_terminals; i++) {
		if (terminal == terminals[i]) {
			current_terminal = i;
			return;
		}
	}
	LOG(ERROR, "set_current_to: terminal not in array");
}

int term_switch_to(unsigned int vt)
{
	terminal_t *terminal;
	if (vt == term_get_current()) {
		terminal = term_get_current_terminal();
		if (term_is_valid(terminal)) {
			if (!term_is_active(terminal))
				term_activate(terminal);
			return vt;
		}
	}

	if (vt >= term_num_terminals)
		return -EINVAL;

	terminal = term_get_current_terminal();
	if (term_is_active(terminal))
		term_deactivate(terminal);

	/* Always background the splash terminal, so Chrome can become DRM
	 * master. */
	if (vt == TERM_SPLASH_TERMINAL
	    && !command_flags.enable_vt1) {
		term_set_current(vt);
                /* Returning to Chrome. Splash screen animation may be still
                 * running in background. */
		term_background(true);
		return vt;
	}

	term_foreground();

	term_set_current(vt);
	terminal = term_get_current_terminal();
	if (!terminal) {
		/* No terminal where we are switching to, create new one. */
		term_set_current_terminal(term_init(vt, -1));
		terminal = term_get_current_terminal();
		if (!term_is_valid(terminal)) {
			LOG(ERROR, "Term init failed VT%u.", vt);
			return -1;
		}
		term_activate(terminal);
	} else {
		term_activate(terminal);
	}

	return vt;
}

void term_monitor_hotplug(void)
{
	unsigned int t;

	if (in_background) {
		hotplug_occured = true;
		return;
	}

	if (!drm_rescan())
		return;

	for (t = 0; t < term_num_terminals; t++) {
		if (!terminals[t])
			continue;
		if (!terminals[t]->fb)
			continue;
		fb_buffer_destroy(terminals[t]->fb);
		font_free();
	}

	for (t = 0; t < term_num_terminals; t++) {
		if (!terminals[t])
			continue;
		if (!terminals[t]->fb)
			continue;
		fb_buffer_init(terminals[t]->fb);
		term_resize(terminals[t], 0);
		if (current_terminal == t && terminals[t]->active)
			fb_setmode(terminals[t]->fb);
		terminals[t]->term->age = 0;
		term_redraw(terminals[t]);
		if (current_terminal == t && terminals[t]->active)
			term_pointer_update(terminals[t]);
	}
}

void term_redrm(terminal_t* terminal)
{
	fb_buffer_destroy(terminal->fb);
	font_free();
	fb_buffer_init(terminal->fb);
	term_resize(terminal, 0);
	terminal->term->age = 0;
	term_redraw(terminal);
}

void term_clear(terminal_t* terminal)
{
	term_clear_border(terminal);
	tsm_screen_erase_screen(terminal->term->screen, false);
	term_redraw(terminal);
}

void term_zoom(bool zoom_in)
{
	int scaling = font_get_scaling();
	if (zoom_in && scaling < 4)
		scaling++;
	else if (!zoom_in && scaling > 1)
		scaling--;
	else
		return;

	unsigned int t;
	for (t = 0; t < term_num_terminals; t++) {
		if (terminals[t])
			font_free();
	}
	for (t = 0; t < term_num_terminals; t++) {
		terminal_t* term = terminals[t];
		if (term) {
			term_resize(term, scaling);
			term->term->age = 0;
			term_redraw(term);
		}
	}
}

/*
 * Put frecon in background. Give up DRM master.
 * onetry - if true, do not retry to notify Chrome multiple times. For use at
 * time when Chrome may be not around yet to receive the message.
 */
void term_background(bool onetry)
{
        terminal_t* terminal = term_get_current_terminal();
	int retry = onetry ? 1 : 5;
	if (in_background)
		return;
	in_background = true;

        /* The terminal also needs to be deactivated so it doesn't consume key
         * presses. */
	if (term_is_active(terminal))
		term_deactivate(terminal);

	drm_dropmaster(NULL);

	/* On a kernel VT, the kernel hands the display to the next owner. */
	if (vt_is_enabled())
		return;

	if (!dbus_is_initialized()) {
		LOG(WARNING, "Unable to send display ownership DBus message to "
                	"Chrome DisplayService: DBus not initialized");
		return;
	}

	while (!dbus_take_display_ownership() && retry--) {
		if (onetry)
			break;
		LOG(ERROR, "Chrome failed to take display ownership. %s",
		    retry ? "Trying again." : "Giving up, Chrome is probably dead.");
		if (retry > 0)
			usleep(500 * 1000);
	}
}

void term_foreground(void)
{
	int ret;
	int retry = 5;

	if (!in_background)
		return;
	if (vt_is_enabled() && !vt_is_foreground())
		return;
	in_background = false;

	/* LOG(INFO, "TIMING: Console switch time start."); */ /* Keep around for timing it in the future. */
	while (!vt_is_enabled() && !dbus_release_display_ownership() && retry--) {
		LOG(ERROR, "Chrome did not release master. %s",
		    retry ? "Trying again." : "Frecon will steal master.");
		if (retry > 0)
			usleep(500 * 1000);
	}

	/* LOG(INFO, "TIMING: Console switch setmaster."); */
	ret = drm_setmaster(NULL);
	if (ret < 0)
		LOG(ERROR, "Could not set master when switching to foreground %m.");

	if (hotplug_occured) {
		hotplug_occured = false;
		term_monitor_hotplug();
	}
}

void term_suspend_done(void* ignore)
{
	term_monitor_hotplug();
}

void term_input_enable(terminal_t* terminal, bool input_enable)
{
	terminal->input_enable = input_enable;
}

void term_mouse_enable(terminal_t* terminal, bool enable)
{
	terminal->mouse.enable = enable;
	terminal->mouse.buttons = 0;
	terminal->mouse.selecting = false;
	/* Only the active terminal is on the screen. */
	if (terminal->active)
		term_pointer_update(terminal);
}

/* The application asked for mouse events and shift is not held to override. */
static bool term_mouse_is_reporting(terminal_t* terminal, unsigned int mods)
{
	return terminal->input_enable && terminal->mouse.track &&
	       !(mods & TSM_MOUSE_MODIFIER_SHIFT);
}

#define MOUSE_REPORT_NO_BUTTON		3
#define MOUSE_REPORT_WHEEL_UP		64
#define MOUSE_REPORT_WHEEL_DOWN		65
#define MOUSE_REPORT_MOTION		32

/*
 * Send an xterm mouse report of button (TSM_MOUSE_BUTTON_LEFT, _MIDDLE or
 * _RIGHT, or MOUSE_REPORT_*) being pressed or released, or of motion with the
 * button held if motion is set.
 */
static void term_mouse_report(terminal_t* terminal, unsigned int button,
			      bool pressed, bool motion, unsigned int mods)
{
	struct mouse* mouse = &terminal->mouse;
	unsigned int x, y, code;
	char buf[32];
	int len;

	if (!term_pointer_cell(terminal, &x, &y))
		return;

	if (mouse->track == MOUSE_TRACK_X10) {
		if (!pressed || motion)
			return;
		mods = 0;
	}

	code = button | mods | (motion ? MOUSE_REPORT_MOTION : 0);
	if (mouse->encoding == MOUSE_ENCODING_SGR ||
	    mouse->encoding == MOUSE_ENCODING_PIXELS) {
		if (mouse->encoding == MOUSE_ENCODING_PIXELS) {
			x = pointer.x;
			y = pointer.y;
		}
		len = snprintf(buf, sizeof(buf), "\033[<%u;%u;%u%c",
			       code, x + 1, y + 1, pressed || motion ? 'M' : 'm');
	} else {
		/* The legacy encoding doesn't say which button was released. */
		if (!pressed && !motion)
			code = MOUSE_REPORT_NO_BUTTON | mods;
		buf[0] = '\033';
		buf[1] = '[';
		buf[2] = 'M';
		buf[3] = 32 + code;
		buf[4] = (char)MIN(32 + 1 + x, 255);
		buf[5] = (char)MIN(32 + 1 + y, 255);
		len = 6;
	}

	if (shl_pty_write(terminal->term->pty, buf, len) < 0)
		LOG(ERROR, "OOM in pty-write");
	shl_pty_dispatch(terminal->term->pty);
}

static void term_mouse_copy(terminal_t* terminal)
{
	char* text;

	if (tsm_screen_selection_copy(terminal->term->screen, &text) < 0)
		return;

	free(clipboard);
	clipboard = text;
}

static void term_mouse_paste(terminal_t* terminal)
{
	char* text;

	if (!clipboard || !terminal->input_enable)
		return;

	text = strdup(clipboard);
	if (!text)
		return;

	/* Send line breaks as the Enter key does. */
	for (char* c = text; *c; c++)
		if (*c == '\n')
			*c = '\r';

	tsm_vte_paste(terminal->term->vte, text);
	tsm_screen_sb_reset(terminal->term->screen);
	free(text);
}

static unsigned int term_mouse_count_clicks(terminal_t* terminal,
					    unsigned int x, unsigned int y)
{
	struct mouse* mouse = &terminal->mouse;
	struct timespec now;
	int64_t ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (now.tv_sec - mouse->click_time.tv_sec) * 1000 +
	     (now.tv_nsec - mouse->click_time.tv_nsec) / 1000000;

	if (mouse->clicks && x == mouse->click_x && y == mouse->click_y &&
	    ms < MOUSE_MULTI_CLICK_MS)
		mouse->clicks = mouse->clicks % 3 + 1;
	else
		mouse->clicks = 1;

	mouse->click_x = x;
	mouse->click_y = y;
	mouse->click_time = now;
	return mouse->clicks;
}

/*
 * Left button: a click clears the selection, dragging selects characters, a
 * double click selects a word, and a triple click selects a line.
 */
static void term_mouse_select_press(terminal_t* terminal, unsigned int x,
				    unsigned int y)
{
	struct mouse* mouse = &terminal->mouse;
	struct tsm_screen* screen = terminal->term->screen;

	switch (term_mouse_count_clicks(terminal, x, y)) {
	case 1:
		tsm_screen_selection_reset(screen);
		mouse->selecting = true;
		mouse->sel_started = false;
		mouse->anchor_x = x;
		mouse->anchor_y = y;
		break;
	case 2:
		tsm_screen_selection_word(screen, x, y);
		term_mouse_copy(terminal);
		mouse->selecting = false;
		mouse->sel_started = false;
		break;
	case 3:
		tsm_screen_selection_start(screen, 0, y);
		tsm_screen_selection_target(screen, terminal->term->w_in_char - 1, y);
		term_mouse_copy(terminal);
		mouse->selecting = false;
		mouse->sel_started = true;
		break;
	}
}

static void term_mouse_select_to(terminal_t* terminal, unsigned int x,
				 unsigned int y)
{
	struct mouse* mouse = &terminal->mouse;

	if (!mouse->sel_started) {
		if (x == mouse->anchor_x && y == mouse->anchor_y)
			return;
		tsm_screen_selection_start(terminal->term->screen,
					   mouse->anchor_x, mouse->anchor_y);
		mouse->sel_started = true;
	}
	tsm_screen_selection_target(terminal->term->screen, x, y);
}

static void term_mouse_motion(terminal_t* terminal, unsigned int mods)
{
	struct mouse* mouse = &terminal->mouse;
	unsigned int x, y;

	term_pointer_update(terminal);

	/* The rest only cares about moving to another cell. */
	if (!term_pointer_cell(terminal, &x, &y))
		return;
	if ((int)x == mouse->cell_x && (int)y == mouse->cell_y)
		return;
	mouse->cell_x = x;
	mouse->cell_y = y;

	if (mouse->buttons && mouse->reporting) {
		/* Drag with the lowest held button. */
		if (mouse->track == MOUSE_TRACK_BUTTON ||
		    mouse->track == MOUSE_TRACK_ANY)
			term_mouse_report(terminal,
					  __builtin_ctz(mouse->buttons),
					  true, true, mods);
	} else if (!mouse->buttons && term_mouse_is_reporting(terminal, mods)) {
		if (mouse->track == MOUSE_TRACK_ANY)
			term_mouse_report(terminal, MOUSE_REPORT_NO_BUTTON,
					  true, true, mods);
	} else if (mouse->selecting) {
		term_mouse_select_to(terminal, x, y);
		term_redraw(terminal);
	}
}

void term_mouse_move(terminal_t* terminal, int32_t dx, int32_t dy,
		     unsigned int mods)
{
	if (!terminal->mouse.enable)
		return;

	/* Start in the middle of the screen. */
	if (!pointer.visible) {
		pointer.x = fb_getwidth(terminal->fb) / 2;
		pointer.y = fb_getheight(terminal->fb) / 2;
	}

	term_mouse_move_to(terminal, pointer.x + dx, pointer.y + dy, mods);
}

void term_mouse_move_to(terminal_t* terminal, int32_t x, int32_t y,
			unsigned int mods)
{
	if (!terminal->mouse.enable)
		return;

	pointer.x = MAX(0, MIN(x, fb_getwidth(terminal->fb) - 1));
	pointer.y = MAX(0, MIN(y, fb_getheight(terminal->fb) - 1));
	pointer.visible = true;

	term_pointer_wake(terminal);
	term_mouse_motion(terminal, mods);
}

void term_mouse_button(terminal_t* terminal, unsigned int button,
		       bool pressed, unsigned int mods)
{
	struct mouse* mouse = &terminal->mouse;
	unsigned int x, y;

	if (!mouse->enable)
		return;

	if (!pointer.visible)
		term_mouse_move(terminal, 0, 0, mods);
	term_pointer_wake(terminal);
	if (!term_pointer_cell(terminal, &x, &y))
		return;

	if (pressed) {
		if (!mouse->buttons)
			mouse->reporting = term_mouse_is_reporting(terminal, mods);
		mouse->buttons |= 1u << button;
	} else {
		if (!(mouse->buttons & (1u << button)))
			return;
		mouse->buttons &= ~(1u << button);
	}

	if (mouse->reporting) {
		term_mouse_report(terminal, button, pressed, false, mods);
		return;
	}

	switch (button) {
	case TSM_MOUSE_BUTTON_LEFT:
		if (pressed) {
			term_mouse_select_press(terminal, x, y);
		} else if (mouse->selecting) {
			mouse->selecting = false;
			if (mouse->sel_started)
				term_mouse_copy(terminal);
		}
		break;
	case TSM_MOUSE_BUTTON_MIDDLE:
		if (pressed)
			term_mouse_paste(terminal);
		break;
	case TSM_MOUSE_BUTTON_RIGHT:
		/* Extend the selection to the pointer. */
		if (pressed && mouse->sel_started) {
			tsm_screen_selection_target(terminal->term->screen, x, y);
			term_mouse_copy(terminal);
		}
		break;
	}

	term_redraw(terminal);
}

/*
 * Scroll by notches of the wheel, up if positive. The scrollback is scrolled,
 * or on the alternate screen, which has no scrollback, the arrow keys are sent.
 */
void term_mouse_wheel(terminal_t* terminal, int32_t notches, unsigned int mods)
{
	struct tsm_screen* screen = terminal->term->screen;
	unsigned int count = abs(notches);
	bool up = notches > 0;

	if (!terminal->mouse.enable)
		return;

	if (pointer.visible)
		term_pointer_wake(terminal);

	if (term_mouse_is_reporting(terminal, mods)) {
		while (count--)
			term_mouse_report(terminal,
					  up ? MOUSE_REPORT_WHEEL_UP :
					       MOUSE_REPORT_WHEEL_DOWN,
					  true, false, mods);
		return;
	}

	count *= MOUSE_WHEEL_LINES;
	if (tsm_screen_get_flags(screen) & TSM_SCREEN_ALTERNATE) {
		if (!terminal->input_enable)
			return;
		while (count--)
			tsm_vte_handle_keyboard(terminal->term->vte,
						up ? XKB_KEY_Up : XKB_KEY_Down,
						0, 0, TSM_VTE_INVALID);
	} else if (up) {
		tsm_screen_sb_up(screen, count);
	} else {
		tsm_screen_sb_down(screen, count);
	}

	term_redraw(terminal);
}
