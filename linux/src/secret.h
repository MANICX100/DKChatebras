#ifndef DK_SECRET_H
#define DK_SECRET_H

#include <stdbool.h>
#include <stddef.h>

/* Returns newly allocated API key string (caller frees & zeroes), or NULL if none. */
char *dk_secret_lookup(void);

/* Saves API key to Secret Service (via secret-tool) if available, else 0600 file. */
bool dk_secret_store(const char *api_key, char *err, size_t err_cap, bool *used_keyring);

/* Removes saved API key from both Secret Service and local file. */
bool dk_secret_clear(void);

/* Securely zeroes and frees a key string. */
void dk_secret_free(char *key);

#endif
