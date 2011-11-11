#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <libbamf/bamf-matcher.h>
#include <gio/gio.h>

#include "hud-search.h"
#include "dbusmenu-collector.h"
#include "usage-tracker.h"

struct _HudSearchPrivate {
	BamfMatcher * matcher;
	gulong window_changed_sig;

	guint32 active_xid;
	BamfApplication * active_app;

	DbusmenuCollector * collector;
	UsageTracker * usage;

	GDBusProxy * appmenu;
};

#define HUD_SEARCH_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE ((o), HUD_SEARCH_TYPE, HudSearchPrivate))

static void hud_search_class_init (HudSearchClass *klass);
static void hud_search_init       (HudSearch *self);
static void hud_search_dispose    (GObject *object);
static void hud_search_finalize   (GObject *object);

static void active_window_changed (BamfMatcher * matcher, BamfView * oldview, BamfView * newview, gpointer user_data);


G_DEFINE_TYPE (HudSearch, hud_search, G_TYPE_OBJECT);

static void
hud_search_class_init (HudSearchClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (HudSearchPrivate));

	object_class->dispose = hud_search_dispose;
	object_class->finalize = hud_search_finalize;

	return;
}

static void
hud_search_init (HudSearch *self)
{
	/* Get Private */
	self->priv = HUD_SEARCH_GET_PRIVATE(self);

	/* Initialize Private */
	self->priv->matcher = NULL;
	self->priv->window_changed_sig = 0;
	self->priv->active_xid = 0;
	self->priv->collector = NULL;
	self->priv->usage = NULL;
	self->priv->appmenu = NULL;

	/* BAMF */
	self->priv->matcher = bamf_matcher_get_default();
	self->priv->window_changed_sig = g_signal_connect(G_OBJECT(self->priv->matcher), "active-window-changed", G_CALLBACK(active_window_changed), self);

	BamfWindow * active_window = bamf_matcher_get_active_window(self->priv->matcher);
	if (active_window != NULL) {
		self->priv->active_xid = bamf_window_get_xid(active_window);
		self->priv->active_app = bamf_matcher_get_application_for_window(self->priv->matcher, active_window);
	}

	/* DBusMenu */
	self->priv->collector = dbusmenu_collector_new();

	/* Usage Tracker */
	self->priv->usage = usage_tracker_new();

	/* Appmenu */
	self->priv->appmenu = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
	                                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                                                    /* info */ NULL,
	                                                    "com.canonical.AppMenu.Registrar",
	                                                    "/com/canonical/AppMenu/Registrar",
	                                                    "com.canonical.AppMenu.Registrar",
	                                                    NULL, NULL);

	return;
}

static void
hud_search_dispose (GObject *object)
{
	HudSearch * self = HUD_SEARCH(object);

	if (self->priv->window_changed_sig != 0) {
		g_signal_handler_disconnect(self->priv->matcher, self->priv->window_changed_sig);
		self->priv->window_changed_sig = 0;
	}

	if (self->priv->matcher != NULL) {
		g_object_unref(self->priv->matcher);
		self->priv->matcher = NULL;
	}

	if (self->priv->collector != NULL) {
		g_object_unref(self->priv->collector);
		self->priv->collector = NULL;
	}

	if (self->priv->usage != NULL) {
		g_object_unref(self->priv->usage);
		self->priv->usage = NULL;
	}

	if (self->priv->appmenu != NULL) {
		g_object_unref(self->priv->appmenu);
		self->priv->appmenu = NULL;
	}

	G_OBJECT_CLASS (hud_search_parent_class)->dispose (object);
	return;
}

static void
hud_search_finalize (GObject *object)
{

	G_OBJECT_CLASS (hud_search_parent_class)->finalize (object);
	return;
}

HudSearch *
hud_search_new (void)
{
	HudSearch * ret = HUD_SEARCH(g_object_new(HUD_SEARCH_TYPE, NULL));
	return ret;
}

static void
get_current_window_address(HudSearch * search, gchar ** address, gchar ** path)
{
	if (search->priv->active_xid == 0) {
		g_warning("Active application is unknown");
		return;
	}

	if (search->priv->appmenu == NULL) {
		g_warning("Unable to proxy appmenu");
		return;
	}

	GError * error = NULL;
	GVariant * dbusinfo = g_dbus_proxy_call_sync(search->priv->appmenu,
	                                             "GetMenuForWindow",
	                                             g_variant_new("(u)", search->priv->active_xid),
	                                             G_DBUS_CALL_FLAGS_NONE,
	                                             -1,
	                                             NULL,
	                                             &error);

	if (error != NULL) {
		g_warning("Unable to get menus from appmenu: %s", error->message);
		g_error_free(error);
		return;
	}

	g_variant_get(dbusinfo, "(so)", address, path);
	return;
}

typedef struct _usage_wrapper_t usage_wrapper_t;
struct _usage_wrapper_t {
	DbusmenuCollectorFound * found;
	guint count;
	gfloat percent_usage;
	gfloat percent_distance;
};

static gint
usage_sort (gconstpointer a, gconstpointer b)
{
	usage_wrapper_t * wa = (usage_wrapper_t *)a;
	usage_wrapper_t * wb = (usage_wrapper_t *)b;

	gfloat totala = (1.0 - wa->percent_usage) + wa->percent_distance;
	gfloat totalb = (1.0 - wb->percent_usage) + wb->percent_distance;

	gfloat difference = (totala - totalb) * 100.0;
	return (gint)difference;
}

static void
search_and_sort (HudSearch * search, const gchar * searchstr, GArray * usagedata, GList ** foundlist)
{
	gchar * address = NULL;
	gchar * path = NULL;
	GList * found_list = NULL;
	guint count = 0;
	GList * found = NULL;

	get_current_window_address(search, &address, &path);

	if (address != NULL && path != NULL) {
		found_list = dbusmenu_collector_search(search->priv->collector, address, path, searchstr);
	}

	g_free(address);
	g_free(path);

	count = 0;
	found = found_list;
	guint overall_usage = 0;
	guint overall_distance = 0;
	while (found != NULL) {
		usage_wrapper_t usage;
		usage.found = (DbusmenuCollectorFound *)found->data;
		usage.count = 0;

		const gchar * desktopfile = NULL;
		if (search->priv->active_app != NULL) {
			desktopfile = bamf_application_get_desktop_file(search->priv->active_app);
		}

		if (desktopfile != NULL) {
			usage.count = usage_tracker_get_usage(search->priv->usage, desktopfile, dbusmenu_collector_found_get_display(usage.found));
		}

		overall_usage += usage.count;
		overall_distance += dbusmenu_collector_found_get_distance(usage.found);

		g_array_append_val(usagedata, usage);
		found = g_list_next(found);
		count++;
		if (count >= 15) {
			break;
		}
	}

	for (count = 0; count < usagedata->len; count++) {
		usage_wrapper_t * usage = &g_array_index(usagedata, usage_wrapper_t, count);

		if (overall_usage != 0) {
			usage->percent_usage = (gfloat)usage->count/(gfloat)overall_usage;
		} else {
			usage->percent_usage = 1.0;
		}

		usage->percent_distance = (gfloat)dbusmenu_collector_found_get_distance(usage->found)/(gfloat)overall_distance;
	}

	g_array_sort(usagedata, usage_sort);
	*foundlist = found_list;
	return;
}

GStrv
hud_search_suggestions (HudSearch * search, const gchar * searchstr)
{
	g_return_val_if_fail(IS_HUD_SEARCH(search), NULL);

	GArray * usagedata = g_array_sized_new(FALSE, TRUE, sizeof(usage_wrapper_t), 15);
	GList * found_list = NULL;

	search_and_sort(search, searchstr, usagedata, &found_list);

	int count;
	gchar ** retval = g_new0(gchar *, 6);
	for (count = 0; count < 5 && count < usagedata->len; count++) {
		usage_wrapper_t * usage = &g_array_index(usagedata, usage_wrapper_t, count);
		retval[count] = g_strdup(dbusmenu_collector_found_get_display(usage->found));
	}
	retval[count] = NULL;

	g_array_free(usagedata, TRUE);
	dbusmenu_collector_found_list_free(found_list);

	return retval;
}

void
hud_search_execute (HudSearch * search, const gchar * searchstr)
{
	g_return_if_fail(IS_HUD_SEARCH(search));

	GArray * usagedata = g_array_sized_new(FALSE, TRUE, sizeof(usage_wrapper_t), 15);
	GList * found_list = NULL;

	search_and_sort(search, searchstr, usagedata, &found_list);

	if (usagedata->len != 0) {
		usage_wrapper_t * usage = &g_array_index(usagedata, usage_wrapper_t, 0);

		dbusmenu_collector_found_exec(usage->found);

		const gchar * desktopfile = NULL;
		if (search->priv->active_app != NULL) {
			desktopfile = bamf_application_get_desktop_file(search->priv->active_app);
		}

		if (desktopfile != NULL) {
			usage_tracker_mark_usage(search->priv->usage, desktopfile, dbusmenu_collector_found_get_display(usage->found));
		}
	} else {
		g_warning("Unable to execute as we couldn't find anything");
	}

	g_array_free(usagedata, TRUE);
	dbusmenu_collector_found_list_free(found_list);

	return;
}

static void
active_window_changed (BamfMatcher * matcher, BamfView * oldview, BamfView * newview, gpointer user_data)
{
	HudSearch * self = HUD_SEARCH(user_data);

	if (!BAMF_IS_WINDOW(newview)) { return; }
	BamfWindow * window = BAMF_WINDOW(newview);

	BamfApplication * app = bamf_matcher_get_application_for_window(self->priv->matcher, window);
	const gchar * desktop = bamf_application_get_desktop_file(app);

	/* If BAMF can't match it to an application we probably don't
	   want to be involved with it anyway. */
	if (desktop == NULL) {
		return;
	}

	/* NOTE: Both of these are debugging tools for now, so we want
	   to be able to use them effectively to work on the HUD, so we're
	   going to pretend we didn't switch windows if we switch to one 
	   of them */
	if (g_strstr_len(desktop, -1, "termina") != NULL ||
	        g_strstr_len(desktop, -1, "dfeet") != NULL) {
		return;
	}

	/* This should ignore most of the windows involved in Unity */
	const gchar * name = bamf_view_get_name(newview);
	if (g_strcmp0(name, "DNDCollectionWindow") == 0) {
		return;
	}

	self->priv->active_xid = bamf_window_get_xid(window);
	self->priv->active_app = app;

	return;
}
