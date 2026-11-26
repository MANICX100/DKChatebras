#ifndef DK_JSON_H
#define DK_JSON_H

#include "str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DK_JSON_NULL = 0,
    DK_JSON_BOOL,
    DK_JSON_NUMBER,
    DK_JSON_STRING,
    DK_JSON_ARRAY,
    DK_JSON_OBJECT,
} DkJsonType;

typedef struct DkJson DkJson;

typedef struct {
    char *key;
    DkJson *val;
} DkJsonMember;

struct DkJson {
    DkJsonType type;
    union {
        bool boolean;
        double number;
        struct {
            char *ptr;
            size_t len;
        } string;
        struct {
            DkJson **items;
            size_t len;
            size_t cap;
        } array;
        struct {
            DkJsonMember *members;
            size_t len;
            size_t cap;
        } object;
    } u;
};

DkJson *dk_json_parse(const char *src, size_t len);
void dk_json_free(DkJson *v);

DkJson *dk_json_new_null(void);
DkJson *dk_json_new_bool(bool b);
DkJson *dk_json_new_int(int64_t n);
DkJson *dk_json_new_string(const char *s);
DkJson *dk_json_new_string_len(const char *s, size_t len);
DkJson *dk_json_new_array(void);
DkJson *dk_json_new_object(void);

bool dk_json_array_push(DkJson *arr, DkJson *val);
bool dk_json_object_set(DkJson *obj, const char *key, DkJson *val);

DkJson *dk_json_object_get(const DkJson *obj, const char *key);
const char *dk_json_get_str(const DkJson *obj, const char *key, const char *def);
int64_t dk_json_get_int(const DkJson *obj, const char *key, int64_t def);

void dk_json_append_escaped(DkStr *out, const char *s, size_t len);
void dk_json_write(const DkJson *v, DkStr *out, bool pretty);

#endif
