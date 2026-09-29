/*
 * Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "term.h"
#include "util.h"
#include "vt.h"

#define VT_RELEASE_SIGNAL	SIGUSR1
#define VT_ACQUIRE_SIGNAL	SIGUSR2

static struct {
	int fd;
	int signal_fd;
	unsigned int num;
	int kd_mode;
	int kb_mode;
	bool foreground;
} vt = {
	.fd = -1,
	.signal_fd = -1,
};

/* Only uses ioctl(), so it is safe to call from a signal handler. */
static void vt_restore(void)
{
	struct vt_mode mode = { .mode = VT_AUTO };

	if (vt.fd < 0)
		return;

	ioctl(vt.fd, KDSKBMODE, vt.kb_mode);
	ioctl(vt.fd, KDSETMODE, vt.kd_mode);
	ioctl(vt.fd, VT_SETMODE, &mode);
}

/*
 * If we crash with the kernel keyboard off, the VT would be left unusable, so
 * give it back to the kernel before dying.
 */
static void vt_fatal_signal(int sig)
{
	vt_restore();
	raise(sig);
}

static void vt_install_fatal_handlers(void)
{
	static const int fatal_signals[] = {
		SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV
	};
	struct sigaction sa = {
		.sa_handler = vt_fatal_signal,
		.sa_flags = SA_RESETHAND,
	};

	sigemptyset(&sa.sa_mask);
	for (unsigned i = 0; i < ARRAY_SIZE(fatal_signals); i++)
		sigaction(fatal_signals[i], &sa, NULL);
}

bool vt_init(unsigned int num)
{
	char path[32];
	struct vt_stat state;
	struct vt_mode mode = {
		.mode = VT_PROCESS,
		.relsig = VT_RELEASE_SIGNAL,
		.acqsig = VT_ACQUIRE_SIGNAL,
	};
	sigset_t mask;

	if (num < 1 || num > MAX_NR_CONSOLES) {
		LOG(ERROR, "Invalid VT number %u.", num);
		return false;
	}

	vt.num = num;
	snprintf(path, sizeof(path), "/dev/tty%u", num);
	vt.fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (vt.fd < 0) {
		LOG(ERROR, "Unable to open %s: %m", path);
		return false;
	}

	if (ioctl(vt.fd, KDGETMODE, &vt.kd_mode) < 0 ||
	    ioctl(vt.fd, KDGKBMODE, &vt.kb_mode) < 0) {
		LOG(ERROR, "Unable to query VT%u mode: %m", vt.num);
		goto close_fd;
	}

	if (ioctl(vt.fd, VT_GETSTATE, &state) == 0 && state.v_active != vt.num) {
		LOG(INFO, "Waiting for VT%u to become active.", vt.num);
		if (ioctl(vt.fd, VT_WAITACTIVE, vt.num) < 0) {
			LOG(ERROR, "Unable to wait for VT%u: %m", vt.num);
			goto close_fd;
		}
	}

	/*
	 * Receive VT release/acquire requests and termination requests through
	 * the main loop. Children get a clean signal mask from shl_pty.
	 */
	sigemptyset(&mask);
	sigaddset(&mask, VT_RELEASE_SIGNAL);
	sigaddset(&mask, VT_ACQUIRE_SIGNAL);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGHUP);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		LOG(ERROR, "Unable to block VT signals: %m");
		goto close_fd;
	}

	vt.signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (vt.signal_fd < 0) {
		LOG(ERROR, "Unable to create signalfd: %m");
		goto unblock;
	}

	if (ioctl(vt.fd, VT_SETMODE, &mode) < 0) {
		LOG(ERROR, "Unable to take over VT%u switching: %m", vt.num);
		goto close_signal_fd;
	}

	/* From here on vt_restore() must run before we exit. */
	vt_install_fatal_handlers();
	atexit(vt_close);

	if (ioctl(vt.fd, KDSETMODE, KD_GRAPHICS) < 0)
		LOG(WARNING, "Unable to set VT%u to graphics mode: %m", vt.num);

	if (ioctl(vt.fd, KDSKBMODE, K_OFF) < 0)
		LOG(WARNING, "Unable to turn off VT%u keyboard: %m", vt.num);

	vt.foreground = true;
	LOG(INFO, "Running on kernel VT%u.", vt.num);
	return true;

close_signal_fd:
	close(vt.signal_fd);
	vt.signal_fd = -1;
unblock:
	sigprocmask(SIG_UNBLOCK, &mask, NULL);
close_fd:
	close(vt.fd);
	vt.fd = -1;
	return false;
}

void vt_close(void)
{
	if (vt.fd < 0)
		return;

	vt_restore();
	close(vt.fd);
	vt.fd = -1;
	if (vt.signal_fd >= 0) {
		close(vt.signal_fd);
		vt.signal_fd = -1;
	}
}

bool vt_is_enabled(void)
{
	return vt.fd >= 0;
}

bool vt_is_foreground(void)
{
	return vt.foreground;
}

int vt_switch_to(unsigned int num)
{
	if (vt.fd < 0)
		return -ENODEV;

	if (ioctl(vt.fd, VT_ACTIVATE, num) < 0) {
		LOG(ERROR, "Unable to switch to VT%u: %m", num);
		return -errno;
	}

	return 0;
}

static void vt_release(void)
{
	term_background(true);
	vt.foreground = false;

	/* The next VT can only get DRM master once we have dropped it. */
	if (ioctl(vt.fd, VT_RELDISP, 1) < 0)
		LOG(ERROR, "Unable to release VT%u: %m", vt.num);
}

static void vt_acquire(void)
{
	if (ioctl(vt.fd, VT_RELDISP, VT_ACKACQ) < 0)
		LOG(ERROR, "Unable to acknowledge VT%u acquire: %m", vt.num);

	vt.foreground = true;
	term_foreground();
	term_switch_to(term_get_current());
}

void vt_add_fds(fd_set* read_set, fd_set* exception_set, int *maxfd)
{
	if (vt.signal_fd < 0)
		return;

	FD_SET(vt.signal_fd, read_set);
	if (vt.signal_fd > *maxfd)
		*maxfd = vt.signal_fd;
}

bool vt_dispatch_io(fd_set* read_set)
{
	struct signalfd_siginfo info;

	if (vt.signal_fd < 0 || !FD_ISSET(vt.signal_fd, read_set))
		return true;

	while (read(vt.signal_fd, &info, sizeof(info)) == sizeof(info)) {
		switch (info.ssi_signo) {
		case VT_RELEASE_SIGNAL:
			vt_release();
			break;
		case VT_ACQUIRE_SIGNAL:
			vt_acquire();
			break;
		default:
			LOG(INFO, "Received signal %u, exiting.", info.ssi_signo);
			return false;
		}
	}

	return true;
}
