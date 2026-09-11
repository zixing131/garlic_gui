#include "browse_index.h"
#include "class_selection.h"
#include <errno.h>
//#include "apk/apk.h"
#include "parser/dex/metadata.h"
#include "dalvik/dex_decompile.h"
#include "dalvik/dex_structure.h"
#include "dalvik/dex_class.h"
#include "decompiler/expression_writter.h"
#include "dex_smali.h"
#include "apk_manifest.h"
#include "file_tools.h"

/* Inflate independent DEX entries concurrently; the main thread still parses
 * and emits them in archive order, preserving deterministic class precedence. */
typedef struct {
    int entry, ready;
    size_t size;
    char *buffer;
} index_inflate_job;
typedef struct {
    const char *path;
    index_inflate_job *jobs;
    int count, next, workers;
    pthread_t threads[8];
    pthread_mutex_t lock;
    pthread_cond_t changed;
} index_inflater;
static void *inflate_index_worker(void *opaque) {
    index_inflater *state = opaque;
    struct zip_t *archive = zip_open(state->path, 0, 'r');
    for (;;) {
        pthread_mutex_lock(&state->lock);
        int n = state->next++;
        pthread_mutex_unlock(&state->lock);
        if (n >= state->count) break;
        index_inflate_job *job = &state->jobs[n];
        char *buffer = malloc(job->size);
        int ok = archive && buffer && zip_entry_openbyindex(archive, job->entry) == 0;
        if (ok) {
            ok = zip_entry_noallocread(archive, buffer, job->size) == (ssize_t)job->size;
            zip_entry_close(archive);
        }
        if (!ok) { free(buffer); buffer = NULL; }
        pthread_mutex_lock(&state->lock);
        job->buffer = buffer; job->ready = 1;
        pthread_cond_broadcast(&state->changed);
        pthread_mutex_unlock(&state->lock);
    }
    if (archive) zip_close(archive);
    return NULL;
}
static index_inflater *start_index_inflater(jd_apk *apk, struct zip_t *zip) {
    if (!class_selection_indexing()) return NULL;
    index_inflater *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->jobs = calloc(zip_entries_total(zip), sizeof(*state->jobs));
    if (!state->jobs) { free(state); return NULL; }
    state->path = apk->path;
    for (int i = 0; i < zip_entries_total(zip); ++i) {
        if (zip_entry_openbyindex(zip, i) < 0) continue;
        const char *name = zip_entry_name(zip);
        if (str_end_with((string)name, ".dex") &&
            (apk->allow_nested_dex || !strchr(name, '/'))) {
            index_inflate_job *job = &state->jobs[state->count++];
            job->entry = i; job->size = zip_entry_size(zip);
        }
        zip_entry_close(zip);
    }
    pthread_mutex_init(&state->lock, NULL);
    pthread_cond_init(&state->changed, NULL);
    for (int i = 0; i < 8 && i < apk->thread_num && i < state->count; ++i) {
        if (pthread_create(&state->threads[state->workers], NULL, inflate_index_worker, state)) break;
        ++state->workers;
    }
    if (!state->workers) inflate_index_worker(state);
    return state;
}
static void stop_index_inflater(index_inflater *state) {
    if (!state) return;
    for (int i = 0; i < state->workers; ++i) pthread_join(state->threads[i], NULL);
    for (int i = 0; i < state->count; ++i) free(state->jobs[i].buffer);
    pthread_cond_destroy(&state->changed); pthread_mutex_destroy(&state->lock);
    free(state->jobs); free(state);
}

static int apk_progress_len = 0;

void apk_status(jd_apk *apk)
{
    if (apk->threadpool)
        pthread_mutex_lock(apk->threadpool->lock);
    apk->done++;
    for (int i = 0; i < apk_progress_len; i++) putchar('\b');
    apk_progress_len = printf("Progress : %d (%d)", apk->done, apk->added);
    fflush(stdout);
    if (apk->threadpool)
        pthread_mutex_unlock(apk->threadpool->lock);
}

void apk_entry_thread_task(jd_meta_dex *meta)
{
    thread_local_data *tls = get_thread_local_data();
    tls->pool = mem_create_pool();

    dex_analyse_in_apk_task(meta);

    mem_pool_free(tls->pool);
    tls->pool = NULL;
}

void apk_decompile_thread_task(jd_dex_task *task)
{
    thread_local_data *tls = get_thread_local_data();
    tls->pool = mem_create_pool();

    jd_dex *dex = task->dex;
    jd_apk *apk = task->apk;
    dex_class_def *cf = task->cf;

    jsource_file *jf = dex_class_inside(dex, cf, NULL);

    if (jf->parent == NULL) {
        writter_for_class(jf, NULL);
        fclose(jf->source);
    }

    mem_pool_free(tls->pool);
    tls->pool = NULL;

    apk_status(apk);
}

void apk_smali_thread_task(jd_dex_task *task)
{
    thread_local_data *tls = get_thread_local_data();
    tls->pool = mem_create_pool();

    jd_dex *dex = task->dex;
    jd_apk *apk = task->apk;
    dex_class_def *cf = task->cf;

    FILE *stream = dex_class_smali_save_dir(dex, cf);

    dex_class_def_to_smali(dex->meta, cf, stream);

    if (stream != NULL)
        fclose(stream);

    mem_pool_free(tls->pool);
    tls->pool = NULL;

    apk_status(apk);
}

static void apk_process_dex_from_zip(jd_apk *apk, struct zip_t *zip, const char *chain, index_inflater *inflater)
{
    if (zip == NULL) return;
    int total = zip_entries_total(zip);
    int dex_total = 0, dex_done = 0;
    if (class_selection_indexing()) {
        for (int i = 0; i < total; ++i) {
            if (zip_entry_openbyindex(zip, i) < 0) continue;
            const char *name = zip_entry_name(zip);
            if (str_end_with((string)name, ".dex") &&
                (apk->allow_nested_dex || !strchr(name, '/'))) ++dex_total;
            zip_entry_close(zip);
        }
        fprintf(stderr, "GARLIC_INDEX_PROGRESS 0 %d\n", dex_total);
    }
    for (int i = 0; i < total; ++i) {
        zip_entry_openbyindex(zip, i);
        string path_in_zip = (string)zip_entry_name(zip);

        if (str_end_with(path_in_zip, ".apk")) {
            size_t buf_size = zip_entry_size(zip);
            string key = str_create("%s|%zu:%s:%u:%zu", chain, strlen(path_in_zip), path_in_zip,
                                    zip_entry_crc32(zip), buf_size);
            const char *cache = getenv("GARLIC_APK_CACHE_DIR");
            if (cache && *cache) {
                char *stored = hex_storage_name(key);
                string path = str_create("%s/%s.apk", cache, stored);
                free(stored);
                if (!file_exist(path)) {
                    char *parent = str_dup(path), *slash = strrchr(parent, '/');
                    if (slash) { *slash = 0; mkdir_p(parent); }
                    string temporary = str_create("%s.%ld.tmp", path, (long)getpid());
                    if (zip_entry_fread(zip, temporary) == 0)
                        rename(temporary, path);
                    remove(temporary);
                }
                struct zip_t *nested = zip_open(path, 0, 'r');
                if (nested) {
                    apk_process_dex_from_zip(apk, nested, key, NULL);
                    zip_close(nested);
                    zip_entry_close(zip);
                    continue;
                }
            }
            char *buf = malloc(buf_size);
            if (buf) {
                zip_entry_noallocread(zip, (void *)buf, buf_size);
                struct zip_t *nested = zip_stream_open(buf, buf_size, 0, 'r');
                if (nested) {
                    apk_process_dex_from_zip(apk, nested, key, NULL);
                    zip_stream_close(nested);
                }
                free(buf);
            }
            zip_entry_close(zip);
            continue;
        }

        if ((!apk->allow_nested_dex && strchr(path_in_zip, '/') != NULL) ||
            !str_end_with(path_in_zip, ".dex")) {
            zip_entry_close(zip);
            continue;
        }

        class_selection_origin(path_in_zip);
        size_t buf_size = zip_entry_size(zip);
        char *buf = NULL;
        if (inflater && zip == apk->zip) {
            for (int n = 0; n < inflater->count; ++n) {
                index_inflate_job *job = &inflater->jobs[n];
                if (job->entry != i) continue;
                pthread_mutex_lock(&inflater->lock);
                while (!job->ready) pthread_cond_wait(&inflater->changed, &inflater->lock);
                buf = job->buffer;
                pthread_mutex_unlock(&inflater->lock);
                break;
            }
        }
        if (!buf) {
            buf = x_alloc_in(apk->pool, buf_size);
            if (zip_entry_noallocread(zip, (void *)buf, buf_size) != (ssize_t)buf_size) {
                fprintf(stderr, "Failed to read DEX entry\n"); exit(EXIT_FAILURE);
            }
        }
        zip_entry_close(zip);

        int directory = browse_index_dex_directory((const unsigned char *)buf, buf_size);
        if (directory < 0) { fprintf(stderr, "Invalid DEX directory\n"); exit(EXIT_FAILURE); }
        if (directory > 0) {
            fprintf(stderr, "GARLIC_INDEX_PROGRESS %d %d\n", ++dex_done, dex_total);
            continue;
        }
        jd_meta_dex *meta = parse_dex_from_buffer(buf, buf_size);
        jd_dex *dex = dex_init_without_thread(meta);
        meta->source_dir = apk->save_dir;

        for (int j = 0; j < meta->header->class_defs_size; ++j) {
            dex_class_def *cf = &meta->class_defs[j];
            if (class_selection_indexing()) { browse_index_dex(meta, cf); continue; }
            if (apk->type == JD_DEX_TASK_DECOMPILE && !class_selection_explicit()) {
                if (dex_class_is_inner_class(dex->meta, cf) ||
                    dex_class_is_anonymous_class(dex->meta, cf))
                    continue;
            }

            if (!class_selection_accept(dex_str_of_type_id(meta, cf->class_idx)))
                continue;

            if (apk->threadpool) {
                jd_dex_task *t = make_obj(jd_dex_task);
                t->dex = dex;
                t->cf = cf;
                t->apk = apk;
                t->type = apk->type;
                int ret;
                if (t->type == JD_DEX_TASK_SMALI) {
                    ret = threadpool_add(apk->threadpool,
                                   &apk_smali_thread_task,
                                   t,
                                   0);
                }
                else {
                    ret = threadpool_add(apk->threadpool,
                                   &apk_decompile_thread_task,
                                   t,
                                   0);
                }
                if (ret != 0) {
                    fprintf(stderr, "[garlic] Warning: threadpool_add failed with %d\n", ret);
                }
                apk->added++;
            } else {
                /* The class helpers create/free their own global scratch pool.
                 * Preserve the APK parsing pool for the next DEX and release. */
                mem_pool *parse_pool = global_pool;
                apk->added++;
                if (apk->type == JD_DEX_TASK_SMALI) {
                    dex_smali_class(dex, cf);
                } else {
                    dex_decompile_class(dex, cf);
                }
                global_pool = parse_pool;
                apk_status(apk);
            }
        }
    }
}

static void apk_decompile_task_start(jd_apk *apk)
{
    struct zip_t *zip = zip_open(apk->path, 0, 'r');
    if (zip == NULL) {
        fprintf(stderr, "\n[garlic] Failed to open APK: %s (invalid zip?)\n", apk->path);
        return;
    }
    apk->zip = zip;
    apk->entries_size = zip_entries_total(zip);

    for (int i = 0; i < apk->entries_size; ++i) {
        zip_entry_openbyindex(zip, i);
        string path_in_zip = (string)zip_entry_name(zip);
        if (path_in_zip == NULL) {
            continue;
        }

        if (!class_selection_indexing() && str_end_with(path_in_zip, "AndroidManifest.xml")) {
            apk_parse_manifest_from_zip(apk);
            zip_entry_close(zip);
            break;
        }
        zip_entry_close(zip);
    }

    struct stat input_stat;
    string chain = stat(apk->path, &input_stat) == 0
        ? str_create("%zu:%s:%lld:%lld", strlen(apk->path), apk->path,
                     (long long)input_stat.st_size, (long long)input_stat.st_mtime)
        : apk->path;
    index_inflater *inflater = start_index_inflater(apk, zip);
    apk_process_dex_from_zip(apk, zip, chain, inflater);
    stop_index_inflater(inflater);
    zip_close(zip);
    apk->zip = NULL;
}

static void apk_release(jd_apk *apk)
{
    if (apk->threadpool)
        threadpool_destroy(apk->threadpool, 1);

    mem_pool_free(apk->pool);
    mem_free_pool();
}

static void archive_decompile_analyse_inner(string path,
                                            string save_dir,
                                            int thread_num,
                                            jd_dex_task_type type,
                                            int allow_nested_dex)
{
    mem_init_pool();

    mem_pool *pool = mem_create_pool();
    jd_apk *apk = make_obj_in(jd_apk, pool);
    apk->pool = pool;
    apk->path = path;
    apk->save_dir = save_dir;
    apk->thread_num = thread_num;
    apk->type = type;
    apk->allow_nested_dex = allow_nested_dex;

    if (thread_num > 1 && !class_selection_indexing()) {
        apk->threadpool = threadpool_create_in(apk->pool, thread_num, 0);
    } else {
        apk->threadpool = NULL;
    }

    apk_decompile_task_start(apk);

    apk_release(apk);
}

void apk_decompile_analyse(string path,
                           string save_dir,
                           int thread_num,
                           jd_dex_task_type type)
{
    archive_decompile_analyse_inner(path, save_dir, thread_num, type, 0);
}

void archive_decompile_analyse(string path,
                               string save_dir,
                               int thread_num,
                               jd_dex_task_type type)
{
    archive_decompile_analyse_inner(path, save_dir, thread_num, type, 1);
}
