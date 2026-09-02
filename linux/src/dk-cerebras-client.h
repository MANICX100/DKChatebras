#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define DK_TYPE_CEREBRAS_CLIENT (dk_cerebras_client_get_type())
G_DECLARE_FINAL_TYPE(DkCerebrasClient, dk_cerebras_client, DK, CEREBRAS_CLIENT, GObject)

typedef void (*DkCerebrasChunkFunc)(const char *text, gpointer user_data);
typedef void (*DkCerebrasDoneFunc)(const GError *error, gpointer user_data);

DkCerebrasClient *dk_cerebras_client_new(void);
void dk_cerebras_client_stream(DkCerebrasClient *self,
                               const char *api_key,
                               const char *model,
                               JsonArray *messages,
                               DkCerebrasChunkFunc chunk_cb,
                               DkCerebrasDoneFunc done_cb,
                               gpointer user_data);
void dk_cerebras_client_stop(DkCerebrasClient *self);
gboolean dk_cerebras_client_is_running(DkCerebrasClient *self);

G_END_DECLS
