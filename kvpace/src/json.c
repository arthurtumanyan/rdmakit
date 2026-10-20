/* json.c - minimal JSON reader, see json.h. */
#include "json.h"

#include <stdlib.h>
#include <string.h>

#include "report.h"

struct parser {
    const char *p;
    const char *err;
    int depth;
};

static void skip_ws(struct parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') {
        ps->p++;
    }
}

static pace_json *node(pace_jtype t) {
    pace_json *v = calloc(1, sizeof(*v));
    if (v != NULL) {
        v->type = t;
    }
    return v;
}

static pace_json *parse_value(struct parser *ps);

/* Appends one UTF-8 encoded code point. */
static size_t put_utf8(char *out, unsigned cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
}

static char *parse_string(struct parser *ps) {
    if (*ps->p != '"') {
        ps->err = "expected a string";
        return NULL;
    }
    ps->p++;
    size_t cap = 32;
    size_t n = 0;
    char *s = malloc(cap);
    while (s != NULL && *ps->p != '"') {
        if (*ps->p == '\0' || (unsigned char)*ps->p < 0x20) {
            ps->err = "unterminated string";
            free(s);
            return NULL;
        }
        if (n + 4 >= cap) {
            char *t = realloc(s, cap *= 2);
            if (t == NULL) {
                free(s);
                return NULL;
            }
            s = t;
        }
        char c = *ps->p++;
        if (c != '\\') {
            s[n++] = c;
            continue;
        }
        c = *ps->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            s[n++] = c;
            break;
        case 'b':
            s[n++] = '\b';
            break;
        case 'f':
            s[n++] = '\f';
            break;
        case 'n':
            s[n++] = '\n';
            break;
        case 'r':
            s[n++] = '\r';
            break;
        case 't':
            s[n++] = '\t';
            break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                char h = *ps->p++;
                cp <<= 4;
                if (h >= '0' && h <= '9') {
                    cp |= (unsigned)(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    cp |= (unsigned)(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                    cp |= (unsigned)(h - 'A' + 10);
                } else {
                    ps->err = "bad \\u escape";
                    free(s);
                    return NULL;
                }
            }
            n += put_utf8(s + n, cp);
            break;
        }
        default:
            ps->err = "bad escape";
            free(s);
            return NULL;
        }
    }
    if (s == NULL) {
        return NULL;
    }
    ps->p++;
    s[n] = '\0';
    return s;
}

static int push(pace_json *v, pace_json *item, char *key) {
    pace_json **items = realloc(v->items, (v->n + 1) * sizeof(*items));
    if (items == NULL) {
        return -1;
    }
    v->items = items;
    if (v->type == PJ_OBJ) {
        char **keys = realloc(v->keys, (v->n + 1) * sizeof(*keys));
        if (keys == NULL) {
            return -1;
        }
        v->keys = keys;
        v->keys[v->n] = key;
    }
    v->items[v->n++] = item;
    return 0;
}

static pace_json *parse_container(struct parser *ps, int obj) {
    if (++ps->depth > 64) {
        ps->err = "nesting too deep";
        return NULL;
    }
    pace_json *v = node(obj ? PJ_OBJ : PJ_ARR);
    ps->p++;
    skip_ws(ps);
    char close = obj ? '}' : ']';
    if (*ps->p == close) {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (obj) {
            key = parse_string(ps);
            skip_ws(ps);
            if (key == NULL || *ps->p != ':') {
                ps->err = ps->err ? ps->err : "expected ':'";
                free(key);
                pace_json_free(v);
                return NULL;
            }
            ps->p++;
        }
        pace_json *item = parse_value(ps);
        if (item == NULL || push(v, item, key) != 0) {
            free(key);
            pace_json_free(item);
            pace_json_free(v);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == close) {
            ps->p++;
            ps->depth--;
            return v;
        }
        ps->err = obj ? "expected ',' or '}'" : "expected ',' or ']'";
        pace_json_free(v);
        return NULL;
    }
}

static pace_json *parse_value(struct parser *ps) {
    skip_ws(ps);
    char c = *ps->p;
    if (c == '{' || c == '[') {
        return parse_container(ps, c == '{');
    }
    if (c == '"') {
        char *s = parse_string(ps);
        if (s == NULL) {
            return NULL;
        }
        pace_json *v = node(PJ_STR);
        v->text = s;
        return v;
    }
    if (strncmp(ps->p, "true", 4) == 0 || strncmp(ps->p, "false", 5) == 0) {
        pace_json *v = node(PJ_BOOL);
        v->boolean = c == 't';
        ps->p += v->boolean ? 4 : 5;
        return v;
    }
    if (strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return node(PJ_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(ps->p, &end);
        if (end == ps->p) {
            ps->err = "bad number";
            return NULL;
        }
        pace_json *v = node(PJ_NUM);
        v->num = d;
        v->text = strndup(ps->p, (size_t)(end - ps->p));
        ps->p = end;
        return v;
    }
    ps->err = "unexpected character";
    return NULL;
}

pace_json *pace_json_parse(const char *text, char *err, size_t errlen) {
    struct parser ps = {.p = text};
    pace_json *v = parse_value(&ps);
    if (v != NULL) {
        skip_ws(&ps);
        if (*ps.p != '\0') {
            ps.err = "trailing data";
            pace_json_free(v);
            v = NULL;
        }
    }
    if (v == NULL) {
        snprintf(err, errlen, "%s at offset %ld", ps.err ? ps.err : "out of memory", (long)(ps.p - text));
    }
    return v;
}

pace_json *pace_json_load(const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        snprintf(err, errlen, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        snprintf(err, errlen, "cannot read %s", path);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    pace_json *v = pace_json_parse(buf, err, errlen);
    free(buf);
    return v;
}

void pace_json_free(pace_json *v) {
    if (v == NULL) {
        return;
    }
    for (size_t i = 0; i < v->n; i++) {
        pace_json_free(v->items[i]);
        if (v->keys != NULL) {
            free(v->keys[i]);
        }
    }
    free(v->items);
    free(v->keys);
    free(v->text);
    free(v);
}

const pace_json *pace_json_get(const pace_json *obj, const char *key) {
    if (obj == NULL || obj->type != PJ_OBJ) {
        return NULL;
    }
    for (size_t i = 0; i < obj->n; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            return obj->items[i];
        }
    }
    return NULL;
}

double pace_json_num(const pace_json *obj, const char *key, double def) {
    const pace_json *v = pace_json_get(obj, key);
    return v != NULL && v->type == PJ_NUM ? v->num : def;
}

uint64_t pace_json_u64(const pace_json *v) {
    if (v == NULL || v->type != PJ_NUM) {
        return 0;
    }
    return strtoull(v->text, NULL, 10);
}

void pace_json_write(FILE *f, const pace_json *v) {
    switch (v->type) {
    case PJ_NULL:
        fputs("null", f);
        break;
    case PJ_BOOL:
        fputs(v->boolean ? "true" : "false", f);
        break;
    case PJ_NUM:
        fputs(v->text, f);
        break;
    case PJ_STR:
        pace_json_str(f, v->text);
        break;
    case PJ_ARR:
    case PJ_OBJ:
        fputc(v->type == PJ_ARR ? '[' : '{', f);
        for (size_t i = 0; i < v->n; i++) {
            if (i > 0) {
                fputs(", ", f);
            }
            if (v->type == PJ_OBJ) {
                pace_json_str(f, v->keys[i]);
                fputs(": ", f);
            }
            pace_json_write(f, v->items[i]);
        }
        fputc(v->type == PJ_ARR ? ']' : '}', f);
        break;
    }
}
