#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

void dk_secret_lookup_async(GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data);
char *dk_secret_lookup_finish(GAsyncResult *result, GError **error);
void dk_secret_password_free(char *password);
void dk_secret_store_async(const char *api_key,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data);
gboolean dk_secret_store_finish(GAsyncResult *result, GError **error);

G_END_DECLS
