#include "dk-application.h"

#include "config.h"
#include "dk-window.h"

struct _DkApplication {
  AdwApplication parent_instance;
};

G_DEFINE_FINAL_TYPE(DkApplication, dk_application, ADW_TYPE_APPLICATION)

static void
on_quit(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  g_application_quit(G_APPLICATION(user_data));
}

static void
dk_application_startup(GApplication *application)
{
  G_APPLICATION_CLASS(dk_application_parent_class)->startup(application);

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(provider, "/io/github/dkchatebras/DKChatebras/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  const GActionEntry actions[] = {
    { "quit", on_quit, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(application), actions,
                                  G_N_ELEMENTS(actions), application);
  gtk_application_set_accels_for_action(GTK_APPLICATION(application),
                                        "app.quit", (const char *[]) { "<Ctrl>q", NULL });
}

static void
dk_application_activate(GApplication *application)
{
  GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(application));
  if (window == NULL)
    window = GTK_WINDOW(dk_window_new(GTK_APPLICATION(application)));
  gtk_window_present(window);
}

static void
dk_application_class_init(DkApplicationClass *klass)
{
  GApplicationClass *application_class = G_APPLICATION_CLASS(klass);
  application_class->startup = dk_application_startup;
  application_class->activate = dk_application_activate;
}

static void
dk_application_init(DkApplication *self)
{
}

DkApplication *
dk_application_new(void)
{
  return g_object_new(DK_TYPE_APPLICATION,
                      "application-id", APP_ID,
                      "flags", G_APPLICATION_DEFAULT_FLAGS,
                      NULL);
}
