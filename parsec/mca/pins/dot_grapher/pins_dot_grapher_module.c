/*
 * Copyright (c) 2010-2019 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#include "parsec/parsec_config.h"
#include "parsec/data.h"
#include "parsec/mca/pins/dot_grapher/pins_dot_grapher.h"
#include "parsec/parsec_internal.h"
#if defined(PARSEC_PROF_TRACE)
#include "parsec/parsec_binary_profile.h"
#endif
#include "parsec/parsec_internal.h"
#include "parsec/parsec_description_structures.h"
#include "parsec/utils/debug.h"
#include "parsec/class/parsec_hash_table.h"
#include "parsec/data_distribution.h"
#include "parsec/data_internal.h"
#include "parsec/utils/mca_param.h"
#include "parsec/execution_stream.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>

static pthread_mutex_t grapher_file_lock = PTHREAD_MUTEX_INITIALIZER;
static char *grapher_file_basename = NULL;
static FILE *grapher_file = NULL;
static parsec_hash_table_t *data_ht = NULL;
static int parsec_dot_grapher_memmode = 0;

typedef struct {
    parsec_data_collection_t *dc;
    parsec_data_key_t         data_key;
} parsec_grapher_data_identifier_t;

typedef struct {
    parsec_hash_table_item_t         ht_item;
    parsec_grapher_data_identifier_t id;
    char                            *did;
} parsec_grapher_data_identifier_hash_table_item_t;

static int grapher_data_id_key_equal(parsec_key_t a, parsec_key_t b, void *unused)
{
    (void)unused;
    parsec_grapher_data_identifier_t *id_a = (parsec_grapher_data_identifier_t *)a;
    parsec_grapher_data_identifier_t *id_b = (parsec_grapher_data_identifier_t *)b;
    return (id_a->dc == id_b->dc) && (id_a->data_key == id_b->data_key);
}

static char *grapher_data_id_key_print(char *buffer, size_t buffer_size, parsec_key_t k, void *unused)
{
    parsec_grapher_data_identifier_t *id = (parsec_grapher_data_identifier_t*)k;
    (void)unused;
    if( NULL == id->dc )
        snprintf(buffer, buffer_size, "NEW key %"PRIuPTR, (uintptr_t)id->data_key);
    else if( NULL != id->dc->key_base )
        snprintf(buffer, buffer_size, "DC(%s) key %"PRIuPTR, id->dc->key_base, (uintptr_t)id->data_key);
    else
        snprintf(buffer, buffer_size, "Uknown DC(%p) key %"PRIuPTR, id->dc, (uintptr_t)id->data_key);
    return buffer;
}

static uint64_t grapher_data_id_key_hash(parsec_key_t key, void *unused)
{
    parsec_grapher_data_identifier_t *id = (parsec_grapher_data_identifier_t*)key;
    uint64_t k = 0;
    (void)unused;
    k = ((uintptr_t)id->dc) | ((uintptr_t)id->data_key);
    return k;
}

static parsec_key_fn_t parsec_grapher_data_key_fns = {
    .key_equal = grapher_data_id_key_equal,
    .key_print = grapher_data_id_key_print,
    .key_hash  = grapher_data_id_key_hash
};

static char *parsec_dot_grapher_taskid(const parsec_task_t *task, char *tmp, int length)
{
    const parsec_task_class_t* tc = task->task_class;
    unsigned int i, index = 0;

    assert( NULL!= task->taskpool );
    index += snprintf( tmp + index, length - index, "%s_%u", tc->name, task->taskpool->taskpool_id );
    if(!isalpha(tmp[0])) tmp[0] = '_';
    for( i = 1; i < index; i++ ) {
        if(!isalnum(tmp[i])) tmp[i] = '_';
    }
    for( i = 0; i < tc->nb_parameters; i++ ) {
        index += snprintf( tmp + index, length - index, "_%d",
                           task->locals[tc->params[i]->context_index].value );
    }

    return tmp;
}

static void parsec_dot_grapher_task(const parsec_task_t *context,
                                     int thread_id, int vp_id, uint64_t task_hash)
{
    if( NULL != grapher_file ) {
        char tmp[MAX_TASK_STRLEN], nmp[MAX_TASK_STRLEN];
        char sim_date[64];
        assert(NULL != context->task_class->task_snprintf);
        context->task_class->task_snprintf(tmp, MAX_TASK_STRLEN, context);
        parsec_dot_grapher_taskid(context, nmp, MAX_TASK_STRLEN);
#if defined(PARSEC_SIM)
        snprintf(sim_date, 64, " [%d]", context->sim_exec_date);
#else
        sim_date[0]='\0';
#endif
        pthread_mutex_lock(&grapher_file_lock);
        fprintf(grapher_file,
            "%s [shape=\"polygon\","
            "label=\"<%d/%d> %s%s\","
            "tooltip=\"tpid=%u:tcid=%d:tcname=%s:tid=%"PRIu64"\"];\n",
            nmp,
            thread_id, vp_id, tmp, sim_date,
            context->taskpool->taskpool_id,
            context->task_class->task_class_id,
            context->task_class->name,
            task_hash);
        fflush(grapher_file);
        pthread_mutex_unlock(&grapher_file_lock);
    }
}

static void parsec_dot_grapher_complete_exec_begin(struct parsec_pins_next_callback_s* cb_data,
                                                   struct parsec_execution_stream_s*   es,
                                                   struct parsec_task_s*               task)
{
    (void)cb_data;
    if(task->task_class->task_class_id >= task->taskpool->nb_task_classes)
        return; /* We ignore startup tasks if there are some */
    parsec_dot_grapher_task(task, es->th_id, es->virtual_process->vp_id,
                            task->task_class->key_functions->key_hash(task->task_class->make_key( task->taskpool, task->locals), NULL));
}

static void parsec_dot_grapher_dep(struct parsec_pins_next_callback_s* cb_data,
                                   struct parsec_execution_stream_s*   es,
                                   const parsec_task_t* from, const parsec_task_t* to,
                                   int dependency_activates_task,
                                   const parsec_flow_t* origin_flow, const parsec_flow_t* dest_flow)
{
    (void)cb_data;
    (void)es;
    if( NULL != grapher_file ) {
        char tmp[128];
        int index = 0;

        parsec_dot_grapher_taskid( from, tmp, 128 );
        index = strlen(tmp);
        index += snprintf( tmp + index, 128 - index, " -> " );
        parsec_dot_grapher_taskid( to, tmp + index, 128 - index - 4 );
        pthread_mutex_lock(&grapher_file_lock);
        const char *style = "solid";
        if (dest_flow != NULL) {
            if (dest_flow->flow_flags == PARSEC_FLOW_ACCESS_NONE) {
                style = "dotted";
            } else if (dest_flow->flow_flags == PARSEC_FLOW_ACCESS_RW) {
                style = "dashed";
            }
        }
        if (dest_flow != NULL && origin_flow != NULL) {
            fprintf(grapher_file,
                    "%s [label=\"%s=>%s\",color=\"#%s\",style=\"%s\"]\n",
                    tmp, (origin_flow) ? origin_flow->name : "<null>",
                    (dest_flow) ? dest_flow->name : "<null>",
                    dependency_activates_task ? "00FF00" : "FF0000", style);
        } else {
            fprintf(grapher_file,
                    "%s [color=\"#%s\",style=\"%s\"]\n",
                    tmp, dependency_activates_task ? "00FF00" : "FF0000", style);
        }
        fflush(grapher_file);
        pthread_mutex_unlock(&grapher_file_lock);
    }
}

static char *parsec_dot_grapher_dataid(const parsec_data_t *dta, char *did, int size)
{
    parsec_grapher_data_identifier_t id;
    parsec_key_t key;
    parsec_grapher_data_identifier_hash_table_item_t *it;
    parsec_key_handle_t kh;
    char *ret = NULL;

    assert(NULL != dta);
    assert(NULL != grapher_file);
    assert(NULL != data_ht);

    id.dc = dta->dc;
    id.data_key = dta->key;
    key = (parsec_key_t)(uintptr_t)&id;
    parsec_hash_table_lock_bucket_handle(data_ht, key, &kh);
    if( NULL == (it = parsec_hash_table_nolock_find_handle(data_ht, &kh)) ) {
        char data_name[MAX_TASK_STRLEN];
        it = (parsec_grapher_data_identifier_hash_table_item_t*)malloc(sizeof(parsec_grapher_data_identifier_hash_table_item_t));
        it->id = id;
        it->ht_item.key = (parsec_key_t)(uintptr_t)&it->id;
        if(NULL != it->id.dc)
            asprintf(&it->did, "dc%p_%"PRIuPTR, it->id.dc, (uintptr_t)it->id.data_key);
        else
            asprintf(&it->did, "dta%p_%"PRIuPTR, dta, (uintptr_t)it->id.data_key);
        parsec_hash_table_nolock_insert_handle(data_ht, &kh, &it->ht_item);
        parsec_hash_table_unlock_bucket_handle(data_ht, &kh);

        if(NULL != dta->dc && NULL != dta->dc->key_to_string) {
            dta->dc->key_to_string(dta->dc, dta->key, data_name, MAX_TASK_STRLEN);
        } else {
            snprintf(data_name, MAX_TASK_STRLEN, "NEW");
        }
        asprintf(&ret, "%s [label=\"%s%s\",shape=\"circle\"]\n", it->did, NULL != dta->dc->key_base ? dta->dc->key_base : "", data_name);
    } else
        parsec_hash_table_unlock_bucket_handle(data_ht, &kh);
    strncpy(did, it->did, size);
    return ret;
}

static void parsec_dot_grapher_data_input(struct parsec_pins_next_callback_s* cb_data,
                                          struct parsec_execution_stream_s*   es,
                                          const parsec_task_t *task, const parsec_data_t *data, const parsec_flow_t *flow, int direct_reference)
{
    (void)cb_data;
    (void)es;
    char *dataid;
    if( NULL != grapher_file &&
        (( direct_reference == 1 && parsec_dot_grapher_memmode == 1 ) ||
         ( parsec_dot_grapher_memmode == 2 )) ) {
        char tid[128];
        char did[128];
        parsec_dot_grapher_taskid( task, tid, 128 );
        dataid = parsec_dot_grapher_dataid( data, did, 128 );
        pthread_mutex_lock(&grapher_file_lock);
        if(NULL != dataid) {
            fprintf(grapher_file, "%s", dataid);
            free(dataid);
        }
        fprintf(grapher_file, "%s -> %s [label=\"%s\"]\n", did, tid, flow->name);
        fflush(grapher_file);
        pthread_mutex_unlock(&grapher_file_lock);
    }
}

static void parsec_dot_grapher_data_output(struct parsec_pins_next_callback_s* cb_data,
                                           struct parsec_execution_stream_s*   es,
                                           const struct parsec_task_s *task, const struct parsec_data_s *data, const struct parsec_flow_s *flow)
{
    (void)cb_data;
    (void)es;
    char *dataid;

    /* All output are direct references to a data */
    if( NULL != grapher_file &&
        (parsec_dot_grapher_memmode >= 1 ) ) {
        char tid[128];
        char did[128];
        parsec_dot_grapher_taskid( task, tid, 128 );
        dataid = parsec_dot_grapher_dataid( data, did, 128 );
        pthread_mutex_lock(&grapher_file_lock);
        if(NULL != dataid) {
            fprintf(grapher_file, "%s", dataid);
            free(dataid);
        }
        fprintf(grapher_file, "%s -> %s [label=\"%s\"]\n", tid, did, flow->name);
        fflush(grapher_file);
        pthread_mutex_unlock(&grapher_file_lock);
    }
}

static void parsec_grapher_data_ht_free_elt(void *_item, void *table)
{
    parsec_grapher_data_identifier_hash_table_item_t *item = (parsec_grapher_data_identifier_hash_table_item_t*)_item;
    parsec_key_t key = (parsec_key_t)(uintptr_t)&item->id;
    parsec_hash_table_nolock_remove(table, key);
    free(item->did);
    free(item);
}

void pins_dot_grapher_init(parsec_context_t *parsec_context)
{
    (void)parsec_context;

    parsec_mca_param_reg_string_name("pins", "dot_grapher_filename", "Basename of the file into which to outpout a DOT "
                                     "representation of the graph of tasks seen at runtime (file will be called <basename>-<MPI rank>.dot)",
                                     false, false, "", &grapher_file_basename);
    parsec_mca_param_reg_int_name("pins", "dot_grapher_memmode", "How memory references are traced in the DAG of tasks "
                                 "(default is 0, possible values are 0: no tracing of memory references, 1: trace only the "
                                  "direct memory references, 2: trace memory references even when data is passed from task "
                                  "to task)",
                                  false, false, parsec_dot_grapher_memmode, &parsec_dot_grapher_memmode);
    /* The rest of the initialization, including file creation, must happen in
     * the thread-specific init, as the communication engine -- hence the rank
     * of this process -- is not initialized yet. */
}

void pins_dot_grapher_fini(parsec_context_t *parsec_context)
{
    (void)parsec_context;
    if( (NULL == grapher_file) || ((void*)1 == grapher_file) ) {
        return;
    }

    fprintf(grapher_file, "}\n");
    fclose(grapher_file);
    grapher_file = NULL;
    free(grapher_file_basename);

    /* Free all data records */
    parsec_hash_table_for_all(data_ht, parsec_grapher_data_ht_free_elt, data_ht);
    PARSEC_OBJ_RELEASE(data_ht);
    data_ht = NULL;
}

static void pins_dot_grapher_thread_init(struct parsec_execution_stream_s * es)
{
    parsec_pins_next_callback_t* event_cb;
    char *filename;

    if(strlen(grapher_file_basename) == 0) {
        /* dot graphing is not requested */
        return;
    }

    if( ! (PARSEC_PINS_FLAG_ENABLED(COMPLETE_EXEC_BEGIN) &&
           PARSEC_PINS_FLAG_ENABLED(TASK_DEPENDENCY)) ) {
        /* For any DOT to make sense, one must at least be able to see the tasks and their relation to their children */
        return;
    }

    pthread_mutex_lock(&grapher_file_lock);
    if(NULL == grapher_file) {
        asprintf(&filename, "%s-%d.dot", grapher_file_basename, es->virtual_process->parsec_context->my_rank);
        grapher_file = fopen(filename, "w");
        if( NULL == grapher_file ) {
            parsec_warning("PINS dot-grapher:\tunable to create %s (%s) -- DOT graphing disabled", filename, strerror(errno));
            grapher_file = (void*)(1);
        } else {
            if(0 == es->virtual_process->parsec_context->my_rank) {
                parsec_warning("/!\\ PINS dot-grapher strongly degrades performance as it writes the DOT file. /!\\\n");
            }
            data_ht = PARSEC_OBJ_NEW(parsec_hash_table_t);
            parsec_hash_table_init(data_ht, offsetof(parsec_grapher_data_identifier_hash_table_item_t, ht_item), 16, parsec_grapher_data_key_fns, NULL);
            fprintf(grapher_file, "digraph G {\n");
            fflush(grapher_file);
        }
        free(filename);
    }
    pthread_mutex_unlock(&grapher_file_lock);
    if((void*)1 == grapher_file) {
        /* We use this to signal threads that there is an error and they need to bail out */
        return;
    }

    if (PARSEC_PINS_FLAG_ENABLED(DATA_COLLECTION_INPUT)) {
        event_cb = (parsec_pins_next_callback_t*)malloc(sizeof(parsec_pins_next_callback_t));
        PARSEC_PINS_REGISTER(es, DATA_COLLECTION_INPUT, parsec_dot_grapher_data_input, event_cb);
    }

    if (PARSEC_PINS_FLAG_ENABLED(DATA_COLLECTION_OUTPUT)) {
        event_cb = (parsec_pins_next_callback_t*)malloc(sizeof(parsec_pins_next_callback_t));
        PARSEC_PINS_REGISTER(es, DATA_COLLECTION_OUTPUT, parsec_dot_grapher_data_output, event_cb);
    }

    /* The other two must be enabled */
    assert(PARSEC_PINS_FLAG_ENABLED(COMPLETE_EXEC_BEGIN));
    assert(PARSEC_PINS_FLAG_ENABLED(TASK_DEPENDENCY));

    event_cb = (parsec_pins_next_callback_t*)malloc(sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, COMPLETE_EXEC_BEGIN, parsec_dot_grapher_complete_exec_begin, event_cb);

    event_cb = (parsec_pins_next_callback_t*)malloc(sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, TASK_DEPENDENCY, parsec_dot_grapher_dep, event_cb);
}

static void pins_dot_grapher_thread_fini(struct parsec_execution_stream_s * es)
{
    parsec_pins_next_callback_t* event_cb;

    if((NULL == grapher_file) || ((void*)1 == grapher_file)) {
        return;
    }

    if (PARSEC_PINS_FLAG_ENABLED(DATA_COLLECTION_INPUT)) {
        PARSEC_PINS_UNREGISTER(es, DATA_COLLECTION_INPUT, parsec_dot_grapher_data_input, &event_cb);
        free(event_cb);
    }

    if (PARSEC_PINS_FLAG_ENABLED(DATA_COLLECTION_OUTPUT)) {
        PARSEC_PINS_UNREGISTER(es, DATA_COLLECTION_OUTPUT, parsec_dot_grapher_data_output, &event_cb);
        free(event_cb);
    }

    /* The other two must be enabled */
    assert(PARSEC_PINS_FLAG_ENABLED(COMPLETE_EXEC_BEGIN));
    assert(PARSEC_PINS_FLAG_ENABLED(TASK_DEPENDENCY));

    PARSEC_PINS_UNREGISTER(es, COMPLETE_EXEC_BEGIN, parsec_dot_grapher_complete_exec_begin, &event_cb);
    free(event_cb);

    PARSEC_PINS_UNREGISTER(es, TASK_DEPENDENCY, parsec_dot_grapher_dep, &event_cb);
    free(event_cb);
}

const parsec_pins_module_t parsec_pins_dot_grapher_module = {
    &parsec_pins_dot_grapher_component,
    {
        pins_dot_grapher_init,
        pins_dot_grapher_fini,
        NULL,
        NULL,
        pins_dot_grapher_thread_init,
        pins_dot_grapher_thread_fini
    },
    { NULL }
};
