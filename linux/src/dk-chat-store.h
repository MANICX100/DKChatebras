#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
  char *id;
  char *title;
  char *model;
  gint64 updated_at;
} DkChatMetadata;

void dk_chat_metadata_free(DkChatMetadata *metadata);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DkChatMetadata, dk_chat_metadata_free)

GPtrArray *dk_chat_store_load_index(GError **error);
JsonObject *dk_chat_store_load_chat(const char *id, GError **error);
gboolean dk_chat_store_save_chat(JsonObject *chat, GError **error);
gboolean dk_chat_store_delete_chat(const char *id, GError **error);
JsonObject *dk_chat_store_new_chat(const char *model);
void dk_chat_store_append_message(JsonObject *chat, const char *role, const char *content);
DkChatMetadata *dk_chat_store_metadata_from_chat(JsonObject *chat);

G_END_DECLS
