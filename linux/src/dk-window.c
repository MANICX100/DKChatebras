#include "dk-window.h"

#include "dk-cerebras-client.h"
#include "dk-chat-store.h"
#include "dk-markdown.h"
#include "dk-secret.h"

struct _DkWindow {
  AdwApplicationWindow parent_instance;
  AdwOverlaySplitView *split_view;
  AdwToastOverlay *toast_overlay;
  GtkListBox *history_list;
  GtkBox *messages_box;
  GtkScrolledWindow *messages_scroll;
  GtkTextView *composer;
  GtkDropDown *model_dropdown;
  GtkButton *send_button;
  GtkButton *stop_button;
  GtkButton *delete_button;
  GtkLabel *chat_title;
  GtkTextBuffer *stream_buffer;
  GtkWindow *key_window;
  GtkPasswordEntry *key_entry;
  DkCerebrasClient *client;
  JsonObject *current_chat;
  GString *stream_text;
  GCancellable *secret_cancellable;
  char *pending_prompt;
  gboolean generating;
  gboolean changing_model;
  gboolean closing;
};

G_DEFINE_FINAL_TYPE(DkWindow, dk_window, ADW_TYPE_APPLICATION_WINDOW)

static const char *models[] = {
  "llama-3.3-70b",
  "llama3.1-8b",
  "gpt-oss-120b",
  NULL,
};

static void send_message(DkWindow *self);

static void
free_string(GString *string)
{
  g_string_free(string, TRUE);
}

static void
show_toast(DkWindow *self, const char *message)
{
  adw_toast_overlay_add_toast(self->toast_overlay, adw_toast_new(message));
}

static gboolean
scroll_to_bottom(gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  if (self->closing)
    return G_SOURCE_REMOVE;

  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(self->messages_scroll);
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
  return G_SOURCE_REMOVE;
}

static void
queue_scroll(DkWindow *self)
{
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scroll_to_bottom, g_object_ref(self), g_object_unref);
}

static void
clear_messages(DkWindow *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->messages_box))) != NULL)
    gtk_box_remove(self->messages_box, child);
}

static void
clear_history(DkWindow *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->history_list))) != NULL)
    gtk_list_box_remove(self->history_list, child);
}

static GtkTextBuffer *
append_message_widget(DkWindow *self, const char *role, const char *content, gboolean markdown)
{
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *role_label = gtk_label_new(g_str_equal(role, "user") ? "You" : "DKChatebras");
  GtkWidget *view = gtk_text_view_new();
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

  gtk_widget_add_css_class(card, "message-card");
  gtk_widget_add_css_class(card, g_str_equal(role, "user") ? "user-message" : "assistant-message");
  gtk_widget_set_halign(role_label, GTK_ALIGN_START);
  gtk_widget_add_css_class(role_label, "caption");
  gtk_widget_add_css_class(role_label, "dim-label");
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 0);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 0);
  gtk_widget_add_css_class(view, "message-text");
  dk_markdown_setup_buffer(buffer);

  if (markdown)
    dk_markdown_render(buffer, content);
  else
    gtk_text_buffer_set_text(buffer, content != NULL ? content : "", -1);

  gtk_box_append(GTK_BOX(card), role_label);
  gtk_box_append(GTK_BOX(card), view);
  gtk_box_append(self->messages_box, card);
  queue_scroll(self);
  return buffer;
}

static void
render_current_chat(DkWindow *self)
{
  clear_messages(self);

  if (self->current_chat == NULL) {
    gtk_label_set_text(self->chat_title, "DKChatebras");
    gtk_widget_set_sensitive(GTK_WIDGET(self->delete_button), FALSE);
    return;
  }

  gtk_label_set_text(self->chat_title,
                     json_object_get_string_member_with_default(self->current_chat, "title", "New Chat"));
  gtk_widget_set_sensitive(GTK_WIDGET(self->delete_button), !self->generating);

  JsonArray *messages = json_object_get_array_member(self->current_chat, "messages");
  for (guint i = 0; i < json_array_get_length(messages); i++) {
    JsonObject *message = json_array_get_object_element(messages, i);
    append_message_widget(self,
                          json_object_get_string_member(message, "role"),
                          json_object_get_string_member(message, "content"),
                          TRUE);
  }
}

static GtkWidget *
create_history_row(DkChatMetadata *metadata)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title = gtk_label_new(metadata->title);
  g_autoptr(GDateTime) date = g_date_time_new_from_unix_local(metadata->updated_at);
  g_autofree char *subtitle_text = date != NULL ? g_date_time_format(date, "%b %-d · %H:%M") : g_strdup(metadata->model);
  GtkWidget *subtitle = gtk_label_new(subtitle_text);

  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
  gtk_widget_add_css_class(subtitle, "caption");
  gtk_widget_add_css_class(subtitle, "dim-label");
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), subtitle);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "metadata", metadata,
                         (GDestroyNotify) dk_chat_metadata_free);
  return row;
}

static void
reload_history(DkWindow *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) items = dk_chat_store_load_index(&error);
  const char *selected_id = self->current_chat != NULL
    ? json_object_get_string_member(self->current_chat, "id") : NULL;

  clear_history(self);
  if (items == NULL) {
    show_toast(self, error->message);
    return;
  }

  for (gint i = (gint) items->len - 1; i >= 0; i--) {
    DkChatMetadata *metadata = g_ptr_array_steal_index(items, i);
    GtkWidget *row = create_history_row(metadata);
    gtk_list_box_append(self->history_list, row);
    if (selected_id != NULL && g_str_equal(selected_id, metadata->id))
      gtk_list_box_select_row(self->history_list, GTK_LIST_BOX_ROW(row));
  }
}

static guint
model_index(const char *model)
{
  for (guint i = 0; models[i] != NULL; i++)
    if (g_str_equal(models[i], model))
      return i;
  return 0;
}

static const char *
selected_model(DkWindow *self)
{
  guint selected = gtk_drop_down_get_selected(self->model_dropdown);
  return models[selected < G_N_ELEMENTS(models) - 1 ? selected : 0];
}

static void
set_current_chat(DkWindow *self, JsonObject *chat)
{
  g_clear_pointer(&self->current_chat, json_object_unref);
  self->current_chat = chat;
  self->changing_model = TRUE;
  gtk_drop_down_set_selected(self->model_dropdown,
                             model_index(json_object_get_string_member_with_default(chat, "model", models[0])));
  self->changing_model = FALSE;
  render_current_chat(self);
}

static void
on_history_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  DkChatMetadata *metadata = g_object_get_data(G_OBJECT(row), "metadata");
  g_autoptr(GError) error = NULL;

  if (self->generating) {
    show_toast(self, "Stop the current response before switching chats");
    return;
  }

  JsonObject *chat = dk_chat_store_load_chat(metadata->id, &error);
  if (chat == NULL) {
    show_toast(self, error->message);
    return;
  }

  set_current_chat(self, chat);
  if (adw_overlay_split_view_get_collapsed(self->split_view))
    adw_overlay_split_view_set_show_sidebar(self->split_view, FALSE);
}

static void
save_current(DkWindow *self)
{
  g_autoptr(GError) error = NULL;
  if (self->current_chat != NULL && !dk_chat_store_save_chat(self->current_chat, &error))
    show_toast(self, error->message);
  reload_history(self);
}

static void
on_new_chat(GtkButton *button, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  if (self->generating) {
    show_toast(self, "Stop the current response before starting a new chat");
    return;
  }

  JsonObject *chat = dk_chat_store_new_chat(selected_model(self));
  set_current_chat(self, chat);
  save_current(self);
  gtk_widget_grab_focus(GTK_WIDGET(self->composer));
}

static void
on_delete_chat(GtkButton *button, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  if (self->current_chat == NULL || self->generating)
    return;

  g_autofree char *id = g_strdup(json_object_get_string_member(self->current_chat, "id"));
  if (!dk_chat_store_delete_chat(id, &error)) {
    show_toast(self, error->message);
    return;
  }

  g_clear_pointer(&self->current_chat, json_object_unref);
  render_current_chat(self);
  reload_history(self);
}

static void
set_generating(DkWindow *self, gboolean generating)
{
  self->generating = generating;
  gtk_widget_set_visible(GTK_WIDGET(self->send_button), !generating);
  gtk_widget_set_visible(GTK_WIDGET(self->stop_button), generating);
  gtk_widget_set_sensitive(GTK_WIDGET(self->composer), !generating);
  gtk_widget_set_sensitive(GTK_WIDGET(self->model_dropdown), !generating);
  gtk_widget_set_sensitive(GTK_WIDGET(self->delete_button), !generating && self->current_chat != NULL);
}

static void
on_stream_chunk(const char *text, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  if (self->closing)
    return;

  GtkTextIter end;
  g_string_append(self->stream_text, text);
  gtk_text_buffer_get_end_iter(self->stream_buffer, &end);
  gtk_text_buffer_insert(self->stream_buffer, &end, text, -1);
  queue_scroll(self);
}

static void
on_stream_done(const GError *error, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);

  if (self->closing) {
    g_clear_pointer(&self->stream_text, free_string);
    self->stream_buffer = NULL;
    self->generating = FALSE;
    g_object_unref(self);
    return;
  }

  if (self->stream_text != NULL && self->stream_text->len > 0) {
    dk_chat_store_append_message(self->current_chat, "assistant", self->stream_text->str);
    dk_markdown_render(self->stream_buffer, self->stream_text->str);
    save_current(self);
  }

  if (error != NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    show_toast(self, error->message);

  g_clear_pointer(&self->stream_text, free_string);
  self->stream_buffer = NULL;
  set_generating(self, FALSE);
  gtk_widget_grab_focus(GTK_WIDGET(self->composer));
  g_object_unref(self);
}

static void
start_stream(DkWindow *self, const char *api_key)
{
  const char *prompt = self->pending_prompt;
  if (self->current_chat == NULL)
    self->current_chat = dk_chat_store_new_chat(selected_model(self));

  JsonArray *messages = json_object_get_array_member(self->current_chat, "messages");
  if (json_array_get_length(messages) == 0) {
    g_autofree char *title = g_utf8_substring(prompt, 0, MIN(48, g_utf8_strlen(prompt, -1)));
    json_object_set_string_member(self->current_chat, "title", title);
    gtk_label_set_text(self->chat_title, title);
  }

  json_object_set_string_member(self->current_chat, "model", selected_model(self));
  dk_chat_store_append_message(self->current_chat, "user", prompt);
  append_message_widget(self, "user", prompt, TRUE);
  self->stream_buffer = append_message_widget(self, "assistant", "", FALSE);
  self->stream_text = g_string_new(NULL);
  g_clear_pointer(&self->pending_prompt, g_free);
  save_current(self);

  dk_cerebras_client_stream(self->client,
                            api_key,
                            selected_model(self),
                            messages,
                            on_stream_chunk,
                            on_stream_done,
                            g_object_ref(self));
}

static void
on_key_saved(GObject *source, GAsyncResult *result, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  if (!dk_secret_store_finish(result, &error)) {
    if (!self->closing)
      show_toast(self, error->message);
  } else if (!self->closing) {
    show_toast(self, "API key saved securely");
  }
  g_object_unref(self);
}

static void
on_save_key(GtkButton *button, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  const char *key = gtk_editable_get_text(GTK_EDITABLE(self->key_entry));
  if (*key == '\0') {
    show_toast(self, "Enter an API key first");
    return;
  }

  dk_secret_store_async(key, NULL, on_key_saved, g_object_ref(self));
  gtk_window_close(self->key_window);
}

static void
show_key_window(DkWindow *self)
{
  if (self->key_window != NULL) {
    gtk_window_present(self->key_window);
    return;
  }

  self->key_window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(self->key_window, "Cerebras API Key");
  gtk_window_set_transient_for(self->key_window, GTK_WINDOW(self));
  gtk_window_set_modal(self->key_window, TRUE);
  gtk_window_set_default_size(self->key_window, 440, -1);
  g_object_add_weak_pointer(G_OBJECT(self->key_window), (gpointer *) &self->key_window);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *title = gtk_label_new("Connect to Cerebras");
  GtkWidget *description = gtk_label_new("Your API key is stored by GNOME Keyring through Secret Service and is sent only to api.cerebras.ai.");
  self->key_entry = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
  GtkWidget *save = gtk_button_new_with_label("Save Key");

  gtk_widget_add_css_class(title, "title-2");
  gtk_label_set_wrap(GTK_LABEL(description), TRUE);
  gtk_widget_add_css_class(description, "dim-label");
  gtk_password_entry_set_show_peek_icon(self->key_entry, TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self->key_entry), TRUE);
  gtk_widget_add_css_class(save, "suggested-action");
  gtk_widget_set_halign(save, GTK_ALIGN_END);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), description);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->key_entry));
  gtk_box_append(GTK_BOX(box), save);
  gtk_window_set_child(self->key_window, box);
  g_signal_connect(save, "clicked", G_CALLBACK(on_save_key), self);
  gtk_window_present(self->key_window);
}

static void
on_key_lookup(GObject *source, GAsyncResult *result, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  char *api_key = dk_secret_lookup_finish(result, &error);
  g_clear_object(&self->secret_cancellable);

  if (self->closing) {
    dk_secret_password_free(api_key);
    g_object_unref(self);
    return;
  }

  if (error != NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      show_toast(self, error->message);
    else if (self->pending_prompt != NULL) {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->composer);
      gtk_text_buffer_set_text(buffer, self->pending_prompt, -1);
    }
    g_clear_pointer(&self->pending_prompt, g_free);
    set_generating(self, FALSE);
  } else if (api_key == NULL || *api_key == '\0') {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->composer);
    gtk_text_buffer_set_text(buffer, self->pending_prompt, -1);
    g_clear_pointer(&self->pending_prompt, g_free);
    set_generating(self, FALSE);
    show_key_window(self);
    show_toast(self, "Add your Cerebras API key, then send again");
  } else {
    start_stream(self, api_key);
  }

  dk_secret_password_free(api_key);
  g_object_unref(self);
}

static void
send_message(DkWindow *self)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->composer);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_autofree char *stripped = g_strdup(text);
  g_strstrip(stripped);

  if (self->generating || *stripped == '\0')
    return;

  self->pending_prompt = g_steal_pointer(&stripped);
  gtk_text_buffer_set_text(buffer, "", 0);
  set_generating(self, TRUE);
  self->secret_cancellable = g_cancellable_new();
  dk_secret_lookup_async(self->secret_cancellable, on_key_lookup, g_object_ref(self));
}

static void
on_send(GtkButton *button, gpointer user_data)
{
  send_message(DK_WINDOW(user_data));
}

static gboolean
on_composer_key(GtkEventControllerKey *controller,
                guint keyval,
                guint keycode,
                GdkModifierType state,
                gpointer user_data)
{
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
      !(state & GDK_SHIFT_MASK)) {
    send_message(DK_WINDOW(user_data));
    return TRUE;
  }
  return FALSE;
}

static void
on_stop(GtkButton *button, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  if (self->secret_cancellable != NULL)
    g_cancellable_cancel(self->secret_cancellable);
  dk_cerebras_client_stop(self->client);
}

static void
on_model_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  if (self->changing_model || self->current_chat == NULL || self->generating)
    return;
  json_object_set_string_member(self->current_chat, "model", selected_model(self));
  save_current(self);
}

static void
on_show_key(GtkButton *button, gpointer user_data)
{
  show_key_window(DK_WINDOW(user_data));
}

static void
on_toggle_sidebar(GtkButton *button, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(user_data);
  adw_overlay_split_view_set_show_sidebar(self->split_view,
    !adw_overlay_split_view_get_show_sidebar(self->split_view));
}

static GtkWidget *
build_sidebar(DkWindow *self)
{
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  GtkWidget *new_button = gtk_button_new_from_icon_name("document-new-symbolic");
  GtkWidget *scroll = gtk_scrolled_window_new();
  self->history_list = GTK_LIST_BOX(gtk_list_box_new());

  adw_header_bar_set_title_widget(header, gtk_label_new("Chats"));
  adw_header_bar_pack_end(header, new_button);
  gtk_widget_set_tooltip_text(new_button, "New Chat");
  gtk_list_box_set_selection_mode(self->history_list, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(self->history_list, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(self->history_list));
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
  adw_toolbar_view_set_content(toolbar, scroll);
  g_signal_connect(new_button, "clicked", G_CALLBACK(on_new_chat), self);
  g_signal_connect(self->history_list, "row-activated", G_CALLBACK(on_history_activated), self);
  return GTK_WIDGET(toolbar);
}

static GtkWidget *
build_content(DkWindow *self)
{
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
  GtkWidget *sidebar_button = gtk_button_new_from_icon_name("sidebar-show-symbolic");
  self->delete_button = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  GtkWidget *key_button = gtk_button_new_from_icon_name("dialog-password-symbolic");
  self->chat_title = GTK_LABEL(gtk_label_new("DKChatebras"));
  GtkWidget *models_widget = gtk_drop_down_new_from_strings(models);
  self->model_dropdown = GTK_DROP_DOWN(models_widget);

  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->messages_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  self->messages_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 14));
  GtkWidget *composer_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  self->composer = GTK_TEXT_VIEW(gtk_text_view_new());
  self->send_button = GTK_BUTTON(gtk_button_new_from_icon_name("mail-send-symbolic"));
  self->stop_button = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-stop-symbolic"));
  GtkEventController *keys = gtk_event_controller_key_new();

  gtk_widget_add_css_class(GTK_WIDGET(self->chat_title), "heading");
  gtk_label_set_ellipsize(self->chat_title, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(GTK_WIDGET(self->chat_title), TRUE);
  adw_header_bar_set_title_widget(header, GTK_WIDGET(self->chat_title));
  adw_header_bar_pack_start(header, sidebar_button);
  adw_header_bar_pack_end(header, key_button);
  adw_header_bar_pack_end(header, GTK_WIDGET(self->delete_button));
  adw_header_bar_pack_end(header, models_widget);
  gtk_widget_set_tooltip_text(sidebar_button, "Toggle Sidebar");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->delete_button), "Delete Chat");
  gtk_widget_set_tooltip_text(key_button, "API Key");

  gtk_widget_set_margin_top(GTK_WIDGET(self->messages_box), 18);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self->messages_box), 18);
  gtk_widget_set_margin_start(GTK_WIDGET(self->messages_box), 18);
  gtk_widget_set_margin_end(GTK_WIDGET(self->messages_box), 18);
  gtk_widget_set_valign(GTK_WIDGET(self->messages_box), GTK_ALIGN_START);
  gtk_scrolled_window_set_child(self->messages_scroll, GTK_WIDGET(self->messages_box));
  gtk_widget_set_vexpand(GTK_WIDGET(self->messages_scroll), TRUE);

  gtk_text_view_set_wrap_mode(self->composer, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(self->composer, FALSE);
  gtk_widget_set_hexpand(GTK_WIDGET(self->composer), TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(self->composer), -1, 54);
  gtk_widget_add_css_class(composer_card, "composer");
  gtk_widget_set_margin_bottom(composer_card, 12);
  gtk_widget_set_margin_start(composer_card, 12);
  gtk_widget_set_margin_end(composer_card, 12);
  gtk_widget_set_valign(GTK_WIDGET(self->send_button), GTK_ALIGN_CENTER);
  gtk_widget_set_valign(GTK_WIDGET(self->stop_button), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(self->send_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->stop_button), "destructive-action");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->send_button), "Send (Enter)");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->stop_button), "Stop Generation");
  gtk_widget_set_visible(GTK_WIDGET(self->stop_button), FALSE);
  gtk_box_append(GTK_BOX(composer_card), GTK_WIDGET(self->composer));
  gtk_box_append(GTK_BOX(composer_card), GTK_WIDGET(self->send_button));
  gtk_box_append(GTK_BOX(composer_card), GTK_WIDGET(self->stop_button));
  gtk_box_append(GTK_BOX(page), GTK_WIDGET(self->messages_scroll));
  gtk_box_append(GTK_BOX(page), composer_card);
  adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
  adw_toolbar_view_set_content(toolbar, page);

  gtk_widget_add_controller(GTK_WIDGET(self->composer), keys);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_composer_key), self);
  g_signal_connect(self->send_button, "clicked", G_CALLBACK(on_send), self);
  g_signal_connect(self->stop_button, "clicked", G_CALLBACK(on_stop), self);
  g_signal_connect(self->delete_button, "clicked", G_CALLBACK(on_delete_chat), self);
  g_signal_connect(key_button, "clicked", G_CALLBACK(on_show_key), self);
  g_signal_connect(sidebar_button, "clicked", G_CALLBACK(on_toggle_sidebar), self);
  g_signal_connect(self->model_dropdown, "notify::selected", G_CALLBACK(on_model_changed), self);
  return GTK_WIDGET(toolbar);
}

static gboolean
on_close_request(GtkWindow *window, gpointer user_data)
{
  DkWindow *self = DK_WINDOW(window);
  self->closing = TRUE;
  if (self->secret_cancellable != NULL)
    g_cancellable_cancel(self->secret_cancellable);
  if (self->client != NULL)
    dk_cerebras_client_stop(self->client);
  return FALSE;
}

static void
dk_window_dispose(GObject *object)
{
  DkWindow *self = DK_WINDOW(object);
  if (self->secret_cancellable != NULL)
    g_cancellable_cancel(self->secret_cancellable);
  if (self->client != NULL)
    dk_cerebras_client_stop(self->client);
  g_clear_object(&self->secret_cancellable);
  g_clear_object(&self->client);
  g_clear_pointer(&self->current_chat, json_object_unref);
  g_clear_pointer(&self->stream_text, free_string);
  g_clear_pointer(&self->pending_prompt, g_free);
  G_OBJECT_CLASS(dk_window_parent_class)->dispose(object);
}

static void
dk_window_class_init(DkWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = dk_window_dispose;
}

static void
dk_window_init(DkWindow *self)
{
  self->client = dk_cerebras_client_new();
  g_signal_connect(self, "close-request", G_CALLBACK(on_close_request), NULL);
  self->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  self->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
  adw_overlay_split_view_set_sidebar(self->split_view, build_sidebar(self));
  adw_overlay_split_view_set_content(self->split_view, build_content(self));
  adw_overlay_split_view_set_min_sidebar_width(self->split_view, 220);
  adw_overlay_split_view_set_max_sidebar_width(self->split_view, 340);
  adw_toast_overlay_set_child(self->toast_overlay, GTK_WIDGET(self->split_view));
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), GTK_WIDGET(self->toast_overlay));

  AdwBreakpoint *breakpoint = adw_breakpoint_new(
    adw_breakpoint_condition_parse("max-width: 700sp"));
  adw_breakpoint_add_setters(breakpoint,
                             G_OBJECT(self->split_view), "collapsed", TRUE,
                             NULL);
  adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(self), breakpoint);

  reload_history(self);
  render_current_chat(self);
}

DkWindow *
dk_window_new(GtkApplication *application)
{
  return g_object_new(DK_TYPE_WINDOW,
                      "application", application,
                      "title", "DKChatebras",
                      "default-width", 1100,
                      "default-height", 760,
                      NULL);
}
