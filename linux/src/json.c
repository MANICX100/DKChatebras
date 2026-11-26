#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
    int depth;
} Parser;

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ps->p++;
        else
            break;
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex4(Parser *ps, uint32_t *out) {
    if (ps->end - ps->p < 4)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(ps->p[i]);
        if (h < 0)
            return false;
        v = (v << 4) | (uint32_t)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

static bool parse_raw_string(Parser *ps, char **out_ptr, size_t *out_len) {
    if (ps->p >= ps->end || *ps->p != '"')
        return false;
    ps->p++;

    DkStr buf;
    dk_str_init_cap(&buf, 32);

    while (ps->p < ps->end) {
        const char *start = ps->p;
        while (ps->p < ps->end && *ps->p != '"' && *ps->p != '\\' && (unsigned char)*ps->p >= 0x20)
            ps->p++;
        if (ps->p > start)
            dk_str_append_len(&buf, start, (size_t)(ps->p - start));

        if (ps->p >= ps->end) {
            dk_str_free(&buf);
            return false;
        }
        if (*ps->p == '"') {
            ps->p++;
            *out_len = buf.len;
            *out_ptr = dk_str_steal(&buf);
            return *out_ptr != NULL;
        }
        if (*ps->p == '\\') {
            ps->p++;
            if (ps->p >= ps->end) {
                dk_str_free(&buf);
                return false;
            }
            char e = *ps->p++;
            switch (e) {
            case '"':  dk_str_push(&buf, '"');  break;
            case '\\': dk_str_push(&buf, '\\'); break;
            case '/':  dk_str_push(&buf, '/');  break;
            case 'b':  dk_str_push(&buf, '\b'); break;
            case 'f':  dk_str_push(&buf, '\f'); break;
            case 'n':  dk_str_push(&buf, '\n'); break;
            case 'r':  dk_str_push(&buf, '\r'); break;
            case 't':  dk_str_push(&buf, '\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!parse_hex4(ps, &cp)) {
                    dk_str_free(&buf);
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        uint32_t low = 0;
                        if (parse_hex4(ps, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        } else {
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                char enc[4];
                size_t n = dk_utf8_encode(cp, enc);
                dk_str_append_len(&buf, enc, n);
                break;
            }
            default:
                dk_str_free(&buf);
                return false;
            }
        } else {
            dk_str_free(&buf);
            return false;
        }
    }

    dk_str_free(&buf);
    return false;
}

static DkJson *parse_value(Parser *ps);

static DkJson *parse_array(Parser *ps) {
    ps->p++; /* '[' */
    DkJson *arr = dk_json_new_array();
    if (!arr)
        return NULL;

    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }

    while (ps->p < ps->end) {
        DkJson *item = parse_value(ps);
        if (!item) {
            dk_json_free(arr);
            return NULL;
        }
        dk_json_array_push(arr, item);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return arr;
        }
        break;
    }

    dk_json_free(arr);
    return NULL;
}

static DkJson *parse_object(Parser *ps) {
    ps->p++; /* '{' */
    DkJson *obj = dk_json_new_object();
    if (!obj)
        return NULL;

    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }

    while (ps->p < ps->end) {
        skip_ws(ps);
        char *key = NULL;
        size_t key_len = 0;
        if (!parse_raw_string(ps, &key, &key_len)) {
            dk_json_free(obj);
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(key);
            dk_json_free(obj);
            return NULL;
        }
        ps->p++;
        DkJson *val = parse_value(ps);
        if (!val) {
            free(key);
            dk_json_free(obj);
            return NULL;
        }
        dk_json_object_set(obj, key, val);
        free(key);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return obj;
        }
        break;
    }

    dk_json_free(obj);
    return NULL;
}

static DkJson *parse_number(Parser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end || !dk_ascii_isdigit((unsigned char)*ps->p))
        return NULL;
    if (*ps->p == '0') {
        ps->p++;
    } else {
        while (ps->p < ps->end && dk_ascii_isdigit((unsigned char)*ps->p))
            ps->p++;
    }
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || !dk_ascii_isdigit((unsigned char)*ps->p))
            return NULL;
        while (ps->p < ps->end && dk_ascii_isdigit((unsigned char)*ps->p))
            ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;
        if (ps->p >= ps->end || !dk_ascii_isdigit((unsigned char)*ps->p))
            return NULL;
        while (ps->p < ps->end && dk_ascii_isdigit((unsigned char)*ps->p))
            ps->p++;
    }

    size_t len = (size_t)(ps->p - start);
    char tmp[64];
    char *buf = len < sizeof(tmp) ? tmp : malloc(len + 1);
    if (!buf)
        return NULL;
    memcpy(buf, start, len);
    buf[len] = '\0';
    double num = strtod(buf, NULL);
    if (buf != tmp)
        free(buf);

    DkJson *v = calloc(1, sizeof(DkJson));
    if (!v)
        return NULL;
    v->type = DK_JSON_NUMBER;
    v->u.number = num;
    return v;
}

static DkJson *parse_value(Parser *ps) {
    if (ps->depth > 128)
        return NULL;
    skip_ws(ps);
    if (ps->p >= ps->end)
        return NULL;

    char c = *ps->p;
    if (c == '{') {
        ps->depth++;
        DkJson *v = parse_object(ps);
        ps->depth--;
        return v;
    }
    if (c == '[') {
        ps->depth++;
        DkJson *v = parse_array(ps);
        ps->depth--;
        return v;
    }
    if (c == '"') {
        char *s = NULL;
        size_t len = 0;
        if (!parse_raw_string(ps, &s, &len))
            return NULL;
        DkJson *v = calloc(1, sizeof(DkJson));
        if (!v) {
            free(s);
            return NULL;
        }
        v->type = DK_JSON_STRING;
        v->u.string.ptr = s;
        v->u.string.len = len;
        return v;
    }
    if (c == 't' && ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        return dk_json_new_bool(true);
    }
    if (c == 'f' && ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        return dk_json_new_bool(false);
    }
    if (c == 'n' && ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return dk_json_new_null();
    }
    if (c == '-' || dk_ascii_isdigit((unsigned char)c))
        return parse_number(ps);

    return NULL;
}

DkJson *dk_json_parse(const char *src, size_t len) {
    if (!src)
        return NULL;
    Parser ps = { src, src + len, 0 };
    DkJson *v = parse_value(&ps);
    if (!v)
        return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        dk_json_free(v);
        return NULL;
    }
    return v;
}

void dk_json_free(DkJson *v) {
    if (!v)
        return;
    switch (v->type) {
    case DK_JSON_STRING:
        free(v->u.string.ptr);
        break;
    case DK_JSON_ARRAY:
        for (size_t i = 0; i < v->u.array.len; i++)
            dk_json_free(v->u.array.items[i]);
        free(v->u.array.items);
        break;
    case DK_JSON_OBJECT:
        for (size_t i = 0; i < v->u.object.len; i++) {
            free(v->u.object.members[i].key);
            dk_json_free(v->u.object.members[i].val);
        }
        free(v->u.object.members);
        break;
    default:
        break;
    }
    free(v);
}

DkJson *dk_json_new_null(void) {
    DkJson *v = calloc(1, sizeof(DkJson));
    if (v) v->type = DK_JSON_NULL;
    return v;
}

DkJson *dk_json_new_bool(bool b) {
    DkJson *v = calloc(1, sizeof(DkJson));
    if (v) {
        v->type = DK_JSON_BOOL;
        v->u.boolean = b;
    }
    return v;
}

DkJson *dk_json_new_int(int64_t n) {
    DkJson *v = calloc(1, sizeof(DkJson));
    if (v) {
        v->type = DK_JSON_NUMBER;
        v->u.number = (double)n;
    }
    return v;
}

DkJson *dk_json_new_string_len(const char *s, size_t len) {
    DkJson *v = calloc(1, sizeof(DkJson));
    if (!v) return NULL;
    v->type = DK_JSON_STRING;
    v->u.string.ptr = dk_strdup_len(s ? s : "", len);
    v->u.string.len = len;
    return v;
}

DkJson *dk_json_new_string(const char *s) {
    return dk_json_new_string_len(s ? s : "", s ? strlen(s) : 0);
}

DkJson *dk_json_new_array(void) {
    DkJson *v = calloc(1, sizeof(DkJson));
    if (v) v->type = DK_JSON_ARRAY;
    return v;
}

DkJson *dk_json_new_object(void) {
    DkJson *v = calloc(1, sizeof(DkJson));
    if (v) v->type = DK_JSON_OBJECT;
    return v;
}

bool dk_json_array_push(DkJson *arr, DkJson *val) {
    if (!arr || arr->type != DK_JSON_ARRAY || !val)
        return false;
    if (arr->u.array.len == arr->u.array.cap) {
        size_t ncap = arr->u.array.cap ? arr->u.array.cap * 2 : 8;
        DkJson **p = realloc(arr->u.array.items, ncap * sizeof(DkJson *));
        if (!p) return false;
        arr->u.array.items = p;
        arr->u.array.cap = ncap;
    }
    arr->u.array.items[arr->u.array.len++] = val;
    return true;
}

bool dk_json_object_set(DkJson *obj, const char *key, DkJson *val) {
    if (!obj || obj->type != DK_JSON_OBJECT || !key || !val)
        return false;
    for (size_t i = 0; i < obj->u.object.len; i++) {
        if (strcmp(obj->u.object.members[i].key, key) == 0) {
            dk_json_free(obj->u.object.members[i].val);
            obj->u.object.members[i].val = val;
            return true;
        }
    }
    if (obj->u.object.len == obj->u.object.cap) {
        size_t ncap = obj->u.object.cap ? obj->u.object.cap * 2 : 8;
        DkJsonMember *p = realloc(obj->u.object.members, ncap * sizeof(DkJsonMember));
        if (!p) return false;
        obj->u.object.members = p;
        obj->u.object.cap = ncap;
    }
    obj->u.object.members[obj->u.object.len].key = dk_strdup(key);
    obj->u.object.members[obj->u.object.len].val = val;
    obj->u.object.len++;
    return true;
}

DkJson *dk_json_object_get(const DkJson *obj, const char *key) {
    if (!obj || obj->type != DK_JSON_OBJECT || !key)
        return NULL;
    for (size_t i = 0; i < obj->u.object.len; i++) {
        if (strcmp(obj->u.object.members[i].key, key) == 0)
            return obj->u.object.members[i].val;
    }
    return NULL;
}

const char *dk_json_get_str(const DkJson *obj, const char *key, const char *def) {
    DkJson *v = dk_json_object_get(obj, key);
    if (v && v->type == DK_JSON_STRING && v->u.string.ptr)
        return v->u.string.ptr;
    return def;
}

int64_t dk_json_get_int(const DkJson *obj, const char *key, int64_t def) {
    DkJson *v = dk_json_object_get(obj, key);
    if (v && v->type == DK_JSON_NUMBER)
        return (int64_t)v->u.number;
    return def;
}

void dk_json_append_escaped(DkStr *out, const char *s, size_t len) {
    dk_str_push(out, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  dk_str_append_len(out, "\\\"", 2); break;
        case '\\': dk_str_append_len(out, "\\\\", 2); break;
        case '\b': dk_str_append_len(out, "\\b", 2);  break;
        case '\f': dk_str_append_len(out, "\\f", 2);  break;
        case '\n': dk_str_append_len(out, "\\n", 2);  break;
        case '\r': dk_str_append_len(out, "\\r", 2);  break;
        case '\t': dk_str_append_len(out, "\\t", 2);  break;
        default:
            if (c < 0x20)
                dk_str_appendf(out, "\\u%04x", (unsigned)c);
            else
                dk_str_push(out, (char)c);
            break;
        }
    }
    dk_str_push(out, '"');
}

static void write_indent(DkStr *out, int level) {
    for (int i = 0; i < level * 2; i++)
        dk_str_push(out, ' ');
}

static void write_rec(const DkJson *v, DkStr *out, bool pretty, int level) {
    if (!v) {
        dk_str_append_len(out, "null", 4);
        return;
    }
    switch (v->type) {
    case DK_JSON_NULL:
        dk_str_append_len(out, "null", 4);
        break;
    case DK_JSON_BOOL:
        if (v->u.boolean)
            dk_str_append_len(out, "true", 4);
        else
            dk_str_append_len(out, "false", 5);
        break;
    case DK_JSON_NUMBER: {
        double d = v->u.number;
        if (isfinite(d) && floor(d) == d && d >= -9007199254740992.0 && d <= 9007199254740992.0)
            dk_str_appendf(out, "%lld", (long long)(int64_t)d);
        else if (isfinite(d))
            dk_str_appendf(out, "%.15g", d);
        else
            dk_str_append_len(out, "null", 4);
        break;
    }
    case DK_JSON_STRING:
        dk_json_append_escaped(out, v->u.string.ptr, v->u.string.len);
        break;
    case DK_JSON_ARRAY:
        dk_str_push(out, '[');
        for (size_t i = 0; i < v->u.array.len; i++) {
            if (i > 0) dk_str_push(out, ',');
            if (pretty) {
                dk_str_push(out, '\n');
                write_indent(out, level + 1);
            }
            write_rec(v->u.array.items[i], out, pretty, level + 1);
        }
        if (pretty && v->u.array.len > 0) {
            dk_str_push(out, '\n');
            write_indent(out, level);
        }
        dk_str_push(out, ']');
        break;
    case DK_JSON_OBJECT:
        dk_str_push(out, '{');
        for (size_t i = 0; i < v->u.object.len; i++) {
            if (i > 0) dk_str_push(out, ',');
            if (pretty) {
                dk_str_push(out, '\n');
                write_indent(out, level + 1);
            }
            const char *k = v->u.object.members[i].key;
            dk_json_append_escaped(out, k, strlen(k));
            dk_str_append_len(out, pretty ? ": " : ":", pretty ? 2 : 1);
            write_rec(v->u.object.members[i].val, out, pretty, level + 1);
        }
        if (pretty && v->u.object.len > 0) {
            dk_str_push(out, '\n');
            write_indent(out, level);
        }
        dk_str_push(out, '}');
        break;
    }
}

void dk_json_write(const DkJson *v, DkStr *out, bool pretty) {
    write_rec(v, out, pretty, 0);
}
