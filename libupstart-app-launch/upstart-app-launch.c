/*
 * Copyright 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 */

#include "upstart-app-launch.h"
#include <json-glib/json-glib.h>
#include <upstart.h>
#include <gio/gio.h>
#include <string.h>

#include "upstart-app-launch-trace.h"
#include "second-exec-core.h"
#include "../helpers.h"

static void apps_for_job (GDBusConnection * con, const gchar * name, GArray * apps, gboolean truncate_legacy);
static void free_helper (gpointer value);

/* Function to take the urls and escape them so that they can be
   parsed on the other side correctly. */
static gchar *
app_uris_string (const gchar * const * uris)
{
	guint i = 0;
	GArray * array = g_array_new(TRUE, TRUE, sizeof(gchar *));
	g_array_set_clear_func(array, free_helper);

	for (i = 0; i < g_strv_length((gchar**)uris); i++) {
		gchar * escaped = g_shell_quote(uris[i]);
		g_array_append_val(array, escaped);
	}

	gchar * urisjoin = g_strjoinv(" ", (gchar**)array->data);
	g_array_unref(array);

	return urisjoin;
}

typedef struct {
	gchar * appid;
	gchar * uris;
} app_start_t;

static void
application_start_cb (GObject * obj, GAsyncResult * res, gpointer user_data)
{
	app_start_t * data = (app_start_t *)user_data;
	GError * error = NULL;
	GVariant * result = NULL;

	tracepoint(upstart_app_launch, libual_start_message_callback, data->appid);
	g_debug("Started Message Callback: %s", data->appid);

	result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(obj), res, &error);

	if (result != NULL)
		g_variant_unref(result);
	
	if (error != NULL) {
		if (g_dbus_error_is_remote_error(error)) {
			gchar * remote_error = g_dbus_error_get_remote_error(error);
			g_debug("Remote error: %s", remote_error);
			if (g_strcmp0(remote_error, "com.ubuntu.Upstart0_6.Error.AlreadyStarted") == 0) {
				second_exec(data->appid, data->uris);
			}

			g_free(remote_error);
		} else {
			g_warning("Unable to emit event to start application: %s", error->message);
		}
		g_error_free(error);
	}

	g_free(data->appid);
	g_free(data->uris);
	g_free(data);
}

/* Get the path of the job from Upstart, if we've got it already, we'll just
   use the cache of the value */
static const gchar *
get_jobpath (GDBusConnection * con, const gchar * jobname)
{
	gchar * cachepath = g_strdup_printf("upstart-app-lauch-job-path-cache-%s", jobname);
	gpointer cachedata = g_object_get_data(G_OBJECT(con), cachepath);

	if (cachedata != NULL) {
		g_free(cachepath);
		return cachedata;
	}

	GError * error = NULL;
	GVariant * job_path_variant = g_dbus_connection_call_sync(con,
		DBUS_SERVICE_UPSTART,
		DBUS_PATH_UPSTART,
		DBUS_INTERFACE_UPSTART,
		"GetJobByName",
		g_variant_new("(s)", jobname),
		G_VARIANT_TYPE("(o)"),
		G_DBUS_CALL_FLAGS_NONE,
		-1, /* timeout: default */
		NULL, /* cancelable */
		&error);

	if (error != NULL) {	
		g_warning("Unable to find job '%s': %s", jobname, error->message);
		g_error_free(error);
		g_free(cachepath);
		return NULL;
	}

	gchar * job_path = NULL;
	g_variant_get(job_path_variant, "(o)", &job_path);
	g_variant_unref(job_path_variant);

	g_object_set_data_full(G_OBJECT(con), cachepath, job_path, g_free);
	g_free(cachepath);

	return job_path;
}

/* Check to see if a legacy app wants us to manage whether they're
   single instance or not */
static gboolean
legacy_single_instance (const gchar * appid)
{
	tracepoint(upstart_app_launch, desktop_single_start, appid);

	GKeyFile * keyfile = keyfile_for_appid(appid, NULL);

	if (keyfile == NULL) {
		g_error("Unable to find keyfile for application '%s'", appid);
		return FALSE;
	}

	tracepoint(upstart_app_launch, desktop_single_found, appid);

	gboolean singleinstance = FALSE;

	if (g_key_file_has_key(keyfile, "Desktop Entry", "X-Ubuntu-Single-Instance", NULL)) {
		GError * error = NULL;

		singleinstance = g_key_file_get_boolean(keyfile, "Desktop Entry", "X-Ubuntu-Single-Instance", &error);

		if (error != NULL) {
			g_warning("Unable to get single instance key for app '%s': %s", appid, error->message);
			g_error_free(error);
			/* Ensure that if we got an error, we assume standard case */
			singleinstance = FALSE;
		}
	}
	
	g_key_file_free(keyfile);

	tracepoint(upstart_app_launch, desktop_single_finished, appid, singleinstance ? "single" : "unmanaged");

	return singleinstance;
}

gboolean
upstart_app_launch_start_application (const gchar * appid, const gchar * const * uris)
{
	g_return_val_if_fail(appid != NULL, FALSE);

	tracepoint(upstart_app_launch, libual_start, appid);

	GDBusConnection * con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	g_return_val_if_fail(con != NULL, FALSE);

	/* Determine whether it's a click package by looking for the symlink
	   that is created by the desktop hook */
	gchar * appiddesktop = g_strdup_printf("%s.desktop", appid);
	gchar * click_link = NULL;
	const gchar * link_farm_dir = g_getenv("UPSTART_APP_LAUNCH_LINK_FARM");
	if (G_LIKELY(link_farm_dir == NULL)) {
		click_link = g_build_filename(g_get_home_dir(), ".cache", "upstart-app-launch", "desktop", appiddesktop, NULL);
	} else {
		click_link = g_build_filename(link_farm_dir, appiddesktop, NULL);
	}
	g_free(appiddesktop);
	gboolean click = g_file_test(click_link, G_FILE_TEST_EXISTS);
	g_free(click_link);

	tracepoint(upstart_app_launch, libual_determine_type, appid, click ? "click" : "legacy");

	/* Figure out the DBus path for the job */
	const gchar * jobpath = NULL;
	if (click) {
		jobpath = get_jobpath(con, "application-click");
	} else {
		jobpath = get_jobpath(con, "application-legacy");
	}

	if (jobpath == NULL)
		return FALSE;

	tracepoint(upstart_app_launch, libual_job_path_determined, appid, jobpath);

	/* Callback data */
	app_start_t * app_start_data = g_new0(app_start_t, 1);
	app_start_data->appid = g_strdup(appid);

	/* Build up our environment */
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_TUPLE);

	g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);

	g_variant_builder_add_value(&builder, g_variant_new_take_string(g_strdup_printf("APP_ID=%s", appid)));

	if (uris != NULL) {
		gchar * urisjoin = app_uris_string(uris);
		gchar * urienv = g_strdup_printf("APP_URIS=%s", urisjoin);
		app_start_data->uris = urisjoin;
		g_variant_builder_add_value(&builder, g_variant_new_take_string(urienv));
	}

	if (!click) {
		if (legacy_single_instance(appid)) {
			g_variant_builder_add_value(&builder, g_variant_new_string("INSTANCE_ID="));
		} else {
			gchar * instanceid = g_strdup_printf("INSTANCE_ID=%" G_GUINT64_FORMAT, g_get_real_time());
			g_variant_builder_add_value(&builder, g_variant_new_take_string(instanceid));
		}
	}

	g_variant_builder_close(&builder);
	g_variant_builder_add_value(&builder, g_variant_new_boolean(TRUE));
	
	/* Call the job start function */
	g_dbus_connection_call(con,
	                       DBUS_SERVICE_UPSTART,
	                       jobpath,
	                       DBUS_INTERFACE_UPSTART_JOB,
	                       "Start",
	                       g_variant_builder_end(&builder),
	                       NULL,
	                       G_DBUS_CALL_FLAGS_NONE,
	                       -1,
	                       NULL, /* cancelable */
	                       application_start_cb,
	                       app_start_data);

	tracepoint(upstart_app_launch, libual_start_message_sent, appid);

	g_object_unref(con);

	return TRUE;
}

static void
stop_job (GDBusConnection * con, const gchar * jobname, const gchar * appname, const gchar * instanceid)
{
	g_debug("Stopping job %s app_id %s instance_id %s", jobname, appname, instanceid);

	const gchar * job_path = get_jobpath(con, jobname);
	if (job_path == NULL)
		return;

	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_TUPLE);
	g_variant_builder_open(&builder, G_VARIANT_TYPE_ARRAY);

	g_variant_builder_add_value(&builder,
		g_variant_new_take_string(g_strdup_printf("APP_ID=%s", appname)));
	
	if (instanceid != NULL) {
		g_variant_builder_add_value(&builder,
			g_variant_new_take_string(g_strdup_printf("INSTANCE_ID=%s", instanceid)));
	}

	g_variant_builder_close(&builder);
	g_variant_builder_add_value(&builder, g_variant_new_boolean(FALSE)); /* wait */

	GError * error = NULL;
	GVariant * stop_variant = g_dbus_connection_call_sync(con,
		DBUS_SERVICE_UPSTART,
		job_path,
		DBUS_INTERFACE_UPSTART_JOB,
		"Stop",
		g_variant_builder_end(&builder),
		NULL,
		G_DBUS_CALL_FLAGS_NONE,
		-1, /* timeout: default */
		NULL, /* cancelable */
		&error);

	if (error != NULL) {
		g_warning("Unable to stop job %s app_id %s instance_id %s: %s", jobname, appname, instanceid, error->message);
		g_error_free(error);
	}

	g_variant_unref(stop_variant);
}

static void
free_helper (gpointer value)
{
	gchar ** strp = (gchar **)value;
	g_free(*strp);
}

gboolean
upstart_app_launch_stop_application (const gchar * appid)
{
	g_return_val_if_fail(appid != NULL, FALSE);

	GDBusConnection * con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	g_return_val_if_fail(con != NULL, FALSE);

	gboolean found = FALSE;
	int i;

	GArray * apps = g_array_new(TRUE, TRUE, sizeof(gchar *));
	g_array_set_clear_func(apps, free_helper);

	/* Look through the click jobs and see if any match.  There can
	   only be one instance for each ID in the click world */
	apps_for_job(con, "application-click", apps, FALSE);
	for (i = 0; i < apps->len; i++) {
		const gchar * array_id = g_array_index(apps, const gchar *, i);
		if (g_strcmp0(array_id, appid) == 0) {
			stop_job(con, "application-click", appid, NULL);
			found = TRUE;
			break; /* There can be only one with click */
		}
	}

	if (apps->len > 0)
		g_array_remove_range(apps, 0, apps->len);

	/* Look through the legacy apps.  Trickier because we know that there
	   can be many instances of the legacy jobs out there, so we might
	   have to kill more than one of them. */
	apps_for_job(con, "application-legacy", apps, FALSE);
	gchar * appiddash = g_strdup_printf("%s-", appid); /* Probably could go RegEx here, but let's start with just a prefix lookup */
	for (i = 0; i < apps->len; i++) {
		const gchar * array_id = g_array_index(apps, const gchar *, i);
		if (g_str_has_prefix(array_id, appiddash)) {
			gchar * instanceid = g_strrstr(array_id, "-");
			stop_job(con, "application-legacy", appid, &(instanceid[1]));
			found = TRUE;
		}
	}
	g_free(appiddash);

	g_array_free(apps, TRUE);
	g_object_unref(con);

	return found;
}

static GDBusConnection *
gdbus_upstart_ref (void) {
	static GDBusConnection * gdbus_upstart = NULL;

	if (gdbus_upstart != NULL) {
		return g_object_ref(gdbus_upstart);
	}

	GError * error = NULL;
	gdbus_upstart = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

	if (error != NULL) {
		g_warning("Unable to connect to Upstart bus: %s", error->message);
		g_error_free(error);
		return NULL;
	}

	g_object_add_weak_pointer(G_OBJECT(gdbus_upstart), (gpointer)&gdbus_upstart);

	return gdbus_upstart;
}

/* The data we keep for each observer */
typedef struct _observer_t observer_t;
struct _observer_t {
	GDBusConnection * conn;
	guint sighandle;
	upstart_app_launch_app_observer_t func;
	gpointer user_data;
};

/* The data we keep for each failed observer */
typedef struct _failed_observer_t failed_observer_t;
struct _failed_observer_t {
	GDBusConnection * conn;
	guint sighandle;
	upstart_app_launch_app_failed_observer_t func;
	gpointer user_data;
};

/* The lists of Observers */
static GList * starting_array = NULL;
static GList * started_array = NULL;
static GList * stop_array = NULL;
static GList * focus_array = NULL;
static GList * resume_array = NULL;
static GList * failed_array = NULL;

static void
observer_cb (GDBusConnection * conn, const gchar * sender, const gchar * object, const gchar * interface, const gchar * signal, GVariant * params, gpointer user_data)
{
	observer_t * observer = (observer_t *)user_data;

	const gchar * signalname = NULL;
	g_variant_get_child(params, 0, "&s", &signalname);

	tracepoint(upstart_app_launch, observer_start, signalname);

	gchar * env = NULL;
	GVariant * envs = g_variant_get_child_value(params, 1);
	GVariantIter iter;
	g_variant_iter_init(&iter, envs);

	gboolean job_found = FALSE;
	gboolean job_legacy = FALSE;
	gchar * instance = NULL;

	while (g_variant_iter_loop(&iter, "s", &env)) {
		if (g_strcmp0(env, "JOB=application-click") == 0) {
			job_found = TRUE;
		} else if (g_strcmp0(env, "JOB=application-legacy") == 0) {
			job_found = TRUE;
			job_legacy = TRUE;
		} else if (g_str_has_prefix(env, "INSTANCE=")) {
			instance = g_strdup(env + strlen("INSTANCE="));
		}
	}

	g_variant_unref(envs);

	if (job_legacy && instance != NULL) {
		gchar * dash = g_strrstr(instance, "-");
		if (dash != NULL) {
			dash[0] = '\0';
		}
	}

	if (job_found && instance != NULL) {
		observer->func(instance, observer->user_data);
	}

	tracepoint(upstart_app_launch, observer_finish, signalname);

	g_free(instance);
}

/* Creates the observer structure and registers for the signal with
   GDBus so that we can get a callback */
static gboolean
add_app_generic (upstart_app_launch_app_observer_t observer, gpointer user_data, const gchar * signal, GList ** list)
{
	GDBusConnection * conn = gdbus_upstart_ref();

	if (conn == NULL) {
		return FALSE;
	}

	observer_t * observert = g_new0(observer_t, 1);

	observert->conn = conn;
	observert->func = observer;
	observert->user_data = user_data;

	*list = g_list_prepend(*list, observert);

	observert->sighandle = g_dbus_connection_signal_subscribe(conn,
		NULL, /* sender */
		DBUS_INTERFACE_UPSTART, /* interface */
		"EventEmitted", /* signal */
		DBUS_PATH_UPSTART, /* path */
		signal, /* arg0 */
		G_DBUS_SIGNAL_FLAGS_NONE,
		observer_cb,
		observert,
		NULL); /* user data destroy */

	return TRUE;
}

gboolean
upstart_app_launch_observer_add_app_started (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return add_app_generic(observer, user_data, "started", &started_array);
}

gboolean
upstart_app_launch_observer_add_app_stop (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return add_app_generic(observer, user_data, "stopped", &stop_array);
}

/* Creates the observer structure and registers for the signal with
   GDBus so that we can get a callback */
static gboolean
add_session_generic (upstart_app_launch_app_observer_t observer, gpointer user_data, const gchar * signal, GList ** list, GDBusSignalCallback session_cb)
{
	GDBusConnection * conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

	if (conn == NULL) {
		return FALSE;
	}

	observer_t * observert = g_new0(observer_t, 1);

	observert->conn = conn;
	observert->func = observer;
	observert->user_data = user_data;

	*list = g_list_prepend(*list, observert);

	observert->sighandle = g_dbus_connection_signal_subscribe(conn,
		NULL, /* sender */
		"com.canonical.UpstartAppLaunch", /* interface */
		signal, /* signal */
		"/", /* path */
		NULL, /* arg0 */
		G_DBUS_SIGNAL_FLAGS_NONE,
		session_cb,
		observert,
		NULL); /* user data destroy */

	return TRUE;
}

/* Handle the focus signal when it occurs, call the observer */
static void
focus_signal_cb (GDBusConnection * conn, const gchar * sender, const gchar * object, const gchar * interface, const gchar * signal, GVariant * params, gpointer user_data)
{
	observer_t * observer = (observer_t *)user_data;
	const gchar * appid = NULL;

	tracepoint(upstart_app_launch, observer_start, "focus");

	if (observer->func != NULL) {
		g_variant_get(params, "(&s)", &appid);
		observer->func(appid, observer->user_data);
	}

	tracepoint(upstart_app_launch, observer_finish, "focus");
}

gboolean
upstart_app_launch_observer_add_app_focus (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return add_session_generic(observer, user_data, "UnityFocusRequest", &focus_array, focus_signal_cb);
}

/* Handle the resume signal when it occurs, call the observer, then send a signal back when we're done */
static void
resume_signal_cb (GDBusConnection * conn, const gchar * sender, const gchar * object, const gchar * interface, const gchar * signal, GVariant * params, gpointer user_data)
{
	tracepoint(upstart_app_launch, observer_start, "resume");

	focus_signal_cb(conn, sender, object, interface, signal, params, user_data);

	GError * error = NULL;
	g_dbus_connection_emit_signal(conn,
		sender, /* destination */
		"/", /* path */
		"com.canonical.UpstartAppLaunch", /* interface */
		"UnityResumeResponse", /* signal */
		params, /* params, the same */
		&error);

	if (error != NULL) {
		g_warning("Unable to emit response signal: %s", error->message);
		g_error_free(error);
	}

	tracepoint(upstart_app_launch, observer_finish, "resume");
}

gboolean
upstart_app_launch_observer_add_app_resume (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return add_session_generic(observer, user_data, "UnityResumeRequest", &resume_array, resume_signal_cb);
}

/* Handle the starting signal when it occurs, call the observer, then send a signal back when we're done */
static void
starting_signal_cb (GDBusConnection * conn, const gchar * sender, const gchar * object, const gchar * interface, const gchar * signal, GVariant * params, gpointer user_data)
{
	tracepoint(upstart_app_launch, observer_start, "starting");

	focus_signal_cb(conn, sender, object, interface, signal, params, user_data);

	GError * error = NULL;
	g_dbus_connection_emit_signal(conn,
		sender, /* destination */
		"/", /* path */
		"com.canonical.UpstartAppLaunch", /* interface */
		"UnityStartingSignal", /* signal */
		params, /* params, the same */
		&error);

	if (error != NULL) {
		g_warning("Unable to emit response signal: %s", error->message);
		g_error_free(error);
	}

	tracepoint(upstart_app_launch, observer_finish, "starting");
}

gboolean
upstart_app_launch_observer_add_app_starting (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return add_session_generic(observer, user_data, "UnityStartingBroadcast", &starting_array, starting_signal_cb);
}

/* Handle the failed signal when it occurs, call the observer */
static void
failed_signal_cb (GDBusConnection * conn, const gchar * sender, const gchar * object, const gchar * interface, const gchar * signal, GVariant * params, gpointer user_data)
{
	failed_observer_t * observer = (failed_observer_t *)user_data;
	const gchar * appid = NULL;
	const gchar * typestr = NULL;

	tracepoint(upstart_app_launch, observer_start, "failed");

	if (observer->func != NULL) {
		upstart_app_launch_app_failed_t type = UPSTART_APP_LAUNCH_APP_FAILED_CRASH;
		g_variant_get(params, "(&s&s)", &appid, &typestr);

		if (g_strcmp0("crash", typestr) == 0) {
			type = UPSTART_APP_LAUNCH_APP_FAILED_CRASH;
		} else if (g_strcmp0("start-failure", typestr) == 0) {
			type = UPSTART_APP_LAUNCH_APP_FAILED_START_FAILURE;
		} else {
			g_warning("Application failure type '%s' unknown, reporting as a crash", typestr);
		}

		observer->func(appid, type, observer->user_data);
	}

	tracepoint(upstart_app_launch, observer_finish, "failed");
}

gboolean
upstart_app_launch_observer_add_app_failed (upstart_app_launch_app_failed_observer_t observer, gpointer user_data)
{
	GDBusConnection * conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

	if (conn == NULL) {
		return FALSE;
	}

	failed_observer_t * observert = g_new0(failed_observer_t, 1);

	observert->conn = conn;
	observert->func = observer;
	observert->user_data = user_data;

	failed_array = g_list_prepend(failed_array, observert);

	observert->sighandle = g_dbus_connection_signal_subscribe(conn,
		NULL, /* sender */
		"com.canonical.UpstartAppLaunch", /* interface */
		"ApplicationFailed", /* signal */
		"/", /* path */
		NULL, /* arg0 */
		G_DBUS_SIGNAL_FLAGS_NONE,
		failed_signal_cb,
		observert,
		NULL); /* user data destroy */

	return TRUE;
}

static gboolean
delete_app_generic (upstart_app_launch_app_observer_t observer, gpointer user_data, GList ** list)
{
	observer_t * observert = NULL;
	GList * look;

	for (look = *list; look != NULL; look = g_list_next(look)) {
		observert = (observer_t *)look->data;

		if (observert->func == observer && observert->user_data == user_data) {
			break;
		}
	}

	if (look == NULL) {
		return FALSE;
	}

	g_dbus_connection_signal_unsubscribe(observert->conn, observert->sighandle);
	g_object_unref(observert->conn);

	g_free(observert);
	*list = g_list_delete_link(*list, look);

	return TRUE;
}

gboolean
upstart_app_launch_observer_delete_app_started (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return delete_app_generic(observer, user_data, &started_array);
}

gboolean
upstart_app_launch_observer_delete_app_stop (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return delete_app_generic(observer, user_data, &stop_array);
}

gboolean
upstart_app_launch_observer_delete_app_resume (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return delete_app_generic(observer, user_data, &resume_array);
}

gboolean
upstart_app_launch_observer_delete_app_focus (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return delete_app_generic(observer, user_data, &focus_array);
}

gboolean
upstart_app_launch_observer_delete_app_starting (upstart_app_launch_app_observer_t observer, gpointer user_data)
{
	return delete_app_generic(observer, user_data, &starting_array);
}

gboolean
upstart_app_launch_observer_delete_app_failed (upstart_app_launch_app_failed_observer_t observer, gpointer user_data)
{
	failed_observer_t * observert = NULL;
	GList * look;

	for (look = failed_array; look != NULL; look = g_list_next(look)) {
		observert = (failed_observer_t *)look->data;

		if (observert->func == observer && observert->user_data == user_data) {
			break;
		}
	}

	if (look == NULL) {
		return FALSE;
	}

	g_dbus_connection_signal_unsubscribe(observert->conn, observert->sighandle);
	g_object_unref(observert->conn);

	g_free(observert);
	failed_array = g_list_delete_link(failed_array, look);

	return TRUE;
}

typedef void (*per_instance_func_t) (GDBusConnection * con, GVariant * prop_dict, gpointer user_data);

static void
foreach_job_instance (GDBusConnection * con, const gchar * jobname, per_instance_func_t func, gpointer user_data)
{
	const gchar * job_path = get_jobpath(con, jobname);
	if (job_path == NULL)
		return;

	GError * error = NULL;
	GVariant * instance_tuple = g_dbus_connection_call_sync(con,
		DBUS_SERVICE_UPSTART,
		job_path,
		DBUS_INTERFACE_UPSTART_JOB,
		"GetAllInstances",
		NULL,
		G_VARIANT_TYPE("(ao)"),
		G_DBUS_CALL_FLAGS_NONE,
		-1, /* timeout: default */
		NULL, /* cancelable */
		&error);

	if (error != NULL) {
		g_warning("Unable to get instances of job '%s': %s", jobname, error->message);
		g_error_free(error);
		return;
	}

	GVariant * instance_list = g_variant_get_child_value(instance_tuple, 0);
	g_variant_unref(instance_tuple);

	GVariantIter instance_iter;
	g_variant_iter_init(&instance_iter, instance_list);
	const gchar * instance_path = NULL;

	while (g_variant_iter_loop(&instance_iter, "&o", &instance_path)) {
		GVariant * props_tuple = g_dbus_connection_call_sync(con,
			DBUS_SERVICE_UPSTART,
			instance_path,
			"org.freedesktop.DBus.Properties",
			"GetAll",
			g_variant_new("(s)", DBUS_INTERFACE_UPSTART_INSTANCE),
			G_VARIANT_TYPE("(a{sv})"),
			G_DBUS_CALL_FLAGS_NONE,
			-1, /* timeout: default */
			NULL, /* cancelable */
			&error);

		if (error != NULL) {
			g_warning("Unable to name of instance '%s': %s", instance_path, error->message);
			g_error_free(error);
			error = NULL;
			continue;
		}

		GVariant * props_dict = g_variant_get_child_value(props_tuple, 0);

		func(con, props_dict, user_data);

		g_variant_unref(props_dict);
		g_variant_unref(props_tuple);

	}

	g_variant_unref(instance_list);
}

typedef struct {
	GArray * apps;
	gboolean truncate_legacy;
	const gchar * jobname;
} apps_for_job_t;

static void
apps_for_job_instance (GDBusConnection * con, GVariant * props_dict, gpointer user_data)
{
	GVariant * namev = g_variant_lookup_value(props_dict, "name", G_VARIANT_TYPE_STRING);
	if (namev == NULL) {
		return;
	}

	apps_for_job_t * data = (apps_for_job_t *)user_data;
	gchar * instance_name = g_variant_dup_string(namev, NULL);
	g_variant_unref(namev);

	if (data->truncate_legacy && g_strcmp0(data->jobname, "application-legacy") == 0) {
		gchar * last_dash = g_strrstr(instance_name, "-");
		if (last_dash != NULL) {
			last_dash[0] = '\0';
		}
	}

	g_array_append_val(data->apps, instance_name);
}

/* Get all the instances for a given job name */
static void
apps_for_job (GDBusConnection * con, const gchar * jobname, GArray * apps, gboolean truncate_legacy)
{
	apps_for_job_t data = {
		.jobname = jobname,
		.apps = apps,
		.truncate_legacy = truncate_legacy
	};

	foreach_job_instance(con, jobname, apps_for_job_instance, &data);
}

gchar **
upstart_app_launch_list_running_apps (void)
{
	GDBusConnection * con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	g_return_val_if_fail(con != NULL, g_new0(gchar *, 1));

	GArray * apps = g_array_new(TRUE, TRUE, sizeof(gchar *));

	apps_for_job(con, "application-legacy", apps, TRUE);
	apps_for_job(con, "application-click", apps, FALSE);

	g_object_unref(con);

	return (gchar **)g_array_free(apps, FALSE);
}

typedef struct {
	GPid pid;
	const gchar * appid;
	const gchar * jobname;
} pid_for_job_t;

static void
pid_for_job_instance (GDBusConnection * con, GVariant * props_dict, gpointer user_data)
{
	GVariant * namev = g_variant_lookup_value(props_dict, "name", G_VARIANT_TYPE_STRING);
	if (namev == NULL) {
		return;
	}

	pid_for_job_t * data = (pid_for_job_t *)user_data;
	gchar * instance_name = g_variant_dup_string(namev, NULL);
	g_variant_unref(namev);

	if (g_strcmp0(data->jobname, "application-legacy") == 0) {
		gchar * last_dash = g_strrstr(instance_name, "-");
		if (last_dash != NULL) {
			last_dash[0] = '\0';
		}
	}

	if (g_strcmp0(instance_name, data->appid) == 0) {
		GVariant * processv = g_variant_lookup_value(props_dict, "processes", G_VARIANT_TYPE("a(si)"));

		if (processv != NULL) {
			if (g_variant_n_children(processv) > 0) {
				GVariant * first_entry = g_variant_get_child_value(processv, 0);
				GVariant * pidv = g_variant_get_child_value(first_entry, 1);

				data->pid = g_variant_get_int32(pidv);

				g_variant_unref(pidv);
				g_variant_unref(first_entry);
			}

			g_variant_unref(processv);
		}
	}

	g_free(instance_name);
}

/* Look for the app for a job */
static GPid
pid_for_job (GDBusConnection * con, const gchar * jobname, const gchar * appid)
{
	pid_for_job_t data = {
		.jobname = jobname,
		.appid = appid,
		.pid = 0
	};

	foreach_job_instance(con, jobname, pid_for_job_instance, &data);

	return data.pid;
}

GPid
upstart_app_launch_get_primary_pid (const gchar * appid)
{
	g_return_val_if_fail(appid != NULL, 0);

	GDBusConnection * con = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	g_return_val_if_fail(con != NULL, 0);

	GPid pid = 0;

	if (pid == 0) {
		pid = pid_for_job(con, "application-legacy", appid);
	}

	if (pid == 0) {
		pid = pid_for_job(con, "application-click", appid);
	}

	g_object_unref(con);

	return pid;
}

gboolean
upstart_app_launch_pid_in_app_id (GPid pid, const gchar * appid)
{
	g_return_val_if_fail(appid != NULL, FALSE);

	if (pid == 0) {
		return FALSE;
	}

	GPid primary = upstart_app_launch_get_primary_pid(appid);

	return primary == pid;
}

/* Try and get a manifest file and do a couple sanity checks on it */
static JsonParser *
get_manifest_file (const gchar * pkg)
{
	/* Get the directory from click */
	GError * error = NULL;
	const gchar * click_exec = NULL;

	if (g_getenv("UAL_CLICK_EXEC") != NULL) {
		click_exec = g_getenv("UAL_CLICK_EXEC");
	} else {
		click_exec = "click";
	}

	gchar * cmdline = g_strdup_printf("%s info \"%s\"",
		click_exec, pkg);

	gchar * output = NULL;
	g_spawn_command_line_sync(cmdline, &output, NULL, NULL, &error);
	g_free(cmdline);

	if (error != NULL) {
		g_warning("Unable to get manifest for '%s' package: %s", pkg, error->message);
		g_error_free(error);
		g_free(output);
		return NULL;
	}

	/* Let's look at that manifest file */
	JsonParser * parser = json_parser_new();
	json_parser_load_from_data(parser, output, -1, &error);
	g_free(output);

	if (error != NULL) {
		g_warning("Unable to load manifest for '%s': %s", pkg, error->message);
		g_error_free(error);
		g_object_unref(parser);
		return NULL;
	}
	JsonNode * root = json_parser_get_root(parser);
	if (json_node_get_node_type(root) != JSON_NODE_OBJECT) {
		g_warning("Manifest file for package '%s' does not have an object as its root node", pkg);
		g_object_unref(parser);
		return NULL;
	}

	JsonObject * rootobj = json_node_get_object(root);
	if (!json_object_has_member(rootobj, "version")) {
		g_warning("Manifest file for package '%s' does not have a version", pkg);
		g_object_unref(parser);
		return NULL;
	}

	return parser;
}

/* Types of search we can do for an app name */
typedef enum _app_name_t app_name_t;
enum _app_name_t {
	APP_NAME_ONLY,
	APP_NAME_FIRST,
	APP_NAME_LAST
};

/* Figure out the app name if it's one of the keywords */
static const gchar *
manifest_app_name (JsonParser ** manifest, const gchar * pkg, const gchar * original_app)
{
	app_name_t app_type = APP_NAME_FIRST;

	if (original_app == NULL) {
		/* first */
	} else if (g_strcmp0(original_app, "first-listed-app") == 0) {
		/* first */
	} else if (g_strcmp0(original_app, "last-listed-app") == 0) {
		app_type = APP_NAME_LAST;
	} else if (g_strcmp0(original_app, "only-listed-app") == 0) {
		app_type = APP_NAME_ONLY;
	} else {
		return original_app;
	}

	if (*manifest == NULL) {
		*manifest = get_manifest_file(pkg);
	}

	JsonNode * root_node = json_parser_get_root(*manifest);
	JsonObject * root_obj = json_node_get_object(root_node);
	JsonObject * hooks = json_object_get_object_member(root_obj, "hooks");

	if (hooks == NULL) {
		return NULL;
	}

	GList * apps = json_object_get_members(hooks);
	if (apps == NULL) {
		return NULL;
	}

	const gchar * retapp = NULL;

	switch (app_type) {
	case APP_NAME_ONLY:
		if (g_list_length(apps) == 1) {
			retapp = (const gchar *)apps->data;
		}
		break;
	case APP_NAME_FIRST:
		retapp = (const gchar *)apps->data;
		break;
	case APP_NAME_LAST:
		retapp = (const gchar *)(g_list_last(apps)->data);
		break;
	default:
		break;
	}

	g_list_free(apps);

	return retapp;
}

/* Figure out the app version using the manifest */
static const gchar *
manifest_version (JsonParser ** manifest, const gchar * pkg, const gchar * original_ver)
{
	if (original_ver != NULL && g_strcmp0(original_ver, "current-user-version") != 0) {
		return original_ver;
	} else  {
		if (*manifest == NULL) {
			*manifest = get_manifest_file(pkg);
		}
		g_return_val_if_fail(*manifest != NULL, NULL);

		JsonNode * node = json_parser_get_root(*manifest);
		JsonObject * obj = json_node_get_object(node);

		return g_strdup(json_object_get_string_member(obj, "version"));
	}

	return NULL;
}

gchar *
upstart_app_launch_triplet_to_app_id (const gchar * pkg, const gchar * app, const gchar * ver)
{
	g_return_val_if_fail(pkg != NULL, NULL);

	const gchar * version = NULL;
	const gchar * application = NULL;
	JsonParser * manifest = NULL;

	version = manifest_version(&manifest, pkg, ver);
	g_return_val_if_fail(version != NULL, NULL);

	application = manifest_app_name(&manifest, pkg, app);
	g_return_val_if_fail(application != NULL, NULL);

	gchar * retval = g_strdup_printf("%s_%s_%s", pkg, application, version);

	/* The parser may hold allocation for some of our strings used above */
	g_clear_object(&manifest);

	return retval;
}
