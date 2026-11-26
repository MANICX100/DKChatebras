#ifndef DK_STR_H
#define DK_STR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define DK_PRINTF_FMT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define DK_PRINTF_FMT(fmt_idx, arg_idx)
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DkStr;

void dk_str_init(DkStr *s);
void dk_str_init_cap(DkStr *s, size_t cap);
void dk_str_free(DkStr *s);
void dk_str_clear(DkStr *s);
bool dk_str_reserve(DkStr *s, size_t extra);
bool dk_str_push(DkStr *s, char c);
bool dk_str_append_len(DkStr *s, const char *src, size_t len);
bool dk_str_append(DkStr *s, const char *src);
bool dk_str_appendf(DkStr *s, const char *fmt, ...) DK_PRINTF_FMT(2, 3);
void dk_str_erase(DkStr *s, size_t pos, size_t count);
char *dk_str_steal(DkStr *s);
char *dk_strdup_len(const char *src, size_t len);
char *dk_strdup(const char *src);

/* UTF-8 utilities */
size_t dk_utf8_decode(const char *p, const char *end, uint32_t *out_cp);
size_t dk_utf8_encode(uint32_t cp, char out[4]);
const char *dk_utf8_next(const char *p, const char *end);
const char *dk_utf8_prev(const char *begin, const char *p);
size_t dk_utf8_strlen(const char *s, size_t byte_len);
bool dk_utf8_validate(const char *s, size_t len);
char *dk_utf8_sanitize(const char *s, size_t len);
void dk_utf8_filter_problem_glyphs(DkStr *dst, const char *s, size_t len);

/* Character classification */
bool dk_cp_is_space(uint32_t cp);
bool dk_cp_is_alnum(uint32_t cp);
bool dk_ascii_ispunct(unsigned char c);
bool dk_ascii_isalpha(unsigned char c);
bool dk_ascii_isdigit(unsigned char c);
int dk_ascii_strncasecmp(const char *a, const char *b, size_t n);

#endif
