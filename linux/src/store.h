#ifndef DK_STORE_H
#define DK_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *id;
    char *title;
    char *model;
    int64_t updated_at;
} DkChatMeta;

typedef struct {
    char *role;
    char *content;
} DkMessage;

typedef struct {
    char *id;
    char *title;
    char *model;
    int64_t updated_at;
    DkMessage *messages;
    size_t msg_count;
    size_t msg_cap;
} DkChat;

typedef struct {
    DkChatMeta *items;
    size_t count;
} DkMetaList;

void dk_store_init(void);
void dk_store_shutdown(void);

void dk_meta_list_free(DkMetaList *list);
bool dk_store_load_index(DkMetaList *out_list, char *err, size_t err_cap);

DkChat *dk_chat_new(const char *model);
DkChat *dk_chat_clone(const DkChat *chat);
void dk_chat_free(DkChat *chat);
bool dk_chat_append(DkChat *chat, const char *role, const char *content);
void dk_chat_set_title(DkChat *chat, const char *title);
void dk_chat_set_model(DkChat *chat, const char *model);

DkChat *dk_store_load_chat(const char *id, char *err, size_t err_cap);
bool dk_store_save_chat_sync(const DkChat *chat, char *err, size_t err_cap);
void dk_store_save_chat_async(const DkChat *chat);
bool dk_store_delete_chat(const char *id, char *err, size_t err_cap);

#endif
