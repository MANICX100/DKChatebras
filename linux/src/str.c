#include "str.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dk_str_init(DkStr *s) {
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void dk_str_init_cap(DkStr *s, size_t cap) {
    dk_str_init(s);
    if (cap > 0)
        dk_str_reserve(s, cap);
}

void dk_str_free(DkStr *s) {
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void dk_str_clear(DkStr *s) {
    s->len = 0;
    if (s->data)
        s->data[0] = '\0';
}

bool dk_str_reserve(DkStr *s, size_t extra) {
    size_t needed = s->len + extra + 1;
    if (needed <= s->cap)
        return true;
    size_t new_cap = s->cap ? s->cap * 2 : 64;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2)) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    char *p = realloc(s->data, new_cap);
    if (!p)
        return false;
    s->data = p;
    s->cap = new_cap;
    if (s->len == 0)
        s->data[0] = '\0';
    return true;
}

bool dk_str_push(DkStr *s, char c) {
    if (!dk_str_reserve(s, 1))
        return false;
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
    return true;
}

bool dk_str_append_len(DkStr *s, const char *src, size_t len) {
    if (len == 0) {
        if (!s->data)
            return dk_str_reserve(s, 0);
        return true;
    }
    if (!dk_str_reserve(s, len))
        return false;
    memcpy(s->data + s->len, src, len);
    s->len += len;
    s->data[s->len] = '\0';
    return true;
}

bool dk_str_append(DkStr *s, const char *src) {
    if (!src)
        return true;
    return dk_str_append_len(s, src, strlen(src));
}

bool dk_str_appendf(DkStr *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return false;
    }
    if (!dk_str_reserve(s, (size_t)n)) {
        va_end(ap2);
        return false;
    }
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)n;
    return true;
}

void dk_str_erase(DkStr *s, size_t pos, size_t count) {
    if (pos >= s->len || count == 0)
        return;
    if (count > s->len - pos)
        count = s->len - pos;
    memmove(s->data + pos, s->data + pos + count, s->len - pos - count + 1);
    s->len -= count;
}

char *dk_str_steal(DkStr *s) {
    if (!s->data) {
        char *empty = malloc(1);
        if (empty)
            empty[0] = '\0';
        return empty;
    }
    char *p = s->data;
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
    return p;
}

char *dk_strdup_len(const char *src, size_t len) {
    char *p = malloc(len + 1);
    if (!p)
        return NULL;
    if (len > 0 && src)
        memcpy(p, src, len);
    p[len] = '\0';
    return p;
}

char *dk_strdup(const char *src) {
    if (!src)
        return dk_strdup_len("", 0);
    return dk_strdup_len(src, strlen(src));
}

size_t dk_utf8_decode(const char *p, const char *end, uint32_t *out_cp) {
    if (p >= end) {
        if (out_cp) *out_cp = 0;
        return 0;
    }
    unsigned char b0 = (unsigned char)p[0];
    if (b0 < 0x80) {
        if (out_cp) *out_cp = b0;
        return 1;
    }
    size_t rem = (size_t)(end - p);
    if ((b0 & 0xE0) == 0xC0 && rem >= 2) {
        unsigned char b1 = (unsigned char)p[1];
        if ((b1 & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (b1 & 0x3F);
            if (cp >= 0x80) {
                if (out_cp) *out_cp = cp;
                return 2;
            }
        }
    } else if ((b0 & 0xF0) == 0xE0 && rem >= 3) {
        unsigned char b1 = (unsigned char)p[1];
        unsigned char b2 = (unsigned char)p[2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                          ((uint32_t)(b1 & 0x3F) << 6) |
                          (b2 & 0x3F);
            if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                if (out_cp) *out_cp = cp;
                return 3;
            }
        }
    } else if ((b0 & 0xF8) == 0xF0 && rem >= 4) {
        unsigned char b1 = (unsigned char)p[1];
        unsigned char b2 = (unsigned char)p[2];
        unsigned char b3 = (unsigned char)p[3];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                          ((uint32_t)(b1 & 0x3F) << 12) |
                          ((uint32_t)(b2 & 0x3F) << 6) |
                          (b3 & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                if (out_cp) *out_cp = cp;
                return 4;
            }
        }
    }
    if (out_cp) *out_cp = 0xFFFD;
    return 1;
}

size_t dk_utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

const char *dk_utf8_next(const char *p, const char *end) {
    if (p >= end)
        return end;
    return p + dk_utf8_decode(p, end, NULL);
}

const char *dk_utf8_prev(const char *begin, const char *p) {
    if (p <= begin)
        return NULL;
    const char *q = p - 1;
    while (q > begin && (((unsigned char)*q) & 0xC0) == 0x80)
        q--;
    return q;
}

size_t dk_utf8_strlen(const char *s, size_t byte_len) {
    const char *p = s;
    const char *end = s + byte_len;
    size_t n = 0;
    while (p < end) {
        p += dk_utf8_decode(p, end, NULL);
        n++;
    }
    return n;
}

bool dk_utf8_validate(const char *s, size_t len) {
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        unsigned char b0 = (unsigned char)*p;
        if (b0 < 0x80) {
            p++;
            continue;
        }
        uint32_t cp = 0;
        size_t adv = dk_utf8_decode(p, end, &cp);
        if (adv == 1 && cp == 0xFFFD && b0 >= 0x80)
            return false;
        p += adv;
    }
    return true;
}

char *dk_utf8_sanitize(const char *s, size_t len) {
    if (!s)
        return dk_strdup_len("", 0);
    if (dk_utf8_validate(s, len))
        return dk_strdup_len(s, len);

    DkStr out;
    dk_str_init_cap(&out, len + 16);
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        unsigned char b0 = (unsigned char)*p;
        if (b0 < 0x80) {
            dk_str_push(&out, (char)b0);
            p++;
            continue;
        }
        uint32_t cp = 0;
        size_t adv = dk_utf8_decode(p, end, &cp);
        if (adv == 1 && cp == 0xFFFD) {
            dk_str_append_len(&out, "\xEF\xBF\xBD", 3);
        } else {
            dk_str_append_len(&out, p, adv);
        }
        p += adv;
    }
    return dk_str_steal(&out);
}

void dk_utf8_filter_problem_glyphs(DkStr *dst, const char *s, size_t len) {
    const char *p = s;
    const char *end = s + len;
    while (p < end) {
        if ((unsigned char)*p < 0x80) {
            const char *run = p;
            while (p < end && (unsigned char)*p < 0x80)
                p++;
            dk_str_append_len(dst, run, (size_t)(p - run));
            continue;
        }
        uint32_t cp = 0;
        size_t adv = dk_utf8_decode(p, end, &cp);
        if (cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D || cp == 0x200B || cp == 0x200C) {
            p += adv;
            continue;
        }
        if (cp == 0x20E3) {
            dk_str_push(dst, '.');
            p += adv;
            continue;
        }
        dk_str_append_len(dst, p, adv);
        p += adv;
    }
}

bool dk_cp_is_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
           cp == '\v' || cp == '\f' || cp == 0x00A0 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x3000;
}

bool dk_cp_is_alnum(uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9'))
        return true;
    /* Latin-1 supplement letters, Greek, Cyrillic, CJK */
    if ((cp >= 0x00C0 && cp <= 0x024F && cp != 0x00D7 && cp != 0x00F7) ||
        (cp >= 0x0370 && cp <= 0x052F) ||
        (cp >= 0x4E00 && cp <= 0x9FFF))
        return true;
    return false;
}

bool dk_ascii_ispunct(unsigned char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

bool dk_ascii_isalpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool dk_ascii_isdigit(unsigned char c) {
    return c >= '0' && c <= '9';
}

int dk_ascii_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return (int)ca - (int)cb;
        if (ca == '\0')
            return 0;
    }
    return 0;
}
