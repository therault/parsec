/*
 * Copyright (c) 2013      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "dague_config.h"
#include "dague_internal.h"
#include "debug.h"
#include "dague/mca/sched/sched.h"
#include "dague/mca/sched/rnd/sched_rnd.h"
#include "dequeue.h"
#include "dague/mca/pins/pins.h"
static int SYSTEM_NEIGHBOR = 0;

/*
 * Module functions
 */
static int sched_rnd_install(dague_context_t* master);
static int sched_rnd_schedule(dague_execution_unit_t* eu_context, dague_execution_context_t* new_context);
static dague_execution_context_t *sched_rnd_select( dague_execution_unit_t *eu_context );
static void sched_rnd_remove(dague_context_t* master);

const dague_sched_module_t dague_sched_rnd_module = {
    &dague_sched_rnd_component,
    {
        sched_rnd_install,
        NULL,
        sched_rnd_schedule,
        sched_rnd_select,
        NULL,
        sched_rnd_remove
    }
};

static int sched_rnd_install( dague_context_t *master )
{
    int p, t;
    dague_vp_t *vp;
    dague_execution_unit_t *eu;

    SYSTEM_NEIGHBOR = master->nb_vp * master->virtual_processes[0]->nb_cores; // defined for instrumentation

    for(p = 0; p < master->nb_vp; p++) {
        vp = master->virtual_processes[p];
        for(t = 0; t < vp->nb_cores; t++) {
            eu = vp->execution_units[t];
            if( eu->th_id == 0 ) {
                eu->scheduler_object = (dague_list_t*)malloc(sizeof(dague_list_t));
                OBJ_CONSTRUCT(eu->scheduler_object, dague_list_t);
            } else {
                eu->scheduler_object = eu->virtual_process->execution_units[0]->scheduler_object;
            }
        }
    }

    return 0;
}

static dague_execution_context_t *sched_rnd_select( dague_execution_unit_t *eu_context )
{
    dague_execution_context_t * context =
        (dague_execution_context_t*)dague_list_pop_front((dague_list_t*)eu_context->scheduler_object);
    if (NULL != context)
        context->victim_core = SYSTEM_NEIGHBOR;
    return context;
}

static int sched_rnd_schedule( dague_execution_unit_t* eu_context,
                               dague_execution_context_t* new_context )
{
#if defined(DAGUE_DEBUG_VERBOSE3)
    dague_list_item_t *it = (dague_list_item_t*)new_context;
    char tmp[MAX_TASK_STRLEN];
    do {
        DEBUG3(("RND:\t Pushing task %s\n",
                dague_snprintf_execution_context(tmp, MAX_TASK_STRLEN, (dague_execution_context_t*)it)));
        /* randomly assign priority */
        (*((int*)(((uintptr_t)it)+dague_execution_context_priority_comparator))) = rand();
        it = (dague_list_item_t*)((dague_list_item_t*)it)->list_next;
    } while( it != (dague_list_item_t*)new_context );
#endif
    dague_list_chain_sorted((dague_list_t*)eu_context->scheduler_object,
                            (dague_list_item_t*)new_context,
                            dague_execution_context_priority_comparator);
    return 0;
}

static void sched_rnd_remove( dague_context_t *master )
{
    int p, t;
    dague_vp_t *vp;
    dague_execution_unit_t *eu;

    for(p = 0; p < master->nb_vp; p++) {
        vp = master->virtual_processes[p];
        for(t = 0; t < vp->nb_cores; t++) {
            eu = vp->execution_units[t];
            if( eu->th_id == 0 ) {
                OBJ_DESTRUCT( eu->scheduler_object );
                free(eu->scheduler_object);
            }
            eu->scheduler_object = NULL;
        }
    }
}
