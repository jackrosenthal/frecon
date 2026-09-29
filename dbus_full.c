/*
 * Copyright 2014 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dbus/dbus.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dbus.h"
#include "dbus_interface.h"
#include "image.h"
#include "main.h"
#include "term.h"
#include "util.h"

#define DBUS_DEFAULT_DELAY             3000

typedef struct _dbus_t dbus_t;

static void (*login_prompt_visible_callback)(void) = NULL;
static void (*suspend_done_callback)(void*) = NULL;
static void* suspend_done_callback_userptr = NULL;
static bool chrome_is_already_up = false;
static bool dbus_connect_fail = false;
static int64_t dbus_connect_fail_time;
static bool dbus_first_init = true;
static int64_t dbus_first_init_time;

struct _dbus_t {
	DBusConnection* conn;
	DBusWatch* watch;
	int fd;
};

static dbus_t *dbus = NULL;

static void frecon_dbus_unregister(DBusConnection* connection, void* user_data)
{
}

static DBusHandlerResult frecon_dbus_message_handler(DBusConnection* connection,
						     DBusMessage* message,
						     void* user_data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable
frecon_vtable = {
	frecon_dbus_unregister,
	frecon_dbus_message_handler,
	NULL
};

static dbus_bool_t add_watch(DBusWatch* w, void* data)
{
	dbus_t* dbus = (dbus_t*)data;
	dbus->watch = w;

	return TRUE;
}

static void remove_watch(DBusWatch* w, void* data)
{
}

static void toggle_watch(DBusWatch* w, void* data)
{
}

static DBusHandlerResult handle_login_prompt_visible(DBusMessage* message)
{
	if (login_prompt_visible_callback) {
		login_prompt_visible_callback();
		login_prompt_visible_callback = NULL;
	}
	chrome_is_already_up = true;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_suspend_done(DBusMessage* message)
{
	if (suspend_done_callback)
		suspend_done_callback(suspend_done_callback_userptr);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult frecon_dbus_message_filter(DBusConnection* connection,
						    DBusMessage* message,
						    void* user_data)
{
	if (dbus_message_is_signal(message,
				kSessionManagerInterface, kLoginPromptVisibleSignal))
		return handle_login_prompt_visible(message);
	else if (dbus_message_is_signal(message,
				kPowerManagerInterface, kSuspendDoneSignal))
		return handle_suspend_done(message);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool dbus_is_initialized(void)
{
	return !!dbus;
}

bool dbus_init()
{
	dbus_t* new_dbus;
	DBusError err;
	int result;
	dbus_bool_t stat;

	if (dbus_first_init) {
		dbus_first_init = false;
		dbus_first_init_time = get_monotonic_time_ms();
	}
	dbus_error_init(&err);

	new_dbus = (dbus_t*)calloc(1, sizeof(*new_dbus));

	if (!new_dbus)
		return false;

	new_dbus->fd = -1;

	new_dbus->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (dbus_error_is_set(&err)) {
		if (!dbus_connect_fail) {
			LOG(DEBUG, "Cannot get DBUS connection");
			dbus_connect_fail = true;
			dbus_connect_fail_time = get_monotonic_time_ms();
		}
		free(new_dbus);
		return false;
	}

	if (dbus_connect_fail) {
		int64_t t = get_monotonic_time_ms() - dbus_connect_fail_time;
		LOG(DEBUG, "DBUS connected after %.1f seconds", (float)t / 1000.0f);
	}

	result = dbus_bus_request_name(new_dbus->conn, kFreconDbusInterface,
			DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);

	if (result <= 0) {
		LOG(ERROR, "Unable to get name for server: %s",
		    dbus_error_is_set(&err) ? err.message : "unknown error");
		dbus_error_free(&err);
	}

	stat = dbus_connection_register_object_path(new_dbus->conn,
			kFreconDbusPath,
			&frecon_vtable,
			NULL);

	if (!stat) {
		LOG(ERROR, "failed to register object path");
	}

	dbus_bus_add_match(new_dbus->conn, kLoginPromptVisibleRule, &err);
	dbus_bus_add_match(new_dbus->conn, kSuspendDoneRule, &err);

	stat = dbus_connection_add_filter(new_dbus->conn, frecon_dbus_message_filter, NULL, NULL);
	if (!stat) {
		LOG(ERROR, "failed to add message filter");
	}

	stat = dbus_connection_set_watch_functions(new_dbus->conn,
			add_watch, remove_watch, toggle_watch,
			new_dbus, NULL);

	if (!stat) {
		LOG(ERROR, "Failed to set watch functions");
	}

	dbus_connection_set_exit_on_disconnect(new_dbus->conn, FALSE);

	dbus = new_dbus;
	return true;
}

static bool dbus_method_call0(const char* service_name,
			      const char* service_path,
			      const char* service_interface,
			      const char* method)
{
	DBusMessage* msg = NULL;
	if (!dbus) {
		LOG(ERROR, "dbus not initialized");
		return false;
	}

	msg = dbus_message_new_method_call(service_name,
			service_path, service_interface, method);

	if (!msg)
		return false;

	if (!dbus_connection_send_with_reply_and_block(dbus->conn,
				msg, DBUS_DEFAULT_DELAY, NULL)) {
		dbus_message_unref(msg);
		return false;
	}

	dbus_connection_flush(dbus->conn);
	dbus_message_unref(msg);

	return true;
}

static bool dbus_method_call0_bool(const char* service_name,
				   const char* service_path,
				   const char* service_interface,
				   const char* method)
{
	DBusMessage* msg = NULL;
	DBusMessage* reply = NULL;
	int res = false;

	if (!dbus) {
		LOG(ERROR, "dbus not initialized");
		return false;
	}

	msg = dbus_message_new_method_call(service_name,
			service_path, service_interface, method);

	if (!msg)
		return false;

	reply = dbus_connection_send_with_reply_and_block(dbus->conn,
				msg, DBUS_DEFAULT_DELAY, NULL);
	if (!reply) {
		dbus_message_unref(msg);
		return false;
	}

	dbus_message_get_args(reply, NULL, DBUS_TYPE_BOOLEAN, &res, DBUS_TYPE_INVALID);

	dbus_connection_flush(dbus->conn);
	dbus_message_unref(msg);
	dbus_message_unref(reply);

	return (bool)res;
}

static bool dbus_method_call1(const char* service_name,
			      const char* service_path,
			      const char* service_interface,
			      const char* method, int arg_type, void* param)
{
	DBusMessage* msg = NULL;
	if (!dbus) {
		LOG(ERROR, "dbus not initialized");
		return false;
	}

	msg = dbus_message_new_method_call(service_name,
			service_path, service_interface, method);

	if (!msg)
		return false;

	if (!dbus_message_append_args(msg,
				arg_type, param, DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return false;
	}

	if (!dbus_connection_send_with_reply_and_block(dbus->conn,
				msg, DBUS_DEFAULT_DELAY, NULL)) {
		dbus_message_unref(msg);
		return false;
	}

	dbus_connection_flush(dbus->conn);
	dbus_message_unref(msg);

	return true;
}

void dbus_destroy(void)
{
	/* FIXME - not sure what the right counterpart to
	 * dbus_bus_get() is, unref documentation is rather
	 * unclear. Not a big issue but it would be nice to
	 * clean up properly here
	 */
	/* dbus_connection_unref(dbus->conn); */
	if (dbus) {
		free(dbus);
		dbus = NULL;
	}
}

void dbus_add_fds(fd_set* read_set, fd_set* exception_set, int *maxfd)
{
	if (!dbus)
		return;

	if (dbus->fd < 0)
		dbus->fd = dbus_watch_get_unix_fd(dbus->watch);

	if (dbus->fd >= 0) {
		FD_SET(dbus->fd, read_set);
		FD_SET(dbus->fd, exception_set);
	}

	if (dbus->fd > *maxfd)
		*maxfd = dbus->fd;
}

void dbus_dispatch_io(void)
{
	if (!dbus)
		return;

	dbus_watch_handle(dbus->watch, DBUS_WATCH_READABLE);
	while (dbus_connection_get_dispatch_status(dbus->conn)
			== DBUS_DISPATCH_DATA_REMAINS) {
		dbus_connection_dispatch(dbus->conn);
	}
}

void dbus_report_user_activity(int activity_type)
{
	dbus_bool_t allow_off = false;
	if (!dbus)
		return;

	dbus_method_call1(kPowerManagerServiceName,
			kPowerManagerServicePath,
			kPowerManagerInterface,
			kHandleUserActivityMethod,
			DBUS_TYPE_INT32, &activity_type);

	switch (activity_type) {
		case USER_ACTIVITY_BRIGHTNESS_UP_KEY_PRESS:
				(void)dbus_method_call0(kPowerManagerServiceName,
					kPowerManagerServicePath,
					kPowerManagerInterface,
					kIncreaseScreenBrightnessMethod);
				break;
		case USER_ACTIVITY_BRIGHTNESS_DOWN_KEY_PRESS:
				/*
				 * Shouldn't allow the screen to go
				 * completely off while frecon is active
				 * so passing false to allow_off
				 */
				(void)dbus_method_call1(kPowerManagerServiceName,
					kPowerManagerServicePath,
					kPowerManagerInterface,
					kDecreaseScreenBrightnessMethod,
					DBUS_TYPE_BOOLEAN, &allow_off);
				break;
	}
}

/*
 * tell Chrome to take ownership of the display (DRM master)
 */
bool dbus_take_display_ownership(void)
{
	if (!dbus)
		return true;
	return dbus_method_call0_bool(kDisplayServiceName,
				      kDisplayServicePath,
				      kDisplayServiceInterface,
				      kTakeOwnership);
}

/*
 * ask Chrome to give up display ownership (DRM master)
 */
bool dbus_release_display_ownership(void)
{
	if (!dbus)
		return true;
	return dbus_method_call0_bool(kDisplayServiceName,
				      kDisplayServicePath,
				      kDisplayServiceInterface,
				      kReleaseOwnership);
}

void dbus_set_login_prompt_visible_callback(void (*callback)(void))
{
	if (chrome_is_already_up) {
		if (callback)
			callback();
	} else {
		if (login_prompt_visible_callback && callback) {
			LOG(ERROR, "trying to register login prompt visible callback multiple times");
			return;
		}
		login_prompt_visible_callback = callback;
	}
}

void dbus_set_suspend_done_callback(void (*callback)(void*),
				    void* userptr)
{
	if (suspend_done_callback && callback) {
		LOG(ERROR, "trying to register login prompt visible callback multiple times");
		return;
	}
	suspend_done_callback = callback;
	suspend_done_callback_userptr = userptr;
}

static char* dbus_get_string_property(DBusConnection* conn,
				      const char* service_name,
				      const char* service_path,
				      const char* interface,
				      const char* property)
{
	DBusMessage* msg;
	DBusMessage* reply;
	DBusMessageIter iter, variant;
	DBusError err;
	const char* value;
	char* result = NULL;

	msg = dbus_message_new_method_call(service_name, service_path,
					   DBUS_INTERFACE_PROPERTIES, "Get");
	if (!msg)
		return NULL;

	if (!dbus_message_append_args(msg,
				      DBUS_TYPE_STRING, &interface,
				      DBUS_TYPE_STRING, &property,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return NULL;
	}

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(conn, msg,
			DBUS_DEFAULT_DELAY, &err);
	dbus_message_unref(msg);
	if (!reply) {
		LOG(WARNING, "Unable to get %s: %s", property, err.message);
		dbus_error_free(&err);
		return NULL;
	}

	if (dbus_message_iter_init(reply, &iter) &&
	    dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&iter, &variant);
		if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&variant, &value);
			result = strdup(value);
		}
	}

	dbus_message_unref(reply);
	return result;
}

/*
 * Get the system keyboard layout that systemd-localed stores for X11.  This
 * runs before dbus_init(), so it uses its own connection.  On success, the
 * caller must free the strings.
 */
bool dbus_get_x11_keymap(char** model, char** layout, char** variant,
			 char** options)
{
	static const char* const properties[] = {
		"X11Model", "X11Layout", "X11Variant", "X11Options",
	};
	char** values[] = { model, layout, variant, options };
	DBusConnection* conn;
	DBusError err;
	bool ret = true;

	dbus_error_init(&err);
	conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	if (!conn) {
		LOG(DEBUG, "Cannot get DBUS connection: %s", err.message);
		dbus_error_free(&err);
		return false;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	for (unsigned i = 0; i < ARRAY_SIZE(properties); i++) {
		*values[i] = dbus_get_string_property(conn, kLocaleServiceName,
						      kLocaleServicePath,
						      kLocaleInterface,
						      properties[i]);
		if (!*values[i])
			ret = false;
	}

	dbus_connection_close(conn);
	dbus_connection_unref(conn);

	if (!ret) {
		for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
			free(*values[i]);
			*values[i] = NULL;
		}
	}

	return ret;
}
