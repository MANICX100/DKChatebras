#define _POSIX_C_SOURCE 200809L

#include "store.h"
#include "json.h"
#include "str.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap > 0) {
        snprintf(err, cap, "%s", msg ? msg : "Unknown error");
    }
}

static char *get_chats_dir(void) {
    const char *xdg = getenv("XDG_DATA_HOME");
    DkStr p;
    dk_str_init_cap(&p, 128);
    if (xdg && *xdg) {
        dk_str_appendf(&p, "%s/dkchatebras/chats", xdg);
    } else {
        const char *home = getenv("HOME");
        dk_str_appendf(&p, "%s/.local/share/dkchatebras/chats", home ? home : ".");
    }
    return dk_str_steal(&p);
}

static bool mkdir_p(const char *path, mode_t mode) {
    char *tmp = dk_strdup(path);
    if (!tmp) return false;
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                free(tmp);
                return false;
            }
            *p = '/';
        }
    }
    bool ok = (mkdir(tmp, mode) == 0 || errno == EEXIST);
    free(tmp);
    return ok;
}

static bool write_file_atomic(const char *path, const char *data, size_t len, char *err, size_t err_cap) {
    DkStr tmp_path;
    dk_str_init_cap(&tmp_path, strlen(path) + 32);
    dk_str_appendf(&tmp_path, "%s.tmp.%ld.%u", path, (long)getpid(), (unsigned)time(NULL));

    int fd = open(tmp_path.data, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "Could not write %s: %s", path, strerror(errno));
        dk_str_free(&tmp_path);
        return false;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err && err_cap)
                snprintf(err, err_cap, "Write failed: %s", strerror(errno));
            close(fd);
            unlink(tmp_path.data);
            dk_str_free(&tmp_path);
            return false;
        }
        written += (size_t)n;
    }
    fchmod(fd, 0600);
    fsync(fd);
    close(fd);

    if (rename(tmp_path.data, path) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "Rename failed: %s", strerror(errno));
        unlink(tmp_path.data);
        dk_str_free(&tmp_path);
        return false;
    }

    dk_str_free(&tmp_path);
    return true;
}

static char *read_file_all(const char *path, size_t *out_len, char *err, size_t err_cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "Could not open %s: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    size_t init_cap = 4096;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (64 * 1024 * 1024))
        init_cap = (size_t)st.st_size + 1;

    DkStr buf;
    dk_str_init_cap(&buf, init_cap);
    char chunk[8192];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            dk_str_free(&buf);
            set_err(err, err_cap, "Failed reading file");
            return NULL;
        }
        if (n == 0) break;
        dk_str_append_len(&buf, chunk, (size_t)n);
    }
    close(fd);
    *out_len = buf.len;
    return dk_str_steal(&buf);
}

static char *generate_uuid_v4(void) {
    uint8_t b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    bool got = false;
    if (fd >= 0) {
        got = (read(fd, b, sizeof(b)) == (ssize_t)sizeof(b));
        close(fd);
    }
    if (!got) {
        uint64_t t = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        for (int i = 0; i < 16; i++) {
            t = t * 6364136223846793005ULL + 1442695040888963407ULL;
            b[i] = (uint8_t)(t >> 56);
        }
    }
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);
    char *out = malloc(37);
    if (!out) return NULL;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

void dk_meta_list_free(DkMetaList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].id);
        free(list->items[i].title);
        free(list->items[i].model);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static int cmp_meta_asc(const void *a, const void *b) {
    const DkChatMeta *la = (const DkChatMeta *)a;
    const DkChatMeta *lb = (const DkChatMeta *)b;
    return (la->updated_at > lb->updated_at) - (la->updated_at < lb->updated_at);
}

bool dk_store_load_index(DkMetaList *out_list, char *err, size_t err_cap) {
    out_list->items = NULL;
    out_list->count = 0;

    char *dir = get_chats_dir();
    DkStr path;
    dk_str_init_cap(&path, strlen(dir) + 16);
    dk_str_appendf(&path, "%s/index.json", dir);
    free(dir);

    struct stat st;
    if (stat(path.data, &st) != 0) {
        dk_str_free(&path);
        return true; /* empty index */
    }

    size_t len = 0;
    char *raw = read_file_all(path.data, &len, err, err_cap);
    dk_str_free(&path);
    if (!raw) return false;

    DkJson *root = dk_json_parse(raw, len);
    free(raw);
    if (!root || root->type != DK_JSON_ARRAY) {
        dk_json_free(root);
        set_err(err, err_cap, "History index is not a valid JSON array");
        return false;
    }

    size_t n = root->u.array.len;
    if (n > 0) {
        out_list->items = calloc(n, sizeof(DkChatMeta));
        for (size_t i = 0; i < n; i++) {
            DkJson *obj = root->u.array.items[i];
            if (!obj || obj->type != DK_JSON_OBJECT) continue;
            const char *id = dk_json_get_str(obj, "id", NULL);
            if (!id || !*id) continue;
            DkChatMeta *m = &out_list->items[out_list->count++];
            m->id = dk_strdup(id);
            m->title = dk_strdup(dk_json_get_str(obj, "title", "New Chat"));
            m->model = dk_strdup(dk_json_get_str(obj, "model", "gpt-oss-120b"));
            m->updated_at = dk_json_get_int(obj, "updated_at", 0);
        }
    }
    dk_json_free(root);
    return true;
}

DkChat *dk_chat_new(const char *model) {
    DkChat *c = calloc(1, sizeof(DkChat));
    if (!c) return NULL;
    c->id = generate_uuid_v4();
    c->title = dk_strdup("New Chat");
    c->model = dk_strdup(model && *model ? model : "gpt-oss-120b");
    c->updated_at = (int64_t)time(NULL);
    return c;
}

DkChat *dk_chat_clone(const DkChat *src) {
    if (!src) return NULL;
    DkChat *c = calloc(1, sizeof(DkChat));
    if (!c) return NULL;
    c->id = dk_strdup(src->id);
    c->title = dk_strdup(src->title);
    c->model = dk_strdup(src->model);
    c->updated_at = src->updated_at;
    if (src->msg_count > 0) {
        c->messages = calloc(src->msg_count, sizeof(DkMessage));
        c->msg_cap = src->msg_count;
        c->msg_count = src->msg_count;
        for (size_t i = 0; i < src->msg_count; i++) {
            c->messages[i].role = dk_strdup(src->messages[i].role);
            c->messages[i].content = dk_strdup(src->messages[i].content);
        }
    }
    return c;
}

void dk_chat_free(DkChat *chat) {
    if (!chat) return;
    free(chat->id);
    free(chat->title);
    free(chat->model);
    for (size_t i = 0; i < chat->msg_count; i++) {
        free(chat->messages[i].role);
        free(chat->messages[i].content);
    }
    free(chat->messages);
    free(chat);
}

bool dk_chat_append(DkChat *chat, const char *role, const char *content) {
    if (!chat) return false;
    if (chat->msg_count == chat->msg_cap) {
        size_t ncap = chat->msg_cap ? chat->msg_cap * 2 : 8;
        DkMessage *p = realloc(chat->messages, ncap * sizeof(DkMessage));
        if (!p) return false;
        chat->messages = p;
        chat->msg_cap = ncap;
    }
    chat->messages[chat->msg_count].role = dk_strdup(role ? role : "user");
    chat->messages[chat->msg_count].content = dk_strdup(content ? content : "");
    chat->msg_count++;
    chat->updated_at = (int64_t)time(NULL);
    return true;
}

void dk_chat_set_title(DkChat *chat, const char *title) {
    if (!chat) return;
    free(chat->title);
    chat->title = dk_strdup(title && *title ? title : "New Chat");
}

void dk_chat_set_model(DkChat *chat, const char *model) {
    if (!chat) return;
    free(chat->model);
    chat->model = dk_strdup(model && *model ? model : "gpt-oss-120b");
}

static bool is_safe_id(const char *id) {
    if (!id || !*id) return false;
    for (const char *p = id; *p; p++) {
        if (*p == '/' || *p == '\\' || (*p == '.' && p[1] == '.'))
            return false;
    }
    return true;
}

DkChat *dk_store_load_chat(const char *id, char *err, size_t err_cap) {
    if (!is_safe_id(id)) {
        set_err(err, err_cap, "Invalid chat ID");
        return NULL;
    }
    char *dir = get_chats_dir();
    DkStr path;
    dk_str_init_cap(&path, strlen(dir) + strlen(id) + 16);
    dk_str_appendf(&path, "%s/%s.json", dir, id);
    free(dir);

    size_t len = 0;
    char *raw = read_file_all(path.data, &len, err, err_cap);
    dk_str_free(&path);
    if (!raw) return NULL;

    DkJson *root = dk_json_parse(raw, len);
    free(raw);
    if (!root || root->type != DK_JSON_OBJECT) {
        dk_json_free(root);
        set_err(err, err_cap, "Chat file is not a valid JSON object");
        return NULL;
    }

    DkChat *chat = calloc(1, sizeof(DkChat));
    chat->id = dk_strdup(dk_json_get_str(root, "id", id));
    chat->title = dk_strdup(dk_json_get_str(root, "title", "New Chat"));
    chat->model = dk_strdup(dk_json_get_str(root, "model", "gpt-oss-120b"));
    chat->updated_at = dk_json_get_int(root, "updated_at", 0);

    DkJson *msgs = dk_json_object_get(root, "messages");
    if (msgs && msgs->type == DK_JSON_ARRAY) {
        for (size_t i = 0; i < msgs->u.array.len; i++) {
            DkJson *m = msgs->u.array.items[i];
            if (!m || m->type != DK_JSON_OBJECT) continue;
            const char *role = dk_json_get_str(m, "role", "user");
            const char *content = dk_json_get_str(m, "content", "");
            dk_chat_append(chat, role, content);
        }
        /* Restore original updated_at */
        chat->updated_at = dk_json_get_int(root, "updated_at", chat->updated_at);
    }

    dk_json_free(root);
    return chat;
}

static pthread_mutex_t g_store_io_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool save_index_locked(const DkChat *chat, const char *deleted_id, char *err, size_t err_cap) {
    DkMetaList list;
    if (!dk_store_load_index(&list, err, err_cap)) {
        /* If index was corrupt, start fresh */
        list.items = NULL;
        list.count = 0;
    }

    size_t cap = list.count + 2;
    DkChatMeta *next = calloc(cap, sizeof(DkChatMeta));
    size_t n = 0;
    bool replaced = false;

    for (size_t i = 0; i < list.count; i++) {
        if (deleted_id && strcmp(list.items[i].id, deleted_id) == 0) {
            continue;
        }
        if (chat && strcmp(list.items[i].id, chat->id) == 0) {
            next[n].id = dk_strdup(chat->id);
            next[n].title = dk_strdup(chat->title);
            next[n].model = dk_strdup(chat->model);
            next[n].updated_at = chat->updated_at;
            n++;
            replaced = true;
            continue;
        }
        next[n].id = dk_strdup(list.items[i].id);
        next[n].title = dk_strdup(list.items[i].title);
        next[n].model = dk_strdup(list.items[i].model);
        next[n].updated_at = list.items[i].updated_at;
        n++;
    }

    if (chat && !replaced) {
        next[n].id = dk_strdup(chat->id);
        next[n].title = dk_strdup(chat->title);
        next[n].model = dk_strdup(chat->model);
        next[n].updated_at = chat->updated_at;
        n++;
    }
    dk_meta_list_free(&list);

    if (n > 1)
        qsort(next, n, sizeof(DkChatMeta), cmp_meta_asc);

    DkJson *arr = dk_json_new_array();
    for (size_t i = 0; i < n; i++) {
        DkJson *obj = dk_json_new_object();
        dk_json_object_set(obj, "id", dk_json_new_string(next[i].id));
        dk_json_object_set(obj, "title", dk_json_new_string(next[i].title));
        dk_json_object_set(obj, "model", dk_json_new_string(next[i].model));
        dk_json_object_set(obj, "updated_at", dk_json_new_int(next[i].updated_at));
        dk_json_array_push(arr, obj);
        free(next[i].id);
        free(next[i].title);
        free(next[i].model);
    }
    free(next);

    DkStr out;
    dk_str_init_cap(&out, 256);
    dk_json_write(arr, &out, true);
    dk_str_push(&out, '\n');
    dk_json_free(arr);

    char *dir = get_chats_dir();
    if (!mkdir_p(dir, 0700)) {
        free(dir);
        dk_str_free(&out);
        set_err(err, err_cap, "Could not create history directory");
        return false;
    }

    DkStr path;
    dk_str_init_cap(&path, strlen(dir) + 16);
    dk_str_appendf(&path, "%s/index.json", dir);
    free(dir);

    bool ok = write_file_atomic(path.data, out.data, out.len, err, err_cap);
    dk_str_free(&path);
    dk_str_free(&out);
    return ok;
}

bool dk_store_save_chat_sync(const DkChat *chat, char *err, size_t err_cap) {
    if (!chat || !is_safe_id(chat->id)) {
        set_err(err, err_cap, "Invalid chat");
        return false;
    }

    pthread_mutex_lock(&g_store_io_mtx);
    char *dir = get_chats_dir();
    if (!mkdir_p(dir, 0700)) {
        free(dir);
        pthread_mutex_unlock(&g_store_io_mtx);
        set_err(err, err_cap, "Could not create history directory");
        return false;
    }

    DkJson *root = dk_json_new_object();
    dk_json_object_set(root, "id", dk_json_new_string(chat->id));
    dk_json_object_set(root, "title", dk_json_new_string(chat->title));
    dk_json_object_set(root, "model", dk_json_new_string(chat->model));
    dk_json_object_set(root, "updated_at", dk_json_new_int(chat->updated_at));
    DkJson *msgs = dk_json_new_array();
    for (size_t i = 0; i < chat->msg_count; i++) {
        DkJson *m = dk_json_new_object();
        dk_json_object_set(m, "role", dk_json_new_string(chat->messages[i].role));
        dk_json_object_set(m, "content", dk_json_new_string(chat->messages[i].content));
        dk_json_array_push(msgs, m);
    }
    dk_json_object_set(root, "messages", msgs);

    DkStr out;
    dk_str_init_cap(&out, 512);
    dk_json_write(root, &out, true);
    dk_str_push(&out, '\n');
    dk_json_free(root);

    DkStr path;
    dk_str_init_cap(&path, strlen(dir) + strlen(chat->id) + 16);
    dk_str_appendf(&path, "%s/%s.json", dir, chat->id);
    free(dir);

    bool ok = write_file_atomic(path.data, out.data, out.len, err, err_cap);
    dk_str_free(&path);
    dk_str_free(&out);

    if (ok)
        ok = save_index_locked(chat, NULL, err, err_cap);

    pthread_mutex_unlock(&g_store_io_mtx);
    return ok;
}

bool dk_store_delete_chat(const char *id, char *err, size_t err_cap) {
    if (!is_safe_id(id)) {
        set_err(err, err_cap, "Invalid chat ID");
        return false;
    }

    pthread_mutex_lock(&g_store_io_mtx);
    char *dir = get_chats_dir();
    DkStr path;
    dk_str_init_cap(&path, strlen(dir) + strlen(id) + 16);
    dk_str_appendf(&path, "%s/%s.json", dir, id);
    free(dir);

    if (unlink(path.data) != 0 && errno != ENOENT) {
        if (err && err_cap)
            snprintf(err, err_cap, "Could not delete chat: %s", strerror(errno));
        dk_str_free(&path);
        pthread_mutex_unlock(&g_store_io_mtx);
        return false;
    }
    dk_str_free(&path);

    bool ok = save_index_locked(NULL, id, err, err_cap);
    pthread_mutex_unlock(&g_store_io_mtx);
    return ok;
}

/* Background writer thread so saving never blocks UI frames */
typedef struct SaveJob {
    DkChat *chat;
    struct SaveJob *next;
} SaveJob;

static pthread_t g_writer_thread;
static pthread_mutex_t g_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_cv = PTHREAD_COND_INITIALIZER;
static SaveJob *g_q_head = NULL;
static SaveJob *g_q_tail = NULL;
static bool g_writer_started = false;
static bool g_writer_stop = false;

static void *writer_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_q_mtx);
        while (!g_q_head && !g_writer_stop)
            pthread_cond_wait(&g_q_cv, &g_q_mtx);
        SaveJob *job = g_q_head;
        if (job) {
            g_q_head = job->next;
            if (!g_q_head) g_q_tail = NULL;
        }
        bool done = (!job && g_writer_stop);
        pthread_mutex_unlock(&g_q_mtx);

        if (done) break;
        if (job) {
            char err[128];
            dk_store_save_chat_sync(job->chat, err, sizeof(err));
            dk_chat_free(job->chat);
            free(job);
        }
    }
    return NULL;
}

void dk_store_init(void) {
    pthread_mutex_lock(&g_q_mtx);
    if (!g_writer_started) {
        g_writer_stop = false;
        if (pthread_create(&g_writer_thread, NULL, writer_main, NULL) == 0)
            g_writer_started = true;
    }
    pthread_mutex_unlock(&g_q_mtx);
}

void dk_store_shutdown(void) {
    pthread_mutex_lock(&g_q_mtx);
    bool started = g_writer_started;
    g_writer_stop = true;
    pthread_cond_signal(&g_q_cv);
    pthread_mutex_unlock(&g_q_mtx);

    if (started) {
        pthread_join(g_writer_thread, NULL);
        g_writer_started = false;
    }
}

void dk_store_save_chat_async(const DkChat *chat) {
    if (!chat) return;
    DkChat *copy = dk_chat_clone(chat);
    if (!copy) return;

    pthread_mutex_lock(&g_q_mtx);
    if (!g_writer_started) {
        pthread_mutex_unlock(&g_q_mtx);
        char err[128];
        dk_store_save_chat_sync(copy, err, sizeof(err));
        dk_chat_free(copy);
        return;
    }
    SaveJob *job = malloc(sizeof(SaveJob));
    if (!job) {
        pthread_mutex_unlock(&g_q_mtx);
        dk_chat_free(copy);
        return;
    }
    job->chat = copy;
    job->next = NULL;
    if (g_q_tail) g_q_tail->next = job;
    else g_q_head = job;
    g_q_tail = job;
    pthread_cond_signal(&g_q_cv);
    pthread_mutex_unlock(&g_q_mtx);
}
