/*
 * acl.h - users, passwords and permissions (internal interface of the server).
 *
 * The ACL file uses a subset of the syntax of Valkey ACL files (valkey.io/topics/acl): one user per line,
 *   user <name> <rule> <rule> ...
 * with the rules applied from left to right to a user that starts disabled, without passwords and with no
 * permissions:
 *   on, off                    enable or disable the user
 *   >password, <password       add or remove a password (stored as its SHA-256 hash)
 *   #<64 hex digits>, !<hex>   add or remove a SHA-256 password hash
 *   nopass, resetpass          any password is accepted / remove all passwords and nopass
 *   ~pattern                   keys matching the glob pattern may be read and written
 *   %R~pattern, %W~pattern, %RW~pattern   read only, write only, both
 *   allkeys, resetkeys         ~* / remove all key patterns
 *   &pattern, allchannels, resetchannels  pub/sub channels
 *   +@category, -@category     allow or deny every command of a category (see acl_categories)
 *   +command, -command         allow or deny one RESP command
 *   allcommands, nocommands    +@all / -@all
 *   reset                      off, resetpass, resetkeys, resetchannels, nocommands
 * Lines that are empty or start with '#' are ignored. When the file defines no user "default", or defines
 * it without nopass, connections must authenticate.
 *
 * Commands of all three protocols are listed in one table with their categories and key positions. The
 * +command / -command rules name RESP commands; memcached and native commands follow the category rules.
 * Every key a command touches must match a pattern of the user for the kind of access (read, write, or
 * both for commands that read and modify). Commands of the connection category (AUTH, HELLO, PING, QUIT,
 * ...) are always allowed, so a client can always authenticate and leave.
 *
 * Thread safety: an acl is immutable after acl_parse(); a reload creates a new one. Users are referenced
 * by sessions until the server stops, so older lists are kept until then (reloads are rare).
 */
#ifndef AKV_ACL_H
#define AKV_ACL_H

#include <stddef.h>
#include <stdint.h>

/* Categories (bit set). */
enum acl_cat {
    ACL_CAT_READ = 1u << 0,
    ACL_CAT_WRITE = 1u << 1,
    ACL_CAT_KEYSPACE = 1u << 2,
    ACL_CAT_ADMIN = 1u << 3,
    ACL_CAT_DANGEROUS = 1u << 4,
    ACL_CAT_PUBSUB = 1u << 5,
    ACL_CAT_TRANSACTION = 1u << 6,
    ACL_CAT_CONNECTION = 1u << 7,
    ACL_CAT_REPLICATION = 1u << 8,
};

/* Kinds of key access. */
enum acl_access {
    ACL_KEY_READ = 1,
    ACL_KEY_WRITE = 2,
};

/* Protocols of the command table. */
enum acl_proto {
    ACL_RESP = 0,
    ACL_MC = 1,
    ACL_NATIVE = 2,
};

/*
 * One command: name (upper case for RESP, as sent for memcached, a symbolic name for native operations),
 * categories, and key positions: arguments first..last (last < 0 counts from the end, -1 = last) every
 * step-th argument are keys accessed as `access`; first = 0 means no keys. Native operations use
 * first = 1 for "the key of the request".
 */
typedef struct acl_cmd {
    enum acl_proto proto;
    const char *name;
    uint32_t cats;
    int8_t first;
    int8_t last;
    int8_t step;
    uint8_t access;
} acl_cmd;

typedef struct acl_user acl_user;
typedef struct acl acl;

/* Number of entries of the command table; command ids are indexes into it. */
unsigned acl_ncmds(void);
const acl_cmd *acl_cmd_at(unsigned id);

/* Id of a command of a protocol by name (RESP: case-insensitive); -1 when unknown. */
int acl_cmd_find(enum acl_proto proto, const char *name, size_t n);

/* Category names in bit order ("read", "write", ...), NULL-terminated. */
extern const char *const acl_categories[];

/*
 * Parses an ACL file. Returns the list, or NULL with a message (file and line) in err. When `compat` is
 * set, the file is a credentials file of earlier versions ("user:password" or a bare password for the
 * user "default" per line), and every user gets all permissions.
 */
acl *acl_load(const char *path, int compat, char *err, size_t errlen);

/* Parses ACL text (the content of a file). Same results as acl_load(). */
acl *acl_parse(const char *text, int compat, char *err, size_t errlen);

void acl_free(acl *a);

/* Number of users and the user at index i. */
size_t acl_nusers(const acl *a);
const acl_user *acl_user_at(const acl *a, size_t i);

/* User by name, NULL when absent. */
const acl_user *acl_find(const acl *a, const char *name, size_t n);

/* The default user when it is enabled and needs no password (connections start logged in as it). */
const acl_user *acl_default_login(const acl *a);

/*
 * Checks a password for user `name` (NULL or empty: "default"). Compares the hash with every stored hash
 * of the user without stopping at a match. Returns the user, or NULL (unknown, disabled, wrong password).
 */
const acl_user *acl_login(const acl *a, const char *name, size_t nlen, const char *pass, size_t plen);

const char *acl_user_name(const acl_user *u);

/* 1 when u is enabled ("on"). */
int acl_user_enabled(const acl_user *u);

/* 1 when u may run every command on every key and channel (the checks below are then skipped). */
int acl_unrestricted(const acl_user *u);

/* 1 when u may run command id. */
int acl_cmd_allowed(const acl_user *u, unsigned id);

/* 1 when u may access key with the given access bits (enum acl_access). */
int acl_key_allowed(const acl_user *u, const void *key, size_t n, unsigned access);

/* 1 when u may use channel. */
int acl_channel_allowed(const acl_user *u, const void *ch, size_t n);

/* Writes the rules of u in canonical form ("user <name> on #<hash> ~<pattern> ... <command rules>"). */
size_t acl_describe(const acl_user *u, char *out, size_t cap);

/* Glob match with *, ?, [...] and \ escapes (Valkey's stringmatchlen semantics). */
int acl_glob(const char *pat, size_t plen, const char *s, size_t n);

#endif
