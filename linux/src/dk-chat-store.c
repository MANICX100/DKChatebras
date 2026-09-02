#include "dk-chat-store.h"

#include <errno.h>
#include <glib/gstdio.h>

static char *
get_chats_dir(void)
{
  return g_build_filename(g_get_user_data_dir(), "dkchatebras", "chats", NULL);
}

static gboolean
ensure_chats_dir(GError **error)
{
  g_autofree char *directory = get_chats_dir();

  if (g_mkdir_with_parents(directory, 0700) == 0)
    return TRUE;

  g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
              "Could not create history directory: %s", g_strerror(errno));
  return FALSE;
}

static gboolean
write_json(JsonNode *root, const char *path, GError **error)
{
  g_autoptr(JsonGenerator) generator = json_generator_new();
  g_autofree char *data = NULL;

  json_generator_set_root(generator, root);
  json_generator_set_pretty(generator, TRUE);
  data = json_generator_to_data(generator, NULL);
  return g_file_set_contents_full(path, data, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT,
                                  0600, error);
}

void
dk_chat_metadata_free(DkChatMetadata *metadata)
{
  if (metadata == NULL)
    return;
  g_free(metadata->id);
  g_free(metadata->title);
  g_free(metadata->model);
  g_free(metadata);
}

DkChatMetadata *
dk_chat_store_metadata_from_chat(JsonObject *chat)
{
  DkChatMetadata *metadata = g_new0(DkChatMetadata, 1);
  metadata->id = g_strdup(json_object_get_string_member(chat, "id"));
  metadata->title = g_strdup(json_object_get_string_member_with_default(chat, "title", "New Chat"));
  metadata->model = g_strdup(json_object_get_string_member_with_default(chat, "model", "gpt-oss-120b"));
  metadata->updated_at = json_object_get_int_member_with_default(chat, "updated_at", 0);
  return metadata;
}

GPtrArray *
dk_chat_store_load_index(GError **error)
{
  g_autofree char *directory = get_chats_dir();
  g_autofree char *path = g_build_filename(directory, "index.json", NULL);
  g_autoptr(JsonParser) parser = json_parser_new();
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify) dk_chat_metadata_free);

  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    return items;

  if (!json_parser_load_from_file(parser, path, error)) {
    g_ptr_array_unref(items);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "History index is not a JSON array");
    g_ptr_array_unref(items);
    return NULL;
  }

  JsonArray *array = json_node_get_array(root);
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonObject *object = json_array_get_object_element(array, i);
    DkChatMetadata *metadata = g_new0(DkChatMetadata, 1);
    metadata->id = g_strdup(json_object_get_string_member(object, "id"));
    metadata->title = g_strdup(json_object_get_string_member_with_default(object, "title", "New Chat"));
    metadata->model = g_strdup(json_object_get_string_member_with_default(object, "model", "gpt-oss-120b"));
    metadata->updated_at = json_object_get_int_member_with_default(object, "updated_at", 0);
    g_ptr_array_add(items, metadata);
  }

  return items;
}

JsonObject *
dk_chat_store_load_chat(const char *id, GError **error)
{
  g_autofree char *directory = get_chats_dir();
  g_autofree char *filename = g_strdup_printf("%s.json", id);
  g_autofree char *path = g_build_filename(directory, filename, NULL);
  g_autoptr(JsonParser) parser = json_parser_new();

  if (!json_parser_load_from_file(parser, path, error))
    return NULL;

  JsonNode *copy = json_node_copy(json_parser_get_root(parser));
  if (!JSON_NODE_HOLDS_OBJECT(copy)) {
    json_node_unref(copy);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Chat file is not a JSON object");
    return NULL;
  }

  JsonObject *chat = json_node_dup_object(copy);
  json_node_unref(copy);
  return chat;
}

static gint
compare_metadata(gconstpointer a, gconstpointer b)
{
  const DkChatMetadata *left = *(DkChatMetadata * const *) a;
  const DkChatMetadata *right = *(DkChatMetadata * const *) b;
  return (left->updated_at > right->updated_at) - (left->updated_at < right->updated_at);
}

static gboolean
save_index_with_chat(JsonObject *chat, const char *deleted_id, GError **error)
{
  g_autoptr(GPtrArray) items = dk_chat_store_load_index(error);
  g_autofree char *directory = get_chats_dir();
  g_autofree char *path = g_build_filename(directory, "index.json", NULL);
  g_autoptr(JsonArray) array = json_array_new();
  gboolean replaced = FALSE;

  if (items == NULL)
    return FALSE;

  for (gint i = (gint) items->len - 1; i >= 0; i--) {
    DkChatMetadata *item = g_ptr_array_index(items, i);
    if (deleted_id != NULL && g_str_equal(item->id, deleted_id))
      g_ptr_array_remove_index(items, i);
    else if (chat != NULL && g_str_equal(item->id, json_object_get_string_member(chat, "id"))) {
      g_ptr_array_remove_index(items, i);
      g_ptr_array_add(items, dk_chat_store_metadata_from_chat(chat));
      replaced = TRUE;
    }
  }

  if (chat != NULL && !replaced)
    g_ptr_array_add(items, dk_chat_store_metadata_from_chat(chat));
  g_ptr_array_sort(items, compare_metadata);

  for (guint i = 0; i < items->len; i++) {
    DkChatMetadata *item = g_ptr_array_index(items, i);
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "id", item->id);
    json_object_set_string_member(object, "title", item->title);
    json_object_set_string_member(object, "model", item->model);
    json_object_set_int_member(object, "updated_at", item->updated_at);
    json_array_add_object_element(array, object);
  }

  g_autoptr(JsonNode) root = json_node_new(JSON_NODE_ARRAY);
  json_node_take_array(root, g_steal_pointer(&array));
  return write_json(root, path, error);
}

gboolean
dk_chat_store_save_chat(JsonObject *chat, GError **error)
{
  g_autofree char *directory = get_chats_dir();
  g_autofree char *filename = NULL;
  g_autofree char *path = NULL;
  g_autoptr(JsonNode) root = NULL;

  if (!ensure_chats_dir(error))
    return FALSE;

  json_object_set_int_member(chat, "updated_at", g_get_real_time() / G_USEC_PER_SEC);
  filename = g_strdup_printf("%s.json", json_object_get_string_member(chat, "id"));
  path = g_build_filename(directory, filename, NULL);
  root = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(root, chat);

  return write_json(root, path, error) && save_index_with_chat(chat, NULL, error);
}

gboolean
dk_chat_store_delete_chat(const char *id, GError **error)
{
  g_autofree char *directory = get_chats_dir();
  g_autofree char *filename = g_strdup_printf("%s.json", id);
  g_autofree char *path = g_build_filename(directory, filename, NULL);

  if (g_unlink(path) != 0 && errno != ENOENT) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Could not delete chat: %s", g_strerror(errno));
    return FALSE;
  }

  return save_index_with_chat(NULL, id, error);
}

JsonObject *
dk_chat_store_new_chat(const char *model)
{
  JsonObject *chat = json_object_new();
  g_autofree char *id = g_uuid_string_random();
  json_object_set_string_member(chat, "id", id);
  json_object_set_string_member(chat, "title", "New Chat");
  json_object_set_string_member(chat, "model", model);
  json_object_set_int_member(chat, "updated_at", g_get_real_time() / G_USEC_PER_SEC);
  json_object_set_array_member(chat, "messages", json_array_new());
  return chat;
}

void
dk_chat_store_append_message(JsonObject *chat, const char *role, const char *content)
{
  JsonObject *message = json_object_new();
  json_object_set_string_member(message, "role", role);
  json_object_set_string_member(message, "content", content);
  json_array_add_object_element(json_object_get_array_member(chat, "messages"), message);
}
