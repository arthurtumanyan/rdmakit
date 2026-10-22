/*
 * test_acl.c - access control lists (src/acl.c) and SHA-256 (src/sha256.c).
 *
 * - SHA-256 against the FIPS 180-4 example vectors (one block, two blocks, one million 'a') and the
 *   padding boundaries of 55, 56 and 64 bytes
 * - glob patterns: *, ?, character classes and ranges, negation, escapes
 * - the rules of the ACL file: passwords and hashes, enabling, key patterns by access kind, channels,
 *   categories and commands applied in order, reset; error messages with line numbers
 * - logins: wrong password, disabled user, nopass, the default user, several passwords
 * - credentials files of earlier versions, including several lines for one user
 * - canonical descriptions of users
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acl.h"
#include "sha256.h"

static int failures;

#define EXPECT(cond, ...)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                             \
            fprintf(stderr, __VA_ARGS__);                                                                    \
            fputc('\n', stderr);                                                                             \
            failures++;                                                                                      \
        }                                                                                                    \
    } while (0)

static void hex(const uint8_t *d, char *out) {
    for (unsigned i = 0; i < AKV_SHA256_LEN; i++) {
        sprintf(out + 2 * i, "%02x", d[i]);
    }
}

static void expect_sha(const void *data, size_t n, const char *want) {
    uint8_t d[AKV_SHA256_LEN];
    char h[65];
    akv_sha256(data, n, d);
    hex(d, h);
    EXPECT(strcmp(h, want) == 0, "sha256 of %zu bytes: %s, expected %s", n, h, want);
}

static void test_sha256(void) {
    /* FIPS 180-4 examples (NIST CSRC "SHA256.pdf", "SHA256_2.pdf", and the one million 'a' vector). */
    expect_sha("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect_sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect_sha("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    char *m = malloc(1000000);
    memset(m, 'a', 1000000);
    expect_sha(m, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    /* padding boundaries: 55 bytes fit one block with the length, 56 and 64 need a second block */
    expect_sha(m, 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    expect_sha(m, 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    expect_sha(m, 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    free(m);
}

static void test_glob(void) {
    struct {
        const char *pat;
        const char *s;
        int want;
    } cases[] = {
        {"*", "", 1},
        {"*", "anything", 1},
        {"user:*", "user:42", 1},
        {"user:*", "users:42", 0},
        {"h?llo", "hello", 1},
        {"h?llo", "hllo", 0},
        {"h[ae]llo", "hallo", 1},
        {"h[ae]llo", "hillo", 0},
        {"h[^e]llo", "hallo", 1},
        {"h[^e]llo", "hello", 0},
        {"h[a-c]llo", "hbllo", 1},
        {"h[a-c]llo", "hdllo", 0},
        {"a\\*b", "a*b", 1},
        {"a\\*b", "axb", 0},
        {"*:*:x", "a:b:x", 1},
        {"*:*:x", "a:b:y", 0},
        {"**a", "bbba", 1},
        {"", "", 1},
        {"", "a", 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = acl_glob(cases[i].pat, strlen(cases[i].pat), cases[i].s, strlen(cases[i].s));
        EXPECT(got == cases[i].want, "glob '%s' on '%s': %d", cases[i].pat, cases[i].s, got);
    }
}

static int cmd_ok(const acl_user *u, enum acl_proto proto, const char *name) {
    int id = acl_cmd_find(proto, name, strlen(name));
    EXPECT(id >= 0, "command %s is in the table", name);
    return id >= 0 && acl_cmd_allowed(u, (unsigned)id);
}

static void test_rules(void) {
    char err[256] = "";
    const char *text =
        "# comment\n"
        "user default on nopass ~cache:* -@all +@read +@connection\n"
        "user admin on >secret allkeys allchannels +@all\n"
        "user app on >p1 >p2 ~app:* %R~shared:* &news.* +@all -@dangerous\n"
        "user order on >x -@dangerous +@all ~*\n"
        "user writer on #5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8 %W~log:* "
        "-@all +set\n"
        "user off1 off >pw +@all ~*\n"
        "user wiped on >pw +@all ~* reset\n";
    acl *a = acl_parse(text, 0, err, sizeof(err));
    EXPECT(a != NULL, "parse: %s", err);
    if (a == NULL) {
        return;
    }
    EXPECT(acl_nusers(a) == 7, "7 users");
    const acl_user *def = acl_default_login(a);
    EXPECT(def != NULL, "default user logs in without a password");
    EXPECT(cmd_ok(def, ACL_RESP, "GET") && !cmd_ok(def, ACL_RESP, "SET"), "default: read only");
    EXPECT(cmd_ok(def, ACL_RESP, "auth") && cmd_ok(def, ACL_RESP, "PING"), "connection commands");
    EXPECT(acl_key_allowed(def, "cache:1", 7, ACL_KEY_READ) &&
               !acl_key_allowed(def, "other", 5, ACL_KEY_READ),
           "default: key pattern");
    EXPECT(cmd_ok(def, ACL_MC, "get") && !cmd_ok(def, ACL_MC, "set") && !cmd_ok(def, ACL_NATIVE, "PUT"),
           "categories govern memcached and native commands");

    EXPECT(acl_login(a, "admin", 5, "secret", 6) != NULL, "admin logs in");
    EXPECT(acl_login(a, "admin", 5, "Secret", 6) == NULL, "wrong password");
    EXPECT(acl_login(a, "nobody", 6, "secret", 6) == NULL, "unknown user");
    const acl_user *adm = acl_find(a, "admin", 5);
    EXPECT(acl_unrestricted(adm), "admin is unrestricted");

    const acl_user *app = acl_login(a, "app", 3, "p2", 2);
    EXPECT(app != NULL && acl_login(a, "app", 3, "p1", 2) == app, "two passwords");
    EXPECT(!acl_unrestricted(app), "app is restricted");
    EXPECT(cmd_ok(app, ACL_RESP, "SET") && !cmd_ok(app, ACL_RESP, "FLUSHALL") &&
               !cmd_ok(app, ACL_RESP, "KEYS"),
           "+@all -@dangerous");
    EXPECT(!cmd_ok(app, ACL_MC, "flush_all") && !cmd_ok(app, ACL_NATIVE, "FLUSH") &&
               cmd_ok(app, ACL_MC, "set"),
           "dangerous memcached and native commands denied");
    EXPECT(acl_key_allowed(app, "app:x", 5, ACL_KEY_READ | ACL_KEY_WRITE), "app: own keys read and write");
    EXPECT(acl_key_allowed(app, "shared:x", 8, ACL_KEY_READ), "app: shared keys readable");
    EXPECT(!acl_key_allowed(app, "shared:x", 8, ACL_KEY_WRITE), "app: shared keys not writable");
    EXPECT(!acl_key_allowed(app, "shared:x", 8, ACL_KEY_READ | ACL_KEY_WRITE), "read-modify needs both");
    EXPECT(acl_channel_allowed(app, "news.sport", 10) && !acl_channel_allowed(app, "chat", 4), "channels");

    const acl_user *ord = acl_find(a, "order", 5);
    EXPECT(cmd_ok(ord, ACL_RESP, "FLUSHALL"), "-@dangerous +@all allows dangerous commands (order matters)");

    const acl_user *wr = acl_login(a, "writer", 6, "password", 8);
    EXPECT(wr != NULL, "login with a stored SHA-256 hash");
    EXPECT(cmd_ok(wr, ACL_RESP, "SET") && !cmd_ok(wr, ACL_RESP, "GET"), "-@all +set");
    EXPECT(acl_key_allowed(wr, "log:1", 5, ACL_KEY_WRITE) && !acl_key_allowed(wr, "log:1", 5, ACL_KEY_READ),
           "%%W~ grants writing only");

    EXPECT(acl_login(a, "off1", 4, "pw", 2) == NULL, "disabled user cannot log in");
    const acl_user *wiped = acl_find(a, "wiped", 5);
    EXPECT(acl_login(a, "wiped", 5, "pw", 2) == NULL && !cmd_ok(wiped, ACL_RESP, "GET"), "reset");
    EXPECT(cmd_ok(wiped, ACL_RESP, "AUTH"), "reset keeps the connection commands");

    char desc[1024];
    acl_describe(app, desc, sizeof(desc));
    EXPECT(strstr(desc, "user app on #") != NULL && strstr(desc, " ~app:*") != NULL &&
               strstr(desc, " %R~shared:*") != NULL && strstr(desc, " &news.*") != NULL &&
               strstr(desc, "+@all -@dangerous") != NULL,
           "describe: %s", desc);
    acl_describe(wr, desc, sizeof(desc));
    EXPECT(strstr(desc, "#5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8") != NULL &&
               strstr(desc, "resetchannels") != NULL,
           "describe keeps the hash: %s", desc);
    acl_free(a);

    /* no default login without nopass */
    a = acl_parse("user default on >pw +@all ~*\n", 0, err, sizeof(err));
    EXPECT(a != NULL && acl_default_login(a) == NULL && acl_login(a, NULL, 0, "pw", 2) != NULL,
           "default user with a password");
    acl_free(a);

    struct {
        const char *text;
        const char *msg;
    } bad[] = {
        {"user a on\nuser a off\n", "line 2: user 'a' is defined twice"},
        {"\nuser a on +nosuchcmd\n", "line 2: '+nosuchcmd': unknown command"},
        {"user a on +@nosuch\n", "line 1: '+@nosuch': unknown category"},
        {"user a on #abc\n", "line 1: '#abc': a password hash has 64 hexadecimal digits"},
        {"user a on +client|kill\n", "line 1: '+client|kill': rules for subcommands are not supported"},
        {"users a on\n", "line 1: a line starts with 'user'"},
        {"user a on %X~k\n", "line 1: '%X~k': expected %R~, %W~ or %RW~"},
        {"user a on frobnicate\n", "line 1: 'frobnicate': unknown rule"},
        {"# only comments\n", "no users defined"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err[0] = '\0';
        a = acl_parse(bad[i].text, 0, err, sizeof(err));
        EXPECT(a == NULL && strcmp(err, bad[i].msg) == 0, "error for %s: '%s'", bad[i].text, err);
        acl_free(a);
    }
}

static void test_compat(void) {
    char err[256];
    acl *a = acl_parse("# credentials\nalice:one\nalice:two\nbob:x:y\nbare\n", 1, err, sizeof(err));
    EXPECT(a != NULL, "compat parse: %s", err);
    if (a == NULL) {
        return;
    }
    EXPECT(acl_nusers(a) == 3, "three users (alice twice)");
    EXPECT(acl_login(a, "alice", 5, "one", 3) != NULL && acl_login(a, "alice", 5, "two", 3) != NULL,
           "repeated user: both passwords");
    EXPECT(acl_login(a, "bob", 3, "x:y", 3) != NULL, "password after the first colon");
    const acl_user *d = acl_login(a, NULL, 0, "bare", 4);
    EXPECT(d != NULL && acl_unrestricted(d), "bare password: default user with all rights");
    EXPECT(acl_default_login(a) == NULL, "authentication required");
    acl_free(a);
}

int main(void) {
    test_sha256();
    test_glob();
    test_rules();
    test_compat();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_acl: ok");
    return 0;
}
