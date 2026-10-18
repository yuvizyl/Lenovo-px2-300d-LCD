/* mincjson.c - minimal flat JSON parser */
#include "mincjson.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void skip_ws(const char **p)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    *p = s;
}

/* Read a JSON string starting at *p (after the opening quote).
 * Returns malloc'd unescaped string, sets *p to past the closing quote.
 * Supports \" \\ \/ \b \f \n \r \t \uXXXX (latter stored raw as-is).
 */
static char *read_string(const char **p)
{
    const char *s = *p;
    size_t cap = 16, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;

    while (*s && *s != '"') {
        char c = *s++;
        if (c == '\\') {
            if (!*s) { free(out); return NULL; }
            char esc = *s++;
            switch (esc) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u':  /* \uXXXX - store literal */
                    if (s[0] && s[1] && s[2] && s[3]) {
                        /* leave the 4 hex chars as-is in out */
                        if (len + 6 >= cap) {
                            cap *= 2;
                            char *t = (char *)realloc(out, cap);
                            if (!t) { free(out); return NULL; }
                            out = t;
                        }
                        out[len++] = '\\';
                        out[len++] = 'u';
                        for (int i = 0; i < 4; i++)
                            out[len++] = s[i];
                        s += 4;
                        continue;
                    } else { free(out); return NULL; }
                default:   c = esc;  break;
            }
        }
        if (len + 2 >= cap) {
            cap *= 2;
            char *t = (char *)realloc(out, cap);
            if (!t) { free(out); return NULL; }
            out = t;
        }
        out[len++] = c;
    }
    if (*s != '"') { free(out); return NULL; }
    s++; /* skip closing quote */
    *p = s;
    out[len] = '\0';
    return out;
}

mcj_obj *mcj_parse(const char *s)
{
    if (!s) return NULL;
    mcj_obj *obj = (mcj_obj *)calloc(1, sizeof(*obj));
    if (!obj) return NULL;
    obj->capacity = 8;
    obj->pairs = (mcj_pair *)calloc(obj->capacity, sizeof(mcj_pair));
    if (!obj->pairs) { free(obj); return NULL; }

    skip_ws(&s);
    if (*s != '{') { mcj_obj_free(obj); return NULL; }
    s++;
    skip_ws(&s);

    if (*s == '}') {
        s++;
        *obj->pairs = (mcj_pair){0};
        obj->count = 0;
        return obj;
    }

    while (1) {
        skip_ws(&s);
        if (*s != '"') { mcj_obj_free(obj); return NULL; }
        s++;
        char *key = read_string(&s);
        if (!key) { mcj_obj_free(obj); return NULL; }

        skip_ws(&s);
        if (*s != ':') { free(key); mcj_obj_free(obj); return NULL; }
        s++;
        skip_ws(&s);

        mcj_pair pair = {0};
        pair.key = key;

        if (*s == '"') {
            s++;
            pair.value = read_string(&s);
            if (!pair.value) { mcj_obj_free(obj); return NULL; }
        } else if (strncmp(s, "true", 4) == 0) {
            pair.is_true = 1;
            pair.value = "true";
            s += 4;
        } else if (strncmp(s, "false", 5) == 0) {
            pair.is_false = 1;
            pair.value = "false";
            s += 5;
        } else if (strncmp(s, "null", 4) == 0) {
            pair.is_null = 1;
            pair.value = "null";
            s += 4;
        } else if (*s == '-' || (*s >= '0' && *s <= '9')) {
            const char *start = s;
            if (*s == '-') s++;
            while (*s >= '0' && *s <= '9') s++;
            pair.is_number = 1;
            size_t n = (size_t)(s - start);
            pair.value = (char *)malloc(n + 1);
            if (!pair.value) { mcj_obj_free(obj); return NULL; }
            memcpy(pair.value, start, n);
            pair.value[n] = '\0';
        } else {
            mcj_obj_free(obj);
            return NULL;
        }

        if (obj->count >= obj->capacity) {
            obj->capacity *= 2;
            mcj_pair *t = (mcj_pair *)realloc(obj->pairs, obj->capacity * sizeof(mcj_pair));
            if (!t) { mcj_obj_free(obj); return NULL; }
            obj->pairs = t;
        }
        obj->pairs[obj->count++] = pair;

        skip_ws(&s);
        if (*s == ',') { s++; continue; }
        if (*s == '}') { s++; break; }
        mcj_obj_free(obj);
        return NULL;
    }

    return obj;
}

void mcj_obj_free(mcj_obj *obj)
{
    if (!obj) return;
    for (size_t i = 0; i < obj->count; i++) {
        free(obj->pairs[i].key);
        if (obj->pairs[i].value && !obj->pairs[i].is_true && !obj->pairs[i].is_false && !obj->pairs[i].is_null)
            free(obj->pairs[i].value);
    }
    free(obj->pairs);
    free(obj);
}

mcj_pair *mcj_get(mcj_obj *obj, const char *key)
{
    if (!obj || !key) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->pairs[i].key, key) == 0)
            return &obj->pairs[i];
    }
    return NULL;
}

const char *mcj_get_string(mcj_obj *obj, const char *key)
{
    mcj_pair *p = mcj_get(obj, key);
    if (!p) return NULL;
    if (p->is_true || p->is_false || p->is_null || p->is_number) return NULL;
    return p->value;
}

int mcj_get_int(mcj_obj *obj, const char *key, int def)
{
    mcj_pair *p = mcj_get(obj, key);
    if (!p || !p->is_number) return def;
    return atoi(p->value);
}

int mcj_get_bool(mcj_obj *obj, const char *key, int def)
{
    mcj_pair *p = mcj_get(obj, key);
    if (!p) return def;
    if (p->is_true) return 1;
    if (p->is_false) return 0;
    return def;
}
