/*
 * Copyright (c) 2024      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#include <errno.h>
#include <stdio.h>
#include "parsec/parsec_config.h"
#include "parsec/mca/pins/pins.h"
#include "pins_task_stubs.h"
#include "parsec/parsec_internal.h"
#include "parsec/profiling.h"
#include "parsec/execution_stream.h"
#include "parsec/parsec_description_structures.h"
#include "parsec/parsec_binary_profile.h"
#include "parsec/utils/argv.h"
#include "parsec/utils/mca_param.h"
#include "parsec/class/parsec_hash_table.h"

#include "tasktimer.h"

/* init functions */
static void pins_init_task_stubs(parsec_context_t *master_context);
static void pins_fini_task_stubs(parsec_context_t *master_context);
static void pins_thread_init_task_stubs(struct parsec_execution_stream_s * es);
static void pins_thread_fini_task_stubs(struct parsec_execution_stream_s * es);

parsec_pins_module_t parsec_pins_task_stubs_module = {
    &parsec_pins_task_stubs_component,
    {
        pins_init_task_stubs,
        pins_fini_task_stubs,
        NULL,
        NULL,
        pins_thread_init_task_stubs,
        pins_thread_fini_task_stubs
    },
    { NULL }
};

static uint64_t task_stubs_make_guid(const struct parsec_task_s *task)
{
    uint64_t tuid = task->task_class->make_key(task->taskpool, task->locals);
    uint64_t tpuid = ((uint64_t)task->taskpool->taskpool_id)<<56;
    uint64_t tcuid = ((uint64_t)task->task_class->task_class_id) << 48;
#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[128];
    task->task_class->task_snprintf(tmp, 128, task);
    PARSEC_DEBUG_VERBOSE(10, parsec_debug_output,
                         "task_stubs create guid for %s -> tuid %"PRIx64" tpuid %"PRIx64" tcuid %"PRIx64" -> %"PRIx64" \n", 
                         tmp, tuid, tpuid, tcuid, tuid|tpuid|tcuid);
#endif
    return tuid | tpuid | tcuid;
 }

typedef struct {
    parsec_hash_table_item_t  ht_item;
    int tp_id;
    int tc_id;
    uint64_t tuid;

    int nb_parents;
    int parent_uid_size;
    uint64_t *parent_uid;
} pins_task_stubs_parent_set_t;

static parsec_hash_table_t pins_task_stubs_parents_table;

static void task_stubs_dep(struct parsec_pins_next_callback_s* cb_data,
                           struct parsec_execution_stream_s*   es,
                           const parsec_task_t* from, const parsec_task_t* to,
                           int dependency_activates_task,
                           const parsec_flow_t* origin_flow, const parsec_flow_t* dest_flow)
{
    if(from->task_class->task_class_id >= from->taskpool->nb_task_classes ||
       to->task_class->task_class_id >= to->taskpool->nb_task_classes) {
        /* Skip startup tasks */
        return;
    }

    uint64_t child_guid = task_stubs_make_guid(to);
    uint64_t parent_guid = task_stubs_make_guid(from);
    parsec_key_handle_t kh;
    parsec_hash_table_lock_bucket_handle(&pins_task_stubs_parents_table, child_guid, &kh);
    pins_task_stubs_parent_set_t *parent_set = (pins_task_stubs_parent_set_t*) parsec_hash_table_nolock_find(&pins_task_stubs_parents_table, child_guid);
    if(NULL == parent_set) {
        parent_set = malloc(sizeof(pins_task_stubs_parent_set_t));
        parent_set->tc_id = to->task_class->task_class_id;
        parent_set->tp_id = to->taskpool->taskpool_id;
        parent_set->tuid  = to->task_class->make_key(to->taskpool, to->locals);
        parent_set->ht_item.key = child_guid;
        parent_set->nb_parents = 1;
        parent_set->parent_uid_size = 8;
        parent_set->parent_uid = (uint64_t*)malloc(parent_set->parent_uid_size * sizeof(uint64_t));
        parent_set->parent_uid[0] = parent_guid;
        parsec_hash_table_nolock_insert(&pins_task_stubs_parents_table, &parent_set->ht_item);
    } else {
        if(parent_set->nb_parents + 1 >= parent_set->parent_uid_size) {
            parent_set->parent_uid_size += 8;
            parent_set->parent_uid = (uint64_t*)realloc(parent_set->parent_uid, parent_set->parent_uid_size * sizeof(uint64_t));
        }
        parent_set->parent_uid[parent_set->nb_parents] = parent_guid;
        parent_set->nb_parents++;
    }
    parsec_hash_table_unlock_bucket_handle(&pins_task_stubs_parents_table, &kh);
    (void)cb_data; (void)es; (void)dependency_activates_task; (void)origin_flow; (void)dest_flow;
}

static void task_stubs_prepare_input_begin(parsec_pins_next_callback_t* data,
                                           parsec_execution_stream_t* es,
                                           parsec_task_t* task)
{
    if(task->task_class->task_class_id >= task->taskpool->nb_task_classes) {
        /* Skip startup tasks */
        return;
    }

    uint64_t myguid = task_stubs_make_guid(task);
    pins_task_stubs_parent_set_t *parent_set = (pins_task_stubs_parent_set_t*)parsec_hash_table_remove(&pins_task_stubs_parents_table, myguid);
    tasktimer_guid_t *parents;
    uint64_t nb_parents;
    char *parents_str;
    if(NULL == parent_set) {
        parents = NULL;
        nb_parents = 0;
        parents_str = strdup("");
    } else {
        parents = parent_set->parent_uid;
        nb_parents = parent_set->nb_parents;
        parents_str = calloc(64, nb_parents);
        for(int i = 0; i < nb_parents; i++) {
            sprintf(parents_str + strlen(parents_str), "%p ", (uintptr_t)parents[i]);
        }
        free(parent_set);
    }
    TASKTIMER_CREATE(task->task_class->incarnations[0].hook, task->task_class->name, myguid, parents, nb_parents, timer);
    if(NULL != parents) {
        free(parents);
        free(parents_str);
    }
    task->taskstub_timer = timer;
    tasktimer_argument_value_t args[1];
    char task_name[MAX_TASK_STRLEN];
    args[0].type = TASKTIMER_STRING_TYPE;
    parsec_task_snprintf(task_name, MAX_TASK_STRLEN, task);
    args[0].c_value = task_name;
    TASKTIMER_SCHEDULE(task->taskstub_timer, args, 1);
    (void)es;(void)data;
}

static void task_stubs_exec_begin(parsec_pins_next_callback_t* data,
                                  parsec_execution_stream_t* es,
                                  parsec_task_t* task)
{
    tasktimer_execution_space_t resource;

    if(task->task_class->task_class_id >= task->taskpool->nb_task_classes) {
        /* Skip startup tasks */
        return;
    }

    resource.type = TASKTIMER_DEVICE_CPU;
    resource.device_id = es->th_id;

    TASKTIMER_START(task->taskstub_timer, &resource);
    (void)data;
}

static void task_stubs_exec_end(parsec_pins_next_callback_t* data,
                                parsec_execution_stream_t* es,
                                parsec_task_t* task)
{
    if(task->task_class->task_class_id >= task->taskpool->nb_task_classes) {
        /* Skip startup tasks */
        return;
    }

    TASKTIMER_STOP(task->taskstub_timer);
    (void)es;(void)data;
}

static void task_stubs_gpu_submit_begin(parsec_pins_next_callback_t* data,
                                        parsec_execution_stream_t* es,
                                        const parsec_gpu_task_t* gpu_task,
                                        const parsec_device_gpu_module_t* module,
                                        const parsec_gpu_exec_stream_t* gpu_stream)
{
    tasktimer_execution_space_t resource;
    parsec_task_t *task = gpu_task->ec;
    int i;
    assert(task->task_class->task_class_id < task->taskpool->nb_task_classes /* no startup tasks */);
    (void)es;
    resource.type = TASKTIMER_DEVICE_GPU;
    resource.device_id = module->super.device_index; // TODO: need to convert the PaRSEC device index to something consistent with the tool
    for(i = 0; module->exec_stream[i] != gpu_stream; i++) /* nothing */;
    resource.instance_id = i;
    TASKTIMER_START(task->taskstub_timer, &resource);
    (void)data;
}

static void task_stubs_gpu_task_poll_completed(parsec_pins_next_callback_t* data,
                                               parsec_execution_stream_t* es,
                                               const parsec_gpu_task_t* gpu_task,
                                               const parsec_device_gpu_module_t* module,
                                               const parsec_gpu_exec_stream_t* gpu_stream)
{
    parsec_task_t *task = gpu_task->ec;
    assert(task->task_class->task_class_id < task->taskpool->nb_task_classes /* No startup tasks */);

    TASKTIMER_STOP(task->taskstub_timer);
    (void)es;(void)data; (void)module; (void)gpu_stream;
}

static void task_stubs_complete_exec_end(parsec_pins_next_callback_t* data,
                                         parsec_execution_stream_t* es,
                                         parsec_task_t* task)
{
    if(task->task_class->task_class_id >= task->taskpool->nb_task_classes) {
        /* Skip startup tasks */
        return;
    }

    TASKTIMER_DESTROY(task->taskstub_timer);
    (void)es;(void)data;
}

static void pins_init_task_stubs(parsec_context_t *master_context)
{
    (void)master_context;
    parsec_hash_table_init(&pins_task_stubs_parents_table, offsetof(pins_task_stubs_parent_set_t, ht_item),
                           10, parsec_hash_table_generic_key_fn, NULL);
    TASKTIMER_INITIALIZE();
}

static void pins_fini_task_stubs(parsec_context_t *master_context)
{
    (void)master_context;
    TASKTIMER_FINALIZE();
    parsec_hash_table_fini(&pins_task_stubs_parents_table);
}

static void pins_thread_init_task_stubs(struct parsec_execution_stream_s * es)
{
    parsec_pins_next_callback_t* event_cb;

    event_cb = (parsec_pins_next_callback_t*)calloc(1, sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, PREPARE_INPUT_BEGIN, task_stubs_prepare_input_begin, event_cb);
    event_cb = (parsec_pins_next_callback_t*)calloc(1, sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, EXEC_BEGIN, task_stubs_exec_begin, event_cb);
    event_cb = (parsec_pins_next_callback_t*)calloc(1, sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, EXEC_END, task_stubs_exec_end, event_cb);
    event_cb = (parsec_pins_next_callback_t*)calloc(1, sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, GPU_TASK_SUBMIT_BEGIN, task_stubs_gpu_submit_begin, event_cb);
    event_cb = (parsec_pins_next_callback_t*)calloc(1, sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, GPU_TASK_POLL_COMPLETED, task_stubs_gpu_task_poll_completed, event_cb);
    event_cb = (parsec_pins_next_callback_t*)malloc(sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, COMPLETE_EXEC_END, task_stubs_complete_exec_end, event_cb);
    event_cb = (parsec_pins_next_callback_t*)malloc(sizeof(parsec_pins_next_callback_t));
    PARSEC_PINS_REGISTER(es, TASK_DEPENDENCY, task_stubs_dep, event_cb);
    (void)es;
}

static void pins_thread_fini_task_stubs(struct parsec_execution_stream_s * es)
{
    parsec_pins_next_callback_t* event_cb;

    PARSEC_PINS_UNREGISTER(es, PREPARE_INPUT_BEGIN, task_stubs_prepare_input_begin, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, EXEC_BEGIN, task_stubs_exec_begin, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, GPU_TASK_SUBMIT_BEGIN, task_stubs_gpu_submit_begin, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, GPU_TASK_POLL_COMPLETED, task_stubs_gpu_task_poll_completed, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, EXEC_END, task_stubs_exec_end, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, COMPLETE_EXEC_END, task_stubs_complete_exec_end, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, TASK_DEPENDENCY, task_stubs_dep, &event_cb);
    free(event_cb);
    (void)es;
}
