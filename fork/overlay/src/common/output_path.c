#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "output_path.h"
#include "common/str_tools.h"
#include "libs/hashmap/hashmap_tools.h"

/* Keyed on the folded path, so a second spelling of it finds the first;
 * the value is the file name that owns it. `resolved` remembers what each
 * requested path was actually written as, which is what a caller asking
 * twice has to get back.
 *
 * Both maps and everything in them come out of `paths` rather than the
 * caller's pool: a lookup happens in the middle of a worker's task, and
 * that task's pool is freed when the task ends - a key allocated from it
 * would be a dangling pointer in here the moment the next class asked. */
static mem_pool *paths;
static hashmap *claimed;
static hashmap *resolved;

static pthread_mutex_t path_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t path_once = PTHREAD_ONCE_INIT;

static void output_path_init(void)
{
    paths = mem_create_pool();
    claimed = hashmap_init_in(paths, (hcmp_fn) s2o_cmp, 0);
    resolved = hashmap_init_in(paths, (hcmp_fn) s2o_cmp, 0);
}

/* ASCII only. tolower() follows the process locale, so a Turkish locale
 * would fail to treat Foo.java and foo.java as the same Windows path. */
static string folded_copy(string s)
{
    string lower = str_create_in(paths, "%s", s);
    for (unsigned char *p = (unsigned char *)lower; *p != '\0'; ++p)
        if (*p >= 'A' && *p <= 'Z')
            *p = (unsigned char)(*p + 32);
    return lower;
}

/* `Foo.java` and 2 make `Foo_2.java` - the extension stays last. */
static string suffixed(string name, int n)
{
    char *dot = strrchr(name, '.');
    if (dot == NULL)
        return str_create_in(paths, "%s_%d", name, n);
    return str_create_in(paths, "%.*s_%d%s",
                         (int) (dot - name), name, n, dot);
}

/* CON.java, AUX.java, COM1.java and the other device names are rejected by
 * Windows even when they sit inside a directory. */
static int windows_reserved_file(const char *name)
{
#ifdef _WIN32
    const char *dot = strchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    char buf[8];
    if (n == 0 || n >= sizeof buf)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        buf[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    buf[n] = 0;
    if (!strcmp(buf, "con") || !strcmp(buf, "prn") ||
        !strcmp(buf, "aux") || !strcmp(buf, "nul"))
        return 1;
    if (n == 4 && (!strncmp(buf, "com", 3) || !strncmp(buf, "lpt", 3)) &&
        buf[3] >= '1' && buf[3] <= '9')
        return 1;
    return 0;
#else
    (void)name;
    return 0;
#endif
}

/* The name this file would be written as, claiming it if it is free.
 * Called with the lock held; a returned string outlives the call. */
static string claim_locked(string dir, string name, bool *gave_way)
{
    string exact = str_create("%s/%s", dir, name);
    string done = hget_s2o(resolved, exact);
    if (done != NULL)
        return done;

    string folded = folded_copy(exact);
    if (!windows_reserved_file(name) && hget_s2o(claimed, folded) == NULL) {
        string owned = str_create_in(paths, "%s", name);
        hset_s2o(claimed, folded, owned);
        hset_s2o(resolved, str_create_in(paths, "%s", exact), owned);
        return owned;
    }

    /* A class spelled differently already owns this name. The suffix is
     * handed out in the order the names were reserved, and the classes
     * are reserved in the order they are declared, so a given build
     * always resolves a pair the same way. */
    for (int n = 2; ; ++n) {
        string candidate = suffixed(name, n);
        if (windows_reserved_file(candidate))
            continue;
        string candidate_key = folded_copy(
                str_create("%s/%s", dir, candidate));
        if (hget_s2o(claimed, candidate_key) == NULL) {
            hset_s2o(claimed, candidate_key, candidate);
            hset_s2o(resolved, str_create_in(paths, "%s", exact), candidate);
            if (gave_way != NULL)
                *gave_way = true;
            return candidate;
        }
    }
}

bool output_path_reserve(string dir, string name)
{
    pthread_once(&path_once, output_path_init);
    bool gave_way = false;
    pthread_mutex_lock(&path_lock);
    claim_locked(dir, name, &gave_way);
    pthread_mutex_unlock(&path_lock);
    return gave_way;
}

string output_path_resolve(string dir, string name)
{
    pthread_once(&path_once, output_path_init);
    pthread_mutex_lock(&path_lock);
    string assigned = claim_locked(dir, name, NULL);
    pthread_mutex_unlock(&path_lock);
    return assigned;
}
