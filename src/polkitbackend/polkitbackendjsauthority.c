/*
 * Copyright (C) 2008-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include "config.h"
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <netdb.h>
#include <string.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <glib/gi18n-lib.h>

#include <polkit/polkit.h>
#include "polkitbackendjsauthority.h"

#include <polkit/polkitprivate.h>

#include <systemd/sd-login.h>

#include <jsapi.h>

#include "initjs.h" /* init.js */

/**
 * SECTION:polkitbackendjsauthority
 * @title: PolkitBackendJsAuthority
 * @short_description: JS Authority
 * @stability: Unstable
 *
 * An implementation of #PolkitBackendAuthority that reads and
 * evalates Javascript files and supports interaction with
 * authentication agents (virtue of being based on
 * #PolkitBackendInteractiveAuthority).
 */

/* ---------------------------------------------------------------------------------------------------- */

struct _PolkitBackendJsAuthorityPrivate
{
  gchar **rules_dirs;
  GFileMonitor **dir_monitors; /* NULL-terminated array of GFileMonitor instances */

  JSRuntime *rt;
  JSContext *cx;
  JSObject *js_global;
  JSObject *js_polkit;

  /* A list of JSObject instances */
  GList *scripts;
};

static void utils_spawn (const gchar *const  *argv,
                         guint                timeout_seconds,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data);

gboolean utils_spawn_finish (GAsyncResult   *res,
                             gint           *out_exit_status,
                             gchar         **out_standard_output,
                             gchar         **out_standard_error,
                             GError        **error);

static void on_dir_monitor_changed (GFileMonitor     *monitor,
                                    GFile            *file,
                                    GFile            *other_file,
                                    GFileMonitorEvent event_type,
                                    gpointer          user_data);

/* ---------------------------------------------------------------------------------------------------- */

enum
{
  PROP_0,
  PROP_RULES_DIRS,
};

/* ---------------------------------------------------------------------------------------------------- */

static GList *polkit_backend_js_authority_get_admin_auth_identities (PolkitBackendInteractiveAuthority *authority,
                                                                     PolkitSubject                     *caller,
                                                                     PolkitSubject                     *subject,
                                                                     PolkitIdentity                    *user_for_subject,
                                                                     const gchar                       *action_id,
                                                                     PolkitDetails                     *details);

static PolkitImplicitAuthorization polkit_backend_js_authority_check_authorization_sync (
                                                          PolkitBackendInteractiveAuthority *authority,
                                                          PolkitSubject                     *caller,
                                                          PolkitSubject                     *subject,
                                                          PolkitIdentity                    *user_for_subject,
                                                          gboolean                           subject_is_local,
                                                          gboolean                           subject_is_active,
                                                          const gchar                       *action_id,
                                                          PolkitDetails                     *details,
                                                          PolkitImplicitAuthorization        implicit);

G_DEFINE_TYPE_WITH_CODE (PolkitBackendJsAuthority,
                         polkit_backend_js_authority,
                         POLKIT_BACKEND_TYPE_INTERACTIVE_AUTHORITY,
                         g_io_extension_point_implement (POLKIT_BACKEND_AUTHORITY_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "js-authority" PACKAGE_VERSION,
                                                         10));

/* ---------------------------------------------------------------------------------------------------- */

static JSClass js_global_class = {
  "global",
  JSCLASS_GLOBAL_FLAGS,
  JS_PropertyStub,
  JS_PropertyStub,
  JS_PropertyStub,
  JS_StrictPropertyStub,
  JS_EnumerateStub,
  JS_ResolveStub,
  JS_ConvertStub,
  JS_FinalizeStub,
  JSCLASS_NO_OPTIONAL_MEMBERS
};

/* ---------------------------------------------------------------------------------------------------- */

static JSClass js_polkit_class = {
  "Polkit",
  0,
  JS_PropertyStub,
  JS_PropertyStub,
  JS_PropertyStub,
  JS_StrictPropertyStub,
  JS_EnumerateStub,
  JS_ResolveStub,
  JS_ConvertStub,
  JS_FinalizeStub,
  JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSBool js_polkit_log (JSContext *cx, uintN argc, jsval *vp);
static JSBool js_polkit_spawn (JSContext *cx, uintN argc, jsval *vp);
static JSBool js_polkit_user_is_in_netgroup (JSContext *cx, uintN argc, jsval *vp);

static JSFunctionSpec js_polkit_functions[] =
{
  JS_FS("log",            js_polkit_log,            0, 0),
  JS_FS("spawn",          js_polkit_spawn,          0, 0),
  JS_FS("_userIsInNetGroup", js_polkit_user_is_in_netgroup,          0, 0),
  JS_FS_END
};

/* ---------------------------------------------------------------------------------------------------- */

static void report_error (JSContext     *cx,
                          const char    *message,
                          JSErrorReport *report)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (JS_GetContextPrivate (cx));
  polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                "%s:%u: %s",
                                report->filename ? report->filename : "<no filename>",
                                (unsigned int) report->lineno,
                                message);
}

static void
polkit_backend_js_authority_init (PolkitBackendJsAuthority *authority)
{
  authority->priv = G_TYPE_INSTANCE_GET_PRIVATE (authority,
                                                 POLKIT_BACKEND_TYPE_JS_AUTHORITY,
                                                 PolkitBackendJsAuthorityPrivate);
}

static gint
rules_file_name_cmp (const gchar *a,
                     const gchar *b)
{
  gint ret;
  const gchar *a_base;
  const gchar *b_base;

  a_base = strrchr (a, '/');
  b_base = strrchr (b, '/');

  g_assert (a_base != NULL);
  g_assert (b_base != NULL);
  a_base += 1;
  b_base += 1;

  ret = g_strcmp0 (a_base, b_base);
  if (ret == 0)
    {
      /* /etc wins over /usr */
      ret = g_strcmp0 (a, b);
      g_assert (ret != 0);
    }

  return ret;
}

static void
load_scripts (PolkitBackendJsAuthority  *authority)
{
  GList *files = NULL;
  GList *l;
  guint num_scripts = 0;
  GError *error = NULL;
  guint n;

  files = NULL;

  for (n = 0; authority->priv->rules_dirs != NULL && authority->priv->rules_dirs[n] != NULL; n++)
    {
      const gchar *dir_name = authority->priv->rules_dirs[n];
      GDir *dir = NULL;

      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Loading scripts from directory %s",
                                    dir_name);

      dir = g_dir_open (dir_name,
                        0,
                        &error);
      if (dir == NULL)
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Error opening rules directory: %s (%s, %d)",
                                        error->message, g_quark_to_string (error->domain), error->code);
          g_clear_error (&error);
        }
      else
        {
          const gchar *name;
          while ((name = g_dir_read_name (dir)) != NULL)
            {
              if (g_str_has_suffix (name, ".rules"))
                files = g_list_prepend (files, g_strdup_printf ("%s/%s", dir_name, name));
            }
          g_dir_close (dir);
        }
    }

  files = g_list_sort (files, (GCompareFunc) rules_file_name_cmp);

  for (l = files; l != NULL; l = l->next)
    {
      const gchar *filename = l->data;
      JSObject *script;

      script = JS_CompileFile (authority->priv->cx,
                               authority->priv->js_global,
                               filename);
      if (script == NULL)
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Error compiling script %s",
                                        filename);
          continue;
        }

      /* evaluate the script */
      jsval rval;
      if (!JS_ExecuteScript (authority->priv->cx,
                             authority->priv->js_global,
                             script,
                             &rval))
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Error executing script %s",
                                        filename);
          continue;
        }

      //g_print ("Successfully loaded and evaluated script `%s'\n", filename);

      num_scripts++;
    }

  polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                "Finished loading, compiling and executing %d scripts",
                                num_scripts);
  g_list_free_full (files, g_free);
}

static void
reload_scripts (PolkitBackendJsAuthority *authority)
{
  jsval argv[1] = {0};
  jsval rval = {0};

  if (!JS_CallFunctionName(authority->priv->cx,
                           authority->priv->js_polkit,
                           "_deleteRules",
                           0,
                           argv,
                           &rval))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error deleting old rules, not loading new ones");
      goto out;
    }

  polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                "Collecting garbage unconditionally...");
  JS_GC (authority->priv->cx);

  load_scripts (authority);

  /* Let applications know we have new rules... */
  g_signal_emit_by_name (authority, "changed");
 out:
  ;
}

static void
on_dir_monitor_changed (GFileMonitor     *monitor,
                        GFile            *file,
                        GFile            *other_file,
                        GFileMonitorEvent event_type,
                        gpointer          user_data)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (user_data);

  /* TODO: maybe rate-limit so storms of events are collapsed into one with a 500ms resolution?
   *       Because when editing a file with emacs we get 4-8 events..
   */

  if (file != NULL)
    {
      gchar *name;

      name = g_file_get_basename (file);

      /* g_print ("event_type=%d file=%p name=%s\n", event_type, file, name); */
      if (!g_str_has_prefix (name, ".") &&
          !g_str_has_prefix (name, "#") &&
          g_str_has_suffix (name, ".rules") &&
          (event_type == G_FILE_MONITOR_EVENT_CREATED ||
           event_type == G_FILE_MONITOR_EVENT_DELETED ||
           event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT))
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Reloading scripts");
          reload_scripts (authority);
        }
      g_free (name);
    }
}


static void
setup_file_monitors (PolkitBackendJsAuthority *authority)
{
  guint n;
  GPtrArray *p;

  p = g_ptr_array_new ();
  for (n = 0; authority->priv->rules_dirs != NULL && authority->priv->rules_dirs[n] != NULL; n++)
    {
      GFile *file;
      GError *error;
      GFileMonitor *monitor;

      file = g_file_new_for_path (authority->priv->rules_dirs[n]);
      error = NULL;
      monitor = g_file_monitor_directory (file,
                                          G_FILE_MONITOR_NONE,
                                          NULL,
                                          &error);
      if (monitor == NULL)
        {
          g_warning ("Error monitoring directory %s: %s",
                     authority->priv->rules_dirs[n],
                     error->message);
          g_clear_error (&error);
        }
      else
        {
          g_signal_connect (monitor,
                            "changed",
                            G_CALLBACK (on_dir_monitor_changed),
                            authority);
          g_ptr_array_add (p, monitor);
        }
    }
  g_ptr_array_add (p, NULL);
  authority->priv->dir_monitors = (GFileMonitor**) g_ptr_array_free (p, FALSE);
}

static void
polkit_backend_js_authority_constructed (GObject *object)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (object);

  authority->priv->rt = JS_NewRuntime (8L * 1024L * 1024L);
  if (authority->priv->rt == NULL)
    goto fail;

  authority->priv->cx = JS_NewContext (authority->priv->rt, 8192);
  if (authority->priv->cx == NULL)
    goto fail;

  JS_SetOptions (authority->priv->cx,
                 JSOPTION_VAROBJFIX |
                 JSOPTION_JIT |
                 JSOPTION_METHODJIT);
  JS_SetVersion(authority->priv->cx, JSVERSION_LATEST);
  JS_SetErrorReporter(authority->priv->cx, report_error);
  JS_SetContextPrivate (authority->priv->cx, authority);

  authority->priv->js_global = JS_NewCompartmentAndGlobalObject (authority->priv->cx,
                                                                 &js_global_class,
                                                                 NULL);
  if (authority->priv->js_global == NULL)
    goto fail;

  if (!JS_InitStandardClasses (authority->priv->cx, authority->priv->js_global))
    goto fail;

  authority->priv->js_polkit = JS_DefineObject(authority->priv->cx,
                                               authority->priv->js_global,
                                               "polkit",
                                               &js_polkit_class,
                                               NULL,
                                               JSPROP_ENUMERATE);
  if (authority->priv->js_polkit == NULL)
    goto fail;

  if (!JS_DefineFunctions (authority->priv->cx,
                           authority->priv->js_polkit,
                           js_polkit_functions))
    goto fail;

  if (!JS_EvaluateScript (authority->priv->cx,
                          authority->priv->js_global,
                          init_js, strlen (init_js), /* init.js */
                          "init.js",  /* filename */
                          0,     /* lineno */
                          NULL)) /* rval */
    {
      goto fail;
    }

  if (authority->priv->rules_dirs == NULL)
    {
      authority->priv->rules_dirs = g_new0 (gchar *, 3);
      authority->priv->rules_dirs[0] = g_strdup (PACKAGE_SYSCONF_DIR "/polkit-1/rules.d");
      authority->priv->rules_dirs[1] = g_strdup (PACKAGE_DATA_DIR "/polkit-1/rules.d");
    }

  setup_file_monitors (authority);
  load_scripts (authority);

  G_OBJECT_CLASS (polkit_backend_js_authority_parent_class)->constructed (object);
  return;

 fail:
  g_critical ("Error initializing JavaScript environment");
  g_assert_not_reached ();
}

static void
polkit_backend_js_authority_finalize (GObject *object)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (object);
  guint n;

  for (n = 0; authority->priv->dir_monitors != NULL && authority->priv->dir_monitors[n] != NULL; n++)
    {
      GFileMonitor *monitor = authority->priv->dir_monitors[n];
      g_signal_handlers_disconnect_by_func (monitor,
                                            G_CALLBACK (on_dir_monitor_changed),
                                            authority);
      g_object_unref (monitor);
    }
  g_free (authority->priv->dir_monitors);
  g_strfreev (authority->priv->rules_dirs);

  JS_DestroyContext (authority->priv->cx);
  JS_DestroyRuntime (authority->priv->rt);
  /* JS_ShutDown (); */

  G_OBJECT_CLASS (polkit_backend_js_authority_parent_class)->finalize (object);
}

static void
polkit_backend_js_authority_set_property (GObject      *object,
                                          guint         property_id,
                                          const GValue *value,
                                          GParamSpec   *pspec)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (object);

  switch (property_id)
    {
      case PROP_RULES_DIRS:
        g_assert (authority->priv->rules_dirs == NULL);
        authority->priv->rules_dirs = (gchar **) g_value_dup_boxed (value);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static const gchar *
polkit_backend_js_authority_get_name (PolkitBackendAuthority *authority)
{
  return "js";
}

static const gchar *
polkit_backend_js_authority_get_version (PolkitBackendAuthority *authority)
{
  return PACKAGE_VERSION;
}

static PolkitAuthorityFeatures
polkit_backend_js_authority_get_features (PolkitBackendAuthority *authority)
{
  return POLKIT_AUTHORITY_FEATURES_TEMPORARY_AUTHORIZATION;
}

static void
polkit_backend_js_authority_class_init (PolkitBackendJsAuthorityClass *klass)
{
  GObjectClass *gobject_class;
  PolkitBackendAuthorityClass *authority_class;
  PolkitBackendInteractiveAuthorityClass *interactive_authority_class;


  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize                               = polkit_backend_js_authority_finalize;
  gobject_class->set_property                           = polkit_backend_js_authority_set_property;
  gobject_class->constructed                            = polkit_backend_js_authority_constructed;

  authority_class = POLKIT_BACKEND_AUTHORITY_CLASS (klass);
  authority_class->get_name                             = polkit_backend_js_authority_get_name;
  authority_class->get_version                          = polkit_backend_js_authority_get_version;
  authority_class->get_features                         = polkit_backend_js_authority_get_features;

  interactive_authority_class = POLKIT_BACKEND_INTERACTIVE_AUTHORITY_CLASS (klass);
  interactive_authority_class->get_admin_identities     = polkit_backend_js_authority_get_admin_auth_identities;
  interactive_authority_class->check_authorization_sync = polkit_backend_js_authority_check_authorization_sync;

  g_object_class_install_property (gobject_class,
                                   PROP_RULES_DIRS,
                                   g_param_spec_boxed ("rules-dirs",
                                                       NULL,
                                                       NULL,
                                                       G_TYPE_STRV,
                                                       G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));


  g_type_class_add_private (klass, sizeof (PolkitBackendJsAuthorityPrivate));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
set_property_str (PolkitBackendJsAuthority  *authority,
                  JSObject                  *obj,
                  const gchar               *name,
                  const gchar               *value)
{
  JSString *value_jsstr;
  jsval value_jsval;
  value_jsstr = JS_NewStringCopyZ (authority->priv->cx, value);
  value_jsval = STRING_TO_JSVAL (value_jsstr);
  JS_SetProperty (authority->priv->cx, obj, name, &value_jsval);
}

static void
set_property_strv (PolkitBackendJsAuthority  *authority,
                   JSObject                  *obj,
                   const gchar               *name,
                   const gchar *const        *value,
                   gssize                     len)
{
  jsval value_jsval;
  JSObject *array_object;
  jsval *jsvals;
  guint n;

  if (len < 0)
    len = g_strv_length ((gchar **) value);

  jsvals = g_new0 (jsval, len);
  for (n = 0; n < len; n++)
    {
      JSString *jsstr;
      jsstr = JS_NewStringCopyZ (authority->priv->cx, value[n]);
      jsvals[n] = STRING_TO_JSVAL (jsstr);
    }

  array_object = JS_NewArrayObject (authority->priv->cx, (jsint) len, jsvals);

  value_jsval = OBJECT_TO_JSVAL (array_object);
  JS_SetProperty (authority->priv->cx, obj, name, &value_jsval);

  g_free (jsvals);
}


static void
set_property_int32 (PolkitBackendJsAuthority  *authority,
                    JSObject                  *obj,
                    const gchar               *name,
                    gint32                     value)
{
  jsval value_jsval;
  value_jsval = INT_TO_JSVAL ((int32) value);
  JS_SetProperty (authority->priv->cx, obj, name, &value_jsval);
}

static void
set_property_bool (PolkitBackendJsAuthority  *authority,
                   JSObject                  *obj,
                   const gchar               *name,
                   gboolean                   value)
{
  jsval value_jsval;
  value_jsval = BOOLEAN_TO_JSVAL ((JSBool) value);
  JS_SetProperty (authority->priv->cx, obj, name, &value_jsval);
}


static gboolean
subject_to_jsval (PolkitBackendJsAuthority  *authority,
                  PolkitSubject             *subject,
                  PolkitIdentity            *user_for_subject,
                  jsval                     *out_jsval,
                  GError                   **error)
{
  gboolean ret = FALSE;
  jsval ret_jsval;
  const char *src;
  JSObject *obj;
  pid_t pid;
  uid_t uid;
  gchar *user_name = NULL;
  GPtrArray *groups = NULL;
  struct passwd *passwd;
  char *seat_str = NULL;
  char *session_str = NULL;
  gboolean is_local = FALSE;
  gboolean is_active = FALSE;

  src = "new Subject();";

  if (!JS_EvaluateScript (authority->priv->cx,
                          authority->priv->js_global,
                          src, strlen (src),
                          __FILE__, __LINE__,
                          &ret_jsval))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Evaluting '%s' failed", src);
      goto out;
    }

  obj = JSVAL_TO_OBJECT (ret_jsval);

  if (POLKIT_IS_UNIX_PROCESS (subject))
    {
      pid = polkit_unix_process_get_pid (POLKIT_UNIX_PROCESS (subject));
    }
  else if (POLKIT_IS_SYSTEM_BUS_NAME (subject))
    {
      PolkitSubject *process;
      process = polkit_system_bus_name_get_process_sync (POLKIT_SYSTEM_BUS_NAME (subject), NULL, error);
      if (process == NULL)
        goto out;
      pid = polkit_unix_process_get_pid (POLKIT_UNIX_PROCESS (process));
      g_object_unref (process);
    }
  else
    {
      g_assert_not_reached ();
    }

  if (sd_pid_get_session (pid, &session_str) == 0)
    {
      if (sd_session_get_seat (session_str, &seat_str) == 0)
        is_local = TRUE;
      if (sd_session_is_active (session_str))
        is_active = TRUE;
    }

  g_assert (POLKIT_IS_UNIX_USER (user_for_subject));
  uid = polkit_unix_user_get_uid (POLKIT_UNIX_USER (user_for_subject));

  groups = g_ptr_array_new_with_free_func (g_free);

  passwd = getpwuid (uid);
  if (passwd == NULL)
    {
      user_name = g_strdup_printf ("%d", (gint) uid);
      g_warning ("Error looking up info for uid %d: %m", (gint) uid);
    }
  else
    {
      gid_t gids[512];
      int num_gids = 512;

      user_name = g_strdup (passwd->pw_name);

      if (getgrouplist (passwd->pw_name,
                        passwd->pw_gid,
                        gids,
                        &num_gids) < 0)
        {
          g_warning ("Error looking up groups for uid %d: %m", (gint) uid);
        }
      else
        {
          gint n;
          for (n = 0; n < num_gids; n++)
            {
              struct group *group;
              group = getgrgid (gids[n]);
              if (group == NULL)
                {
                  g_ptr_array_add (groups, g_strdup_printf ("%d", (gint) gids[n]));
                }
              else
                {
                  g_ptr_array_add (groups, g_strdup (group->gr_name));
                }
            }
        }
    }

  g_ptr_array_add (groups, NULL);

  set_property_int32 (authority, obj, "pid", pid);
  set_property_str (authority, obj, "user", user_name);
  set_property_strv (authority, obj, "groups", (const gchar* const *) groups->pdata, groups->len);
  set_property_str (authority, obj, "seat", seat_str);
  set_property_str (authority, obj, "session", session_str);
  set_property_bool (authority, obj, "local", is_local);
  set_property_bool (authority, obj, "active", is_active);

  ret = TRUE;

 out:
  free (session_str);
  free (seat_str);
  g_free (user_name);
  if (groups != NULL)
    g_ptr_array_unref (groups);

  if (ret && out_jsval != NULL)
    *out_jsval = ret_jsval;

  return ret;
}

static gboolean
details_to_jsval (PolkitBackendJsAuthority  *authority,
                  PolkitDetails             *details,
                  jsval                     *out_jsval,
                  GError                   **error)
{
  gboolean ret = FALSE;
  jsval ret_jsval;
  const char *src;
  JSObject *obj;
  gchar **keys;
  guint n;

  src = "new Details();";

  if (!JS_EvaluateScript (authority->priv->cx,
                          authority->priv->js_global,
                          src, strlen (src),
                          __FILE__, __LINE__,
                          &ret_jsval))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Evaluting '%s' failed", src);
      goto out;
    }

  obj = JSVAL_TO_OBJECT (ret_jsval);
  keys = polkit_details_get_keys (details);
  for (n = 0; keys != NULL && keys[n] != NULL; n++)
    {
      const gchar *key = keys[n];
      JSString *value_jsstr;
      jsval value_jsval;
      const gchar *value;

      value = polkit_details_lookup (details, keys[n]);
      value_jsstr = JS_NewStringCopyZ (authority->priv->cx, value);
      value_jsval = STRING_TO_JSVAL (value_jsstr);
      JS_SetProperty (authority->priv->cx, obj, key, &value_jsval);
    }
  g_free (keys);

  ret = TRUE;

 out:
  if (ret && out_jsval != NULL)
    *out_jsval = ret_jsval;

  return ret;
}

static GList *
polkit_backend_js_authority_get_admin_auth_identities (PolkitBackendInteractiveAuthority *_authority,
                                                       PolkitSubject                     *caller,
                                                       PolkitSubject                     *subject,
                                                       PolkitIdentity                    *user_for_subject,
                                                       const gchar                       *action_id,
                                                       PolkitDetails                     *details)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (_authority);
  GList *ret = NULL;
  jsval argv[3] = {0};
  jsval rval = {0};
  JSString *action_id_jstr;
  guint n;
  GError *error = NULL;
  JSString *ret_jsstr;
  gchar *ret_str = NULL;
  gchar **ret_strs = NULL;

  action_id_jstr = JS_NewStringCopyZ (authority->priv->cx, action_id);
  argv[0] = STRING_TO_JSVAL (action_id_jstr);

  if (!subject_to_jsval (authority, subject, user_for_subject, &argv[1], &error))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error converting subject to JS object: %s",
                                    error->message);
      g_clear_error (&error);
      goto out;
    }

  if (!details_to_jsval (authority, details, &argv[2], &error))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error converting details to JS object: %s",
                                    error->message);
      g_clear_error (&error);
      goto out;
    }

  if (!JS_CallFunctionName(authority->priv->cx,
                           authority->priv->js_polkit,
                           "_runAdminRules",
                           3,
                           argv,
                           &rval))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error evaluating admin rules");
      goto out;
    }

  if (!JSVAL_IS_STRING (rval) && !JSVAL_IS_NULL (rval))
    {
      g_warning ("Expected a string");
      goto out;
    }

  ret_jsstr = JSVAL_TO_STRING (rval);
  ret_str = g_utf16_to_utf8 (JS_GetStringCharsZ (authority->priv->cx, ret_jsstr), -1, NULL, NULL, NULL);
  if (ret_str == NULL)
    {
      g_warning ("Error converting resulting string to UTF-8: %s", error->message);
      goto out;
    }

  ret_strs = g_strsplit (ret_str, ",", -1);
  for (n = 0; ret_strs != NULL && ret_strs[n] != NULL; n++)
    {
      const gchar *identity_str = ret_strs[n];
      PolkitIdentity *identity;

      error = NULL;
      identity = polkit_identity_from_string (identity_str, &error);
      if (identity == NULL)
        {
          polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                        "Identity `%s' is not valid, ignoring",
                                        identity_str);
        }
      else
        {
          ret = g_list_prepend (ret, identity);
        }
    }
  ret = g_list_reverse (ret);

 out:
  g_strfreev (ret_strs);
  g_free (ret_str);
  /* fallback to root password auth */
  if (ret == NULL)
    ret = g_list_prepend (ret, polkit_unix_user_new (0));

  JS_MaybeGC (authority->priv->cx);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static PolkitImplicitAuthorization
polkit_backend_js_authority_check_authorization_sync (PolkitBackendInteractiveAuthority *_authority,
                                                      PolkitSubject                     *caller,
                                                      PolkitSubject                     *subject,
                                                      PolkitIdentity                    *user_for_subject,
                                                      gboolean                           subject_is_local,
                                                      gboolean                           subject_is_active,
                                                      const gchar                       *action_id,
                                                      PolkitDetails                     *details,
                                                      PolkitImplicitAuthorization        implicit)
{
  PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (_authority);
  PolkitImplicitAuthorization ret = implicit;
  jsval argv[3] = {0};
  jsval rval = {0};
  JSString *action_id_jstr;
  GError *error = NULL;
  JSString *ret_jsstr;
  const jschar *ret_utf16;
  gchar *ret_str = NULL;
  gboolean good = FALSE;
  JSIdArray *ids;
  JSObject *details_obj;
  gint n;

  action_id_jstr = JS_NewStringCopyZ (authority->priv->cx, action_id);
  argv[0] = STRING_TO_JSVAL (action_id_jstr);

  if (!subject_to_jsval (authority, subject, user_for_subject, &argv[1], &error))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error converting subject to JS object: %s",
                                    error->message);
      g_clear_error (&error);
      goto out;
    }

  if (!details_to_jsval (authority, details, &argv[2], &error))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error converting details to JS object: %s",
                                    error->message);
      g_clear_error (&error);
      goto out;
    }

  if (!JS_CallFunctionName(authority->priv->cx,
                           authority->priv->js_polkit,
                           "_runRules",
                           3,
                           argv,
                           &rval))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Error evaluating authorization rules");
      goto out;
    }

  if (!JSVAL_IS_STRING (rval) && !JSVAL_IS_NULL (rval))
    {
      g_warning ("Expected a string");
      goto out;
    }

  ret_jsstr = JSVAL_TO_STRING (rval);
  if (ret_jsstr == NULL)
    {
      /* this fine, means there was no match, use implicit authorizations */
      good = TRUE;
      goto out;
    }

  ret_utf16 = JS_GetStringCharsZ (authority->priv->cx, ret_jsstr);
  ret_str = g_utf16_to_utf8 (ret_utf16, -1, NULL, NULL, &error);
  if (ret_str == NULL)
    {
      g_warning ("Error converting resulting string to UTF-8: %s", error->message);
      g_clear_error (&error);
      goto out;
    }

  g_strstrip (ret_str);
  if (!polkit_implicit_authorization_from_string (ret_str, &ret))
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Returned result `%s' is not valid",
                                    ret_str);
      goto out;
    }


  /* the JS code may have modifed @details - update PolkitDetails
   * object accordingly
   */
  details_obj = JSVAL_TO_OBJECT (argv[2]);
  ids = JS_Enumerate (authority->priv->cx, details_obj);
  if (ids == NULL)
    {
      polkit_backend_authority_log (POLKIT_BACKEND_AUTHORITY (authority),
                                    "Failed to enumerate properties of Details object");
      goto out;
    }
  for (n = 0; n < ids->length; n++)
    {
      jsval id_val;
      jsval value_val;
      char *id_s = NULL;
      char *value_s = NULL;

      if (!JS_IdToValue (authority->priv->cx, ids->vector[n], &id_val))
        {
          g_warning ("Error getting string for property id %d", n);
          goto cont;
        }
      id_s = JS_EncodeString (authority->priv->cx, JSVAL_TO_STRING (id_val));

      if (!JS_GetPropertyById (authority->priv->cx, details_obj, ids->vector[n], &value_val))
        {
          g_warning ("Error getting value string for property value %s", id_s);
          goto cont;
        }

      /* skip e.g. functions */
      if (!JSVAL_IS_STRING (value_val) && !JSVAL_IS_NULL (value_val))
        goto cont;

      value_s = JS_EncodeString (authority->priv->cx, JSVAL_TO_STRING (value_val));

      polkit_details_insert (details, id_s, value_s);
    cont:
      if (id_s != NULL)
        JS_free (authority->priv->cx, id_s);
      if (value_s != NULL)
        JS_free (authority->priv->cx, value_s);
    }
  JS_DestroyIdArray (authority->priv->cx, ids);

  good = TRUE;

 out:
  if (!good)
    ret = POLKIT_IMPLICIT_AUTHORIZATION_NOT_AUTHORIZED;
  g_free (ret_str);

  JS_MaybeGC (authority->priv->cx);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static JSBool
js_polkit_log (JSContext  *cx,
               uintN       argc,
               jsval      *vp)
{
  /* PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (JS_GetContextPrivate (cx)); */
  JSBool ret = JS_FALSE;
  JSString *str;
  char *s;

  if (!JS_ConvertArguments (cx, argc, JS_ARGV (cx, vp), "S", &str))
    goto out;

  s = JS_EncodeString (cx, str);
  JS_ReportWarning (cx, s);
  JS_free (cx, s);

  ret = JS_TRUE;

  JS_SET_RVAL (cx, vp, JSVAL_VOID);  /* return undefined */
 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
get_signal_name (gint signal_number)
{
  switch (signal_number)
    {
#define _HANDLE_SIG(sig) case sig: return #sig;
    _HANDLE_SIG (SIGHUP);
    _HANDLE_SIG (SIGINT);
    _HANDLE_SIG (SIGQUIT);
    _HANDLE_SIG (SIGILL);
    _HANDLE_SIG (SIGABRT);
    _HANDLE_SIG (SIGFPE);
    _HANDLE_SIG (SIGKILL);
    _HANDLE_SIG (SIGSEGV);
    _HANDLE_SIG (SIGPIPE);
    _HANDLE_SIG (SIGALRM);
    _HANDLE_SIG (SIGTERM);
    _HANDLE_SIG (SIGUSR1);
    _HANDLE_SIG (SIGUSR2);
    _HANDLE_SIG (SIGCHLD);
    _HANDLE_SIG (SIGCONT);
    _HANDLE_SIG (SIGSTOP);
    _HANDLE_SIG (SIGTSTP);
    _HANDLE_SIG (SIGTTIN);
    _HANDLE_SIG (SIGTTOU);
    _HANDLE_SIG (SIGBUS);
    _HANDLE_SIG (SIGPOLL);
    _HANDLE_SIG (SIGPROF);
    _HANDLE_SIG (SIGSYS);
    _HANDLE_SIG (SIGTRAP);
    _HANDLE_SIG (SIGURG);
    _HANDLE_SIG (SIGVTALRM);
    _HANDLE_SIG (SIGXCPU);
    _HANDLE_SIG (SIGXFSZ);
#undef _HANDLE_SIG
    default:
      break;
    }
  return "UNKNOWN_SIGNAL";
}

typedef struct
{
  GMainLoop *loop;
  GAsyncResult *res;
} SpawnData;

static void
spawn_cb (GObject       *source_object,
          GAsyncResult  *res,
          gpointer       user_data)
{
  SpawnData *data = user_data;
  data->res = g_object_ref (res);
  g_main_loop_quit (data->loop);
}

static JSBool
js_polkit_spawn (JSContext  *cx,
                 uintN       js_argc,
                 jsval      *vp)
{
  /* PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (JS_GetContextPrivate (cx)); */
  JSBool ret = JS_FALSE;
  JSObject *array_object;
  gchar *standard_output = NULL;
  gchar *standard_error = NULL;
  gint exit_status;
  GError *error = NULL;
  JSString *ret_jsstr;
  jsuint array_len;
  gchar **argv = NULL;
  GMainContext *context = NULL;
  GMainLoop *loop = NULL;
  SpawnData data = {0};
  guint n;

  if (!JS_ConvertArguments (cx, js_argc, JS_ARGV (cx, vp), "o", &array_object))
    goto out;

  if (!JS_GetArrayLength (cx, array_object, &array_len))
    {
      JS_ReportError (cx, "Failed to get array length");
      goto out;
    }

  argv = g_new0 (gchar*, array_len + 1);
  for (n = 0; n < array_len; n++)
    {
      jsval elem_val;
      char *s;

      if (!JS_GetElement (cx, array_object, n, &elem_val))
        {
          JS_ReportError (cx, "Failed to get element %d", n);
          goto out;
        }
      s = JS_EncodeString (cx, JSVAL_TO_STRING (elem_val));
      argv[n] = g_strdup (s);
      JS_free (cx, s);
    }

  context = g_main_context_new ();
  loop = g_main_loop_new (context, FALSE);

  g_main_context_push_thread_default (context);

  data.loop = loop;
  utils_spawn ((const gchar *const *) argv,
               10, /* timeout_seconds */
               NULL, /* cancellable */
               spawn_cb,
               &data);

  g_main_loop_run (loop);

  g_main_context_pop_thread_default (context);

  if (!utils_spawn_finish (data.res,
                           &exit_status,
                           &standard_output,
                           &standard_error,
                           &error))
    {
      JS_ReportError (cx,
                      "Error spawning helper: %s (%s, %d)",
                      error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
      goto out;
    }

  if (!(WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0))
    {
      GString *gstr;
      gstr = g_string_new (NULL);
      if (WIFEXITED (exit_status))
        {
          g_string_append_printf (gstr,
                                  "Helper exited with non-zero exit status %d",
                                  WEXITSTATUS (exit_status));
        }
      else if (WIFSIGNALED (exit_status))
        {
          g_string_append_printf (gstr,
                                  "Helper was signaled with signal %s (%d)",
                                  get_signal_name (WTERMSIG (exit_status)),
                                  WTERMSIG (exit_status));
        }
      g_string_append_printf (gstr, ", stdout=`%s', stderr=`%s'",
                              standard_output, standard_error);
      JS_ReportError (cx, gstr->str);
      g_string_free (gstr, TRUE);
      goto out;
    }

  ret = JS_TRUE;

  ret_jsstr = JS_NewStringCopyZ (cx, standard_output);
  JS_SET_RVAL (cx, vp, STRING_TO_JSVAL (ret_jsstr));

 out:
  g_strfreev (argv);
  g_free (standard_output);
  g_free (standard_error);
  g_clear_object (&data.res);
  if (loop != NULL)
    g_main_loop_unref (loop);
  if (context != NULL)
    g_main_context_unref (context);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */


static JSBool
js_polkit_user_is_in_netgroup (JSContext  *cx,
                               uintN       argc,
                               jsval      *vp)
{
  /* PolkitBackendJsAuthority *authority = POLKIT_BACKEND_JS_AUTHORITY (JS_GetContextPrivate (cx)); */
  JSBool ret = JS_FALSE;
  JSString *user_str;
  JSString *netgroup_str;
  char *user;
  char *netgroup;
  JSBool is_in_netgroup = JS_FALSE;

  if (!JS_ConvertArguments (cx, argc, JS_ARGV (cx, vp), "SS", &user_str, &netgroup_str))
    goto out;

  user = JS_EncodeString (cx, user_str);
  netgroup = JS_EncodeString (cx, netgroup_str);

  if (innetgr (netgroup,
               NULL,  /* host */
               user,
               NULL)) /* domain */
    {
      is_in_netgroup =  JS_TRUE;
    }

  JS_free (cx, netgroup);
  JS_free (cx, user);

  ret = JS_TRUE;

  JS_SET_RVAL (cx, vp, BOOLEAN_TO_JSVAL (is_in_netgroup));
 out:
  return ret;
}



/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GSimpleAsyncResult *simple; /* borrowed reference */
  GMainContext *main_context; /* may be NULL */

  GCancellable *cancellable;  /* may be NULL */
  gulong cancellable_handler_id;

  GPid child_pid;
  gint child_stdout_fd;
  gint child_stderr_fd;

  GIOChannel *child_stdout_channel;
  GIOChannel *child_stderr_channel;

  GSource *child_watch_source;
  GSource *child_stdout_source;
  GSource *child_stderr_source;

  guint timeout_seconds;
  gboolean timed_out;
  GSource *timeout_source;

  GString *child_stdout;
  GString *child_stderr;

  gint exit_status;
} UtilsSpawnData;

static void
utils_child_watch_from_release_cb (GPid     pid,
                                   gint     status,
                                   gpointer user_data)
{
}

static void
utils_spawn_data_free (UtilsSpawnData *data)
{
  if (data->timeout_source != NULL)
    {
      g_source_destroy (data->timeout_source);
      data->timeout_source = NULL;
    }

  /* Nuke the child, if necessary */
  if (data->child_watch_source != NULL)
    {
      g_source_destroy (data->child_watch_source);
      data->child_watch_source = NULL;
    }

  if (data->child_pid != 0)
    {
      GSource *source;
      kill (data->child_pid, SIGTERM);
      /* OK, we need to reap for the child ourselves - we don't want
       * to use waitpid() because that might block the calling
       * thread (the child might handle SIGTERM and use several
       * seconds for cleanup/rollback).
       *
       * So we use GChildWatch instead.
       *
       * Avoid taking a references to ourselves. but note that we need
       * to pass the GSource so we can nuke it once handled.
       */
      source = g_child_watch_source_new (data->child_pid);
      g_source_set_callback (source,
                             (GSourceFunc) utils_child_watch_from_release_cb,
                             source,
                             (GDestroyNotify) g_source_destroy);
      g_source_attach (source, data->main_context);
      g_source_unref (source);
      data->child_pid = 0;
    }

  if (data->child_stdout != NULL)
    {
      g_string_free (data->child_stdout, TRUE);
      data->child_stdout = NULL;
    }

  if (data->child_stderr != NULL)
    {
      g_string_free (data->child_stderr, TRUE);
      data->child_stderr = NULL;
    }

  if (data->child_stdout_channel != NULL)
    {
      g_io_channel_unref (data->child_stdout_channel);
      data->child_stdout_channel = NULL;
    }
  if (data->child_stderr_channel != NULL)
    {
      g_io_channel_unref (data->child_stderr_channel);
      data->child_stderr_channel = NULL;
    }

  if (data->child_stdout_source != NULL)
    {
      g_source_destroy (data->child_stdout_source);
      data->child_stdout_source = NULL;
    }
  if (data->child_stderr_source != NULL)
    {
      g_source_destroy (data->child_stderr_source);
      data->child_stderr_source = NULL;
    }

  if (data->child_stdout_fd != -1)
    {
      g_warn_if_fail (close (data->child_stdout_fd) == 0);
      data->child_stdout_fd = -1;
    }
  if (data->child_stderr_fd != -1)
    {
      g_warn_if_fail (close (data->child_stderr_fd) == 0);
      data->child_stderr_fd = -1;
    }

  if (data->cancellable_handler_id > 0)
    {
      g_cancellable_disconnect (data->cancellable, data->cancellable_handler_id);
      data->cancellable_handler_id = 0;
    }

  if (data->main_context != NULL)
    g_main_context_unref (data->main_context);

  if (data->cancellable != NULL)
    g_object_unref (data->cancellable);

  g_slice_free (UtilsSpawnData, data);
}

/* called in the thread where @cancellable was cancelled */
static void
utils_on_cancelled (GCancellable *cancellable,
                    gpointer      user_data)
{
  UtilsSpawnData *data = user_data;
  GError *error;

  error = NULL;
  g_warn_if_fail (g_cancellable_set_error_if_cancelled (cancellable, &error));
  g_simple_async_result_take_error (data->simple, error);
  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);
}

static gboolean
utils_read_child_stderr (GIOChannel *channel,
                         GIOCondition condition,
                         gpointer user_data)
{
  UtilsSpawnData *data = user_data;
  gchar buf[1024];
  gsize bytes_read;

  g_io_channel_read_chars (channel, buf, sizeof buf, &bytes_read, NULL);
  g_string_append_len (data->child_stderr, buf, bytes_read);
  return TRUE;
}

static gboolean
utils_read_child_stdout (GIOChannel *channel,
                         GIOCondition condition,
                         gpointer user_data)
{
  UtilsSpawnData *data = user_data;
  gchar buf[1024];
  gsize bytes_read;

  g_io_channel_read_chars (channel, buf, sizeof buf, &bytes_read, NULL);
  g_string_append_len (data->child_stdout, buf, bytes_read);
  return TRUE;
}

static void
utils_child_watch_cb (GPid     pid,
                      gint     status,
                      gpointer user_data)
{
  UtilsSpawnData *data = user_data;
  gchar *buf;
  gsize buf_size;

  if (g_io_channel_read_to_end (data->child_stdout_channel, &buf, &buf_size, NULL) == G_IO_STATUS_NORMAL)
    {
      g_string_append_len (data->child_stdout, buf, buf_size);
      g_free (buf);
    }
  if (g_io_channel_read_to_end (data->child_stderr_channel, &buf, &buf_size, NULL) == G_IO_STATUS_NORMAL)
    {
      g_string_append_len (data->child_stderr, buf, buf_size);
      g_free (buf);
    }

  data->exit_status = status;

  /* ok, child watch is history, make sure we don't free it in spawn_data_free() */
  data->child_pid = 0;
  data->child_watch_source = NULL;

  /* we're done */
  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);
}

static gboolean
utils_timeout_cb (gpointer user_data)
{
  UtilsSpawnData *data = user_data;

  data->timed_out = TRUE;

  /* ok, timeout is history, make sure we don't free it in spawn_data_free() */
  data->timeout_source = NULL;

  /* we're done */
  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);

  return FALSE; /* remove source */
}

static void
utils_spawn (const gchar *const  *argv,
             guint                timeout_seconds,
             GCancellable        *cancellable,
             GAsyncReadyCallback  callback,
             gpointer             user_data)
{
  UtilsSpawnData *data;
  GError *error;

  data = g_slice_new0 (UtilsSpawnData);
  data->timeout_seconds = timeout_seconds;
  data->simple = g_simple_async_result_new (NULL,
                                            callback,
                                            user_data,
                                            utils_spawn);
  data->main_context = g_main_context_get_thread_default ();
  if (data->main_context != NULL)
    g_main_context_ref (data->main_context);

  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;

  data->child_stdout = g_string_new (NULL);
  data->child_stderr = g_string_new (NULL);
  data->child_stdout_fd = -1;
  data->child_stderr_fd = -1;

  /* the life-cycle of UtilsSpawnData is tied to its GSimpleAsyncResult */
  g_simple_async_result_set_op_res_gpointer (data->simple, data, (GDestroyNotify) utils_spawn_data_free);

  error = NULL;
  if (data->cancellable != NULL)
    {
      /* could already be cancelled */
      error = NULL;
      if (g_cancellable_set_error_if_cancelled (data->cancellable, &error))
        {
          g_simple_async_result_take_error (data->simple, error);
          g_simple_async_result_complete_in_idle (data->simple);
          g_object_unref (data->simple);
          goto out;
        }

      data->cancellable_handler_id = g_cancellable_connect (data->cancellable,
                                                            G_CALLBACK (utils_on_cancelled),
                                                            data,
                                                            NULL);
    }

  error = NULL;
  if (!g_spawn_async_with_pipes (NULL, /* working directory */
                                 (gchar **) argv,
                                 NULL, /* envp */
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, /* child_setup */
                                 NULL, /* child_setup's user_data */
                                 &(data->child_pid),
                                 NULL, /* gint *stdin_fd */
                                 &(data->child_stdout_fd),
                                 &(data->child_stderr_fd),
                                 &error))
    {
      g_prefix_error (&error, "Error spawning: ");
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete_in_idle (data->simple);
      g_object_unref (data->simple);
      goto out;
    }

  if (timeout_seconds > 0)
    {
      data->timeout_source = g_timeout_source_new_seconds (timeout_seconds);
      g_source_set_priority (data->timeout_source, G_PRIORITY_DEFAULT);
      g_source_set_callback (data->timeout_source, utils_timeout_cb, data, NULL);
      g_source_attach (data->timeout_source, data->main_context);
      g_source_unref (data->timeout_source);
    }

  data->child_watch_source = g_child_watch_source_new (data->child_pid);
  g_source_set_callback (data->child_watch_source, (GSourceFunc) utils_child_watch_cb, data, NULL);
  g_source_attach (data->child_watch_source, data->main_context);
  g_source_unref (data->child_watch_source);

  data->child_stdout_channel = g_io_channel_unix_new (data->child_stdout_fd);
  g_io_channel_set_flags (data->child_stdout_channel, G_IO_FLAG_NONBLOCK, NULL);
  data->child_stdout_source = g_io_create_watch (data->child_stdout_channel, G_IO_IN);
  g_source_set_callback (data->child_stdout_source, (GSourceFunc) utils_read_child_stdout, data, NULL);
  g_source_attach (data->child_stdout_source, data->main_context);
  g_source_unref (data->child_stdout_source);

  data->child_stderr_channel = g_io_channel_unix_new (data->child_stderr_fd);
  g_io_channel_set_flags (data->child_stderr_channel, G_IO_FLAG_NONBLOCK, NULL);
  data->child_stderr_source = g_io_create_watch (data->child_stderr_channel, G_IO_IN);
  g_source_set_callback (data->child_stderr_source, (GSourceFunc) utils_read_child_stderr, data, NULL);
  g_source_attach (data->child_stderr_source, data->main_context);
  g_source_unref (data->child_stderr_source);

 out:
  ;
}

gboolean
utils_spawn_finish (GAsyncResult   *res,
                    gint           *out_exit_status,
                    gchar         **out_standard_output,
                    gchar         **out_standard_error,
                    GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  UtilsSpawnData *data;
  gboolean ret = FALSE;

  g_return_val_if_fail (G_IS_ASYNC_RESULT (res), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == utils_spawn);

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  data = g_simple_async_result_get_op_res_gpointer (simple);

  if (data->timed_out)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_TIMED_OUT,
                   "Timed out after %d seconds",
                   data->timeout_seconds);
      goto out;
    }

  if (out_exit_status != NULL)
    *out_exit_status = data->exit_status;

  if (out_standard_output != NULL)
    *out_standard_output = g_strdup (data->child_stdout->str);

  if (out_standard_error != NULL)
    *out_standard_error = g_strdup (data->child_stderr->str);

  ret = TRUE;

 out:
  return ret;
}