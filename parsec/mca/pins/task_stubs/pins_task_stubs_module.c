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

    uint64_t child_guid[1] = { task_stubs_make_guid(to) };
    TASKTIMER_ADD_CHILDREN(from->taskstub_timer, child_guid, 1);
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
    TASKTIMER_CREATE(task->task_class->incarnations[0].hook, task->task_class->name, myguid, NULL, 0, timer);
    task->taskstub_timer = timer;
    TASKTIMER_SCHEDULE(task->taskstub_timer, NULL, 0);
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

    if(NULL != task->selected_device && PARSEC_DEV_IS_GPU(task->selected_device->type)) {
        resource.type = TASKTIMER_DEVICE_GPU;
        resource.device_id = es->virtual_process->parsec_context->my_rank;
        resource.instance_id = task->selected_device->device_index; // TODO: need to convert the PaRSEC device index to something consistent with the tool
    } else {
        resource.type = TASKTIMER_DEVICE_CPU;
        resource.device_id = es->virtual_process->parsec_context->my_rank;
        resource.instance_id = es->th_id;
    }
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
    TASKTIMER_INITIALIZE();
}

static void pins_fini_task_stubs(parsec_context_t *master_context)
{
    (void)master_context;
    TASKTIMER_FINALIZE();
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
    PARSEC_PINS_UNREGISTER(es, EXEC_END, task_stubs_exec_end, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, COMPLETE_EXEC_END, task_stubs_complete_exec_end, &event_cb);
    free(event_cb);
    PARSEC_PINS_UNREGISTER(es, TASK_DEPENDENCY, task_stubs_dep, &event_cb);
    free(event_cb);
    (void)es;
}
