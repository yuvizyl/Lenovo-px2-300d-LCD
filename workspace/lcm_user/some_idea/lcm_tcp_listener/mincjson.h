/* mincjson.h - minimal flat JSON object parser
 * Supports: objects with string keys, values that are strings / true / false / null / integers.
 * No nested objects or arrays. Designed for small, controlled JSON messages.
 */
#ifndef MINCJSON_H
#define MINCJSON_H

#include <stddef.h>

typedef struct {
    char *key;
    char *value;   /* string content (NUL terminated) OR "true"/"false"/"null" OR integer as string */
    int is_true;   /* 1 if value is boolean true */
    int is_false;  /* 1 if value is boolean false */
    int is_null;   /* 1 if value is null */
    int is_number; /* 1 if value is an integer */
} mcj_pair;

typedef struct {
    mcj_pair *pairs;
    size_t count;
    size_t capacity;
    char *raw_strings;  /* owns all allocated substrings */
} mcj_obj;

/* Parse a JSON object from s. Returns NULL on parse error.
 * The parser stores internal substrings; mcj_obj_free must be called. */
mcj_obj *mcj_parse(const char *s);

/* Free a parsed object */
void mcj_obj_free(mcj_obj *obj);

/* Look up a field by key. Returns NULL if not found.
 * The returned pointer is owned by obj; do not free. */
mcj_pair *mcj_get(mcj_obj *obj, const char *key);

/* Convenience: get string value (NULL if missing or not a string) */
const char *mcj_get_string(mcj_obj *obj, const char *key);
/* Convenience: get integer value (def if missing or not a number) */
int mcj_get_int(mcj_obj *obj, const char *key, int def);
/* Convenience: get boolean value (def if missing or not bool) */
int mcj_get_bool(mcj_obj *obj, const char *key, int def);

#endif /* MINCJSON_H */
