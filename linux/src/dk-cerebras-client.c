#include "dk-cerebras-client.h"

#include <libsoup/soup.h>
#include <string.h>

#define API_URL "https://api.cerebras.ai/v1/chat/completions"
#define READ_SIZE 8192

struct _DkCerebrasClient {
  GObject parent_instance;
  SoupSession *session;
  SoupMessage *message;
  GInputStream *stream;
  GCancellable *cancellable;
  GString *pending;
  DkCerebrasChunkFunc chunk_cb;
  DkCerebrasDoneFunc done_cb;
  gpointer user_data;
  gboolean running;
};

G_DEFINE_FINAL_TYPE(DkCerebrasClient, dk_cerebras_client, G_TYPE_OBJECT)

static void read_next(DkCerebrasClient *self);

static void
free_string(GString *string)
{
  g_string_free(string, TRUE);
}

static void
finish_stream(DkCerebrasClient *self, const GError *error)
{
  if (!self->running)
    return;

  DkCerebrasDoneFunc done_cb = self->done_cb;
  gpointer user_data = self->user_data;

  self->running = FALSE;
  g_clear_object(&self->stream);
  g_clear_object(&self->message);
  g_clear_object(&self->cancellable);
  g_clear_pointer(&self->pending, free_string);
  self->chunk_cb = NULL;
  self->done_cb = NULL;
  self->user_data = NULL;

  if (done_cb != NULL)
    done_cb(error, user_data);
}

static gboolean
handle_data(DkCerebrasClient *self, const char *data, GError **error)
{
  if (g_str_equal(data, "[DONE]")) {
    finish_stream(self, NULL);
    return FALSE;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, -1, error))
    return FALSE;

  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  if (json_object_has_member(root, "error")) {
    JsonObject *api_error = json_object_get_object_member(root, "error");
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                json_object_get_string_member_with_default(api_error, "message", "Cerebras API error"));
    return FALSE;
  }

  JsonArray *choices = json_object_get_array_member(root, "choices");
  if (choices != NULL && json_array_get_length(choices) > 0) {
    JsonObject *choice = json_array_get_object_element(choices, 0);
    JsonObject *delta = json_object_get_object_member(choice, "delta");
    const char *content = delta != NULL
      ? json_object_get_string_member_with_default(delta, "content", NULL)
      : NULL;
    if (content != NULL && self->chunk_cb != NULL)
      self->chunk_cb(content, self->user_data);
  }

  return TRUE;
}

static gboolean
process_pending(DkCerebrasClient *self, GError **error)
{
  while (self->running) {
    char *newline = strchr(self->pending->str, '\n');
    if (newline == NULL)
      break;

    gsize line_length = newline - self->pending->str;
    if (line_length > 0 && self->pending->str[line_length - 1] == '\r')
      line_length--;
    g_autofree char *line = g_strndup(self->pending->str, line_length);
    g_string_erase(self->pending, 0, (newline - self->pending->str) + 1);

    if (g_str_has_prefix(line, "data:")) {
      const char *data = line + 5;
      while (*data == ' ')
        data++;
      if (!handle_data(self, data, error))
        return *error == NULL;
    }
  }

  return TRUE;
}

static void
on_read(GObject *source, GAsyncResult *result, gpointer user_data)
{
  DkCerebrasClient *self = DK_CEREBRAS_CLIENT(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);

  if (bytes == NULL) {
    finish_stream(self, error);
  } else {
    gsize size = 0;
    const char *data = g_bytes_get_data(bytes, &size);
    if (size == 0) {
      finish_stream(self, NULL);
    } else {
      g_string_append_len(self->pending, data, size);
      if (!process_pending(self, &error))
        finish_stream(self, error);
      else if (self->running)
        read_next(self);
    }
  }

  g_object_unref(self);
}

static void
read_next(DkCerebrasClient *self)
{
  g_input_stream_read_bytes_async(self->stream,
                                  READ_SIZE,
                                  G_PRIORITY_DEFAULT,
                                  self->cancellable,
                                  on_read,
                                  g_object_ref(self));
}

static void
on_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
  DkCerebrasClient *self = DK_CEREBRAS_CLIENT(user_data);
  g_autoptr(GError) error = NULL;
  GInputStream *stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);

  if (stream == NULL) {
    finish_stream(self, error);
  } else if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(self->message))) {
    g_autoptr(GError) http_error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "Cerebras returned HTTP %u: %s",
                                               soup_message_get_status(self->message),
                                               soup_message_get_reason_phrase(self->message));
    g_object_unref(stream);
    finish_stream(self, http_error);
  } else {
    self->stream = stream;
    read_next(self);
  }

  g_object_unref(self);
}

static void
dk_cerebras_client_dispose(GObject *object)
{
  DkCerebrasClient *self = DK_CEREBRAS_CLIENT(object);
  if (self->cancellable != NULL)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->stream);
  g_clear_object(&self->message);
  g_clear_object(&self->cancellable);
  g_clear_object(&self->session);
  g_clear_pointer(&self->pending, free_string);
  G_OBJECT_CLASS(dk_cerebras_client_parent_class)->dispose(object);
}

static void
dk_cerebras_client_class_init(DkCerebrasClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = dk_cerebras_client_dispose;
}

static void
dk_cerebras_client_init(DkCerebrasClient *self)
{
  self->session = soup_session_new_with_options("user-agent", "DKChatebras/1.0", NULL);
}

DkCerebrasClient *
dk_cerebras_client_new(void)
{
  return g_object_new(DK_TYPE_CEREBRAS_CLIENT, NULL);
}

void
dk_cerebras_client_stream(DkCerebrasClient *self,
                          const char *api_key,
                          const char *model,
                          JsonArray *messages,
                          DkCerebrasChunkFunc chunk_cb,
                          DkCerebrasDoneFunc done_cb,
                          gpointer user_data)
{
  g_return_if_fail(DK_IS_CEREBRAS_CLIENT(self));
  g_return_if_fail(!self->running);

  g_autoptr(JsonBuilder) builder = json_builder_new();
  g_autoptr(JsonGenerator) generator = json_generator_new();
  g_autoptr(JsonNode) root = NULL;
  g_autoptr(GBytes) body = NULL;
  g_autofree char *authorization = g_strdup_printf("Bearer %s", api_key);
  g_autofree char *json = NULL;

  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "model");
  json_builder_add_string_value(builder, model);
  json_builder_set_member_name(builder, "stream");
  json_builder_add_boolean_value(builder, TRUE);
  json_builder_set_member_name(builder, "messages");
  json_builder_add_value(builder, json_node_init_array(json_node_alloc(), messages));
  json_builder_end_object(builder);

  root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  json = json_generator_to_data(generator, NULL);
  body = g_bytes_new(json, strlen(json));

  self->message = soup_message_new("POST", API_URL);
  soup_message_headers_append(soup_message_get_request_headers(self->message), "Authorization", authorization);
  soup_message_set_request_body_from_bytes(self->message, "application/json", body);
  self->cancellable = g_cancellable_new();
  self->pending = g_string_new(NULL);
  self->chunk_cb = chunk_cb;
  self->done_cb = done_cb;
  self->user_data = user_data;
  self->running = TRUE;

  soup_session_send_async(self->session,
                          self->message,
                          G_PRIORITY_DEFAULT,
                          self->cancellable,
                          on_response,
                          g_object_ref(self));
}

void
dk_cerebras_client_stop(DkCerebrasClient *self)
{
  g_return_if_fail(DK_IS_CEREBRAS_CLIENT(self));
  if (self->cancellable != NULL)
    g_cancellable_cancel(self->cancellable);
}

gboolean
dk_cerebras_client_is_running(DkCerebrasClient *self)
{
  g_return_val_if_fail(DK_IS_CEREBRAS_CLIENT(self), FALSE);
  return self->running;
}
