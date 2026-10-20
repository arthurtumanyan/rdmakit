/*
 * json.h - minimal JSON reader for kvpace result files (RFC 8259 subset).
 *
 * Parses objects, arrays, strings (escapes \" \\ \/ \b \f \n \r \t and \uXXXX of the Basic Multilingual
 * Plane), numbers, true, false and null into a tree. Numbers keep their source text, so that integers
 * above 2^53 are written back exactly. Nesting is limited to 64 levels.
 */
#ifndef PACE_JSON_H
#define PACE_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { PJ_NULL, PJ_BOOL, PJ_NUM, PJ_STR, PJ_ARR, PJ_OBJ } pace_jtype;

typedef struct pace_json {
    pace_jtype type;
    int boolean;
    double num;
    char *text; /* PJ_NUM: source text; PJ_STR: decoded string */
    struct pace_json **items;
    char **keys; /* PJ_OBJ */
    size_t n;
} pace_json;

/* Parses a document. Returns the tree or NULL with a message in err. */
pace_json *pace_json_parse(const char *text, char *err, size_t errlen);

/* Reads and parses a file. */
pace_json *pace_json_load(const char *path, char *err, size_t errlen);

void pace_json_free(pace_json *v);

/* Member of an object, or NULL. */
const pace_json *pace_json_get(const pace_json *obj, const char *key);

/* Numeric value of a member (def when absent or not a number). */
double pace_json_num(const pace_json *obj, const char *key, double def);

/* Unsigned integer value of a number (parsed from its text). */
uint64_t pace_json_u64(const pace_json *v);

/* Writes a value back as JSON. */
void pace_json_write(FILE *f, const pace_json *v);

#endif
