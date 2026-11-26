#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "gfx.h"
#include "layout.h"
#include "markdown.h"
#include "net.h"
#include "secret.h"
#include "store.h"
#include "str.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Color Palette (cohesive with DKChatebras dark aesthetic) */
#define COL_BG             DK_RGB(8, 11, 15)
#define COL_SIDEBAR        DK_RGB(12, 16, 21)
#define COL_TOPBAR         DK_RGB(10, 14, 19)
#define COL_CARD           DK_RGB(17, 22, 29)
#define COL_CARD_HOVER     DK_RGB(23, 30, 39)
#define COL_USER_CARD      DK_RGB(24, 20, 24)
#define COL_USER_BORDER    DK_RGB(58, 37, 36)
#define COL_COMPOSER       DK_RGB(15, 20, 27)
#define COL_INPUT_BG       DK_RGB(10, 14, 19)
#define COL_BORDER         DK_RGB(30, 38, 48)
#define COL_BORDER_SOFT    DK_RGB(22, 28, 36)
#define COL_TEXT           DK_RGB(241, 244, 247)
#define COL_TEXT_SUB       DK_RGB(142, 154, 168)
#define COL_TEXT_DIM       DK_RGB(92, 104, 118)
#define COL_ACCENT         DK_RGB(255, 91, 56)
#define COL_ACCENT_HOVER   DK_RGB(255, 113, 82)
#define COL_ACCENT_SOFT    DK_RGBA(255, 91, 56, 34)
#define COL_SUCCESS        DK_RGB(100, 215, 149)
#define COL_DANGER         DK_RGB(224, 79, 79)
#define COL_DANGER_SOFT    DK_RGBA(224, 79, 79, 42)
#define COL_SEL_BG         DK_RGBA(255, 91, 56, 82)
#define COL_SCRIM          DK_RGBA(0, 0, 0, 150)
#define COL_SCROLL_THUMB   DK_RGB(48, 58, 70)
#define COL_SCROLL_HOVER   DK_RGB(72, 86, 102)

#define SIDEBAR_WIDTH      268
#define TOPBAR_HEIGHT      54
#define COLLAPSE_BREAKPT   740
#define MAX_CHAT_COL_W     820

static const char *const MODELS[] = {
    "gpt-oss-120b",
    "gemma-4-31b",
};
#define MODEL_COUNT (sizeof(MODELS) / sizeof(MODELS[0]))

static const char *const STARTER_PROMPTS[3] = {
    "Compare quicksort vs mergesort in a concise Markdown table",
    "Explain how C LTO and inlining speed up native binaries",
    "Derive the monthly break-even formula cleanly with math",
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static bool pt_in_rect(int x, int y, DkRect r) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void open_url_external(const char *url, size_t len) {
    if (!url || len == 0) return;
    if (!(len >= 7 && (memcmp(url, "http://", 7) == 0 ||
          (len >= 8 && memcmp(url, "https://", 8) == 0))))
        return;
    char *copy = dk_strdup_len(url, len);
    if (!copy) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        char *const argv[] = { "xdg-open", copy, NULL };
        execvp("xdg-open", argv);
        _exit(127);
    }
    free(copy);
}

/* ---- Reusable UTF-8 Text Input (Single-line & Multi-line) ---- */
typedef struct {
    DkStr text;
    size_t caret;
    size_t sel_anchor;
    bool multiline;
    bool masked;
    int scroll_x;
} TextInput;

static void ti_init(TextInput *ti, bool multiline, bool masked) {
    dk_str_init(&ti->text);
    ti->caret = 0;
    ti->sel_anchor = 0;
    ti->multiline = multiline;
    ti->masked = masked;
    ti->scroll_x = 0;
}

static void ti_free(TextInput *ti) {
    if (ti->masked && ti->text.data)
        memset(ti->text.data, 0, ti->text.cap);
    dk_str_free(&ti->text);
}

static void ti_clear(TextInput *ti) {
    if (ti->masked && ti->text.data)
        memset(ti->text.data, 0, ti->text.cap);
    dk_str_clear(&ti->text);
    ti->caret = 0;
    ti->sel_anchor = 0;
    ti->scroll_x = 0;
}

static void ti_set(TextInput *ti, const char *s) {
    ti_clear(ti);
    if (s && *s) {
        dk_str_append(&ti->text, s);
        ti->caret = ti->text.len;
        ti->sel_anchor = ti->caret;
    }
}

static bool ti_has_sel(const TextInput *ti) {
    return ti->caret != ti->sel_anchor;
}

static void ti_sel_bounds(const TextInput *ti, size_t *a, size_t *b) {
    if (ti->caret <= ti->sel_anchor) { *a = ti->caret; *b = ti->sel_anchor; }
    else { *a = ti->sel_anchor; *b = ti->caret; }
}

static void ti_delete_sel(TextInput *ti) {
    if (!ti_has_sel(ti)) return;
    size_t a, b;
    ti_sel_bounds(ti, &a, &b);
    dk_str_erase(&ti->text, a, b - a);
    ti->caret = a;
    ti->sel_anchor = a;
}

static void ti_insert(TextInput *ti, const char *s, size_t len) {
    if (len == 0) return;
    ti_delete_sel(ti);
    DkStr clean;
    dk_str_init_cap(&clean, len);
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\r') {
            if (ti->multiline) {
                dk_str_push(&clean, '\n');
                if (i + 1 < len && s[i + 1] == '\n') i++;
            }
            continue;
        }
        if (c == '\n' && !ti->multiline) continue;
        if ((unsigned char)c < 0x20 && c != '\n' && c != '\t') continue;
        dk_str_push(&clean, c);
    }
    if (clean.len > 0) {
        dk_str_reserve(&ti->text, clean.len);
        memmove(ti->text.data + ti->caret + clean.len,
                ti->text.data + ti->caret,
                ti->text.len - ti->caret + 1);
        memcpy(ti->text.data + ti->caret, clean.data, clean.len);
        ti->text.len += clean.len;
        ti->caret += clean.len;
        ti->sel_anchor = ti->caret;
    }
    dk_str_free(&clean);
}

static void ti_backspace(TextInput *ti, bool whole_word) {
    if (ti_has_sel(ti)) { ti_delete_sel(ti); return; }
    if (ti->caret == 0 || !ti->text.data) return;
    const char *begin = ti->text.data;
    const char *p = begin + ti->caret;
    if (whole_word) {
        while (p > begin && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n'))
            p = dk_utf8_prev(begin, p);
        while (p > begin && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n')
            p = dk_utf8_prev(begin, p);
    } else {
        p = dk_utf8_prev(begin, p);
    }
    size_t npos = p ? (size_t)(p - begin) : 0;
    dk_str_erase(&ti->text, npos, ti->caret - npos);
    ti->caret = npos;
    ti->sel_anchor = npos;
}

static void ti_delete_fwd(TextInput *ti) {
    if (ti_has_sel(ti)) { ti_delete_sel(ti); return; }
    if (!ti->text.data || ti->caret >= ti->text.len) return;
    const char *next = dk_utf8_next(ti->text.data + ti->caret, ti->text.data + ti->text.len);
    size_t adv = (size_t)(next - (ti->text.data + ti->caret));
    dk_str_erase(&ti->text, ti->caret, adv);
}

static void ti_move_left(TextInput *ti, bool shift, bool word) {
    if (!shift && ti_has_sel(ti)) {
        size_t a, b; ti_sel_bounds(ti, &a, &b);
        ti->caret = ti->sel_anchor = a;
        return;
    }
    if (!ti->text.data || ti->caret == 0) return;
    const char *begin = ti->text.data;
    const char *p = begin + ti->caret;
    if (word) {
        while (p > begin && (p[-1] == ' ' || p[-1] == '\t')) p = dk_utf8_prev(begin, p);
        while (p > begin && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n') p = dk_utf8_prev(begin, p);
    } else {
        p = dk_utf8_prev(begin, p);
    }
    ti->caret = p ? (size_t)(p - begin) : 0;
    if (!shift) ti->sel_anchor = ti->caret;
}

static void ti_move_right(TextInput *ti, bool shift, bool word) {
    if (!shift && ti_has_sel(ti)) {
        size_t a, b; ti_sel_bounds(ti, &a, &b);
        ti->caret = ti->sel_anchor = b;
        return;
    }
    if (!ti->text.data || ti->caret >= ti->text.len) return;
    const char *end = ti->text.data + ti->text.len;
    const char *p = ti->text.data + ti->caret;
    if (word) {
        while (p < end && (*p == ' ' || *p == '\t')) p = dk_utf8_next(p, end);
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n') p = dk_utf8_next(p, end);
    } else {
        p = dk_utf8_next(p, end);
    }
    ti->caret = (size_t)(p - ti->text.data);
    if (!shift) ti->sel_anchor = ti->caret;
}

/* ---- Message Render Node ---- */
typedef struct {
    bool is_user;
    DkStr raw;
    DkStr stream_display; /* filtered plain text while streaming */
    DkMdDoc *doc;         /* non-NULL once markdown rendered */
    DkMsgLayout layout;
    int card_y;
    int card_h;
    int content_x;
    int content_y;
    int content_w;
    DkRect copy_btn;
    uint64_t copied_until_ms;
} UiMsg;

static void ui_msg_free(UiMsg *m) {
    dk_str_free(&m->raw);
    dk_str_free(&m->stream_display);
    dk_md_free(m->doc);
    dk_layout_free(&m->layout);
}

/* ---- Main App State ---- */
typedef struct {
    Display *dpy;
    int screen;
    Window win;
    XIM xim;
    XIC xic;
    DkGfx *gfx;
    int win_w, win_h;

    Atom atom_wm_delete;
    Atom atom_clipboard;
    Atom atom_primary;
    Atom atom_utf8;
    Atom atom_targets;
    Atom atom_clip_prop;
    Atom atom_net_wm_name;
    Atom atom_net_wm_pid;

    Cursor cur_default;
    Cursor cur_ibeam;
    Cursor cur_hand;
    Cursor cur_active;

    /* Clipboard storage when we own CLIPBOARD */
    DkStr clip_data;

    /* Data & networking */
    DkNetClient *net;
    DkMetaList history;
    DkChat *current_chat;
    UiMsg *msgs;
    size_t msg_count;
    size_t msg_cap;

    int model_idx;
    bool generating;
    bool has_api_key;
    bool key_checked;
    char *pending_prompt;

    /* Background async key lookup/check */
    pthread_t key_thread;
    bool key_thread_running;
    int key_pipe[2];
    char *fetched_key;
    bool key_action_send; /* true = lookup for send, false = initial status probe */

    /* Layout & UI state */
    bool sidebar_open;
    bool sidebar_collapsed;
    int history_scroll_y;
    int chat_scroll_y;
    int chat_content_h;
    bool auto_scroll;
    bool dragging_scrollbar;
    int scroll_drag_offset;

    /* Message text selection */
    int sel_msg_idx;
    size_t sel_anchor;
    size_t sel_caret;
    bool dragging_msg_sel;

    /* Composer & Modals */
    TextInput composer;
    bool composer_focused;
    int composer_h;

    bool model_menu_open;
    bool key_modal_open;
    bool key_peek;
    TextInput key_input;

    /* Toast */
    char toast_msg[256];
    uint64_t toast_until_ms;

    /* Mouse & hover state */
    int mouse_x, mouse_y;
    int hovered_hist_idx;
    int hovered_hist_del_idx;
    bool dirty;
    bool running;
    uint64_t last_blink_toggle;
    bool caret_visible;

    /* Computed hit rects (updated on paint/layout) */
    DkRect r_sidebar_toggle;
    DkRect r_new_chat;
    DkRect r_model_btn;
    DkRect r_key_btn;
    DkRect r_del_btn;
    DkRect r_send_btn;
    DkRect r_composer_box;
    DkRect r_composer_text;
    DkRect r_chat_viewport;
    DkRect r_chat_scrollbar;
    DkRect r_hist_viewport;
    DkRect r_starter[3];
    DkRect r_model_menu;
    DkRect r_model_items[MODEL_COUNT];
    DkRect r_modal_box;
    DkRect r_modal_input;
    DkRect r_modal_peek;
    DkRect r_modal_save;
    DkRect r_modal_clear;
    DkRect r_modal_cancel;
} App;

static void show_toast(App *app, const char *msg) {
    if (!msg || !*msg) return;
    snprintf(app->toast_msg, sizeof(app->toast_msg), "%s", msg);
    app->toast_until_ms = now_ms() + 3500;
    app->dirty = true;
}

static void set_cursor(App *app, Cursor c) {
    if (app->cur_active != c) {
        XDefineCursor(app->dpy, app->win, c);
        app->cur_active = c;
    }
}

static void clipboard_set(App *app, const char *s, size_t len) {
    dk_str_clear(&app->clip_data);
    dk_str_append_len(&app->clip_data, s, len);
    XSetSelectionOwner(app->dpy, app->atom_clipboard, app->win, CurrentTime);
    XSetSelectionOwner(app->dpy, app->atom_primary, app->win, CurrentTime);
    XFlush(app->dpy);
}

static void clipboard_request_paste(App *app) {
    Window owner = XGetSelectionOwner(app->dpy, app->atom_clipboard);
    if (owner == app->win) {
        TextInput *ti = app->key_modal_open ? &app->key_input : &app->composer;
        if (app->clip_data.len > 0) {
            ti_insert(ti, app->clip_data.data, app->clip_data.len);
            app->dirty = true;
        }
        return;
    }
    if (owner != None) {
        XConvertSelection(app->dpy, app->atom_clipboard, app->atom_utf8,
                          app->atom_clip_prop, app->win, CurrentTime);
        XFlush(app->dpy);
    }
}

static void clear_ui_msgs(App *app) {
    for (size_t i = 0; i < app->msg_count; i++)
        ui_msg_free(&app->msgs[i]);
    app->msg_count = 0;
    app->sel_msg_idx = -1;
    app->sel_anchor = 0;
    app->sel_caret = 0;
    app->chat_scroll_y = 0;
    app->chat_content_h = 0;
}

static UiMsg *append_ui_msg(App *app, bool is_user, const char *content, bool render_md) {
    if (app->msg_count == app->msg_cap) {
        size_t ncap = app->msg_cap ? app->msg_cap * 2 : 16;
        UiMsg *p = realloc(app->msgs, ncap * sizeof(UiMsg));
        if (!p) return NULL;
        app->msgs = p;
        app->msg_cap = ncap;
    }
    UiMsg *m = &app->msgs[app->msg_count++];
    memset(m, 0, sizeof(*m));
    m->is_user = is_user;
    dk_str_init(&m->raw);
    dk_str_init(&m->stream_display);
    dk_layout_init(&m->layout);
    if (content) {
        dk_str_append(&m->raw, content);
        if (render_md)
            m->doc = dk_md_parse(content);
        else
            dk_utf8_filter_problem_glyphs(&m->stream_display, content, strlen(content));
    }
    return m;
}

static void reload_history(App *app) {
    char err[256];
    dk_meta_list_free(&app->history);
    if (!dk_store_load_index(&app->history, err, sizeof(err)))
        show_toast(app, err);
    app->dirty = true;
}

static int find_model_index(const char *model) {
    if (!model) return 0;
    for (size_t i = 0; i < MODEL_COUNT; i++) {
        if (strcmp(MODELS[i], model) == 0)
            return (int)i;
    }
    return 0;
}

static void load_chat_into_ui(App *app, DkChat *chat) {
    dk_chat_free(app->current_chat);
    app->current_chat = chat;
    clear_ui_msgs(app);
    if (chat) {
        app->model_idx = find_model_index(chat->model);
        for (size_t i = 0; i < chat->msg_count; i++) {
            bool is_user = strcmp(chat->messages[i].role, "user") == 0;
            append_ui_msg(app, is_user, chat->messages[i].content, true);
        }
    }
    app->auto_scroll = true;
    app->dirty = true;
}

static void update_sidebar_collapse(App *app) {
    bool narrow = app->win_w < COLLAPSE_BREAKPT;
    if (narrow != app->sidebar_collapsed) {
        app->sidebar_collapsed = narrow;
        app->sidebar_open = !narrow;
    }
}

/* Async key worker so Secret Service lookup NEVER stalls the UI thread */
static void *key_lookup_worker(void *arg) {
    App *app = (App *)arg;
    char *key = dk_secret_lookup();
    app->fetched_key = key;
    char b = 1;
    ssize_t r = write(app->key_pipe[1], &b, 1);
    (void)r;
    return NULL;
}

static void start_async_key_lookup(App *app, bool for_send) {
    if (app->key_thread_running) return;
    app->key_action_send = for_send;
    app->fetched_key = NULL;
    if (pthread_create(&app->key_thread, NULL, key_lookup_worker, app) == 0)
        app->key_thread_running = true;
}

static void start_stream_with_key(App *app, const char *api_key) {
    if (!app->pending_prompt) return;
    const char *prompt = app->pending_prompt;
    const char *model = MODELS[app->model_idx];

    if (!app->current_chat)
        app->current_chat = dk_chat_new(model);

    if (app->current_chat->msg_count == 0) {
        /* Set title from first 48 UTF-8 codepoints */
        const char *p = prompt;
        const char *end = prompt + strlen(prompt);
        size_t chars = 0;
        while (p < end && chars < 48) {
            if (*p == '\n' || *p == '\r') break;
            p = dk_utf8_next(p, end);
            chars++;
        }
        char *title = dk_strdup_len(prompt, (size_t)(p - prompt));
        dk_chat_set_title(app->current_chat, title);
        free(title);
    }

    dk_chat_set_model(app->current_chat, model);
    dk_chat_append(app->current_chat, "user", prompt);
    append_ui_msg(app, true, prompt, true);
    append_ui_msg(app, false, "", false);

    free(app->pending_prompt);
    app->pending_prompt = NULL;

    dk_store_save_chat_async(app->current_chat);
    /* Update in-memory sidebar metadata immediately without blocking on disk */
    bool found = false;
    for (size_t i = 0; i < app->history.count; i++) {
        if (strcmp(app->history.items[i].id, app->current_chat->id) == 0) {
            free(app->history.items[i].title);
            free(app->history.items[i].model);
            app->history.items[i].title = dk_strdup(app->current_chat->title);
            app->history.items[i].model = dk_strdup(app->current_chat->model);
            app->history.items[i].updated_at = app->current_chat->updated_at;
            /* Move to end (newest) */
            DkChatMeta moved = app->history.items[i];
            memmove(&app->history.items[i], &app->history.items[i + 1],
                    (app->history.count - 1 - i) * sizeof(DkChatMeta));
            app->history.items[app->history.count - 1] = moved;
            found = true;
            break;
        }
    }
    if (!found) {
        DkChatMeta *p = realloc(app->history.items, (app->history.count + 1) * sizeof(DkChatMeta));
        if (p) {
            app->history.items = p;
            app->history.items[app->history.count++] = (DkChatMeta){
                .id = dk_strdup(app->current_chat->id),
                .title = dk_strdup(app->current_chat->title),
                .model = dk_strdup(app->current_chat->model),
                .updated_at = app->current_chat->updated_at,
            };
        }
    }

    app->auto_scroll = true;
    app->dirty = true;

    if (!dk_net_start_stream(app->net, api_key, model,
                             app->current_chat->messages, app->current_chat->msg_count)) {
        app->generating = false;
        show_toast(app, "Failed to start network stream");
    }
}

static void open_key_modal(App *app) {
    app->key_modal_open = true;
    app->model_menu_open = false;
    app->key_peek = false;
    ti_clear(&app->key_input);
    app->dirty = true;
}

static void begin_send_prompt(App *app, const char *raw_text) {
    if (app->generating || !raw_text) return;
    /* Trim whitespace */
    const char *s = raw_text;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    if (len == 0) return;

    free(app->pending_prompt);
    app->pending_prompt = dk_strdup_len(s, len);
    ti_clear(&app->composer);
    app->generating = true;
    app->dirty = true;
    start_async_key_lookup(app, true);
}

static void handle_key_lookup_done(App *app) {
    char drain[16];
    while (read(app->key_pipe[0], drain, sizeof(drain)) > 0) {}

    if (app->key_thread_running) {
        pthread_join(app->key_thread, NULL);
        app->key_thread_running = false;
    }

    char *key = app->fetched_key;
    app->fetched_key = NULL;
    app->key_checked = true;
    app->has_api_key = (key != NULL && *key != '\0');

    if (app->key_action_send) {
        if (!app->generating) {
            /* User stopped before key lookup finished */
            if (app->pending_prompt) {
                ti_set(&app->composer, app->pending_prompt);
                free(app->pending_prompt);
                app->pending_prompt = NULL;
            }
        } else if (!app->has_api_key) {
            if (app->pending_prompt) {
                ti_set(&app->composer, app->pending_prompt);
                free(app->pending_prompt);
                app->pending_prompt = NULL;
            }
            app->generating = false;
            open_key_modal(app);
            show_toast(app, "Add your Cerebras API key, then send again");
        } else {
            start_stream_with_key(app, key);
        }
    }

    dk_secret_free(key);
    app->dirty = true;
}

static void action_new_chat(App *app) {
    if (app->generating) {
        show_toast(app, "Stop the current response before starting a new chat");
        return;
    }
    DkChat *chat = dk_chat_new(MODELS[app->model_idx]);
    load_chat_into_ui(app, chat);
    app->composer_focused = true;
    app->model_menu_open = false;
    if (app->sidebar_collapsed) app->sidebar_open = false;
    app->dirty = true;
}

static void action_delete_chat_by_id(App *app, const char *id) {
    if (!id || app->generating) return;
    char err[256];
    if (!dk_store_delete_chat(id, err, sizeof(err))) {
        show_toast(app, err);
        return;
    }
    if (app->current_chat && strcmp(app->current_chat->id, id) == 0) {
        dk_chat_free(app->current_chat);
        app->current_chat = NULL;
        clear_ui_msgs(app);
    }
    reload_history(app);
    show_toast(app, "Chat deleted");
}

static void action_stop_generation(App *app) {
    if (!app->generating) return;
    dk_net_stop(app->net);
    if (app->key_thread_running) {
        app->generating = false;
        app->dirty = true;
    }
}

static void action_save_key(App *app) {
    const char *k = app->key_input.text.data ? app->key_input.text.data : "";
    while (*k == ' ' || *k == '\t') k++;
    size_t len = strlen(k);
    while (len > 0 && (k[len - 1] == ' ' || k[len - 1] == '\t')) len--;
    if (len == 0) {
        show_toast(app, "Enter an API key first");
        return;
    }
    char *trimmed = dk_strdup_len(k, len);
    char err[256];
    bool used_keyring = false;
    if (dk_secret_store(trimmed, err, sizeof(err), &used_keyring)) {
        app->has_api_key = true;
        app->key_checked = true;
        app->key_modal_open = false;
        ti_clear(&app->key_input);
        show_toast(app, used_keyring ? "API key saved to system keyring" : "API key saved locally (0600)");
    } else {
        show_toast(app, err);
    }
    dk_secret_free(trimmed);
    app->dirty = true;
}

static void action_clear_key(App *app) {
    dk_secret_clear();
    app->has_api_key = false;
    app->key_checked = true;
    ti_clear(&app->key_input);
    app->key_modal_open = false;
    show_toast(app, "API key removed");
    app->dirty = true;
}

/* ---- Layout Computation ---- */
static void compute_composer_geometry(App *app, int main_x, int main_w) {
    int col_w = main_w - 48;
    if (col_w > MAX_CHAT_COL_W) col_w = MAX_CHAT_COL_W;
    if (col_w < 240) col_w = main_w - 16;
    int col_x = main_x + (main_w - col_w) / 2;

    /* Count lines in composer to auto-grow smoothly up to 5 lines */
    int text_w = col_w - 72;
    if (text_w < 80) text_w = 80;
    DkFontMetrics fm = dk_gfx_font_metrics(app->gfx, DK_FONT_REGULAR, 15);
    int lh = fm.line_height + 4;
    int lines = 1;
    if (app->composer.text.len > 0) {
        const char *p = app->composer.text.data;
        const char *end = p + app->composer.text.len;
        int cur_w = 0;
        while (p < end) {
            if (*p == '\n') {
                lines++;
                cur_w = 0;
                p++;
                continue;
            }
            uint32_t cp = 0;
            p += dk_utf8_decode(p, end, &cp);
            int cw = dk_gfx_char_advance(app->gfx, DK_FONT_REGULAR, 15, cp);
            if (cur_w > 0 && cur_w + cw > text_w) {
                lines++;
                cur_w = cw;
            } else {
                cur_w += cw;
            }
        }
    }
    if (lines > 5) lines = 5;
    int box_h = lines * lh + 24;
    if (box_h < 54) box_h = 54;
    app->composer_h = box_h;

    app->r_composer_box = (DkRect){ col_x, app->win_h - box_h - 14, col_w, box_h };
    app->r_send_btn = (DkRect){ col_x + col_w - 44, app->r_composer_box.y + box_h - 44, 36, 36 };
    app->r_composer_text = (DkRect){ col_x + 16, app->r_composer_box.y + 12, col_w - 68, box_h - 24 };
}

static void relayout_messages(App *app) {
    int sb_w = (!app->sidebar_collapsed && app->sidebar_open) ? SIDEBAR_WIDTH : 0;
    int main_x = sb_w;
    int main_w = app->win_w - sb_w;
    compute_composer_geometry(app, main_x, main_w);

    app->r_chat_viewport = (DkRect){
        main_x, TOPBAR_HEIGHT,
        main_w, app->r_composer_box.y - TOPBAR_HEIGHT - 8
    };
    if (app->r_chat_viewport.h < 40) app->r_chat_viewport.h = 40;

    int col_w = main_w - 48;
    if (col_w > MAX_CHAT_COL_W) col_w = MAX_CHAT_COL_W;
    if (col_w < 240) col_w = main_w - 16;
    int col_x = main_x + (main_w - col_w) / 2;

    int y = 20;
    for (size_t i = 0; i < app->msg_count; i++) {
        UiMsg *m = &app->msgs[i];
        int card_x = m->is_user ? (col_x + (col_w > 440 ? 64 : 20)) : col_x;
        int card_w = m->is_user ? (col_w - (col_w > 440 ? 64 : 20)) : col_w;
        int inner_w = card_w - 32;
        if (inner_w < 80) inner_w = 80;

        if (m->layout.cached_width != inner_w) {
            const char *pt = m->stream_display.data ? m->stream_display.data : "";
            dk_layout_build(&m->layout, app->gfx, m->doc, pt, m->stream_display.len, inner_w);
        }

        m->card_y = y;
        m->content_x = card_x + 16;
        m->content_y = y + 34;
        m->content_w = inner_w;
        m->card_h = 34 + m->layout.total_height + 14;
        m->copy_btn = (DkRect){ card_x + card_w - 38, y + 8, 28, 24 };
        y += m->card_h + 14;
    }

    app->chat_content_h = y + 12;
    int max_scroll = app->chat_content_h - app->r_chat_viewport.h;
    if (max_scroll < 0) max_scroll = 0;
    if (app->auto_scroll)
        app->chat_scroll_y = max_scroll;
    else if (app->chat_scroll_y > max_scroll)
        app->chat_scroll_y = max_scroll;
    if (app->chat_scroll_y < 0)
        app->chat_scroll_y = 0;
}

/* ---- Rendering ---- */
static void paint_topbar(App *app, int main_x, int main_w) {
    DkGfx *g = app->gfx;
    dk_gfx_fill_rect(g, main_x, 0, main_w, TOPBAR_HEIGHT, COL_TOPBAR);
    dk_gfx_fill_rect(g, main_x, TOPBAR_HEIGHT - 1, main_w, 1, COL_BORDER_SOFT);

    /* Sidebar toggle */
    app->r_sidebar_toggle = (DkRect){ main_x + 14, 11, 32, 32 };
    bool hov_sb = pt_in_rect(app->mouse_x, app->mouse_y, app->r_sidebar_toggle);
    if (hov_sb)
        dk_gfx_fill_rrect(g, app->r_sidebar_toggle.x, app->r_sidebar_toggle.y, 32, 32, 7.0f, COL_CARD_HOVER);
    dk_gfx_draw_icon(g, DK_ICON_SIDEBAR, app->r_sidebar_toggle.x + 7, app->r_sidebar_toggle.y + 7, 18,
                     hov_sb ? COL_TEXT : COL_TEXT_SUB);

    /* Right-side controls: [Model v] [Key] [Delete] */
    int rx = main_x + main_w - 14;

    /* Delete button */
    rx -= 34;
    app->r_del_btn = (DkRect){ rx, 11, 32, 32 };
    bool can_del = (app->current_chat != NULL && !app->generating);
    bool hov_del = can_del && pt_in_rect(app->mouse_x, app->mouse_y, app->r_del_btn);
    if (hov_del)
        dk_gfx_fill_rrect(g, app->r_del_btn.x, app->r_del_btn.y, 32, 32, 7.0f, COL_DANGER_SOFT);
    dk_gfx_draw_icon(g, DK_ICON_TRASH, app->r_del_btn.x + 7, app->r_del_btn.y + 7, 18,
                     !can_del ? DK_RGB(56, 64, 74) : (hov_del ? COL_DANGER : COL_TEXT_SUB));

    /* API Key button */
    rx -= 8 + 78;
    app->r_key_btn = (DkRect){ rx, 11, 78, 32 };
    bool hov_key = pt_in_rect(app->mouse_x, app->mouse_y, app->r_key_btn);
    dk_gfx_fill_rrect(g, app->r_key_btn.x, app->r_key_btn.y, app->r_key_btn.w, 32, 7.0f,
                      hov_key ? COL_CARD_HOVER : COL_CARD);
    dk_gfx_stroke_rrect(g, app->r_key_btn.x, app->r_key_btn.y, app->r_key_btn.w, 32, 7.0f, COL_BORDER);
    dk_gfx_fill_circle(g, (float)(app->r_key_btn.x + 13), (float)(app->r_key_btn.y + 16), 3.5f,
                       app->has_api_key ? COL_SUCCESS : COL_ACCENT);
    dk_gfx_draw_text(g, app->r_key_btn.x + 23, app->r_key_btn.y + 21, DK_FONT_REGULAR, 13,
                     COL_TEXT, "API Key", 7);

    /* Model dropdown pill */
    const char *mname = MODELS[app->model_idx];
    int mw = dk_gfx_text_width(g, DK_FONT_MONO, 12, mname, strlen(mname)) + 34;
    if (mw < 126) mw = 126;
    rx -= 8 + mw;
    app->r_model_btn = (DkRect){ rx, 11, mw, 32 };
    bool hov_mod = !app->generating && pt_in_rect(app->mouse_x, app->mouse_y, app->r_model_btn);
    dk_gfx_fill_rrect(g, app->r_model_btn.x, app->r_model_btn.y, mw, 32, 7.0f,
                      (hov_mod || app->model_menu_open) ? COL_CARD_HOVER : COL_CARD);
    dk_gfx_stroke_rrect(g, app->r_model_btn.x, app->r_model_btn.y, mw, 32, 7.0f,
                        app->model_menu_open ? COL_ACCENT : COL_BORDER);
    dk_gfx_draw_text(g, app->r_model_btn.x + 11, app->r_model_btn.y + 20, DK_FONT_MONO, 12,
                     app->generating ? COL_TEXT_DIM : COL_TEXT, mname, strlen(mname));
    dk_gfx_draw_icon(g, DK_ICON_CHEVRON_DOWN, app->r_model_btn.x + mw - 22, app->r_model_btn.y + 10, 13,
                     COL_TEXT_SUB);

    /* Chat title */
    int tx = app->r_sidebar_toggle.x + 42;
    int tw = rx - tx - 14;
    if (tw > 40) {
        const char *title = app->current_chat ? app->current_chat->title : "DKChatebras";
        dk_gfx_draw_text_ellipsize(g, tx, 33, tw, DK_FONT_BOLD, 15, COL_TEXT, title, strlen(title));
    }
}

static void paint_sidebar(App *app) {
    if (!app->sidebar_open) return;
    DkGfx *g = app->gfx;

    if (app->sidebar_collapsed) {
        dk_gfx_fill_rect(g, SIDEBAR_WIDTH, 0, app->win_w - SIDEBAR_WIDTH, app->win_h, COL_SCRIM);
    }

    dk_gfx_fill_rect(g, 0, 0, SIDEBAR_WIDTH, app->win_h, COL_SIDEBAR);
    dk_gfx_fill_rect(g, SIDEBAR_WIDTH - 1, 0, 1, app->win_h, COL_BORDER_SOFT);

    /* Brand header */
    dk_gfx_draw_icon(g, DK_ICON_BRAND, 18, 16, 22, COL_ACCENT);
    dk_gfx_draw_text(g, 46, 33, DK_FONT_BOLD, 14, COL_TEXT, "DKCHATEBRAS", 11);

    /* New Chat button */
    app->r_new_chat = (DkRect){ 14, 56, SIDEBAR_WIDTH - 28, 40 };
    bool hov_new = pt_in_rect(app->mouse_x, app->mouse_y, app->r_new_chat);
    dk_gfx_fill_rrect(g, app->r_new_chat.x, app->r_new_chat.y, app->r_new_chat.w, 40, 9.0f,
                      hov_new ? COL_CARD_HOVER : COL_CARD);
    dk_gfx_stroke_rrect(g, app->r_new_chat.x, app->r_new_chat.y, app->r_new_chat.w, 40, 9.0f,
                        hov_new ? DK_RGB(56, 68, 82) : COL_BORDER);
    dk_gfx_draw_icon(g, DK_ICON_PLUS, app->r_new_chat.x + 12, app->r_new_chat.y + 11, 18, COL_ACCENT);
    dk_gfx_draw_text(g, app->r_new_chat.x + 36, app->r_new_chat.y + 25, DK_FONT_BOLD, 13,
                     COL_TEXT, "New Chat", 8);
    dk_gfx_draw_text(g, app->r_new_chat.x + app->r_new_chat.w - 48, app->r_new_chat.y + 24,
                     DK_FONT_MONO, 11, COL_TEXT_DIM, "Ctrl+N", 6);

    /* History heading */
    dk_gfx_draw_text(g, 20, 122, DK_FONT_BOLD, 10, COL_TEXT_DIM, "CONVERSATIONS", 13);

    int footer_h = 54;
    app->r_hist_viewport = (DkRect){ 0, 130, SIDEBAR_WIDTH - 1, app->win_h - 130 - footer_h };
    dk_gfx_clip_push(g, app->r_hist_viewport);

    app->hovered_hist_idx = -1;
    app->hovered_hist_del_idx = -1;

    int item_h = 54;
    int total_h = (int)app->history.count * item_h;
    int max_sc = total_h - app->r_hist_viewport.h;
    if (max_sc < 0) max_sc = 0;
    if (app->history_scroll_y > max_sc) app->history_scroll_y = max_sc;
    if (app->history_scroll_y < 0) app->history_scroll_y = 0;

    if (app->history.count == 0) {
        dk_gfx_draw_text(g, 20, 156, DK_FONT_REGULAR, 13, COL_TEXT_DIM, "No chats yet", 12);
    } else {
        /* Display newest first (reverse order of history.items) */
        for (size_t row = 0; row < app->history.count; row++) {
            size_t idx = app->history.count - 1 - row;
            const DkChatMeta *m = &app->history.items[idx];
            int iy = app->r_hist_viewport.y + (int)row * item_h - app->history_scroll_y;
            if (iy + item_h < app->r_hist_viewport.y || iy > app->r_hist_viewport.y + app->r_hist_viewport.h)
                continue;

            DkRect ir = { 10, iy + 2, SIDEBAR_WIDTH - 22, item_h - 4 };
            bool active = (app->current_chat && strcmp(app->current_chat->id, m->id) == 0);
            bool hov = pt_in_rect(app->mouse_x, app->mouse_y, app->r_hist_viewport) &&
                       pt_in_rect(app->mouse_x, app->mouse_y, ir);
            if (hov) app->hovered_hist_idx = (int)idx;

            if (active || hov) {
                dk_gfx_fill_rrect(g, ir.x, ir.y, ir.w, ir.h, 8.0f,
                                  active ? DK_RGB(22, 29, 38) : DK_RGB(17, 23, 30));
            }
            if (active) {
                dk_gfx_fill_rrect(g, ir.x, ir.y + 10, 3, ir.h - 20, 1.5f, COL_ACCENT);
            }

            int text_max_w = ir.w - (hov ? 44 : 20);
            dk_gfx_draw_text_ellipsize(g, ir.x + 12, ir.y + 21, text_max_w,
                                       active ? DK_FONT_BOLD : DK_FONT_REGULAR, 13,
                                       active ? COL_TEXT : COL_TEXT_SUB,
                                       m->title, strlen(m->title));

            char sub[64];
            time_t t = (time_t)m->updated_at;
            struct tm ltm;
            if (m->updated_at > 0 && localtime_r(&t, &ltm)) {
                strftime(sub, sizeof(sub), "%b %e \xC2\xB7 %H:%M", &ltm);
            } else {
                snprintf(sub, sizeof(sub), "%s", m->model);
            }
            dk_gfx_draw_text_ellipsize(g, ir.x + 12, ir.y + 39, text_max_w,
                                       DK_FONT_REGULAR, 11, COL_TEXT_DIM, sub, strlen(sub));

            if (hov && !app->generating) {
                DkRect dr = { ir.x + ir.w - 30, ir.y + 12, 24, 24 };
                bool hdel = pt_in_rect(app->mouse_x, app->mouse_y, dr);
                if (hdel) {
                    app->hovered_hist_del_idx = (int)idx;
                    dk_gfx_fill_rrect(g, dr.x, dr.y, dr.w, dr.h, 5.0f, COL_DANGER_SOFT);
                }
                dk_gfx_draw_icon(g, DK_ICON_TRASH, dr.x + 4, dr.y + 4, 15,
                                 hdel ? COL_DANGER : COL_TEXT_DIM);
            }
        }
    }
    dk_gfx_clip_pop(g);

    /* Sidebar footer */
    int fy = app->win_h - footer_h;
    dk_gfx_fill_rect(g, 14, fy, SIDEBAR_WIDTH - 28, 1, COL_BORDER_SOFT);
    dk_gfx_fill_circle(g, 24.0f, (float)(fy + 27), 4.0f, app->has_api_key ? COL_SUCCESS : COL_ACCENT);
    const char *st_title = app->has_api_key ? "Cerebras Ready" : "API Key Needed";
    dk_gfx_draw_text(g, 36, fy + 24, DK_FONT_BOLD, 12, COL_TEXT, st_title, strlen(st_title));
    dk_gfx_draw_text(g, 36, fy + 40, DK_FONT_MONO, 10, COL_TEXT_DIM,
                     MODELS[app->model_idx], strlen(MODELS[app->model_idx]));
}

static void paint_empty_state(App *app) {
    DkGfx *g = app->gfx;
    DkRect vp = app->r_chat_viewport;
    int cx = vp.x + vp.w / 2;
    int cy = vp.y + vp.h / 2 - 36;
    if (cy < vp.y + 84) cy = vp.y + 84;

    /* Subtle orbit circle + brand mark */
    dk_gfx_fill_circle(g, (float)cx, (float)(cy - 44), 34.0f, COL_CARD);
    dk_gfx_stroke_rrect(g, cx - 34, cy - 78, 68, 68, 34.0f, COL_BORDER);
    dk_gfx_draw_icon(g, DK_ICON_BRAND, cx - 15, cy - 59, 30, COL_ACCENT);

    const char *heading = "DKChatebras";
    int hw = dk_gfx_text_width(g, DK_FONT_BOLD, 28, heading, strlen(heading));
    dk_gfx_draw_text(g, cx - hw / 2, cy + 26, DK_FONT_BOLD, 28, COL_TEXT, heading, strlen(heading));

    const char *sub = "Native Linux Cerebras client \xC2\xB7 Direct HTTPS streaming";
    int sw = dk_gfx_text_width(g, DK_FONT_REGULAR, 14, sub, strlen(sub));
    dk_gfx_draw_text(g, cx - sw / 2, cy + 52, DK_FONT_REGULAR, 14, COL_TEXT_SUB, sub, strlen(sub));

    /* 3 Starter prompt cards */
    int avail_w = vp.w - 48;
    if (avail_w > 640) avail_w = 640;
    int card_w = (avail_w - 20) / 3;
    bool stacked = (card_w < 165);
    int start_y = cy + 80;

    for (int i = 0; i < 3; i++) {
        DkRect r;
        if (stacked) {
            int w = avail_w > 340 ? 340 : avail_w;
            r = (DkRect){ cx - w / 2, start_y + i * 52, w, 44 };
        } else {
            int total = card_w * 3 + 20;
            r = (DkRect){ cx - total / 2 + i * (card_w + 10), start_y, card_w, 78 };
        }
        app->r_starter[i] = r;
        bool hov = pt_in_rect(app->mouse_x, app->mouse_y, r);
        dk_gfx_fill_rrect(g, r.x, r.y, r.w, r.h, 10.0f, hov ? COL_CARD_HOVER : COL_CARD);
        dk_gfx_stroke_rrect(g, r.x, r.y, r.w, r.h, 10.0f, hov ? DK_RGB(62, 74, 88) : COL_BORDER);

        /* Wrap starter text nicely across up to 3 lines */
        const char *p = STARTER_PROMPTS[i];
        const char *end = p + strlen(p);
        int ty = r.y + (stacked ? 26 : 24);
        int max_lines = stacked ? 1 : 3;
        for (int ln = 0; ln < max_lines && p < end; ln++) {
            const char *ls = p;
            const char *last_sp = NULL;
            int w = 0;
            while (p < end) {
                uint32_t cp = 0;
                size_t adv = dk_utf8_decode(p, end, &cp);
                int cw = dk_gfx_char_advance(g, DK_FONT_REGULAR, 12, cp);
                if (w > 0 && w + cw > r.w - 24) {
                    if (last_sp && ln + 1 < max_lines) p = last_sp;
                    break;
                }
                w += cw;
                p += adv;
                if (cp == ' ') last_sp = p;
            }
            if (ln + 1 == max_lines && p < end)
                dk_gfx_draw_text_ellipsize(g, r.x + 12, ty, r.w - 24, DK_FONT_REGULAR, 12,
                                           hov ? COL_TEXT : COL_TEXT_SUB, ls, (size_t)(end - ls));
            else
                dk_gfx_draw_text(g, r.x + 12, ty, DK_FONT_REGULAR, 12,
                                 hov ? COL_TEXT : COL_TEXT_SUB, ls, (size_t)(p - ls));
            while (p < end && *p == ' ') p++;
            ty += 18;
        }
    }
}

static void paint_messages(App *app) {
    DkGfx *g = app->gfx;
    DkRect vp = app->r_chat_viewport;
    dk_gfx_clip_push(g, vp);

    if (app->msg_count == 0) {
        paint_empty_state(app);
        dk_gfx_clip_pop(g);
        app->r_chat_scrollbar = (DkRect){ 0, 0, 0, 0 };
        return;
    }

    uint64_t tnow = now_ms();
    int vp_y0 = vp.y;
    int vp_y1 = vp.y + vp.h;

    for (size_t i = 0; i < app->msg_count; i++) {
        UiMsg *m = &app->msgs[i];
        int card_y = vp.y + m->card_y - app->chat_scroll_y;
        if (card_y + m->card_h < vp_y0 || card_y > vp_y1)
            continue; /* viewport culling! */

        int card_x = m->content_x - 16;
        int card_w = m->content_w + 32;

        dk_gfx_fill_rrect(g, card_x, card_y, card_w, m->card_h, 13.0f,
                          m->is_user ? COL_USER_CARD : COL_CARD);
        dk_gfx_stroke_rrect(g, card_x, card_y, card_w, m->card_h, 13.0f,
                            m->is_user ? COL_USER_BORDER : COL_BORDER_SOFT);

        /* Role label */
        const char *role = m->is_user ? "YOU" : "DKCHATEBRAS";
        dk_gfx_draw_text(g, card_x + 16, card_y + 21, DK_FONT_BOLD, 10,
                         m->is_user ? COL_TEXT_SUB : COL_ACCENT, role, strlen(role));

        /* Copy button */
        DkRect cb = { m->copy_btn.x, card_y + 7, 28, 24 };
        bool hov_cp = pt_in_rect(app->mouse_x, app->mouse_y, vp) &&
                      pt_in_rect(app->mouse_x, app->mouse_y, cb);
        bool copied = (tnow < m->copied_until_ms);
        if (hov_cp || copied)
            dk_gfx_fill_rrect(g, cb.x, cb.y, cb.w, cb.h, 5.0f, COL_CARD_HOVER);
        dk_gfx_draw_icon(g, copied ? DK_ICON_CHECK : DK_ICON_COPY, cb.x + 7, cb.y + 5, 14,
                         copied ? COL_SUCCESS : (hov_cp ? COL_TEXT : COL_TEXT_DIM));

        /* Message body */
        size_t s0 = 0, s1 = 0;
        if ((int)i == app->sel_msg_idx) {
            s0 = app->sel_anchor;
            s1 = app->sel_caret;
        }
        bool show_stream_cursor = (app->generating && i + 1 == app->msg_count && !m->is_user && app->caret_visible);
        const char *pt = m->stream_display.data ? m->stream_display.data : "";
        dk_layout_paint(g, &m->layout, m->doc, pt, m->stream_display.len,
                        m->content_x, card_y + 34, vp_y0, vp_y1,
                        s0, s1, show_stream_cursor);
    }

    /* Thin custom scrollbar */
    if (app->chat_content_h > vp.h) {
        int track_h = vp.h - 16;
        int thumb_h = (vp.h * track_h) / app->chat_content_h;
        if (thumb_h < 32) thumb_h = 32;
        int max_scroll = app->chat_content_h - vp.h;
        int thumb_y = vp.y + 8 + (int)(((int64_t)app->chat_scroll_y * (track_h - thumb_h)) / max_scroll);
        app->r_chat_scrollbar = (DkRect){ vp.x + vp.w - 11, thumb_y, 6, thumb_h };
        bool hov_sb = app->dragging_scrollbar || pt_in_rect(app->mouse_x, app->mouse_y, app->r_chat_scrollbar);
        dk_gfx_fill_rrect(g, app->r_chat_scrollbar.x, app->r_chat_scrollbar.y, 6, thumb_h, 3.0f,
                          hov_sb ? COL_SCROLL_HOVER : COL_SCROLL_THUMB);
    } else {
        app->r_chat_scrollbar = (DkRect){ 0, 0, 0, 0 };
    }

    dk_gfx_clip_pop(g);
}

static void paint_composer(App *app) {
    DkGfx *g = app->gfx;
    DkRect box = app->r_composer_box;
    bool focused = app->composer_focused && !app->key_modal_open && !app->generating;

    dk_gfx_fill_rrect(g, box.x, box.y, box.w, box.h, 14.0f, COL_COMPOSER);
    dk_gfx_stroke_rrect(g, box.x, box.y, box.w, box.h, 14.0f, focused ? COL_ACCENT : COL_BORDER);

    /* Text area */
    DkRect tr = app->r_composer_text;
    dk_gfx_clip_push(g, tr);
    DkFontMetrics fm = dk_gfx_font_metrics(g, DK_FONT_REGULAR, 15);
    int lh = fm.line_height + 4;

    if (app->composer.text.len == 0) {
        const char *ph = app->generating
            ? "Streaming response... (Esc to stop)"
            : "Message DKChatebras... (Enter to send, Shift+Enter for newline)";
        dk_gfx_draw_text_ellipsize(g, tr.x, tr.y + fm.ascent + 2, tr.w,
                                   DK_FONT_REGULAR, 15, COL_TEXT_DIM, ph, strlen(ph));
        if (focused && app->caret_visible)
            dk_gfx_fill_rect(g, tr.x, tr.y + 3, 2, lh - 5, COL_ACCENT);
    } else {
        size_t sel_a = 0, sel_b = 0;
        ti_sel_bounds(&app->composer, &sel_a, &sel_b);

        const char *s = app->composer.text.data;
        size_t len = app->composer.text.len;
        int cur_x = tr.x;
        int cur_y = tr.y;
        int caret_x = tr.x, caret_y = tr.y;
        bool caret_placed = false;

        size_t pos = 0;
        while (pos <= len) {
            if (pos == app->composer.caret && !caret_placed) {
                caret_x = cur_x;
                caret_y = cur_y;
                caret_placed = true;
            }
            if (pos == len) break;

            if (s[pos] == '\n') {
                cur_x = tr.x;
                cur_y += lh;
                pos++;
                continue;
            }
            uint32_t cp = 0;
            size_t adv = dk_utf8_decode(s + pos, s + len, &cp);
            int cw = dk_gfx_char_advance(g, DK_FONT_REGULAR, 15, cp);
            if (cur_x > tr.x && (cur_x - tr.x) + cw > tr.w) {
                cur_x = tr.x;
                cur_y += lh;
                if (pos == app->composer.caret) {
                    caret_x = cur_x;
                    caret_y = cur_y;
                }
            }
            if (pos >= sel_a && pos < sel_b)
                dk_gfx_fill_rect(g, cur_x, cur_y + 1, cw, lh - 2, COL_SEL_BG);
            dk_gfx_draw_text(g, cur_x, cur_y + fm.ascent + 2, DK_FONT_REGULAR, 15,
                             COL_TEXT, s + pos, adv);
            cur_x += cw;
            pos += adv;
        }
        if (focused && app->caret_visible)
            dk_gfx_fill_rect(g, caret_x, caret_y + 3, 2, lh - 5, COL_ACCENT);
    }
    dk_gfx_clip_pop(g);

    /* Send / Stop button */
    DkRect sb = app->r_send_btn;
    bool hov = pt_in_rect(app->mouse_x, app->mouse_y, sb);
    if (app->generating) {
        dk_gfx_fill_rrect(g, sb.x, sb.y, sb.w, sb.h, 9.0f, hov ? DK_RGB(240, 92, 92) : COL_DANGER);
        dk_gfx_draw_icon(g, DK_ICON_STOP, sb.x + 9, sb.y + 9, 18, DK_RGB(255, 255, 255));
    } else {
        bool has_text = app->composer.text.len > 0;
        uint32_t bg = has_text ? (hov ? COL_ACCENT_HOVER : COL_ACCENT) : DK_RGB(28, 35, 44);
        dk_gfx_fill_rrect(g, sb.x, sb.y, sb.w, sb.h, 9.0f, bg);
        dk_gfx_draw_icon(g, DK_ICON_SEND, sb.x + 9, sb.y + 9, 18,
                         has_text ? DK_RGB(255, 255, 255) : COL_TEXT_DIM);
    }
}

static void paint_model_menu(App *app) {
    if (!app->model_menu_open) return;
    DkGfx *g = app->gfx;
    int w = app->r_model_btn.w + 24;
    if (w < 176) w = 176;
    int h = (int)MODEL_COUNT * 36 + 12;
    int x = app->r_model_btn.x + app->r_model_btn.w - w;
    int y = app->r_model_btn.y + app->r_model_btn.h + 6;
    app->r_model_menu = (DkRect){ x, y, w, h };

    dk_gfx_fill_rrect(g, x, y, w, h, 10.0f, DK_RGB(18, 24, 32));
    dk_gfx_stroke_rrect(g, x, y, w, h, 10.0f, COL_BORDER);

    for (size_t i = 0; i < MODEL_COUNT; i++) {
        DkRect ir = { x + 6, y + 6 + (int)i * 36, w - 12, 32 };
        app->r_model_items[i] = ir;
        bool hov = pt_in_rect(app->mouse_x, app->mouse_y, ir);
        bool sel = ((int)i == app->model_idx);
        if (hov || sel)
            dk_gfx_fill_rrect(g, ir.x, ir.y, ir.w, ir.h, 6.0f, hov ? COL_CARD_HOVER : DK_RGB(24, 31, 41));
        dk_gfx_draw_text(g, ir.x + 10, ir.y + 21, DK_FONT_MONO, 12,
                         sel ? COL_TEXT : COL_TEXT_SUB, MODELS[i], strlen(MODELS[i]));
        if (sel)
            dk_gfx_draw_icon(g, DK_ICON_CHECK, ir.x + ir.w - 22, ir.y + 9, 14, COL_ACCENT);
    }
}

static void paint_key_modal(App *app) {
    if (!app->key_modal_open) return;
    DkGfx *g = app->gfx;

    dk_gfx_fill_rect(g, 0, 0, app->win_w, app->win_h, COL_SCRIM);

    int mw = 460;
    if (mw > app->win_w - 32) mw = app->win_w - 32;
    int mh = 232;
    int mx = (app->win_w - mw) / 2;
    int my = (app->win_h - mh) / 2;
    app->r_modal_box = (DkRect){ mx, my, mw, mh };

    dk_gfx_fill_rrect(g, mx, my, mw, mh, 14.0f, DK_RGB(16, 21, 28));
    dk_gfx_stroke_rrect(g, mx, my, mw, mh, 14.0f, COL_BORDER);

    dk_gfx_draw_icon(g, DK_ICON_KEY, mx + 24, my + 24, 20, COL_ACCENT);
    dk_gfx_draw_text(g, mx + 52, my + 40, DK_FONT_BOLD, 17, COL_TEXT, "Connect to Cerebras", 19);

    const char *d1 = "Stored via system keyring (Secret Service) or local 0600 file.";
    const char *d2 = "Sent directly to api.cerebras.ai and nowhere else.";
    dk_gfx_draw_text(g, mx + 24, my + 70, DK_FONT_REGULAR, 13, COL_TEXT_SUB, d1, strlen(d1));
    dk_gfx_draw_text(g, mx + 24, my + 90, DK_FONT_REGULAR, 13, COL_TEXT_SUB, d2, strlen(d2));

    /* Key input box */
    app->r_modal_input = (DkRect){ mx + 24, my + 112, mw - 48, 42 };
    dk_gfx_fill_rrect(g, app->r_modal_input.x, app->r_modal_input.y,
                      app->r_modal_input.w, 42, 8.0f, COL_INPUT_BG);
    dk_gfx_stroke_rrect(g, app->r_modal_input.x, app->r_modal_input.y,
                        app->r_modal_input.w, 42, 8.0f, COL_ACCENT);

    app->r_modal_peek = (DkRect){ app->r_modal_input.x + app->r_modal_input.w - 36,
                                  app->r_modal_input.y + 5, 32, 32 };
    bool hov_peek = pt_in_rect(app->mouse_x, app->mouse_y, app->r_modal_peek);
    if (hov_peek)
        dk_gfx_fill_rrect(g, app->r_modal_peek.x, app->r_modal_peek.y, 32, 32, 6.0f, COL_CARD_HOVER);
    dk_gfx_draw_icon(g, DK_ICON_EYE, app->r_modal_peek.x + 7, app->r_modal_peek.y + 7, 18,
                     app->key_peek ? COL_ACCENT : COL_TEXT_SUB);

    DkRect text_clip = { app->r_modal_input.x + 12, app->r_modal_input.y + 4,
                         app->r_modal_input.w - 52, 34 };
    dk_gfx_clip_push(g, text_clip);
    int baseline = app->r_modal_input.y + 26;
    if (app->key_input.text.len == 0) {
        const char *ph = app->has_api_key ? "Enter new key to replace saved key..." : "csk-...";
        dk_gfx_draw_text(g, text_clip.x, baseline, DK_FONT_MONO, 13, COL_TEXT_DIM, ph, strlen(ph));
        if (app->caret_visible)
            dk_gfx_fill_rect(g, text_clip.x, app->r_modal_input.y + 10, 2, 22, COL_ACCENT);
    } else {
        DkStr disp;
        dk_str_init_cap(&disp, app->key_input.text.len * 3 + 4);
        size_t caret_disp_bytes = 0;
        const char *p = app->key_input.text.data;
        const char *end = p + app->key_input.text.len;
        while (p < end) {
            size_t off = (size_t)(p - app->key_input.text.data);
            if (off == app->key_input.caret) caret_disp_bytes = disp.len;
            const char *nx = dk_utf8_next(p, end);
            if (app->key_peek) dk_str_append_len(&disp, p, (size_t)(nx - p));
            else dk_str_append_len(&disp, "\xE2\x80\xA2", 3);
            p = nx;
        }
        if (app->key_input.caret >= app->key_input.text.len) caret_disp_bytes = disp.len;

        int caret_px = dk_gfx_text_width(g, DK_FONT_MONO, 13, disp.data, caret_disp_bytes);
        int scroll_x = caret_px > text_clip.w - 10 ? (caret_px - text_clip.w + 10) : 0;
        dk_gfx_draw_text(g, text_clip.x - scroll_x, baseline, DK_FONT_MONO, 13,
                         COL_TEXT, disp.data, disp.len);
        if (app->caret_visible)
            dk_gfx_fill_rect(g, text_clip.x + caret_px - scroll_x, app->r_modal_input.y + 10, 2, 22, COL_ACCENT);
        dk_str_free(&disp);
    }
    dk_gfx_clip_pop(g);

    /* Action buttons */
    int by = my + mh - 56;
    app->r_modal_save = (DkRect){ mx + mw - 124, by, 100, 36 };
    app->r_modal_cancel = (DkRect){ app->r_modal_save.x - 94, by, 84, 36 };
    app->r_modal_clear = app->has_api_key ? (DkRect){ mx + 24, by, 96, 36 } : (DkRect){ 0, 0, 0, 0 };

    if (app->has_api_key) {
        bool hc = pt_in_rect(app->mouse_x, app->mouse_y, app->r_modal_clear);
        dk_gfx_fill_rrect(g, app->r_modal_clear.x, app->r_modal_clear.y, 96, 36, 8.0f,
                          hc ? COL_DANGER_SOFT : COL_CARD);
        dk_gfx_stroke_rrect(g, app->r_modal_clear.x, app->r_modal_clear.y, 96, 36, 8.0f,
                            hc ? COL_DANGER : COL_BORDER);
        int rw = dk_gfx_text_width(g, DK_FONT_REGULAR, 13, "Remove", 6);
        dk_gfx_draw_text(g, app->r_modal_clear.x + (96 - rw) / 2, by + 23, DK_FONT_REGULAR, 13,
                         COL_DANGER, "Remove", 6);
    }

    bool hcan = pt_in_rect(app->mouse_x, app->mouse_y, app->r_modal_cancel);
    dk_gfx_fill_rrect(g, app->r_modal_cancel.x, app->r_modal_cancel.y, 84, 36, 8.0f,
                      hcan ? COL_CARD_HOVER : COL_CARD);
    dk_gfx_stroke_rrect(g, app->r_modal_cancel.x, app->r_modal_cancel.y, 84, 36, 8.0f, COL_BORDER);
    int cw_c = dk_gfx_text_width(g, DK_FONT_REGULAR, 13, "Cancel", 6);
    dk_gfx_draw_text(g, app->r_modal_cancel.x + (84 - cw_c) / 2, by + 23, DK_FONT_REGULAR, 13, COL_TEXT_SUB, "Cancel", 6);

    bool hsav = pt_in_rect(app->mouse_x, app->mouse_y, app->r_modal_save);
    dk_gfx_fill_rrect(g, app->r_modal_save.x, app->r_modal_save.y, 100, 36, 8.0f,
                      hsav ? COL_ACCENT_HOVER : COL_ACCENT);
    int sw_s = dk_gfx_text_width(g, DK_FONT_BOLD, 13, "Save Key", 8);
    dk_gfx_draw_text(g, app->r_modal_save.x + (100 - sw_s) / 2, by + 23, DK_FONT_BOLD, 13,
                     DK_RGB(255, 255, 255), "Save Key", 8);
}

static void paint_toast(App *app) {
    if (!app->toast_msg[0] || now_ms() >= app->toast_until_ms)
        return;
    DkGfx *g = app->gfx;
    int tw = dk_gfx_text_width(g, DK_FONT_REGULAR, 13, app->toast_msg, strlen(app->toast_msg)) + 32;
    int tx = app->r_chat_viewport.x + (app->r_chat_viewport.w - tw) / 2;
    int ty = TOPBAR_HEIGHT + 12;
    dk_gfx_fill_rrect(g, tx, ty, tw, 34, 17.0f, DK_RGB(25, 33, 44));
    dk_gfx_stroke_rrect(g, tx, ty, tw, 34, 17.0f, COL_BORDER);
    dk_gfx_draw_text(g, tx + 16, ty + 22, DK_FONT_REGULAR, 13, COL_TEXT,
                     app->toast_msg, strlen(app->toast_msg));
}

static void render_frame(App *app) {
    relayout_messages(app);
    dk_gfx_clear(app->gfx, COL_BG);

    int sb_w = (!app->sidebar_collapsed && app->sidebar_open) ? SIDEBAR_WIDTH : 0;
    int main_x = sb_w;
    int main_w = app->win_w - sb_w;

    paint_topbar(app, main_x, main_w);
    paint_messages(app);
    paint_composer(app);

    /* Sidebar drawn after content when collapsed (overlay drawer!) */
    paint_sidebar(app);
    paint_model_menu(app);
    paint_toast(app);
    paint_key_modal(app);

    dk_gfx_present(app->gfx);
    app->dirty = false;
}

/* ---- Input & Event Handling ---- */
static void handle_mouse_motion(App *app, int x, int y) {
    app->mouse_x = x;
    app->mouse_y = y;

    if (app->dragging_scrollbar && app->chat_content_h > app->r_chat_viewport.h) {
        int track_h = app->r_chat_viewport.h - 16;
        int thumb_h = app->r_chat_scrollbar.h;
        int max_scroll = app->chat_content_h - app->r_chat_viewport.h;
        int rel = y - (app->r_chat_viewport.y + 8) - app->scroll_drag_offset;
        if (track_h > thumb_h) {
            app->chat_scroll_y = (int)(((int64_t)rel * max_scroll) / (track_h - thumb_h));
            if (app->chat_scroll_y < 0) app->chat_scroll_y = 0;
            if (app->chat_scroll_y > max_scroll) app->chat_scroll_y = max_scroll;
            app->auto_scroll = (app->chat_scroll_y >= max_scroll - 8);
        }
        app->dirty = true;
        return;
    }

    if (app->dragging_msg_sel && app->sel_msg_idx >= 0 && (size_t)app->sel_msg_idx < app->msg_count) {
        UiMsg *m = &app->msgs[app->sel_msg_idx];
        int card_y = app->r_chat_viewport.y + m->card_y - app->chat_scroll_y;
        const char *pt = m->stream_display.data ? m->stream_display.data : "";
        size_t hit = 0;
        if (dk_layout_hit_test(app->gfx, &m->layout, m->doc, pt, m->stream_display.len,
                               x - m->content_x, y - (card_y + 34), &hit, NULL, NULL)) {
            app->sel_caret = hit;
            app->dirty = true;
        }
        return;
    }

    /* Update cursor icon */
    if (app->key_modal_open) {
        if (pt_in_rect(x, y, app->r_modal_input) && !pt_in_rect(x, y, app->r_modal_peek))
            set_cursor(app, app->cur_ibeam);
        else if (pt_in_rect(x, y, app->r_modal_save) || pt_in_rect(x, y, app->r_modal_cancel) ||
                 pt_in_rect(x, y, app->r_modal_peek) || pt_in_rect(x, y, app->r_modal_clear))
            set_cursor(app, app->cur_hand);
        else
            set_cursor(app, app->cur_default);
        app->dirty = true;
        return;
    }

    Cursor desired = app->cur_default;
    if (pt_in_rect(x, y, app->r_composer_text)) {
        desired = app->cur_ibeam;
    } else if (pt_in_rect(x, y, app->r_send_btn) || pt_in_rect(x, y, app->r_sidebar_toggle) ||
               pt_in_rect(x, y, app->r_model_btn) || pt_in_rect(x, y, app->r_key_btn) ||
               pt_in_rect(x, y, app->r_del_btn) ||
               (app->sidebar_open && (pt_in_rect(x, y, app->r_new_chat) ||
                                      (pt_in_rect(x, y, app->r_hist_viewport) && app->history.count > 0)))) {
        desired = app->cur_hand;
    } else if (pt_in_rect(x, y, app->r_chat_viewport)) {
        if (app->msg_count == 0) {
            for (int i = 0; i < 3; i++) {
                if (pt_in_rect(x, y, app->r_starter[i])) desired = app->cur_hand;
            }
        } else {
            for (size_t i = 0; i < app->msg_count; i++) {
                UiMsg *m = &app->msgs[i];
                int card_y = app->r_chat_viewport.y + m->card_y - app->chat_scroll_y;
                if (y < card_y || y > card_y + m->card_h) continue;
                DkRect cb = { m->copy_btn.x, card_y + 7, 28, 24 };
                if (pt_in_rect(x, y, cb)) { desired = app->cur_hand; break; }
                if (x >= m->content_x && x <= m->content_x + m->content_w && y >= card_y + 34) {
                    const char *url = NULL;
                    const char *pt = m->stream_display.data ? m->stream_display.data : "";
                    dk_layout_hit_test(app->gfx, &m->layout, m->doc, pt, m->stream_display.len,
                                       x - m->content_x, y - (card_y + 34), NULL, &url, NULL);
                    desired = url ? app->cur_hand : app->cur_ibeam;
                    break;
                }
            }
        }
    }
    set_cursor(app, desired);
    app->dirty = true;
}

static void handle_mouse_press(App *app, XButtonEvent *ev) {
    int x = ev->x, y = ev->y;
    app->mouse_x = x;
    app->mouse_y = y;

    /* Mouse wheel scroll */
    if (ev->button == Button4 || ev->button == Button5) {
        int delta = (ev->button == Button4) ? -54 : 54;
        if (app->sidebar_open && pt_in_rect(x, y, app->r_hist_viewport)) {
            app->history_scroll_y += delta;
            if (app->history_scroll_y < 0) app->history_scroll_y = 0;
        } else if (!app->key_modal_open && pt_in_rect(x, y, app->r_chat_viewport)) {
            app->chat_scroll_y += delta;
            int max_sc = app->chat_content_h - app->r_chat_viewport.h;
            if (max_sc < 0) max_sc = 0;
            if (app->chat_scroll_y < 0) app->chat_scroll_y = 0;
            if (app->chat_scroll_y > max_sc) app->chat_scroll_y = max_sc;
            app->auto_scroll = (app->chat_scroll_y >= max_sc - 8);
        }
        app->dirty = true;
        return;
    }

    if (ev->button != Button1) return;

    /* 1. API Key Modal */
    if (app->key_modal_open) {
        if (pt_in_rect(x, y, app->r_modal_save)) { action_save_key(app); return; }
        if (pt_in_rect(x, y, app->r_modal_cancel)) { app->key_modal_open = false; app->dirty = true; return; }
        if (app->has_api_key && pt_in_rect(x, y, app->r_modal_clear)) { action_clear_key(app); return; }
        if (pt_in_rect(x, y, app->r_modal_peek)) { app->key_peek = !app->key_peek; app->dirty = true; return; }
        if (!pt_in_rect(x, y, app->r_modal_box)) { app->key_modal_open = false; app->dirty = true; return; }
        return;
    }

    /* 2. Model dropdown popup */
    if (app->model_menu_open) {
        for (size_t i = 0; i < MODEL_COUNT; i++) {
            if (pt_in_rect(x, y, app->r_model_items[i])) {
                app->model_idx = (int)i;
                app->model_menu_open = false;
                if (app->current_chat && !app->generating) {
                    dk_chat_set_model(app->current_chat, MODELS[i]);
                    dk_store_save_chat_async(app->current_chat);
                }
                app->dirty = true;
                return;
            }
        }
        app->model_menu_open = false;
        app->dirty = true;
        if (pt_in_rect(x, y, app->r_model_btn)) return;
    }

    /* 3. Sidebar interactions */
    if (app->sidebar_open) {
        if (app->sidebar_collapsed && x >= SIDEBAR_WIDTH) {
            app->sidebar_open = false;
            app->dirty = true;
            return;
        }
        if (x < SIDEBAR_WIDTH) {
            if (pt_in_rect(x, y, app->r_new_chat)) {
                action_new_chat(app);
                return;
            }
            if (pt_in_rect(x, y, app->r_hist_viewport) && app->history.count > 0) {
                int item_h = 54;
                int rel_y = y - app->r_hist_viewport.y + app->history_scroll_y;
                if (rel_y >= 0) {
                    size_t row = (size_t)(rel_y / item_h);
                    if (row < app->history.count) {
                        size_t idx = app->history.count - 1 - row;
                        int iy = app->r_hist_viewport.y + (int)row * item_h - app->history_scroll_y;
                        DkRect ir = { 10, iy + 2, SIDEBAR_WIDTH - 22, item_h - 4 };
                        DkRect dr = { ir.x + ir.w - 30, ir.y + 12, 24, 24 };
                        if (!app->generating && pt_in_rect(x, y, dr)) {
                            char *id = dk_strdup(app->history.items[idx].id);
                            action_delete_chat_by_id(app, id);
                            free(id);
                            return;
                        }
                        if (pt_in_rect(x, y, ir)) {
                            if (app->generating) {
                                show_toast(app, "Stop the current response before switching chats");
                                return;
                            }
                            char err[256];
                            const char *id = app->history.items[idx].id;
                            DkChat *chat = dk_store_load_chat(id, err, sizeof(err));
                            if (!chat) show_toast(app, err);
                            else {
                                load_chat_into_ui(app, chat);
                                if (app->sidebar_collapsed) app->sidebar_open = false;
                            }
                            return;
                        }
                    }
                }
            }
            return;
        }
    }

    /* 4. Topbar buttons */
    if (pt_in_rect(x, y, app->r_sidebar_toggle)) {
        app->sidebar_open = !app->sidebar_open;
        app->dirty = true;
        return;
    }
    if (pt_in_rect(x, y, app->r_model_btn) && !app->generating) {
        app->model_menu_open = !app->model_menu_open;
        app->dirty = true;
        return;
    }
    if (pt_in_rect(x, y, app->r_key_btn)) {
        open_key_modal(app);
        return;
    }
    if (pt_in_rect(x, y, app->r_del_btn) && app->current_chat && !app->generating) {
        char *id = dk_strdup(app->current_chat->id);
        action_delete_chat_by_id(app, id);
        free(id);
        return;
    }

    /* 5. Composer / Send / Stop */
    if (pt_in_rect(x, y, app->r_send_btn)) {
        if (app->generating) action_stop_generation(app);
        else begin_send_prompt(app, app->composer.text.data);
        return;
    }
    if (pt_in_rect(x, y, app->r_composer_box)) {
        app->composer_focused = true;
        app->sel_msg_idx = -1;
        app->dirty = true;
        return;
    }

    /* 6. Scrollbar drag */
    if (app->r_chat_scrollbar.w > 0 && pt_in_rect(x, y, app->r_chat_scrollbar)) {
        app->dragging_scrollbar = true;
        app->scroll_drag_offset = y - app->r_chat_scrollbar.y;
        return;
    }

    /* 7. Empty state starter cards */
    if (app->msg_count == 0) {
        for (int i = 0; i < 3; i++) {
            if (pt_in_rect(x, y, app->r_starter[i])) {
                begin_send_prompt(app, STARTER_PROMPTS[i]);
                return;
            }
        }
    }

    /* 8. Message cards (copy button, link click, text selection) */
    if (pt_in_rect(x, y, app->r_chat_viewport)) {
        app->sel_msg_idx = -1;
        for (size_t i = 0; i < app->msg_count; i++) {
            UiMsg *m = &app->msgs[i];
            int card_y = app->r_chat_viewport.y + m->card_y - app->chat_scroll_y;
            if (y < card_y || y > card_y + m->card_h) continue;

            DkRect cb = { m->copy_btn.x, card_y + 7, 28, 24 };
            if (pt_in_rect(x, y, cb)) {
                clipboard_set(app, m->raw.data ? m->raw.data : "", m->raw.len);
                m->copied_until_ms = now_ms() + 1800;
                show_toast(app, "Copied message to clipboard");
                return;
            }

            const char *pt = m->stream_display.data ? m->stream_display.data : "";
            size_t hit = 0;
            const char *url = NULL;
            size_t url_len = 0;
            if (dk_layout_hit_test(app->gfx, &m->layout, m->doc, pt, m->stream_display.len,
                                   x - m->content_x, y - (card_y + 34), &hit, &url, &url_len)) {
                if (url && url_len > 0) {
                    open_url_external(url, url_len);
                    return;
                }
                app->sel_msg_idx = (int)i;
                app->sel_anchor = hit;
                app->sel_caret = hit;
                app->dragging_msg_sel = true;
                app->composer_focused = false;
                app->dirty = true;
                return;
            }
        }
    }
}

static void handle_key_press(App *app, XKeyEvent *ev) {
    char buf[64];
    KeySym sym = NoSymbol;
    Status status = 0;
    int len = 0;
    if (app->xic) {
        len = Xutf8LookupString(app->xic, ev, buf, (int)sizeof(buf) - 1, &sym, &status);
        if (status == XBufferOverflow) len = 0;
    } else {
        len = XLookupString(ev, buf, (int)sizeof(buf) - 1, &sym, NULL);
    }
    if (len < 0) len = 0;
    buf[len] = '\0';
    if (sym == NoSymbol)
        sym = XLookupKeysym(ev, 0);

    bool ctrl = (ev->state & ControlMask) != 0;
    bool shift = (ev->state & ShiftMask) != 0;

    /* Global shortcuts */
    if (ctrl && (sym == XK_q || sym == XK_Q)) { app->running = false; return; }
    if (sym == XK_Escape) {
        if (app->key_modal_open) { app->key_modal_open = false; app->dirty = true; return; }
        if (app->model_menu_open) { app->model_menu_open = false; app->dirty = true; return; }
        if (app->generating) { action_stop_generation(app); return; }
        if (app->sel_msg_idx >= 0) { app->sel_msg_idx = -1; app->composer_focused = true; app->dirty = true; return; }
        return;
    }

    if (!app->key_modal_open) {
        if (ctrl && (sym == XK_n || sym == XK_N)) { action_new_chat(app); return; }
        if (ctrl && (sym == XK_b || sym == XK_B)) { app->sidebar_open = !app->sidebar_open; app->dirty = true; return; }
        if (ctrl && (sym == XK_k || sym == XK_K)) { open_key_modal(app); return; }

        /* Copy selected message text with Ctrl+C */
        if (ctrl && (sym == XK_c || sym == XK_C) &&
            app->sel_msg_idx >= 0 && (size_t)app->sel_msg_idx < app->msg_count &&
            app->sel_anchor != app->sel_caret) {
            UiMsg *m = &app->msgs[app->sel_msg_idx];
            const char *txt = m->doc ? m->doc->text : (m->stream_display.data ? m->stream_display.data : "");
            size_t tlen = m->doc ? m->doc->text_len : m->stream_display.len;
            size_t a = app->sel_anchor < app->sel_caret ? app->sel_anchor : app->sel_caret;
            size_t b = app->sel_anchor < app->sel_caret ? app->sel_caret : app->sel_anchor;
            if (b > tlen) b = tlen;
            if (a < b) {
                clipboard_set(app, txt + a, b - a);
                show_toast(app, "Copied selection");
            }
            return;
        }

        /* PageUp / PageDown scrolling */
        if (sym == XK_Page_Up || sym == XK_Page_Down) {
            int step = (app->r_chat_viewport.h * 3) / 4;
            app->chat_scroll_y += (sym == XK_Page_Up) ? -step : step;
            int max_sc = app->chat_content_h - app->r_chat_viewport.h;
            if (max_sc < 0) max_sc = 0;
            if (app->chat_scroll_y < 0) app->chat_scroll_y = 0;
            if (app->chat_scroll_y > max_sc) app->chat_scroll_y = max_sc;
            app->auto_scroll = (app->chat_scroll_y >= max_sc - 8);
            app->dirty = true;
            return;
        }
    }

    /* Active text input (either key modal or composer) */
    TextInput *ti = app->key_modal_open ? &app->key_input : &app->composer;
    if (!app->key_modal_open && app->generating) return;

    app->caret_visible = true;
    app->last_blink_toggle = now_ms();
    if (!app->key_modal_open) app->composer_focused = true;

    if (ctrl && (sym == XK_a || sym == XK_A)) {
        ti->sel_anchor = 0;
        ti->caret = ti->text.len;
        app->dirty = true;
        return;
    }
    if (ctrl && (sym == XK_c || sym == XK_C)) {
        if (ti_has_sel(ti) && !ti->masked) {
            size_t a, b; ti_sel_bounds(ti, &a, &b);
            clipboard_set(app, ti->text.data + a, b - a);
        }
        return;
    }
    if (ctrl && (sym == XK_x || sym == XK_X)) {
        if (ti_has_sel(ti) && !ti->masked) {
            size_t a, b; ti_sel_bounds(ti, &a, &b);
            clipboard_set(app, ti->text.data + a, b - a);
            ti_delete_sel(ti);
            app->dirty = true;
        }
        return;
    }
    if (ctrl && (sym == XK_v || sym == XK_V)) {
        clipboard_request_paste(app);
        return;
    }

    if (sym == XK_Return || sym == XK_KP_Enter) {
        if (app->key_modal_open) {
            action_save_key(app);
        } else if (shift) {
            ti_insert(ti, "\n", 1);
            app->dirty = true;
        } else {
            begin_send_prompt(app, app->composer.text.data);
        }
        return;
    }
    if (sym == XK_BackSpace) { ti_backspace(ti, ctrl); app->dirty = true; return; }
    if (sym == XK_Delete)    { ti_delete_fwd(ti); app->dirty = true; return; }
    if (sym == XK_Left)      { ti_move_left(ti, shift, ctrl); app->dirty = true; return; }
    if (sym == XK_Right)     { ti_move_right(ti, shift, ctrl); app->dirty = true; return; }
    if (sym == XK_Home)      { ti->caret = 0; if (!shift) ti->sel_anchor = 0; app->dirty = true; return; }
    if (sym == XK_End)       { ti->caret = ti->text.len; if (!shift) ti->sel_anchor = ti->caret; app->dirty = true; return; }

    if (!ctrl && len > 0 && ((unsigned char)buf[0] >= 0x20 || (unsigned char)buf[0] >= 0x80)) {
        ti_insert(ti, buf, (size_t)len);
        app->dirty = true;
    }
}

static void handle_selection_request(App *app, XSelectionRequestEvent *req) {
    XEvent resp;
    memset(&resp, 0, sizeof(resp));
    resp.xselection.type = SelectionNotify;
    resp.xselection.display = req->display;
    resp.xselection.requestor = req->requestor;
    resp.xselection.selection = req->selection;
    resp.xselection.target = req->target;
    resp.xselection.time = req->time;
    resp.xselection.property = None;

    if (req->target == app->atom_targets) {
        Atom targets[3] = { app->atom_targets, app->atom_utf8, XA_STRING };
        XChangeProperty(app->dpy, req->requestor, req->property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)targets, 3);
        resp.xselection.property = req->property;
    } else if (req->target == app->atom_utf8 || req->target == XA_STRING) {
        XChangeProperty(app->dpy, req->requestor, req->property, req->target, 8,
                        PropModeReplace,
                        (unsigned char *)(app->clip_data.data ? app->clip_data.data : ""),
                        (int)app->clip_data.len);
        resp.xselection.property = req->property;
    }
    XSendEvent(app->dpy, req->requestor, False, 0, &resp);
    XFlush(app->dpy);
}

static void handle_selection_notify(App *app, XSelectionEvent *ev) {
    if (ev->property == None) return;
    Atom actual_type;
    int actual_fmt;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(app->dpy, app->win, ev->property, 0, 1024 * 256, True,
                           AnyPropertyType, &actual_type, &actual_fmt,
                           &nitems, &bytes_after, &data) == Success && data) {
        if (nitems > 0 && actual_fmt == 8) {
            TextInput *ti = app->key_modal_open ? &app->key_input : &app->composer;
            ti_insert(ti, (const char *)data, (size_t)nitems);
            app->dirty = true;
        }
        XFree(data);
    }
}

static void poll_network_stream(App *app) {
    DkStr chunk;
    dk_str_init(&chunk);
    bool cancelled = false;
    char err[512] = {0};

    bool done = dk_net_poll(app->net, &chunk, &cancelled, err, sizeof(err));

    if (chunk.len > 0 && app->msg_count > 0) {
        UiMsg *m = &app->msgs[app->msg_count - 1];
        dk_str_append_len(&m->raw, chunk.data, chunk.len);
        dk_utf8_filter_problem_glyphs(&m->stream_display, chunk.data, chunk.len);
        m->layout.cached_width = 0; /* invalidate only active streaming message */
        app->dirty = true;
    }
    dk_str_free(&chunk);

    if (done) {
        if (app->msg_count > 0 && app->current_chat) {
            UiMsg *m = &app->msgs[app->msg_count - 1];
            if (m->raw.len > 0) {
                dk_chat_append(app->current_chat, "assistant", m->raw.data);
                dk_md_free(m->doc);
                m->doc = dk_md_parse(m->raw.data);
                m->layout.cached_width = 0;
                dk_store_save_chat_async(app->current_chat);
            } else {
                const char *status_msg;
                if (err[0]) status_msg = err;
                else if (cancelled) status_msg = "Generation stopped.";
                else status_msg = "Cerebras returned an empty response.";
                dk_str_clear(&m->raw);
                dk_str_append(&m->raw, status_msg);
                dk_md_free(m->doc);
                m->doc = dk_md_parse(status_msg);
                m->layout.cached_width = 0;
            }
        }
        if (err[0] && !cancelled)
            show_toast(app, err);
        app->generating = false;
        app->composer_focused = true;
        app->dirty = true;
    }
}

int dk_app_run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setlocale(LC_CTYPE, "");

    App app;
    memset(&app, 0, sizeof(app));
    app.win_w = 1100;
    app.win_h = 760;
    app.sidebar_open = true;
    app.composer_focused = true;
    app.auto_scroll = true;
    app.sel_msg_idx = -1;
    app.hovered_hist_idx = -1;
    app.hovered_hist_del_idx = -1;
    app.running = true;
    app.dirty = true;
    app.caret_visible = true;
    app.last_blink_toggle = now_ms();

    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "dkchatebras: could not open X11 display\n");
        return 1;
    }
    app.screen = DefaultScreen(app.dpy);
    Window root = RootWindow(app.dpy, app.screen);

    XSetWindowAttributes swa;
    swa.background_pixel = 0x080B0F;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | StructureNotifyMask | FocusChangeMask;

    app.win = XCreateWindow(app.dpy, root, 0, 0,
                            (unsigned)app.win_w, (unsigned)app.win_h, 0,
                            DefaultDepth(app.dpy, app.screen), InputOutput,
                            DefaultVisual(app.dpy, app.screen),
                            CWBackPixel | CWEventMask, &swa);

    XSizeHints *sh = XAllocSizeHints();
    if (sh) {
        sh->flags = PMinSize;
        sh->min_width = 480;
        sh->min_height = 420;
        XSetWMNormalHints(app.dpy, app.win, sh);
        XFree(sh);
    }

    XClassHint ch = { "dkchatebras", "DKChatebras" };
    XSetClassHint(app.dpy, app.win, &ch);
    XStoreName(app.dpy, app.win, "DKChatebras");

    app.atom_wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    app.atom_clipboard = XInternAtom(app.dpy, "CLIPBOARD", False);
    app.atom_primary   = XInternAtom(app.dpy, "PRIMARY", False);
    app.atom_utf8      = XInternAtom(app.dpy, "UTF8_STRING", False);
    app.atom_targets   = XInternAtom(app.dpy, "TARGETS", False);
    app.atom_clip_prop = XInternAtom(app.dpy, "DK_CLIP_DATA", False);
    app.atom_net_wm_name = XInternAtom(app.dpy, "_NET_WM_NAME", False);
    app.atom_net_wm_pid  = XInternAtom(app.dpy, "_NET_WM_PID", False);
    XSetWMProtocols(app.dpy, app.win, &app.atom_wm_delete, 1);
    XChangeProperty(app.dpy, app.win, app.atom_net_wm_name, app.atom_utf8, 8,
                    PropModeReplace, (const unsigned char *)"DKChatebras", 11);
    uint32_t pid_val = (uint32_t)getpid();
    XChangeProperty(app.dpy, app.win, app.atom_net_wm_pid, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&pid_val, 1);

    app.cur_default = XCreateFontCursor(app.dpy, XC_left_ptr);
    app.cur_ibeam   = XCreateFontCursor(app.dpy, XC_xterm);
    app.cur_hand    = XCreateFontCursor(app.dpy, XC_hand2);
    app.cur_active  = app.cur_default;

    if (XSupportsLocale()) {
        XSetLocaleModifiers("");
        app.xim = XOpenIM(app.dpy, NULL, NULL, NULL);
        if (app.xim) {
            app.xic = XCreateIC(app.xim,
                                XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                XNClientWindow, app.win,
                                XNFocusWindow, app.win,
                                NULL);
        }
    }

    app.gfx = dk_gfx_create(app.dpy, app.screen, app.win, app.win_w, app.win_h);
    if (!app.gfx) {
        fprintf(stderr, "dkchatebras: failed to initialize graphics\n");
        XCloseDisplay(app.dpy);
        return 1;
    }

    dk_str_init(&app.clip_data);
    ti_init(&app.composer, true, false);
    ti_init(&app.key_input, false, true);
    dk_store_init();
    app.net = dk_net_new();
    if (pipe(app.key_pipe) != 0) {
        app.key_pipe[0] = app.key_pipe[1] = -1;
    } else {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(app.key_pipe[i], F_GETFL, 0);
            if (fl >= 0) fcntl(app.key_pipe[i], F_SETFL, fl | O_NONBLOCK);
            fcntl(app.key_pipe[i], F_SETFD, FD_CLOEXEC);
        }
    }

    reload_history(&app);
    update_sidebar_collapse(&app);

    /* Fast non-blocking initial API key probe */
    if (getenv("CEREBRAS_API_KEY") && *getenv("CEREBRAS_API_KEY")) {
        app.has_api_key = true;
        app.key_checked = true;
    } else if (app.key_pipe[0] >= 0) {
        start_async_key_lookup(&app, false);
    }

    XMapWindow(app.dpy, app.win);
    render_frame(&app);

    int xfd = ConnectionNumber(app.dpy);
    int nfd = dk_net_wake_fd(app.net);
    int kfd = app.key_pipe[0];
    uint64_t last_paint_ms = now_ms();

    while (app.running) {
        /* Drain all queued X11 events before painting */
        while (XPending(app.dpy) > 0) {
            XEvent ev;
            XNextEvent(app.dpy, &ev);
            if (XFilterEvent(&ev, app.win)) continue;
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) app.dirty = true;
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != app.win_w || ev.xconfigure.height != app.win_h) {
                    app.win_w = ev.xconfigure.width;
                    app.win_h = ev.xconfigure.height;
                    dk_gfx_resize(app.gfx, app.win_w, app.win_h);
                    update_sidebar_collapse(&app);
                    app.dirty = true;
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == app.atom_wm_delete)
                    app.running = false;
                break;
            case KeyPress:
                handle_key_press(&app, &ev.xkey);
                break;
            case ButtonPress:
                handle_mouse_press(&app, &ev.xbutton);
                break;
            case ButtonRelease:
                app.dragging_scrollbar = false;
                app.dragging_msg_sel = false;
                break;
            case MotionNotify:
                handle_mouse_motion(&app, ev.xmotion.x, ev.xmotion.y);
                break;
            case SelectionRequest:
                handle_selection_request(&app, &ev.xselectionrequest);
                break;
            case SelectionNotify:
                handle_selection_notify(&app, &ev.xselection);
                break;
            case FocusIn:
                if (app.xic) XSetICFocus(app.xic);
                app.caret_visible = true;
                app.last_blink_toggle = now_ms();
                app.dirty = true;
                break;
            case FocusOut:
                if (app.xic) XUnsetICFocus(app.xic);
                break;
            }
        }

        if (!app.running) break;

        uint64_t t = now_ms();
        /* Caret blink & toast expiry */
        if ((app.composer_focused || app.key_modal_open || app.generating) &&
            t - app.last_blink_toggle >= 530) {
            app.caret_visible = !app.caret_visible;
            app.last_blink_toggle = t;
            app.dirty = true;
        }
        if (app.toast_msg[0] && t >= app.toast_until_ms) {
            app.toast_msg[0] = '\0';
            app.dirty = true;
        }

        /* Frame-paced paint during heavy streaming (max ~60fps) */
        if (app.dirty && (!app.generating || t - last_paint_ms >= 16)) {
            render_frame(&app);
            last_paint_ms = now_ms();
        }

        if (XPending(app.dpy) > 0)
            continue;

        struct pollfd pfds[3] = {
            { .fd = xfd, .events = POLLIN },
            { .fd = nfd, .events = POLLIN },
            { .fd = kfd, .events = POLLIN },
        };
        int nfds = (kfd >= 0) ? 3 : 2;
        int timeout_ms = 530;
        if (app.dirty && app.generating) timeout_ms = 16;
        else if (app.toast_msg[0]) {
            uint64_t rem = (app.toast_until_ms > t) ? (app.toast_until_ms - t) : 10;
            if ((int)rem < timeout_ms) timeout_ms = (int)rem;
        }

        int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
        if (pr > 0) {
            if (pfds[1].revents & POLLIN)
                poll_network_stream(&app);
            if (nfds == 3 && (pfds[2].revents & POLLIN))
                handle_key_lookup_done(&app);
        }
    }

    /* Clean shutdown */
    dk_net_stop(app.net);
    dk_net_free(app.net);
    if (app.key_thread_running) {
        pthread_join(app.key_thread, NULL);
        dk_secret_free(app.fetched_key);
    }
    if (app.key_pipe[0] >= 0) { close(app.key_pipe[0]); close(app.key_pipe[1]); }
    dk_store_shutdown();

    free(app.pending_prompt);
    clear_ui_msgs(&app);
    free(app.msgs);
    dk_chat_free(app.current_chat);
    dk_meta_list_free(&app.history);
    ti_free(&app.composer);
    ti_free(&app.key_input);
    dk_str_free(&app.clip_data);

    dk_gfx_destroy(app.gfx);
    if (app.xic) XDestroyIC(app.xic);
    if (app.xim) XCloseIM(app.xim);
    XFreeCursor(app.dpy, app.cur_default);
    XFreeCursor(app.dpy, app.cur_ibeam);
    XFreeCursor(app.dpy, app.cur_hand);
    XDestroyWindow(app.dpy, app.win);
    XCloseDisplay(app.dpy);
    return 0;
}
