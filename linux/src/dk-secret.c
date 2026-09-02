#include "dk-secret.h"

#include <libsecret/secret.h>

static const SecretSchema api_key_schema = {
  "io.github.dkchatebras.DKChatebras.ApiKey",
  SECRET_SCHEMA_NONE,
  {
    { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  },
};

void
dk_secret_lookup_async(GCancellable *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer user_data)
{
  secret_password_lookup(&api_key_schema,
                         cancellable,
                         callback,
                         user_data,
                         "service", "cerebras-api",
                         NULL);
}

char *
dk_secret_lookup_finish(GAsyncResult *result, GError **error)
{
  return secret_password_lookup_finish(result, error);
}

void
dk_secret_password_free(char *password)
{
  secret_password_free(password);
}

void
dk_secret_store_async(const char *api_key,
                      GCancellable *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
  secret_password_store(&api_key_schema,
                        SECRET_COLLECTION_DEFAULT,
                        "DKChatebras Cerebras API key",
                        api_key,
                        cancellable,
                        callback,
                        user_data,
                        "service", "cerebras-api",
                        NULL);
}

gboolean
dk_secret_store_finish(GAsyncResult *result, GError **error)
{
  return secret_password_store_finish(result, error);
}
