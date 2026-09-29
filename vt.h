/*
 * Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef VT_H
#define VT_H

#include <stdbool.h>
#include <sys/select.h>

/*
 * Integration with kernel virtual terminals. With --vt=N, frecon takes over
 * kernel VT N: the kernel stops drawing and processing the keyboard on it,
 * and frecon releases the display and input when the kernel switches to
 * another VT and reacquires them when it switches back.
 */
bool vt_init(unsigned int num);
void vt_close(void);
bool vt_is_enabled(void);
bool vt_is_foreground(void);
int vt_switch_to(unsigned int vt);
void vt_add_fds(fd_set* read_set, fd_set* exception_set, int *maxfd);
/* Returns false if frecon was asked to terminate. */
bool vt_dispatch_io(fd_set* read_set);

#endif
