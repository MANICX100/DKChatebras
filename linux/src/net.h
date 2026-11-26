#ifndef DK_NET_H
#define DK_NET_H

#include "store.h"
#include "str.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct DkNetClient DkNetClient;

DkNetClient *dk_net_new(void);
void dk_net_free(DkNetClient *client);

/* Returns a pollable read fd that becomes readable when chunks or completion arrive. */
int dk_net_wake_fd(const DkNetClient *client);

bool dk_net_start_stream(DkNetClient *client,
                         const char *api_key,
                         const char *model,
                         const DkMessage *messages,
                         size_t msg_count);

void dk_net_stop(DkNetClient *client);
bool dk_net_is_running(const DkNetClient *client);

/* Drains newly accumulated chunk text into out_chunk.
 * Returns true if streaming finished; sets *out_cancelled and fills err_buf on error. */
bool dk_net_poll(DkNetClient *client,
                 DkStr *out_chunk,
                 bool *out_cancelled,
                 char *err_buf,
                 size_t err_cap);

#endif
