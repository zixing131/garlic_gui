#include "browse_index.h"
#include "class_selection.h"
#include <errno.h>
#include <sys/stat.h>
#include "common/output_error.h"
#include "parser/dex/metadata.h"
#include "dalvik/dex_decompile.h"
#include "dalvik/dex_structure.h"
#include "dalvik/dex_class.h"
#include "dalvik/dex_pre_optimizer.h"
#include "decompiler/expression_branches.h"
#include "decompiler/exception.h"
#include "decompiler/expression_synchronized.h"
#include "decompiler/expression_writter.h"
#include "dex_smali.h"
#include "apk_manifest.h"
#include "file_tools.h"
#include "common/jd_progress.h"
#include "libs/memory/mem_pool.h"

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
    const int done = apk->done;
    const int total = apk->added;
    /* Small class batches must not serialize every worker on a flushed pipe. */
    if (!jd_progress_has() && (done == 1 || (done & 255) == 0)) {
        for (int i = 0; i < apk_progress_len; i++) putchar('\b');
        apk_progress_len = printf("Progress : %d (%d)", done, total);
        fflush(stdout);
    }
    if (apk->threadpool)
        pthread_mutex_unlock(apk->threadpool->lock);
    if (jd_progress_has())
        jd_progress_report(done, total, "apk");
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
        output_close(jf->source, jf->fname);
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
    if (stream != NULL) {
        dex_class_def_to_smali(dex->meta, cf, stream);
        output_close(stream, dex_str_of_type_id(dex->meta, cf->class_idx));
    }

    mem_pool_free(tls->pool);
    tls->pool = NULL;

    apk_status(apk);
}

/* Bounded batches preserve DEX/class order (and duplicate-class precedence),
 * while independent workers build JSON with their own scratch pools. */
typedef struct {
    jd_meta_dex *meta;
    dex_class_def *cf;
    pthread_mutex_t *lock;
    pthread_cond_t *changed;
    char *json;
    int ready;
} index_class_job;
static void index_class_worker(void *opaque) {
    index_class_job *job = opaque;
    char *json = browse_index_dex_json(job->meta, job->cf);
    pthread_mutex_lock(job->lock);
    job->json = json; job->ready = 1;
    pthread_cond_broadcast(job->changed);
    pthread_mutex_unlock(job->lock);
}
static void index_dex_classes(jd_apk *apk, jd_meta_dex *meta) {
    if (!apk->threadpool) {
        for (u4 j = 0; j < meta->header->class_defs_size; ++j) browse_index_dex(meta, &meta->class_defs[j]);
        return;
    }
    pthread_mutex_t lock; pthread_mutex_init(&lock, NULL);
    pthread_cond_t changed; pthread_cond_init(&changed, NULL);
    for (u4 start = 0; start < meta->header->class_defs_size; start += 128) {
        index_class_job jobs[128] = {0};
        u4 count = meta->header->class_defs_size - start;
        if (count > 128) count = 128;
        for (u4 i = 0; i < count; ++i) {
            jobs[i].meta = meta; jobs[i].cf = &meta->class_defs[start + i];
            jobs[i].lock = &lock; jobs[i].changed = &changed;
            if (threadpool_add(apk->threadpool, index_class_worker, &jobs[i], 0) != 0) {
                // A lock/notification error may occur after enqueueing. Never
                // retry the same stack job and risk executing it twice.
                fprintf(stderr, "Failed to queue metadata index task\n");
                exit(EXIT_FAILURE);
            }
        }
        for (u4 i = 0; i < count; ++i) {
            pthread_mutex_lock(&lock);
            while (!jobs[i].ready) pthread_cond_wait(&changed, &lock);
            char *json = jobs[i].json;
            pthread_mutex_unlock(&lock);
            class_selection_write_line(json); free(json);
        }
    }
    pthread_cond_destroy(&changed); pthread_mutex_destroy(&lock);
}

static void apk_run_dex_tasks(jd_apk *apk, jd_meta_dex *meta);
static void apk_process_dex_file(jd_apk *apk, string path);

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

        // The GUI index records the owning DEX. A single-class request can
        // skip every other DEX instead of reparsing the entire application.
        const char *selected_dex = getenv("GARLIC_DEX_ENTRY");
        if (class_selection_explicit() && selected_dex && *selected_dex &&
            strcmp(selected_dex, path_in_zip)) {
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
        if (class_selection_indexing()) {
            index_dex_classes(apk, meta);
            mem_pool_free(meta->pool);
            fprintf(stderr, "GARLIC_INDEX_PROGRESS %d %d\n", ++dex_done, dex_total);
            continue;
        }
        apk_run_dex_tasks(apk, meta);
    }
}

static void apk_process_dex_file(jd_apk *apk, string path)
{
    jd_meta_dex *meta = parse_dex_file(path);
    if (meta == NULL)
        return;
    if (class_selection_indexing()) {
        index_dex_classes(apk, meta);
        mem_pool_free(meta->pool);
        return;
    }
    apk_run_dex_tasks(apk, meta);
}

static void apk_run_dex_tasks(jd_apk *apk, jd_meta_dex *meta)
{
    if (meta == NULL)
        return;

    if (apk->dex_pools != NULL)
        ladd_obj(apk->dex_pools, meta->pool);

    jd_dex *dex = dex_init_without_thread(meta);
    meta->source_dir = apk->save_dir;
    dex_reserve_output_paths(meta, apk->type == JD_DEX_TASK_DECOMPILE);

    string class_filter = getenv("GARLIC_CLASS_FILTER");

    for (int j = 0; j < meta->header->class_defs_size; ++j) {
        dex_class_def *cf = &meta->class_defs[j];
        if (class_filter != NULL &&
            strstr(dex_str_of_type_id(meta, cf->class_idx), class_filter) == NULL)
            continue;
        if (apk->type == JD_DEX_TASK_DECOMPILE) {
            if (dex_class_is_rebuilt_lambda_class(dex->meta, cf))
                continue;
            if (!class_selection_explicit() &&
                (dex_class_is_inner_class(dex->meta, cf) ||
                 dex_class_is_anonymous_class(dex->meta, cf)))
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
                               (void (*)(void *)) &apk_smali_thread_task,
                               t,
                               0);
            }
            else {
                ret = threadpool_add(apk->threadpool,
                               (void (*)(void *)) &apk_decompile_thread_task,
                               t,
                               0);
            }
            if (ret != 0) {
                fprintf(stderr, "[garlic] Warning: threadpool_add failed with %d\n", ret);
            }
            apk->added++;
        } else {
            /* The class helpers create and restore their own scratch pool. */
            mem_pool *parse_pool = global_pool;
            apk->added++;
            if (apk->type == JD_DEX_TASK_SMALI)
                dex_smali_class(dex, cf);
            else
                dex_decompile_class(dex, cf);
            global_pool = parse_pool;
            apk_status(apk);
        }
    }
}

static void apk_decompile_task_start(jd_apk *apk)
{
    bool manifest_done = false;

    for (int s = 0; s < apk->source_count; ++s) {
        string src = apk->sources[s];
        if (src == NULL)
            continue;

        if (str_end_with(src, ".dex")) {
            apk_process_dex_file(apk, src);
            continue;
        }

        struct zip_t *zip = zip_open(src, 0, 'r');
        if (zip == NULL) {
            fprintf(stderr, "\n[garlic] Failed to open APK: %s (invalid zip?)\n", src);
            continue;
        }
        apk->zip = zip;
        apk->path = src;
        apk->entries_size = zip_entries_total(zip);

        if (!manifest_done && !class_selection_indexing()) {
            for (int i = 0; i < (int)apk->entries_size; ++i) {
                zip_entry_openbyindex(zip, i);
                string path_in_zip = (string)zip_entry_name(zip);
                if (path_in_zip == NULL) {
                    continue;
                }

                if (str_end_with(path_in_zip, "AndroidManifest.xml")) {
                    apk_parse_manifest_from_zip(apk);
                    manifest_done = true;
                    zip_entry_close(zip);
                    break;
                }
                zip_entry_close(zip);
            }
        }

        struct stat input_stat;
        string chain = stat(src, &input_stat) == 0
            ? str_create("%zu:%s:%lld:%lld", strlen(src), src,
                         (long long)input_stat.st_size, (long long)input_stat.st_mtime)
            : src;
        index_inflater *inflater = start_index_inflater(apk, zip);
        apk_process_dex_from_zip(apk, zip, chain, inflater);
        stop_index_inflater(inflater);
        zip_close(zip);
        apk->zip = NULL;
    }
}

static void apk_release(jd_apk *apk)
{
    if (apk->threadpool)
        threadpool_destroy(apk->threadpool, 1);

    if (!jd_progress_has() && apk->done) {
        printf("\nProgress : %d (%d)\n", apk->done, apk->added);
        fflush(stdout);
    }

    if (getenv("GARLIC_POOL_STAT") != NULL) {
        double mb = 1024.0 * 1024.0;
        fprintf(stderr, "[garlic] pool apk: %.0f MB\n",
                apk->pool->total_size / mb);
        if (apk->dex_pools != NULL) {
            size_t total = 0;
            for (size_t i = 0; i < apk->dex_pools->size; ++i) {
                mem_pool *p = (mem_pool *) lget_obj(apk->dex_pools, i);
                total += p->total_size;
            }
            fprintf(stderr, "[garlic] pool dex x%zu: %.0f MB\n",
                    apk->dex_pools->size, total / mb);
        }
        if (apk->sources != NULL && apk->source_count > 0)
            fprintf(stderr, "[garlic] source: %s\n", apk->sources[0]);
    }

    if (apk->dex_pools != NULL) {
        for (size_t i = 0; i < apk->dex_pools->size; ++i)
            mem_pool_free((mem_pool *) lget_obj(apk->dex_pools, i));
    }

    mem_pool_free(apk->pool);
    mem_free_pool();
}

static void apk_analyse_sources(string *sources,
                                int source_count,
                                string save_dir,
                                int thread_num,
                                jd_dex_task_type type,
                                int allow_nested_dex)
{
    if (sources == NULL || source_count <= 0)
        return;

    mem_init_pool();

    mem_pool *pool = mem_create_pool();
    jd_apk *apk = make_obj_in(jd_apk, pool);
    apk->pool = pool;
    apk->sources = sources;
    apk->source_count = source_count;
    apk->path = sources[0];
    apk->save_dir = save_dir;
    apk->thread_num = thread_num;
    apk->type = type;
    apk->allow_nested_dex = allow_nested_dex;
    apk->dex_pools = linit_object_with_pool(apk->pool);
    /* One worker still has to take the task path. The synchronous path does
     * not share its scratch arena the way a worker does, and case-colliding
     * output is tracked from that path. */
    if (thread_num > 0)
        apk->threadpool = threadpool_create_in(apk->pool, thread_num, 0);

    apk_decompile_task_start(apk);

    string done_path = apk->path;

    apk_release(apk);

    if (getenv("GARLIC_SUFFIX_STAT") != NULL) {
        fprintf(stderr, "[garlic] shared suffixes copied: %d (after %s)\n",
                share_suffix_copy_count(), done_path);
    }

    if (getenv("GARLIC_IF_STAT") != NULL) {
        int promoted = 0;
        int merged = 0;
        int shared = 0;
        empty_if_branch_stat(&promoted, &merged, &shared);
        fprintf(stderr,
                "[garlic] empty if branches taken out: %d, "
                "nested ifs merged: %d, "
                "branches left empty by a shared target: %d (after %s)\n",
                promoted, merged, shared, done_path);
    }

    exc_time_report();
    empty_if_left_report();
    expand_stat_report();
    sync_stat_report();
}

void apk_decompile_analyse_sources(string *sources,
                                   int source_count,
                                   string save_dir,
                                   int thread_num,
                                   jd_dex_task_type type)
{
    apk_analyse_sources(sources, source_count, save_dir, thread_num, type, 0);
}

void apk_decompile_analyse(string path,
                           string save_dir,
                           int thread_num,
                           jd_dex_task_type type)
{
    string one[1];
    one[0] = path;
    apk_analyse_sources(one, 1, save_dir, thread_num, type, 0);
}

void archive_decompile_analyse(string path,
                               string save_dir,
                               int thread_num,
                               jd_dex_task_type type)
{
    string one[1];
    one[0] = path;
    apk_analyse_sources(one, 1, save_dir, thread_num, type, 1);
}
