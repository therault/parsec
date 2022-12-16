/*
 * Copyright (c) 2021-2022 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#include "parsec/parsec_config.h"
#include "parsec/parsec_internal.h"
#include "parsec/sys/atomic.h"

#include "parsec/utils/mca_param.h"
#include "parsec/constants.h"

#if defined(PARSEC_HAVE_LEVEL_ZERO)
#include "parsec/runtime.h"
#include "parsec/data_internal.h"
#include "parsec/mca/device/level_zero/device_level_zero_internal.h"
#include "parsec/profiling.h"
#include "parsec/execution_stream.h"
#include "parsec/arena.h"
#include "parsec/scheduling.h"
#include "parsec/utils/debug.h"
#include "parsec/utils/argv.h"
#include "parsec/utils/zone_malloc.h"
#include "parsec/class/fifo.h"
#include "parsec/mca/device/level_zero/device_level_zero_dpcpp.h"

#include <level_zero/ze_api.h>

static int parsec_level_zero_data_advise(parsec_device_module_t *dev, parsec_data_t *data, int advice);
static int
parsec_level_zero_memory_reserve( parsec_device_level_zero_module_t* gpu_device,
                           int           memory_percentage,
                           int           number_of_elements,
                           size_t        eltsize );
static int parsec_level_zero_memory_release( parsec_device_level_zero_module_t* gpu_device );
static int parsec_level_zero_flush_lru( parsec_device_module_t *device );

static void* level_zero_find_incarnation(parsec_device_level_zero_module_t* level_zero_device,
                                   const char* fname)
{
    char library_name[FILENAME_MAX], *env;
    parsec_device_gpu_module_t *gpu_device = &level_zero_device->super;
    void *fn = NULL;
    char** argv = NULL;

    /**
     * Prepare the list of PATH or FILE to be searched for a LEVEL_ZERO shared library.
     * In any case this list might be a list of ; separated possible targets,
     * where each target can be either a directory or a specific file.
     */
    env = getenv("PARSEC_CUCORES_LIB");
    if( NULL != env ) {
        argv = parsec_argv_split(env, ';');
    } else if( NULL != level_zero_lib_path ) {
        argv = parsec_argv_split(level_zero_lib_path, ';');
    }

    fn = parsec_device_find_function(fname, library_name, (const char**)argv);
    if( NULL == fn ) {  /* look for the function with lesser capabilities */
        parsec_warning("No function '%s' found for LEVEL_ZERO device %s", fname, gpu_device->super.name);
    }

    if( NULL != argv )
        parsec_argv_free(argv);

    return fn;
}

/**
 * Register a taskpool with a device by checking that the device
 * supports the dynamic function required by the different incarnations.
 * If multiple devices of the same type exists we assume thay all have
 * the same capabilities.
 */
static int
parsec_level_zero_taskpool_register(parsec_device_module_t* device,
                              parsec_taskpool_t* tp)
{
    parsec_device_level_zero_module_t* gpu_device = (parsec_device_level_zero_module_t*)device;
    int32_t rc = PARSEC_ERR_NOT_FOUND;
    uint32_t i, j;

    /**
     * Detect if a particular chore has a dynamic load dependency and if yes
     * load the corresponding module and find the function.
     */
    assert(PARSEC_DEV_LEVEL_ZERO == device->type);
    assert(tp->devices_index_mask & (1 << device->device_index));

    for( i = 0; i < tp->nb_task_classes; i++ ) {
        const parsec_task_class_t* tc = tp->task_classes_array[i];
        __parsec_chore_t* chores = (__parsec_chore_t*)tc->incarnations;
        for( j = 0; NULL != chores[j].hook; j++ ) {
            if( chores[j].type != device->type )
                continue;
            if( NULL != chores[j].dyld_fn ) {
                /* the function has been set for another device of the same type */
                return PARSEC_SUCCESS;
            }
            if ( NULL == chores[j].dyld ) {
                chores[j].dyld_fn = NULL;  /* No dynamic support required for this kernel */
                rc = PARSEC_SUCCESS;
            } else {
                void* devf = level_zero_find_incarnation(gpu_device, chores[j].dyld);
                if( NULL != devf ) {
                    chores[j].dyld_fn = devf;
                    rc = PARSEC_SUCCESS;
                }
            }
        }
    }
    if( PARSEC_SUCCESS != rc ) {
        tp->devices_index_mask &= ~(1 << device->device_index);  /* drop support for this device */
        parsec_debug_verbose(10, parsec_gpu_output_stream,
                             "Device %d (%s) disabled for taskpool %p", device->device_index, device->name, tp);
    }
    return rc;
}

static int
parsec_level_zero_taskpool_unregister(parsec_device_module_t* device, parsec_taskpool_t* tp)
{
    (void)device; (void)tp;
    return PARSEC_SUCCESS;
}

/**
 * Attach a device to a PaRSEC context. A device can only be attached to
 * a single context at the time.
 */
static int
parsec_device_level_zero_attach( parsec_device_module_t* device,
                           parsec_context_t* context )
{
    return parsec_mca_device_add(context, device);
}

/**
 * Detach a device from a context. Both the context and the device remain
 * valid, they are simply disconnected.
 * This function should only be called once all tasks and all data related to the
 * context has been removed from the device.
 */
static int
parsec_device_level_zero_detach( parsec_device_module_t* device,
                           parsec_context_t* context )
{
    (void)context;
    return parsec_mca_device_remove(device);
}

int parsec_level_zero_module_init( int dev_id, parsec_device_level_zero_driver_t *driver, ze_device_handle_t ze_device,
                                   ze_device_properties_t *prop, parsec_device_module_t** module )
{
    int streaming_multiprocessor, len;
    parsec_device_level_zero_module_t* level_zero_device;
    parsec_device_gpu_module_t* gpu_device;
    parsec_device_module_t* device;
    ze_result_t ze_rc;
    int show_caps_index, show_caps = 0, j, k;
    char *szName;
    float clockRate;

    show_caps_index = parsec_mca_param_find("device", NULL, "show_capabilities"); 
    if(0 < show_caps_index) {
        parsec_mca_param_lookup_int(show_caps_index, &show_caps);
    }

    *module = NULL;

    szName    = prop->name;
    clockRate = prop->coreClockRate/1e3f;
    streaming_multiprocessor = prop->numThreadsPerEU;

    level_zero_device = (parsec_device_level_zero_module_t*)calloc(1, sizeof(parsec_device_level_zero_module_t));
    gpu_device = &level_zero_device->super;
    device = &gpu_device->super;
    PARSEC_OBJ_CONSTRUCT(level_zero_device, parsec_device_level_zero_module_t);
    level_zero_device->level_zero_index = (uint8_t)dev_id;
    level_zero_device->driver = driver;
    level_zero_device->ze_device = ze_device;

    len = asprintf(&gpu_device->super.name, "%s ZE(%d)", szName, dev_id);
    if(-1 == len)
        gpu_device->super.name = "";
    gpu_device->data_avail_epoch = 0;

    gpu_device->max_exec_streams = parsec_level_zero_max_streams;
    gpu_device->exec_stream =
        (parsec_gpu_exec_stream_t**)malloc(gpu_device->max_exec_streams * sizeof(parsec_gpu_exec_stream_t*));
    gpu_device->exec_stream[0] =
            (parsec_gpu_exec_stream_t*)malloc( gpu_device->max_exec_streams * sizeof(parsec_level_zero_exec_stream_t));
    for(j = 1; j < gpu_device->max_exec_streams; j++)
        gpu_device->exec_stream[j] = (parsec_gpu_exec_stream_t*)(
                (parsec_level_zero_exec_stream_t*)gpu_device->exec_stream[0]+j);

    // Discover all command queue groups
    uint32_t cmdqueueGroupCount = 0;
    zeDeviceGetCommandQueueGroupProperties(level_zero_device->ze_device, &cmdqueueGroupCount, NULL);

    ze_command_queue_group_properties_t* cmdqueueGroupProperties = (ze_command_queue_group_properties_t*)
            malloc(cmdqueueGroupCount * sizeof(ze_command_queue_group_properties_t));
    zeDeviceGetCommandQueueGroupProperties(level_zero_device->ze_device, &cmdqueueGroupCount, cmdqueueGroupProperties);
    // Find a command queue type that support compute
    uint32_t computeQueueGroupOrdinal = cmdqueueGroupCount;
    uint32_t copyQueueGroupOrdinal = cmdqueueGroupCount;
    for( uint32_t i = 0;
         i < cmdqueueGroupCount &&
                 (computeQueueGroupOrdinal == cmdqueueGroupCount ||
                  copyQueueGroupOrdinal == cmdqueueGroupCount);
         ++i ) {
        if( cmdqueueGroupProperties[ i ].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE ) {
            computeQueueGroupOrdinal = i;
        }
        if( cmdqueueGroupProperties[ i ].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY ) {
            copyQueueGroupOrdinal = i;
        }
    }
    //TODO: it might be more in line with the design to create different command queues for copy
    //      and compute than using the existing queues.
    if( computeQueueGroupOrdinal == cmdqueueGroupCount ) {
        parsec_warning( "level zero device: unable to find a Queue Group with COMPUTE flag");
        goto release_device;
    }
    if( copyQueueGroupOrdinal == cmdqueueGroupCount ) {
        parsec_warning( "level zero device: unable to find a Queue Group with COMPUTE flag");
        goto release_device;
    }

    for( j = 0; j < gpu_device->max_exec_streams; j++ ) {
        parsec_level_zero_exec_stream_t* level_zero_stream =
                (parsec_level_zero_exec_stream_t*)gpu_device->exec_stream[j];
        parsec_gpu_exec_stream_t* exec_stream = &level_zero_stream->super;
        ze_command_queue_desc_t commandQueueDesc = {
                ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
                NULL,
                (uint32_t)-1,
                0, // index
                0, // flags
                ZE_COMMAND_QUEUE_MODE_DEFAULT,
                ZE_COMMAND_QUEUE_PRIORITY_NORMAL
        };
        /* CommandListImmediate would seem better for I/O, but mixing CLImmediate and CL+CQ 
         * seems to create some synchronization issues. */
        commandQueueDesc.ordinal = computeQueueGroupOrdinal;
        ze_rc = zeCommandQueueCreate(level_zero_device->driver->ze_context, level_zero_device->ze_device,
                                     &commandQueueDesc, &level_zero_stream->level_zero_cq);
        PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandQueueCreate ", ze_rc, {goto release_device;} );
        ze_command_list_desc_t commandListDesc = {
                    ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC,
                    NULL,
                    computeQueueGroupOrdinal,
                    0 // flags
        };
        ze_rc = zeCommandListCreate(level_zero_device->driver->ze_context, level_zero_device->ze_device,
                                    &commandListDesc, &level_zero_stream->level_zero_cl);
        PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandListCreate ", ze_rc, {goto release_device;} );
        exec_stream->workspace    = NULL;
        PARSEC_OBJ_CONSTRUCT(&exec_stream->infos, parsec_info_object_array_t);
        parsec_info_object_array_init(&exec_stream->infos, &parsec_per_stream_infos, exec_stream);
        exec_stream->max_events   = PARSEC_MAX_EVENTS_PER_STREAM;
        exec_stream->executed     = 0;
        exec_stream->start        = 0;
        exec_stream->end          = 0;
        exec_stream->name         = NULL;
        exec_stream->fifo_pending = (parsec_list_t*)PARSEC_OBJ_NEW(parsec_list_t);
        PARSEC_OBJ_CONSTRUCT(exec_stream->fifo_pending, parsec_list_t);
        exec_stream->tasks    = (parsec_gpu_task_t**)malloc(exec_stream->max_events
                                                            * sizeof(parsec_gpu_task_t*));
        level_zero_stream->events   = (ze_event_handle_t*)malloc(exec_stream->max_events * sizeof(ze_event_handle_t));

        // Create event pool
        ze_event_pool_desc_t eventPoolDesc = {
                ZE_STRUCTURE_TYPE_EVENT_POOL_DESC,
                NULL,
                ZE_EVENT_POOL_FLAG_HOST_VISIBLE, // all events in pool are visible to Host
                1 // count
        };
        zeEventPoolCreate(level_zero_device->driver->ze_context, &eventPoolDesc, 0, NULL, 
                          &level_zero_stream->ze_event_pool);

        /* and the corresponding events */
        for( k = 0; k < exec_stream->max_events; k++ ) {
            ze_event_desc_t eventDesc = {
                    ZE_STRUCTURE_TYPE_EVENT_DESC,
                    NULL,
                    0, // index
                    0, // no additional memory/cache coherency required on signal
                    ZE_EVENT_SCOPE_FLAG_HOST  // ensure memory coherency across device and Host after event completes
            };
            level_zero_stream->events[k]   = NULL;
            exec_stream->tasks[k]    = NULL;
            ze_rc = zeEventCreate(level_zero_stream->ze_event_pool, &eventDesc, &(level_zero_stream->events[k]));
            PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeEventCreate ", ze_rc,
                                     {goto release_device;} );
        }
        if(j == 0) {
            len = asprintf(&exec_stream->name, "h2d(%d)", j);
            if(-1 == len)
                exec_stream->name = "h2d";
        } else if(j == 1) {
            len = asprintf(&exec_stream->name, "d2h(%d)", j);
            if(-1 == len)
                exec_stream->name = "d2h";
        } else {
            len = asprintf(&exec_stream->name, "level_zero(%d)", j);
            if(-1 == len)
                exec_stream->name = "level_zero";
        }
#if defined(PARSEC_PROF_TRACE)
        /* Each 'exec' stream gets its own profiling stream, except IN and OUT stream that share it.
         * It's good to separate the exec streams to know what was submitted to what stream
         * We don't have this issue for the IN and OUT streams because types of event discriminate
         * what happens where, and separating them consumes memory and increases the number of 
         * events that needs to be matched between streams because we cannot differentiate some
         * ends between IN or OUT, so they are all logged on the same stream. */
        if(j == 0 || (parsec_device_gpu_one_profiling_stream_per_gpu_stream == 1 && j != 1))
            exec_stream->profiling = parsec_profiling_stream_init( 2*1024*1024, PARSEC_PROFILE_STREAM_STR, dev_id, j );
        else
            exec_stream->profiling = gpu_device->exec_stream[0]->profiling;
        if(j == 0) {
            exec_stream->prof_event_track_enable = parsec_gpu_trackable_events & ( PARSEC_PROFILE_LEVEL_ZERO_TRACK_DATA_IN | PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE );
        } else if(j == 1) {
            exec_stream->prof_event_track_enable = parsec_gpu_trackable_events & ( PARSEC_PROFILE_LEVEL_ZERO_TRACK_DATA_OUT | PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE );
        } else {
            exec_stream->prof_event_track_enable = parsec_gpu_trackable_events & ( PARSEC_PROFILE_LEVEL_ZERO_TRACK_EXEC | PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE );
        }
#endif  /* defined(PARSEC_PROF_TRACE) */
    }

    device->type                 = PARSEC_DEV_LEVEL_ZERO;
    device->executed_tasks       = 0;
    device->data_in_array_size   = 0;     // We'll let the modules_attach allocate the array of the right size for us
    device->data_in_from_device  = NULL;
    device->data_out_to_host     = 0;
    device->required_data_in     = 0;
    device->required_data_out    = 0;

    device->attach              = parsec_device_level_zero_attach;
    device->detach              = parsec_device_level_zero_detach;
    device->memory_register     = NULL; // TODO: could not find a memory register equivalent in level zero
    device->memory_unregister   = NULL; // TODO: could not find a memory unregister equivalent in level zero
    device->taskpool_register   = parsec_level_zero_taskpool_register;
    device->taskpool_unregister = parsec_level_zero_taskpool_unregister;
    device->data_advise         = parsec_level_zero_data_advise;
    device->memory_release      = parsec_level_zero_flush_lru;

    device->device_hweight = (float)streaming_multiprocessor * (float)clockRate * 2e-3f;
    device->device_tweight = (float)streaming_multiprocessor * (float)clockRate * 2e-3f;
    device->device_sweight = (float)streaming_multiprocessor * (float)clockRate * 2e-3f;
    device->device_dweight = (float)streaming_multiprocessor * (float)clockRate * 2e-3f;

    /* Initialize internal lists */
    PARSEC_OBJ_CONSTRUCT(&gpu_device->gpu_mem_lru,       parsec_list_t);
    PARSEC_OBJ_CONSTRUCT(&gpu_device->gpu_mem_owned_lru, parsec_list_t);
    PARSEC_OBJ_CONSTRUCT(&gpu_device->pending,           parsec_fifo_t);

    gpu_device->sort_starting_p = NULL;
    gpu_device->peer_access_mask = 0;  /* No GPU to GPU direct transfer by default */

    if( PARSEC_SUCCESS != parsec_level_zero_memory_reserve(level_zero_device,
                                                     level_zero_memory_percentage,
                                                     level_zero_memory_number_of_blocks,
                                                     level_zero_memory_block_size) ) {
        goto release_device;
    }

    if( show_caps ) {
        parsec_inform("LEVEL ZERO GPU Device %d: %s\n"
                      "\tLocation (PCI Bus/Device/Domain): %x:%x.%x\n"
                      "\tnumThreadsPerEU     : %u\n"
                      "\tphysicalEUSimdWidth : %u\n"
                      "\tnumEUsPerSubslice   : %u\n"
                      "\tnumSubslicesPerSlice: %u\n"
                      "\tnumSlices:            %u\n"
                      "\tcoreClockRate (GHz): %2.2f\n"
                      "\tpeak Gflops         : double %2.3f, single %2.3f tensor %2.3f half %2.3f\n",
                      level_zero_device->level_zero_index, device->name,
                      prop->deviceId, prop->subdeviceId, prop->vendorId,
                      streaming_multiprocessor,
                      prop->physicalEUSimdWidth,
                      prop->numEUsPerSubslice,
                      prop->numSubslicesPerSlice,
                      prop->numSlices,
                      clockRate*1e-3,
                      device->device_dweight, device->device_sweight, device->device_tweight, device->device_hweight);
    }

    *module = device;
    return PARSEC_SUCCESS;

 release_device:
    if( NULL != gpu_device->exec_stream) {
        for( j = 0; j < gpu_device->max_exec_streams; j++ ) {
            parsec_level_zero_exec_stream_t *level_zero_stream =
                    (parsec_level_zero_exec_stream_t*)gpu_device->exec_stream[j];
            parsec_gpu_exec_stream_t* exec_stream = &level_zero_stream->super;

            if( NULL != exec_stream->fifo_pending ) {
                PARSEC_OBJ_RELEASE(exec_stream->fifo_pending);
            }
            if( NULL != exec_stream->tasks ) {
                free(exec_stream->tasks); exec_stream->tasks = NULL;
            }
            if( NULL != level_zero_stream->events ) {
                for( k = 0; k < exec_stream->max_events; k++ ) {
                    if( NULL != level_zero_stream->events[k] ) {
                        (void)zeEventDestroy(level_zero_stream->events[k]);
                    }
                }
                free(level_zero_stream->events); level_zero_stream->events = NULL;
                zeEventPoolDestroy(level_zero_stream->ze_event_pool);
            }
            if( NULL != exec_stream->name ) {
                free(exec_stream->name); exec_stream->name = NULL;
            }
#if defined(PARSEC_PROF_TRACE)
            if( NULL != exec_stream->profiling ) {
                /* No function to clean the profiling stream. If one is introduced
                 * some day, remember that exec streams 0 and 1 always share the same 
                 * ->profiling stream, and that all of them share the same
                 * ->profiling stream if parsec_device_gpu_one_profiling_stream_per_gpu_stream == 0 */
            }
#endif  /* defined(PARSEC_PROF_TRACE) */
        }
        // All exec_stream_t are allocated in a single malloc in gpu_device->exec_stream[0]
        free(gpu_device->exec_stream[0]);
        free(gpu_device->exec_stream);
        gpu_device->exec_stream = NULL;
    }
    free(gpu_device);
    return PARSEC_ERROR;
}

int
parsec_level_zero_module_fini(parsec_device_module_t* device)
{
    parsec_device_gpu_module_t* gpu_device = (parsec_device_gpu_module_t*)device;
    parsec_device_level_zero_module_t* level_zero_device = (parsec_device_level_zero_module_t*)device;
    ze_result_t status;
    int j, k;

    /* Release the registered memory */
    parsec_level_zero_memory_release(level_zero_device);

    /* Release pending queue */
    PARSEC_OBJ_DESTRUCT(&gpu_device->pending);

    /* Release all streams */
    for( j = 0; j < gpu_device->max_exec_streams; j++ ) {
        parsec_level_zero_exec_stream_t* level_zero_stream = (parsec_level_zero_exec_stream_t*)gpu_device->exec_stream[j];
        parsec_gpu_exec_stream_t* exec_stream = &level_zero_stream->super;

        exec_stream->executed = 0;
        exec_stream->start    = 0;
        exec_stream->end      = 0;

        for( k = 0; k < exec_stream->max_events; k++ ) {
            assert( NULL == exec_stream->tasks[k] );
            status = zeEventDestroy(level_zero_stream->events[k]);
            PARSEC_LEVEL_ZERO_CHECK_ERROR( "(parsec_level_zero_device_fini) level_zeroEventDestroy ", status, {} );
        }
        zeEventPoolDestroy(level_zero_stream->ze_event_pool);
        exec_stream->max_events = 0;
        free(level_zero_stream->events); level_zero_stream->events = NULL;
        free(exec_stream->tasks); exec_stream->tasks = NULL;
        free(exec_stream->fifo_pending); exec_stream->fifo_pending = NULL;
        /* Release the stream */
        ze_result_t ze_rc;
        ze_rc = zeCommandListDestroy(level_zero_stream->level_zero_cl);
        PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandListDestroy ", ze_rc, {} );
        /* Deleting the sycl queue wrapper and/or the command queue conflicts with the
         *   cleaning procedure of the Level Zero runtime... Didn't find a way to do it
         *   cleanly. Don't cleanup for now... */
        //parsec_sycl_wrapper_queue_destroy(level_zero_stream->swq);
        level_zero_stream->swq = NULL;
        free(exec_stream->name);

        /* Release Info object array */
        PARSEC_OBJ_DESTRUCT(&exec_stream->infos);
    }
    // All exec_stream_t are allocated in a single malloc in gpu_device->exec_stream[0]
    free(gpu_device->exec_stream[0]);
    free(gpu_device->exec_stream);
    gpu_device->exec_stream = NULL;
    level_zero_device->level_zero_index = -1;
    parsec_sycl_wrapper_device_destroy(level_zero_device->swd);
    level_zero_device->swd = NULL;

    /* Cleanup the GPU memory. */
    PARSEC_OBJ_DESTRUCT(&gpu_device->gpu_mem_lru);
    PARSEC_OBJ_DESTRUCT(&gpu_device->gpu_mem_owned_lru);

    return PARSEC_SUCCESS;
}

/**
 * This function reserve the memory_percentage of the total device memory for PaRSEC.
 * This memory will be managed in chuncks of size eltsize. However, multiple chuncks
 * can be reserved in a single allocation.
 */
static int
parsec_level_zero_memory_reserve( parsec_device_level_zero_module_t* level_zero_device,
                                  int           memory_percentage,
                                  int           number_blocks,
                                  size_t        eltsize )
{
    ze_result_t status;
    parsec_device_gpu_module_t *gpu_device = &level_zero_device->super;
    (void)eltsize;
    ze_device_properties_t devProperties;
    ze_device_memory_properties_t *devMemProperties;
    ze_device_memory_access_properties_t memAccessProperties;
    uint32_t count = 0;

    size_t how_much_we_allocate;
    size_t initial_free_mem, alignment = 1 << 3;
    int memIndex = -1;
    uint32_t mem_elem_per_gpu = 0;

    status = zeDeviceGetMemoryAccessProperties(level_zero_device->ze_device, &memAccessProperties);
    PARSEC_LEVEL_ZERO_CHECK_ERROR("zeDeviceGetMemoryAccessProperties ", status, {
        return PARSEC_ERROR;
    });
    if( 0 == (ZE_MEMORY_ACCESS_CAP_FLAG_RW & memAccessProperties.deviceAllocCapabilities) ) {
        parsec_warning("%s:%d -- Device %s does not have memory allocation capabilities with RW access\n",
                       __FILE__, __LINE__, gpu_device->super.name);
        return PARSEC_ERROR;
    }
    status = zeDeviceGetProperties(level_zero_device->ze_device, &devProperties);
    PARSEC_LEVEL_ZERO_CHECK_ERROR("zeDeviceGetProperties ", status, {
       return PARSEC_ERROR;
    });
    status = zeDeviceGetMemoryProperties(level_zero_device->ze_device, &count, NULL);
    PARSEC_LEVEL_ZERO_CHECK_ERROR("zeDeviceGetMemoryProperties ", status, {
        return PARSEC_ERROR;
    });
    devMemProperties = (ze_device_memory_properties_t*)malloc(count * sizeof(ze_device_memory_properties_t));
    status = zeDeviceGetMemoryProperties(level_zero_device->ze_device, &count, devMemProperties);
    PARSEC_LEVEL_ZERO_CHECK_ERROR("zeDeviceGetMemoryProperties ", status, {
        free(devMemProperties);
        return PARSEC_ERROR;
    });
    for(int i = 0; i < (int)count; i++) {
        // TODO: better approach would be to keep a list of pointers?
        //   for now we just take the memory that has the highest amount of memory available
        if( memIndex == -1 || devMemProperties[memIndex].totalSize < devMemProperties[i].totalSize)
            memIndex = i;
    }
    initial_free_mem = devProperties.maxMemAllocSize < devMemProperties[memIndex].totalSize ?
            devProperties.maxMemAllocSize : devMemProperties[memIndex].totalSize;
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                         "level-zero device %s: initial_free_mem is %lu (device max alloc size is %lu, device memory "
                         "property total size for memory bank %d is %lu)", gpu_device->super.name, initial_free_mem,
                        devProperties.maxMemAllocSize, memIndex, devMemProperties[memIndex].totalSize);
    free(devMemProperties); devMemProperties = NULL;

    if( number_blocks != -1 ) {
        if( number_blocks == 0 ) {
            parsec_warning("LEVEL_ZERO[%d] Invalid argument: requesting 0 bytes of memory on LEVEL_ZERO device %s",
                           gpu_device->super.name, gpu_device->super.name);
            return PARSEC_ERROR;
        } else {
            how_much_we_allocate = number_blocks * eltsize;
        }
    } else {
        /** number_blocks == -1 means memory_percentage is used */
        how_much_we_allocate = (memory_percentage * initial_free_mem) / 100;
    }
    if( how_much_we_allocate > initial_free_mem ) {
        /** Handle the case of jokers who require more than 100% of memory,
         *  and eleventh case of computer scientists who don't know how
         *  to divide a number by another
         */
        parsec_warning("LEVEL_ZERO[%d] Requested %zd bytes on LEVEL_ZERO device %s, but only %zd bytes are available -- reducing allocation to max available",
                       level_zero_device->level_zero_index, how_much_we_allocate, gpu_device->super.name, initial_free_mem);
        how_much_we_allocate = initial_free_mem;
    }
    if( how_much_we_allocate < eltsize ) {
        /** Handle another kind of jokers entirely, and cases of
         *  not enough memory on the device
         */
        parsec_warning("LEVEL_ZERO[%d] Cannot allocate at least one element on LEVEL_ZERO device %s",
                       level_zero_device->level_zero_index, gpu_device->super.name);
        return PARSEC_ERROR;
    }
    ze_device_mem_alloc_desc_t memAllocDesc = {
            .stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
            .pNext = NULL,
            .flags = ZE_DEVICE_MEM_ALLOC_FLAG_BIAS_UNCACHED,
            .ordinal = memIndex
    };

#if defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE)
    size_t free_mem = initial_free_mem;
    /*
     * We allocate a bunch of tiles that will be used
     * during the computations
     */
    while( (free_mem > eltsize )
           && ((total_mem - free_mem) < how_much_we_allocate) ) {
        parsec_gpu_data_copy_t* gpu_elem;
        void *device_ptr;

        status = zeMemAllocDevice(level_zero_device->ze_context, &memAllocDesc, eltsize, alignment,
                                  level_zero_device->ze_device, &device_ptr);
        PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeMemAllocDevice ", status, { break; } );
        gpu_elem = PARSEC_OBJ_NEW(parsec_data_copy_t);
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                            "GPU[%s] Allocate LEVEL_ZERO copy %p [ref_count %d] for data [%p]",
                             gpu_device->super.name,gpu_elem, gpu_elem->super.obj_reference_count, NULL);
        gpu_elem->device_private = (void*)(long)device_ptr;
        gpu_elem->flags |= PARSEC_DATA_FLAG_PARSEC_OWNED;
        gpu_elem->device_index = gpu_device->super.device_index;
        mem_elem_per_gpu++;
        PARSEC_OBJ_RETAIN(gpu_elem);
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                            "GPU[%s] Retain and insert LEVEL_ZERO copy %p [ref_count %d] in LRU",
                             gpu_device->super.name, gpu_elem, gpu_elem->super.obj_reference_count);
        parsec_list_push_back( &gpu_device->gpu_mem_lru, (parsec_list_item_t*)gpu_elem );
        free_mem -= alignment * ((size + alignment - 1) / alignment);
    }
    if( 0 == mem_elem_per_gpu && parsec_list_is_empty( &gpu_device->gpu_mem_lru ) ) {
        parsec_warning("GPU[%s] Cannot allocate memory on GPU %s. Skip it!", gpu_device->super.name, gpu_device->super.name);
    }
    else {
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s] Allocate %u tiles on the GPU memory",
                             gpu_device->super.name, mem_elem_per_gpu );
    }
    PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                         "GPU[%s] Allocate %u tiles on the GPU memory", gpu_device->super.name, mem_elem_per_gpu);
#else
    if( NULL == gpu_device->memory ) {
        void* base_ptr;
        /* We allocate all the memory on the GPU and we use our memory management. */
        /* This computation leads to allocating more than available if we asked for more than GPU memory */
        mem_elem_per_gpu = (how_much_we_allocate + eltsize - 1 ) / eltsize;
        size_t total_size = (size_t)mem_elem_per_gpu * eltsize;

        if (total_size > initial_free_mem) {
            /* Mapping more than 100% of GPU memory is obviously wrong */
            /* Mapping exactly 100% of the GPU memory ends up producing errors about __global__ function call is not configured */
            /* Mapping 95% works with low-end GPUs like 1060, how much to let available for level_zero runtime, I don't know how to calculate */
            total_size = (size_t)((int)(.9*initial_free_mem / eltsize)) * eltsize;
            mem_elem_per_gpu = total_size / eltsize;
        }
        status = zeMemAllocDevice(level_zero_device->driver->ze_context, &memAllocDesc, total_size, alignment,
                                  level_zero_device->ze_device, &base_ptr);
        PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeMemAllocDevice ", status,
                                 ({ parsec_warning("GPU[%s] Allocating %zu bytes of memory on the GPU device failed",
                                                   gpu_device->super.name, total_size); }) );

        gpu_device->memory = zone_malloc_init( base_ptr, mem_elem_per_gpu, eltsize );

        if( gpu_device->memory == NULL ) {
            parsec_warning("GPU[%s] Cannot allocate memory on GPU %s. Skip it!",
                           gpu_device->super.name, gpu_device->super.name);
            return PARSEC_ERROR;
        }
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                            "GPU[%s] Allocate %u segments of size %d on the GPU memory",
                             gpu_device->super.name, mem_elem_per_gpu, eltsize );
    }
#endif
    gpu_device->mem_block_size = eltsize;
    gpu_device->mem_nb_blocks = mem_elem_per_gpu;

    return PARSEC_SUCCESS;
}

static void parsec_level_zero_memory_release_list(parsec_device_level_zero_module_t* level_zero_device,
                                            parsec_list_t* list)
{
    parsec_list_item_t* item;
#if defined(PARSEC_DEBUG_VERBOSE)
    parsec_device_gpu_module_t *gpu_device = &level_zero_device->super;
#endif

    while(NULL != (item = parsec_list_pop_front(list)) ) {
        parsec_gpu_data_copy_t* gpu_copy = (parsec_gpu_data_copy_t*)item;
        parsec_data_t* original = gpu_copy->original;

        PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                            "GPU[%s] Release LEVEL_ZERO copy %p (device_ptr %p) [ref_count %d: must be 1], attached to %p, in map %p",
                             gpu_device->super.name, gpu_copy, gpu_copy->device_private, gpu_copy->super.super
                             .obj_reference_count,
                             original, (NULL != original ? original->dc : NULL));
        assert( gpu_copy->device_index == level_zero_device->super.super.device_index );

        if( PARSEC_DATA_COHERENCY_OWNED == gpu_copy->coherency_state ) {
            parsec_warning("GPU[%s] still OWNS the master memory copy for data %d and it is discarding it!",
                          gpu_device->super.name, original->key);
        }
        assert(0 != (gpu_copy->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) );

#if defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE)
        level_zeroFree( gpu_copy->device_private );
#else

#if defined(PARSEC_PROF_TRACE)
        if((parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE) &&
           (gpu_device->exec_stream[0]->prof_event_track_enable ||
            gpu_device->exec_stream[1]->prof_event_track_enable)) {
            parsec_profiling_trace_flags(gpu_device->exec_stream[0]->profiling,
                                         parsec_gpu_free_memory_key, (int64_t)gpu_copy->device_private,
                                         gpu_device->super.device_index,
                                         NULL, PARSEC_PROFILING_EVENT_COUNTER);
            parsec_profiling_trace_flags(gpu_device->exec_stream[0]->profiling,
                                         parsec_gpu_use_memory_key_end,
                                         (uint64_t)gpu_copy->device_private,
                                         gpu_device->super.device_index, NULL, 0);
        }
#endif
        zone_free( level_zero_device->super.memory, (void*)gpu_copy->device_private );
#endif
        gpu_copy->device_private = NULL;

        /* At this point the data copies should have no attachement to a data_t. Thus,
         * before we get here (aka below parsec_fini), the destructor of the data
         * collection must have been called, releasing all the copies.
         */
        PARSEC_OBJ_RELEASE(gpu_copy); assert(NULL == gpu_copy);
    }
}

/**
 * This function only flushes the data copies pending in LRU, and checks
 * (in debug mode) that the entire allocated memory is free to use */
static int
parsec_level_zero_flush_lru( parsec_device_module_t *device )
{
    size_t in_use;
    parsec_device_gpu_module_t *gpu_device = (parsec_device_gpu_module_t*)device;
    parsec_device_level_zero_module_t *level_zero_device = (parsec_device_level_zero_module_t*)device;
    /* Free all memory on GPU */
    parsec_level_zero_memory_release_list(level_zero_device, &gpu_device->gpu_mem_lru);
    parsec_level_zero_memory_release_list(level_zero_device, &gpu_device->gpu_mem_owned_lru);
#if !defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE) && !defined(_NDEBUG)
    if( (in_use = zone_in_use(gpu_device->memory)) != 0 ) {
        parsec_warning("GPU[%s] memory leak detected: %lu bytes still allocated on GPU",
                       device->name, in_use);
        assert(0);
    }
#endif
    return PARSEC_SUCCESS;
}

/**
 * This function release the LEVEL_ZERO memory reserved for this device.
 *
 * One has to notice that all the data available on the GPU is stored in one of
 * the two used to keep track of the allocated data, either the gpu_mem_lru or
 * the gpu_mem_owner_lru. Thus, going over all the elements in these two lists
 * should be enough to enforce a clean release.
 */
static int
parsec_level_zero_memory_release( parsec_device_level_zero_module_t* level_zero_device )
{
    ze_result_t status;

    parsec_level_zero_flush_lru(&level_zero_device->super.super);

#if !defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE)
    assert( NULL != level_zero_device->super.memory );
    void* ptr = zone_malloc_fini(&level_zero_device->super.memory);
    status = zeMemFree(level_zero_device->driver->ze_context, ptr);
    PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeMemFree ", status,
                             { parsec_warning("Failed to free the GPU backend memory."); } );
#endif

    return PARSEC_SUCCESS;
}

/**
 * Try to find memory space to move all data on the GPU. We attach a device_elem to
 * a memory_elem as soon as a device_elem is available. If we fail to find enough
 * available elements, we push all the elements handled during this allocation
 * back into the pool of available device_elem, to be picked up by another call
 * (this call will remove them from the current task).
 * Returns:
 *   PARSEC_HOOK_RETURN_DONE:  All gpu_mem/mem_elem have been initialized
 *   PARSEC_HOOK_RETURN_AGAIN: At least one flow is marked under transfer, task cannot be scheduled yet
 *   PARSEC_HOOK_RETURN_NEXT:  The task needs to rescheduled
 */
static inline int
parsec_gpu_data_reserve_device_space( parsec_device_level_zero_module_t* level_zero_device,
                                      parsec_gpu_task_t *gpu_task )
{
    parsec_task_t *this_task = gpu_task->ec;
    parsec_gpu_data_copy_t* temp_loc[MAX_PARAM_COUNT], *gpu_elem, *lru_gpu_elem;
    parsec_data_t* master, *oldmaster;
    const parsec_flow_t *flow;
    int i, j, data_avail_epoch = 0;
    parsec_gpu_data_copy_t *gpu_mem_lru_cycling;
    parsec_device_gpu_module_t *gpu_device = &level_zero_device->super;

#if defined(PARSEC_DEBUG_NOISIER)
    char task_name[MAX_TASK_STRLEN];
    parsec_task_snprintf(task_name, MAX_TASK_STRLEN, this_task);
#endif  /* defined(PARSEC_DEBUG_NOISIER) */

    /**
     * Parse all the input and output flows of data and ensure all have
     * corresponding data on the GPU available.
     */
    for( i = 0; i < this_task->task_class->nb_flows; i++ ) {
        gpu_mem_lru_cycling = NULL;
        flow = gpu_task->flow[i];
        assert( flow && (flow->flow_index == i) );

        /* Skip CTL flows only */
        if(PARSEC_FLOW_ACCESS_NONE == (PARSEC_FLOW_ACCESS_MASK & flow->flow_flags)) continue;

        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s]:%s: Investigating flow %s:%d",
                             gpu_device->super.name, task_name, flow->name, i);
        temp_loc[i] = NULL;
        if (this_task->data[i].data_in == NULL)
            continue;

        master   = this_task->data[i].data_in->original;
        parsec_atomic_lock(&master->lock);
        gpu_elem = PARSEC_DATA_GET_COPY(master, gpu_device->super.device_index);
        this_task->data[i].data_out = gpu_elem;

        /* There is already a copy on the device */
        if( NULL != gpu_elem ) {
            PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                 "GPU[%s]:%s: Flow %s:%i has a copy on the device %p%s",
                                 gpu_device->super.name, task_name,
                                 flow->name, i, gpu_elem,
                                 gpu_elem->data_transfer_status == PARSEC_DATA_STATUS_UNDER_TRANSFER ? " [in transfer]" : "");
            if ( gpu_elem->data_transfer_status == PARSEC_DATA_STATUS_UNDER_TRANSFER ) {
                PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                     "GPU[%s]:%s: Copy %p [ref_count %d] is still in transfer, descheduling...",
                                     gpu_device->super.name, task_name,
                                     gpu_elem, gpu_elem->super.super.obj_reference_count);
                SET_HIGHEST_PRIORITY(gpu_task->ec, parsec_execution_context_priority_comparator);
                parsec_atomic_unlock(&master->lock);
                return PARSEC_HOOK_RETURN_AGAIN;
            }
            parsec_atomic_unlock(&master->lock);
            continue;
        }

#if !defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE)
        gpu_elem = PARSEC_OBJ_NEW(parsec_data_copy_t);
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s]:%s: Allocate LEVEL_ZERO copy %p sz %d[ref_count %d] for data %p",
                             gpu_device->super.name, task_name,
                             gpu_elem, gpu_task->flow_nb_elts[i], gpu_elem->super.super.obj_reference_count, master);
        gpu_elem->flags = PARSEC_DATA_FLAG_PARSEC_OWNED | PARSEC_DATA_FLAG_PARSEC_MANAGED;
      malloc_data:
        assert(0 != (gpu_elem->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) );
        gpu_elem->device_private = zone_malloc(gpu_device->memory, gpu_task->flow_nb_elts[i]);
        if( NULL == gpu_elem->device_private ) {
#endif

          find_another_data:
            /* Look for a data_copy to free */
            lru_gpu_elem = (parsec_gpu_data_copy_t*)parsec_list_pop_front(&gpu_device->gpu_mem_lru);
            if( NULL == lru_gpu_elem ) {
                /* We can't find enough room on the GPU. Insert the tiles in the begining of
                 * the LRU (in order to be reused asap) and return without scheduling the task.
                 */
#if defined(PARSEC_DEBUG_NOISIER)
                PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                                     "GPU[%s]:%s:\tRequest space on GPU failed for flow %s index %d/%d for task %s",
                                     gpu_device->super.name, task_name,
                                     flow->name, i, this_task->task_class->nb_flows, task_name );
#endif  /* defined(PARSEC_DEBUG_NOISIER) */
                for( j = 0; j < i; j++ ) {
                    /* This flow could be a control flow */
                    if( NULL == temp_loc[j] ) continue;
                    /* This flow could be non-parsec-owned, in which case we can't reclaim it */
                    if( 0 == (temp_loc[j]->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) continue;
                    PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                         "GPU[%s]:%s:\tAdd copy %p [ref_count %d] back to the LRU list",
                                         gpu_device->super.name, task_name,
                                         temp_loc[j], temp_loc[j]->super.super.obj_reference_count);
                    /* push them at the head to reach them again at the next iteration */
                    parsec_list_push_front(&gpu_device->gpu_mem_lru, (parsec_list_item_t*)temp_loc[j]);
                }
#if !defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE)
                PARSEC_OBJ_RELEASE(gpu_elem);
#endif
                parsec_atomic_unlock(&master->lock);
                return PARSEC_HOOK_RETURN_NEXT;
            }

            PARSEC_LIST_ITEM_SINGLETON(lru_gpu_elem);
            PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                 "GPU[%s]:%s: Evaluate LRU-retrieved LEVEL_ZERO copy %p [ref_count %d] original %p",
                                 gpu_device->super.name, task_name,
                                 lru_gpu_elem, lru_gpu_elem->super.super.obj_reference_count,
                                 lru_gpu_elem->original);

            /* If there are pending readers, let the gpu_elem loose. This is a weak coordination
             * protocol between here and the parsec_gpu_data_stage_in, where the readers don't necessarily
             * always remove the data from the LRU.
             */
            if( 0 != lru_gpu_elem->readers ) {
                PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                     "GPU[%s]:%s: Drop LRU-retrieved LEVEL_ZERO copy %p [readers %d, ref_count %d] original %p",
                                     gpu_device->super.name, task_name,
                                     lru_gpu_elem, lru_gpu_elem->readers, lru_gpu_elem->super.super.obj_reference_count, lru_gpu_elem->original);
                goto find_another_data; // TODO: add an assert of some sort to check for leaks here?
            }
            /* It's also possible that the ref_count of that element is bigger than 1
             * In that case, it's because some task completion did not execute yet, and
             * we need to keep it in the list until it reaches 1.
             */
            if( lru_gpu_elem->super.super.obj_reference_count > 1 ) {
                /* It's also possible (although unlikely) that we livelock here:
                 * if gpu_mem_lru has *only* elements with readers == 0 but
                 * ref_count > 1, then we might pop/push forever, and we need
                 * to make progress on something else to get ref_count == 1 && readers == 0.
                 * So, we return that there is no more free to do.
                 * To detect that, we use the first one we push back: if we see it
                 * again, we're cycling. */
                PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                     "GPU[%s]:%s: Push back LRU-retrieved LEVEL_ZERO copy %p [readers %d, ref_count %d] original %p",
                                     gpu_device->super.name, task_name,
                                     lru_gpu_elem, lru_gpu_elem->readers, lru_gpu_elem->super.super.obj_reference_count, lru_gpu_elem->original);
                assert(0 != (lru_gpu_elem->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) );
                parsec_list_push_back(&gpu_device->gpu_mem_lru, &lru_gpu_elem->super);
                if( NULL == gpu_mem_lru_cycling ) {
                    gpu_mem_lru_cycling = lru_gpu_elem;
                    goto find_another_data;
                } else {
                    if( gpu_mem_lru_cycling == lru_gpu_elem ) {
                        PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                                             "GPU[%s]: Cycle detected on allocating memory for %s",
                                             gpu_device->super.name, task_name);
                        parsec_atomic_unlock(&master->lock);
                        return PARSEC_HOOK_RETURN_NEXT;
                    } else {
                        goto find_another_data;
                    }
                }
            }

            /* Make sure the new GPU element is clean and ready to be used */
            assert( master != lru_gpu_elem->original );
            if ( NULL != lru_gpu_elem->original ) {
                /* Let's check we're not trying to steal one of our own data */
                oldmaster = lru_gpu_elem->original;
                if( !parsec_atomic_trylock( &oldmaster->lock ) ) {
                    /* Even if we have the lock on oldmaster, any other thread
                     * might be adding/removing other elements to the list, so we
                     * need to protect all accesses to gpu_mem_lru with the locked version */
                    assert(0 != (lru_gpu_elem->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) );
                    parsec_list_push_back(&gpu_device->gpu_mem_lru, &lru_gpu_elem->super);
                    if( NULL == gpu_mem_lru_cycling ) {
                        gpu_mem_lru_cycling = lru_gpu_elem;
                        goto find_another_data;
                    } else {
                        if( gpu_mem_lru_cycling == lru_gpu_elem ) {
                            PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                                                 "GPU[%s]: Cycle detected on allocating memory for %s",
                                                 gpu_device->super.name, task_name);
                            parsec_atomic_unlock(&master->lock);
                            return PARSEC_HOOK_RETURN_NEXT;
                        } else {
                            goto find_another_data;
                        }
                    }
                }
                for( j = 0; j < i; j++ ) {
                    if( NULL == this_task->data[j].data_in ) continue;
                    if( this_task->data[j].data_in->original == oldmaster ) {
                        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                             "GPU[%s]:%s: Drop LRU-retrieved LEVEL_ZERO copy %p [ref_count %d] already in use by same task %d:%d original %p",
                                             gpu_device->super.name, task_name,
                                             lru_gpu_elem, lru_gpu_elem->super.super.obj_reference_count, i, j, lru_gpu_elem->original);
                        /* If we are the owner of this tile we need to make sure it remains available for
                         * other tasks or we run in deadlock situations.
                         */
                        if( temp_loc[j] != lru_gpu_elem )
                            temp_loc[j] = lru_gpu_elem;
#if defined(PARSEC_DEBUG_NOISIER)
                        /* Make sure the data copy is indeed referenced from the current task */
                        for( j = 0; j < i; j++ ) {
                            if( lru_gpu_elem == temp_loc[j] ) break;
                        }
                        assert( j < i );
#endif  /* defined(PARSEC_DEBUG_NOISIER) */
                        parsec_atomic_unlock( &oldmaster->lock );
                        goto find_another_data;
                    }
                }
                if( lru_gpu_elem->readers != 0 ) {
                    /* Damn, another thread started to use this data. */
                    parsec_atomic_unlock( &oldmaster->lock );
                    goto find_another_data;
                }
                int do_unlock = oldmaster->super.obj_reference_count != 1;
                parsec_data_copy_detach(oldmaster, lru_gpu_elem, gpu_device->super.device_index);
                if( do_unlock )
                    parsec_atomic_unlock( &oldmaster->lock );
                assert(lru_gpu_elem->readers == 0);
                /* The data is not used, it's not one of ours, and it has been detached from the device
                 * so no other device can use it as a source for their copy : we can free it or reuse it */
                PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                     "GPU[%s]:%s:\ttask %s:%d repurpose copy %p [ref_count %d] to data %p instead of %p",
                                     gpu_device->super.name, task_name, this_task->task_class->name, i, lru_gpu_elem,
                                     lru_gpu_elem->super.super.obj_reference_count, master, oldmaster);
            }
            else {
                PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                     "GPU[%s]:%s:\ttask %s:%d found detached memory from previously destructed data %p",
                                     gpu_device->super.name, task_name, this_task->task_class->name, i, lru_gpu_elem);
                oldmaster = NULL;
            }
#if !defined(PARSEC_GPU_LEVEL_ZERO_ALLOC_PER_TILE)
            /* Let's free this space, and try again to malloc some space */
            PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                                 "GPU[%s] Release LEVEL_ZERO copy %p (device_ptr %p) [ref_count %d: must be 1], attached to %p",
                                 gpu_device->super.name,
                                 lru_gpu_elem, lru_gpu_elem->device_private, lru_gpu_elem->super.super.obj_reference_count,
                                 oldmaster);
#if defined(PARSEC_PROF_TRACE)
            if((parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE) &&
               (gpu_device->exec_stream[0]->prof_event_track_enable ||
                gpu_device->exec_stream[1]->prof_event_track_enable)) {
                parsec_profiling_trace_flags(gpu_device->exec_stream[0]->profiling,
                                             parsec_gpu_free_memory_key, (int64_t)lru_gpu_elem->device_private,
                                             gpu_device->super.device_index,
                                             NULL, PARSEC_PROFILING_EVENT_COUNTER);
                parsec_profiling_trace_flags(gpu_device->exec_stream[0]->profiling,
                                             parsec_gpu_use_memory_key_end,
                                             (uint64_t)lru_gpu_elem->device_private,
                                             gpu_device->super.device_index, NULL, 0);
            }
#endif
            assert( 0 != (lru_gpu_elem->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) );
            zone_free( gpu_device->memory, (void*)(lru_gpu_elem->device_private) );
            lru_gpu_elem->device_private = NULL;
            data_avail_epoch++;
            PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                                 "GPU[%s]:%s: Release LRU-retrieved LEVEL_ZERO copy %p [ref_count %d: must be 1]",
                                 gpu_device->super.name, task_name,
                                 lru_gpu_elem, lru_gpu_elem->super.super.obj_reference_count);
            PARSEC_OBJ_RELEASE(lru_gpu_elem);
            assert( NULL == lru_gpu_elem );
            goto malloc_data;
        }
        PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                             "GPU[%s] Succeeded Allocating LEVEL_ZERO copy %p at real address %p [ref_count %d] for data %p",
                             gpu_device->super.name,
                             gpu_elem, gpu_elem->device_private, gpu_elem->super.super.obj_reference_count, master);
#if defined(PARSEC_PROF_TRACE)
        if((parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE) &&
                        (gpu_device->exec_stream[0]->prof_event_track_enable ||
                         gpu_device->exec_stream[1]->prof_event_track_enable)) {
            parsec_profiling_trace_flags(gpu_device->exec_stream[0]->profiling,
                                         parsec_gpu_allocate_memory_key, (int64_t)gpu_elem->device_private,
                                         gpu_device->super.device_index,
                                         &gpu_task->flow_nb_elts[i], PARSEC_PROFILING_EVENT_COUNTER|PARSEC_PROFILING_EVENT_HAS_INFO);
        }
#endif
#else
        gpu_elem = lru_gpu_elem;
#endif
        assert( 0 == gpu_elem->readers );
        gpu_elem->coherency_state = PARSEC_DATA_COHERENCY_INVALID;
        gpu_elem->version = 0;
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                             "GPU[%s]: GPU copy %p [ref_count %d] gets created with version 0 at %s:%d",
                             gpu_device->super.name,
                             gpu_elem, gpu_elem->super.super.obj_reference_count,
                             __FILE__, __LINE__);
        parsec_data_copy_attach(master, gpu_elem, gpu_device->super.device_index);
        this_task->data[i].data_out = gpu_elem;
        /* set the new datacopy type to the correct one */
        this_task->data[i].data_out->dtt = this_task->data[i].data_in->dtt;
        temp_loc[i] = gpu_elem;
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s]:%s: Retain and insert LEVEL_ZERO copy %p [ref_count %d] in LRU",
                             gpu_device->super.name, task_name,
                             gpu_elem, gpu_elem->super.super.obj_reference_count);
        assert(0 != (gpu_elem->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) );
        parsec_list_push_back(&gpu_device->gpu_mem_lru, (parsec_list_item_t*)gpu_elem);
        parsec_atomic_unlock(&master->lock);
    }
    if( data_avail_epoch ) {
        gpu_device->data_avail_epoch++;
    }
    return PARSEC_HOOK_RETURN_DONE;
}

/* Default stage_in function to transfer data to the GPU device.
 * Transfer transfer the <count> contiguous bytes from
 * task->data[i].data_in to task->data[i].data_out.
 *
 * @param[in] task parsec_task_t containing task->data[i].data_in, task->data[i].data_out.
 * @param[in] flow_mask indicating task flows for which to transfer.
 * @param[in] gpu_stream parsec_gpu_exec_stream_t used for the transfer.
 *
 */
int
parsec_default_level_zero_stage_in(parsec_gpu_task_t        *gtask,
                                   uint32_t                  flow_mask,
                                   parsec_gpu_exec_stream_t *gpu_stream)
{
    ze_result_t ret;
    parsec_data_copy_t * copy_in;
    parsec_data_copy_t * copy_out;
    parsec_task_t *task = gtask->ec;
    size_t count;
    parsec_level_zero_exec_stream_t *level_zero_stream = (parsec_level_zero_exec_stream_t *)gpu_stream;
    int i;
    for(i = 0; i < task->task_class->nb_flows; i++){
        if(flow_mask & (1U << i)){
            copy_in = task->data[i].data_in;
            copy_out = task->data[i].data_out;
            count = (copy_in->original->nb_elts <= copy_out->original->nb_elts) ?
                          copy_in->original->nb_elts : copy_out->original->nb_elts;
            ret = (ze_result_t)zeCommandListAppendMemoryCopy(level_zero_stream->level_zero_cl,
                                                             copy_out->device_private,
                                                             copy_in->device_private,
                                                             count,
                                                             NULL,
                                                             0, NULL);
            PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandListAppendMemoryCopy ", ret, { return PARSEC_ERROR; } );
        }
    }
    return PARSEC_SUCCESS;
}

/* Default stage_out function to transfer data from the GPU device.
 * Transfer transfer the <count> contiguous bytes from
 * task->data[i].data_in to task->data[i].data_out.
 *
 * @param[in] task parsec_task_t containing task->data[i].data_in, task->data[i].data_out.
 * @param[in] flow_mask indicating task flows for which to transfer.
 * @param[in] gpu_stream parsec_gpu_exec_stream_t used for the transfer.
 *
 */
int
parsec_default_level_zero_stage_out(parsec_gpu_task_t        *gtask,
                                    uint32_t                  flow_mask,
                                    parsec_gpu_exec_stream_t *gpu_stream)
{
    ze_result_t ret;
    parsec_data_copy_t * copy_in;
    parsec_data_copy_t * copy_out;
    parsec_task_t *task = gtask->ec;
    size_t count;
    parsec_level_zero_exec_stream_t *level_zero_stream = (parsec_level_zero_exec_stream_t*)gpu_stream;
    int i;
    for(i = 0; i < task->task_class->nb_flows; i++){
        if(flow_mask & (1U << i)){
            copy_in = task->data[i].data_out;
            copy_out = copy_in->original->device_copies[0];
            count = (copy_in->original->nb_elts <= copy_out->original->nb_elts) ? copy_in->original->nb_elts :
                        copy_out->original->nb_elts;
            ret = (ze_result_t)zeCommandListAppendMemoryCopy(level_zero_stream->level_zero_cl,
                                                             copy_out->device_private,
                                                             copy_in->device_private,
                                                             count,
                                                             NULL,
                                                             0, NULL);
            PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandListAppendMemoryCopy ", ret, { return PARSEC_ERROR; } );
        }
    }
    return PARSEC_SUCCESS;
}

/**
 * If the most current version of the data is not yet available on the GPU memory
 * schedule a transfer.
 * Returns:
 *    0: The most recent version of the data is already available on the GPU
 *    1: A copy has been scheduled on the corresponding stream
 *   -1: A copy cannot be issued due to LEVEL_ZERO.
 */
static inline int
parsec_gpu_data_stage_in( parsec_device_level_zero_module_t* level_zero_device,
                          const parsec_flow_t *flow,
                          parsec_data_pair_t* task_data,
                          parsec_gpu_task_t *gpu_task,
                          parsec_gpu_exec_stream_t *gpu_stream )
{
    parsec_device_gpu_module_t *gpu_device = &level_zero_device->super;
    int32_t type = flow->flow_flags;
    parsec_data_copy_t* in_elem = task_data->data_in;
    parsec_data_copy_t* release_after_data_in_is_attached = NULL;
    parsec_data_t* original = in_elem->original;
    parsec_gpu_data_copy_t* gpu_elem = task_data->data_out;
    uint32_t nb_elts = gpu_task->flow_nb_elts[flow->flow_index];
    int transfer_from = -1;
    int undo_readers_inc_if_no_transfer = 0;

    if( gpu_task->task_type == GPU_TASK_TYPE_PREFETCH ) {
        PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                             "GPU[%s]: Prefetch task %p is staging in",
                             gpu_device->super.name, gpu_task);
    }

    parsec_atomic_lock( &original->lock );

    /**
     * If the data will be accessed in write mode, remove it from any GPU data management
     * lists until the task is completed.
     */
    if( PARSEC_FLOW_ACCESS_WRITE & type ) {
        if (gpu_elem->readers > 0 ) {
            if( !((1 == gpu_elem->readers) && (PARSEC_FLOW_ACCESS_READ & type)) ) {
                parsec_warning("GPU[%s]:\tWrite access to data copy %p [ref_count %d] with existing readers [%d] "
                               "(possible anti-dependency,\n"
                               "or concurrent accesses), please prevent that with CTL dependencies\n",
                               gpu_device->super.name, gpu_elem, gpu_elem->super.super.obj_reference_count, gpu_elem->readers);
                parsec_atomic_unlock( &original->lock );
                return -1;
            }
        }
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s]:\tDetach writable LEVEL_ZERO copy %p [ref_count %d] from any lists",
                             gpu_device->super.name, gpu_elem, gpu_elem->super.super.obj_reference_count);
        /* make sure the element is not in any tracking lists */
        parsec_list_item_ring_chop((parsec_list_item_t*)gpu_elem);
        PARSEC_LIST_ITEM_SINGLETON(gpu_elem);
    }

    /* Detect if we can do a device to device copy.
     * Current limitations: only for read-only data used read-only on the hosting GPU. */
    parsec_device_level_zero_module_t *in_elem_dev = (parsec_device_level_zero_module_t*)parsec_mca_device_get( in_elem->device_index );
    if( (PARSEC_FLOW_ACCESS_READ & type) && !(PARSEC_FLOW_ACCESS_WRITE & type) ) {
        int potential_alt_src = 0;
        if( PARSEC_DEV_LEVEL_ZERO == in_elem_dev->super.super.type ) {
            if( gpu_device->peer_access_mask & (1 << in_elem_dev->level_zero_index) ) {
                /* We can directly do D2D, so let's skip the selection */
                goto src_selected;
            }
        }

        /* If gpu_elem is not invalid, then it is already there and the right version,
         * and we're not going to transfer from another source, skip the selection */
        if( gpu_elem->coherency_state != PARSEC_DATA_COHERENCY_INVALID )
            goto src_selected;

        for(int t = 1; t < (int)parsec_nb_devices; t++) {
            parsec_device_level_zero_module_t *target = (parsec_device_level_zero_module_t*)parsec_mca_device_get(t);
            if( PARSEC_DEV_LEVEL_ZERO != target->super.super.type ) continue;
            if(gpu_device->peer_access_mask & (1 << target->level_zero_index)) {
                parsec_data_copy_t *candidate = original->device_copies[t];
                if( NULL != candidate && candidate->version == in_elem->version ) {
                    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                         "GPU[%s]:\tData copy %p [ref_count %d] on LEVEL_ZERO device %d is a potential alternative source for in_elem %p on data %p",
                                         gpu_device->super.name, candidate, candidate->super.super.obj_reference_count, target->level_zero_index, in_elem, original);
                    if(PARSEC_DATA_COHERENCY_INVALID == candidate->coherency_state) {
                        /* We're already pulling this data on candidate...
                         * If there is another candidate that already has it, we'll use
                         * that one; otherwise, we'll fall back on the CPU version. */
                        potential_alt_src = 1;
                        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                             "GPU[%s]:\tData copy %p [ref_count %d] on LEVEL_ZERO device %d is invalid, continuing to look for alternatives",
                                             gpu_device->super.name, candidate, candidate->super.super.obj_reference_count, target->level_zero_index);
                        continue;
                    }
                    /* candidate is the best candidate to do D2D. Let's register as a reader for this
                     * data copy, and we can unlock and schedule the D2D. */
                    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                         "GPU[%s]:\tData copy %p [ref_count %d] on LEVEL_ZERO device %d is the best candidate to to Device to Device copy, increasing its readers to %d",
                                         gpu_device->super.name, candidate, candidate->super.super.obj_reference_count, target->level_zero_index, candidate->readers+1);
                    parsec_atomic_fetch_inc_int32( &candidate->readers );
                    undo_readers_inc_if_no_transfer = 1;
                    /* We swap data_in with candidate, so we update the reference counters */
                    PARSEC_OBJ_RETAIN(candidate);
                    release_after_data_in_is_attached = task_data->data_in;
                    task_data->data_in = candidate;
                    in_elem = candidate;
                    in_elem_dev = target;
                    goto src_selected;
                }
            }
        }
        if( potential_alt_src ) {
            /* We found a potential alternative source, but it's not ready now,
             * we delay the scheduling of this task. */
            /** TODO: when considering RW acccesses, don't forget to unchop gpu_elem
             *        from its queue... */
            PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                 "GPU[%s]:\tThere is a potential alternative source for in_elem %p [ref_count %d] in original %p to go in copy %p [ref_count %d], but it is not ready, falling back on CPU source",
                                 gpu_device->super.name, in_elem, in_elem->super.super.obj_reference_count, original, gpu_elem, gpu_elem->super.super.obj_reference_count);
            //return PARSEC_HOOK_RETURN_NEXT;
        }

        /* We fall back on the CPU copy */
        assert( original->device_copies[0] != NULL && in_elem->version == original->device_copies[0]->version );
        in_elem = original->device_copies[0];
        assert(task_data->data_in == in_elem);
        assert(in_elem->device_index == 0);
        in_elem_dev = (parsec_device_level_zero_module_t*)parsec_mca_device_get(in_elem->device_index);
    }

 src_selected:
    transfer_from = parsec_data_start_transfer_ownership_to_copy(original, gpu_device->super.device_index, (uint8_t)type);

    if( PARSEC_FLOW_ACCESS_WRITE & type && gpu_task->task_type != GPU_TASK_TYPE_PREFETCH ) {
        gpu_elem->version++;  /* on to the next version */
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                             "GPU[%s]: GPU copy %p [ref_count %d] increments version to %d at %s:%d",
                             gpu_device->super.name,
                             gpu_elem, gpu_elem->super.super.obj_reference_count, gpu_elem->version,
                             __FILE__, __LINE__);
    }

    gpu_device->super.required_data_in += nb_elts;
    if( -1 != transfer_from ) {
        /* If it is already under transfer, don't schedule the transfer again.
         * This happens if the task refers twice (or more) to the same input flow */
        if( gpu_elem->data_transfer_status == PARSEC_DATA_STATUS_UNDER_TRANSFER ) {
            PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                 "GPU[%s]:\t\tMove %s data copy %p [ref_count %d, key %x] of %d bytes\t(src dev: %d, v:%d, ptr:%p, copy:%p [ref_count %d] / dst dev: %d, v:%d, ptr:%p): data copy is already under transfer, ignoring double request",
                                 gpu_device->super.name,
                                 in_elem_dev->super.super.type == PARSEC_DEV_LEVEL_ZERO ? "D2D": "H2D",
                                 gpu_elem, gpu_elem->super.super.obj_reference_count, original->key, nb_elts,
                                 in_elem_dev->super.super.device_index, in_elem->version, (void*)
                                 in_elem->device_private, in_elem, in_elem->super.super.obj_reference_count,
                                 gpu_device->super.device_index, gpu_elem->version, (void*)
                                 gpu_elem->device_private);
        } else {
            PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                 "GPU[%s]:\t\tMove %s data copy %p [ref_count %d, key %x] of %d bytes\t(src dev: %d, v:%d, ptr:%p, copy:%p [ref_count %d] / dst dev: %d, v:%d, ptr:%p)",
                                 gpu_device->super.name,
                                 in_elem_dev->super.super.type == PARSEC_DEV_LEVEL_ZERO ? "D2D": "H2D",
                                 gpu_elem, gpu_elem->super.super.obj_reference_count, original->key, nb_elts,
                                 in_elem_dev->super.super.device_index, in_elem->version, (void*)in_elem->device_private, in_elem, in_elem->super.super.obj_reference_count,
                                 gpu_device->super.device_index, gpu_elem->version, (void*)gpu_elem->device_private);

            assert((gpu_elem->version < in_elem->version) || (gpu_elem->data_transfer_status == PARSEC_DATA_STATUS_NOT_TRANSFER));

#if defined(PARSEC_PROF_TRACE)
            if( gpu_stream->prof_event_track_enable  ) {
                parsec_profile_data_collection_info_t info;

                if( NULL != original->dc ) {
                    info.desc    = original->dc;
                    info.data_id = original->key;
                } else {
                    assert( GPU_TASK_TYPE_PREFETCH != gpu_task->task_type );
                    info.desc    = (parsec_dc_t*)original;
                    info.data_id = -1;
                }
                gpu_task->prof_key_end = -1;

                if( GPU_TASK_TYPE_PREFETCH == gpu_task->task_type && (parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_PREFETCH) ) {
                    gpu_task->prof_key_end = parsec_gpu_prefetch_key_end;
                    gpu_task->prof_event_id = (int64_t)gpu_elem->device_private;
                    gpu_task->prof_tp_id = level_zero_device->level_zero_index;
                    PARSEC_PROFILING_TRACE(gpu_stream->profiling,
                                           parsec_gpu_prefetch_key_start,
                                           gpu_task->prof_event_id,
                                           gpu_task->prof_tp_id,
                                           &info);
                }
                if(GPU_TASK_TYPE_PREFETCH != gpu_task->task_type && (parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_DATA_IN) ) {
                    PARSEC_PROFILING_TRACE(gpu_stream->profiling,
                                           parsec_gpu_movein_key_start,
                                           (int64_t)gpu_elem->device_private,
                                           level_zero_device->level_zero_index,
                                           &info);
                }
                if(parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_MEM_USE) {
                        parsec_device_level_zero_memory_prof_info_t _info;
                        _info.size = (uint64_t)nb_elts;
                        _info.data_key = gpu_elem->original->key;
                        _info.dc_id = (uint64_t)(gpu_elem->original->dc);
                        parsec_profiling_trace_flags(gpu_stream->profiling,
                                                     parsec_gpu_use_memory_key_start, (uint64_t)gpu_elem->device_private,
                                                     level_zero_device->level_zero_index, &_info, PARSEC_PROFILING_EVENT_HAS_INFO);
                }
            }
#endif
            /* Push data into the GPU from the source device */
            if(PARSEC_SUCCESS != gpu_task->stage_in(gpu_task, (1U << flow->flow_index), gpu_stream)){
                parsec_warning( "%s:%d %s", __FILE__, __LINE__,
                                "gpu_task->stage_in");
                if( in_elem_dev->super.super.type != PARSEC_DEV_LEVEL_ZERO )
                    parsec_warning("<<%p>> -> <<%p on LEVEL_ZERO device %d>> [%d, H2D]",
                                   in_elem->device_private, gpu_elem->device_private, level_zero_device->level_zero_index,
                                   nb_elts);
                else
                    parsec_warning("<<%p on LEVEL_ZERO device %d>> -> <<%p on LEVEL_ZERO device %d>> [%d, D2D]",
                                   in_elem->device_private, in_elem_dev->level_zero_index,
                                   gpu_elem->device_private, level_zero_device->level_zero_index,
                                   nb_elts);
                parsec_atomic_unlock( &original->lock );
                if( NULL != release_after_data_in_is_attached )
                    PARSEC_OBJ_RELEASE(release_after_data_in_is_attached);
                assert(0);
                return -1;
            }
            assert(in_elem_dev->super.super.device_index < gpu_device->super.data_in_array_size);
            gpu_device->super.data_in_from_device[in_elem_dev->super.super.device_index] += nb_elts;
            if( PARSEC_GPU_TASK_TYPE_KERNEL == gpu_task->task_type )
                gpu_device->super.nb_data_faults += nb_elts;

            /* update the data version in GPU immediately, and mark the data under transfer */
            assert((gpu_elem->version != in_elem->version) || (gpu_elem->data_transfer_status == PARSEC_DATA_STATUS_NOT_TRANSFER));
            gpu_elem->version = in_elem->version;
            PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                 "GPU[%s]: GPU copy %p [ref_count %d] gets the same version %d as copy %p [ref_count %d] at %s:%d",
                                 gpu_device->super.name,
                                 gpu_elem, gpu_elem->super.super.obj_reference_count, gpu_elem->version, in_elem, in_elem->super.super.obj_reference_count,
                                 __FILE__, __LINE__);

            gpu_elem->data_transfer_status = PARSEC_DATA_STATUS_UNDER_TRANSFER;
        }
        gpu_elem->push_task = gpu_task->ec;  /* only the task who does the transfer can modify the data status later. */
        parsec_atomic_unlock( &original->lock );
        if( NULL != release_after_data_in_is_attached )
            PARSEC_OBJ_RELEASE(release_after_data_in_is_attached);
        return 1;
    }
    if( undo_readers_inc_if_no_transfer )
        parsec_atomic_fetch_dec_int32( &in_elem->readers );
    assert( gpu_elem->data_transfer_status == PARSEC_DATA_STATUS_COMPLETE_TRANSFER );

    parsec_data_end_transfer_ownership_to_copy(original, gpu_device->super.device_index, (uint8_t)type);

    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                         "GPU[%s]:\t\tNO Move %s for data copy %p [ref_count %d, key %x] of %d bytes (host v:%d / device v:%d)",
                         gpu_device->super.name,
                         (NULL == in_elem_dev) ? "h2d" : (in_elem_dev->super.super.type == PARSEC_DEV_LEVEL_ZERO ? "D2D": "H2D"),
                         gpu_elem, gpu_elem->super.super.obj_reference_count, original->key, nb_elts,
                         in_elem->version, gpu_elem->version);
    parsec_atomic_unlock( &original->lock );
    if( NULL != release_after_data_in_is_attached )
        PARSEC_OBJ_RELEASE(release_after_data_in_is_attached);
    /* TODO: data keeps the same coherence flags as before */
    return 0;
}

static parsec_flow_t parsec_level_zero_data_prefetch_flow = {
    .name = "FLOW",
    .flow_flags = PARSEC_FLOW_ACCESS_READ,
    .flow_index = 0,
};

static parsec_task_class_t parsec_level_zero_data_prefetch_tc = {
    .name = "LEVEL_ZERO PREFETCH",
    .flags = 0,
    .task_class_id = 0,
    .nb_flows = 1,
    .nb_parameters = 0,
    .nb_locals = 0,
    .dependencies_goal = 0,
    .params = { NULL, },
    .in = { &parsec_level_zero_data_prefetch_flow, NULL },
    .out = { NULL, },
    .priority = NULL,
    .properties = NULL,
    .initial_data = NULL,
    .final_data = NULL,
    .data_affinity = NULL,
    .key_functions = NULL,
    .make_key = NULL,
    .get_datatype = NULL,
    .prepare_input = NULL,
    .incarnations = NULL,
    .prepare_output = NULL,
    .find_deps = NULL,
    .iterate_successors = NULL,
    .iterate_predecessors = NULL,
    .release_deps = NULL,
    .complete_execution = NULL,
    .new_task = NULL,
    .release_task = NULL,
    .fini = NULL
};

static int
parsec_level_zero_destroy_task(parsec_device_gpu_module_t* gpu_device,
                         parsec_gpu_task_t** out_task)
{
#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[MAX_TASK_STRLEN];
#endif
    parsec_gpu_task_t *gpu_task = *out_task;
    (void)gpu_device;
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,  "GPU[%s]: Destroying task %s (%p with ec %p)",
                         gpu_device->super.name, parsec_gpu_describe_gpu_task(tmp, MAX_TASK_STRLEN, gpu_task),
                         gpu_task, gpu_task->ec);
    assert( GPU_TASK_TYPE_PREFETCH == gpu_task->task_type || GPU_TASK_TYPE_D2D_COMPLETE == gpu_task->task_type );
    if( GPU_TASK_TYPE_PREFETCH == gpu_task->task_type) PARSEC_DATA_COPY_RELEASE( gpu_task->ec->data[0].data_in);
    PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream, "GPU[%s]: gpu_task %p freed at %s:%d\n",
                         gpu_device->super.name, gpu_task, __FILE__, __LINE__);
    free( gpu_task->ec );
    gpu_task->ec = NULL;
    free( *out_task );
    *out_task = NULL;
    return 0;
}

#if defined(PARSEC_DEBUG_NOISIER)
static char *parsec_level_zero_debug_advice_to_string(int advice)
{
    switch(advice) {
    case PARSEC_DEV_DATA_ADVICE_PREFETCH:
        return "Prefetch";
    case PARSEC_DEV_DATA_ADVICE_PREFERRED_DEVICE:
        return "Set Preferred Device";
    case PARSEC_DEV_DATA_ADVICE_WARMUP:
        return "Mark data as recently used";
    default:
        assert(0);
        return "Undefined advice";
    }
}
#endif

static int
parsec_level_zero_data_advise(parsec_device_module_t *dev, parsec_data_t *data, int advice)
{
    parsec_device_gpu_module_t* gpu_device = (parsec_device_gpu_module_t*)dev;
#if defined(PARSEC_DEBUG_NOISIER)
    char buffer[64];
    if(NULL != data->dc) {
        data->dc->key_to_string(data->dc, data->key, buffer, 64);
    } else {
        snprintf(buffer, 64, "unbound data");
    }
#endif

    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,  "GPU[%s]: User provides advice %s of %s (%p)",
                         gpu_device->super.name,
                         parsec_level_zero_debug_advice_to_string(advice),
                         buffer,
                         data);

    switch(advice) {
    case PARSEC_DEV_DATA_ADVICE_PREFERRED_DEVICE:
        data->preferred_device = dev->device_index;
        /* We continue on to the next case, as we want to also
         * prefetch the data on the target device, if it is the
         * preferred device */
        break; //__attribute__ ((fallthrough));
    case PARSEC_DEV_DATA_ADVICE_PREFETCH:
        {
            if( parsec_type_contiguous(data->device_copies[ data->owner_device ]->dtt) != PARSEC_SUCCESS){
                parsec_warning( "%s:%d %s", __FILE__, __LINE__,
                                " PARSEC_DEV_DATA_ADVICE_PREFETCH cannot be applied to non contiguous types ");
                return PARSEC_ERROR;
            }
            parsec_gpu_task_t* gpu_task = NULL;
            gpu_task = (parsec_gpu_task_t*)calloc(1, sizeof(parsec_gpu_task_t));
            gpu_task->task_type = GPU_TASK_TYPE_PREFETCH;
            gpu_task->ec = calloc(1, sizeof(parsec_task_t));
            PARSEC_OBJ_CONSTRUCT(gpu_task->ec, parsec_task_t);
            gpu_task->ec->task_class = &parsec_level_zero_data_prefetch_tc;
            gpu_task->flow[0] = &parsec_level_zero_data_prefetch_flow;
            gpu_task->flow_nb_elts[0] = data->device_copies[ data->owner_device ]->original->nb_elts;
            gpu_task->stage_in  = parsec_default_level_zero_stage_in;
            gpu_task->stage_out = parsec_default_level_zero_stage_out;
            PARSEC_DEBUG_VERBOSE(20, parsec_debug_output, "Retain data copy %p [ref_count %d] at %s:%d",
                                 data->device_copies[ data->owner_device ],
                                 data->device_copies[ data->owner_device ]->super.super.obj_reference_count,
                                 __FILE__, __LINE__);
            PARSEC_OBJ_RETAIN(data->device_copies[ data->owner_device ]);
            gpu_task->ec->data[0].data_in = data->device_copies[ data->owner_device ];
            gpu_task->ec->data[0].data_out = NULL;
            gpu_task->ec->data[0].source_repo_entry = NULL;
            gpu_task->ec->data[0].source_repo = NULL;
            PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                 "GPU[%s]: data copy %p [ref_count %d] linked to prefetch gpu task %p on GPU copy %p [ref_count %d]",
                                 gpu_device->super.name, gpu_task->ec->data[0].data_in, gpu_task->ec->data[0].data_in->super.super.obj_reference_count,
                                 gpu_task, gpu_task->ec->data[0].data_out, gpu_task->ec->data[0].data_out->super.super.obj_reference_count);
            parsec_fifo_push( &(gpu_device->pending), (parsec_list_item_t*)gpu_task );
            return PARSEC_SUCCESS;
        }
        break;
    case PARSEC_DEV_DATA_ADVICE_WARMUP:
        return PARSEC_ERR_NOT_IMPLEMENTED;
        break;
    default:
        assert(0);
        return PARSEC_ERR_NOT_FOUND;
    }
    return PARSEC_SUCCESS;
}

#if PARSEC_GPU_USE_PRIORITIES

static inline parsec_list_item_t* parsec_push_task_ordered( parsec_list_t* list,
                                                          parsec_list_item_t* elem )
{
    parsec_list_push_sorted(list, elem, parsec_execution_context_priority_comparator);
    return elem;
}
#define PARSEC_PUSH_TASK parsec_push_task_ordered
#else
#define PARSEC_PUSH_TASK parsec_list_push_back
#endif

static parsec_flow_t parsec_level_zero_d2d_complete_flow = {
    .name = "FLOW",
    .flow_flags = PARSEC_FLOW_ACCESS_READ,
    .flow_index = 0,
};

static parsec_task_class_t parsec_level_zero_d2d_complete_tc = {
    .name = "D2D TRANSFER COMPLETE",
    .flags = 0,
    .task_class_id = 0,
    .nb_flows = 1,
    .nb_parameters = 0,
    .nb_locals = 0,
    .dependencies_goal = 0,
    .params = { NULL, },
    .in = { &parsec_level_zero_d2d_complete_flow, NULL },
    .out = { NULL, },
    .priority = NULL,
    .properties = NULL,
    .initial_data = NULL,
    .final_data = NULL,
    .data_affinity = NULL,
    .key_functions = NULL,
    .make_key = NULL,
    .get_datatype = NULL,
    .prepare_input = NULL,
    .incarnations = NULL,
    .prepare_output = NULL,
    .find_deps = NULL,
    .iterate_successors = NULL,
    .iterate_predecessors = NULL,
    .release_deps = NULL,
    .complete_execution = NULL,
    .new_task = NULL,
    .release_task = NULL,
    .fini = NULL
};

static void
parsec_gpu_send_transfercomplete_cmd_to_device(parsec_data_copy_t *copy,
                                               parsec_device_module_t *current_dev,
                                               parsec_device_module_t *dst_dev)
{
    parsec_gpu_task_t* gpu_task = NULL;
    gpu_task = (parsec_gpu_task_t*)calloc(1, sizeof(parsec_gpu_task_t));
    gpu_task->task_type = GPU_TASK_TYPE_D2D_COMPLETE;
    gpu_task->ec = calloc(1, sizeof(parsec_task_t));
    PARSEC_OBJ_CONSTRUCT(gpu_task->ec, parsec_task_t);
    gpu_task->ec->task_class = &parsec_level_zero_d2d_complete_tc;
    gpu_task->flow[0] = &parsec_level_zero_d2d_complete_flow;
    gpu_task->flow_nb_elts[0] = copy->original->nb_elts;
    gpu_task->stage_in  = parsec_default_level_zero_stage_in;
    gpu_task->stage_out = parsec_default_level_zero_stage_out;
    gpu_task->ec->data[0].data_in = copy;  /* We need to set not-null in data_in, so that the fake flow is
                                            * not ignored when poping the data from the fake task */ 
    gpu_task->ec->data[0].data_out = copy; /* We "free" data[i].data_out if its readers reaches 0 */
    gpu_task->ec->data[0].source_repo_entry = NULL;
    gpu_task->ec->data[0].source_repo = NULL;
#if defined(PARSEC_PROF_TRACE)
    gpu_task->prof_key_end = -1; /* D2D complete tasks are pure internal management, we do not trace them */
#endif
    (void)current_dev;
    PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                         "GPU[%s]: data copy %p [ref_count %d] D2D transfer is complete, sending order to count it "
                         "to LEVEL_ZERO Device %s",
                         current_dev->name, gpu_task->ec->data[0].data_out,
                         gpu_task->ec->data[0].data_out->super.super.obj_reference_count,
                         dst_dev->name);
    parsec_fifo_push( &(((parsec_device_gpu_module_t*)dst_dev)->pending), (parsec_list_item_t*)gpu_task );
}

static int
parsec_gpu_callback_complete_push(parsec_device_gpu_module_t   *gpu_device,
                                  parsec_gpu_task_t           **gpu_task,
                                  parsec_gpu_exec_stream_t     *gpu_stream)
{
    (void)gpu_stream;

    parsec_gpu_task_t *gtask = *gpu_task;
    parsec_task_t *task;
    int32_t i;
#if defined(PARSEC_DEBUG_NOISIER)
    char task_str[MAX_TASK_STRLEN];
    char task_str2[MAX_TASK_STRLEN];
#endif
    const parsec_flow_t        *flow;
    /**
     * Even though level_zero event return success, the PUSH may not be
     * completed if no PUSH is required by this task and the PUSH is
     * actually done by another task, so we need to check if the data is
     * actually ready to use
     */
    assert(gpu_stream == gpu_device->exec_stream[0]);
    task = gtask->ec;
    PARSEC_DEBUG_VERBOSE(19, parsec_gpu_output_stream,
                         "GPU[%s]: parsec_gpu_callback_complete_push, PUSH of %s",
                         gpu_device->super.name, parsec_task_snprintf(task_str, MAX_TASK_STRLEN, task));

    for( i = 0; i < task->task_class->nb_flows; i++ ) {
        /* Make sure data_in is not NULL */
        if( NULL == task->data[i].data_in ) continue;
        /* We also don't push back non-parsec-owned copies */
        if(NULL != task->data[i].data_out &&
           0 == (task->data[i].data_out->flags & PARSEC_DATA_FLAG_PARSEC_OWNED)) continue;

        flow = gtask->flow[i];
        assert( flow );
        assert( flow->flow_index == i );
        if(PARSEC_FLOW_ACCESS_NONE == (PARSEC_FLOW_ACCESS_MASK & flow->flow_flags)) continue;
        if(task->data[i].data_out->push_task == task ) {   /* only the task who did this PUSH can modify the status */
            parsec_atomic_lock(&task->data[i].data_out->original->lock);
            task->data[i].data_out->data_transfer_status = PARSEC_DATA_STATUS_COMPLETE_TRANSFER;
            parsec_data_end_transfer_ownership_to_copy(task->data[i].data_out->original,
                                                       gpu_device->super.device_index,
                                                       flow->flow_flags);
#if defined(PARSEC_PROF_TRACE)
            if(parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_DATA_IN) {
                PARSEC_PROFILING_TRACE(gpu_stream->profiling,
                                       parsec_gpu_movein_key_end,
                                       (int64_t)(int64_t)task->data[i].data_out->device_private,
                                       level_zero_device->level_zero_index,
                                       NULL);
            }
#endif
            task->data[i].data_out->push_task = NULL;
            parsec_atomic_unlock(&task->data[i].data_out->original->lock);
            parsec_device_gpu_module_t *src_device =
                    (parsec_device_gpu_module_t*)parsec_mca_device_get( task->data[i].data_in->device_index );
            if( PARSEC_DEV_LEVEL_ZERO == src_device->super.type ) {
                int om;
                while(1) {
                    /* There are two ways out:
                     *   either we exit with om = 0, and then nobody was managing src_device,
                     *   and nobody can start managing src_device until we make it change from -1 to 0
                     *   (but anybody who has work to do will wait until that happens), or
                     *   we exit with om > 0, then there is a manager for that thread, and we have
                     *   increased mutex to warn the manager that there is another task for it to do.
                     */
                    om = src_device->mutex;
                    if(om == 0) {
                        /* Nobody at the door, let's try to lock the door */
                        if( parsec_atomic_cas_int32(&src_device->mutex, 0, -1) )
                            break;
                        continue;
                    }
                    if(om < 0 ) {
                        /* Damn, another thread is also trying to do an atomic operation on src_device,
                         * we give it some time and try again */
                        struct timespec delay;
                        delay.tv_nsec = 100;
                        delay.tv_sec = 0;
                        nanosleep(&delay, NULL);
                        continue;
                    }
                    /* There is a manager, let's try to reserve another task to do.
                     * If that fails, the manager may have leaved, try a gain. */
                    if( parsec_atomic_cas_int32(&src_device->mutex, om, om+1) )
                        break;
                }
                if( 0 == om ) {
                    int rc;
                    /* Nobody is at the door to handle that event on the source of that data...
                     * we do the command directly */
                    parsec_atomic_lock( &task->data[i].data_in->original->lock );
                    task->data[i].data_in->readers--;
                    PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                         "GPU[%s]:\tExecuting D2D transfer complete for copy %p [ref_count %d] for "
                                         "device %s -- readers now %d",
                                         gpu_device->super.name, task->data[i].data_in,
                                         task->data[i].data_in->super.super.obj_reference_count, src_device->super.name,
                                         task->data[i].data_in->readers);
                    assert(task->data[i].data_in->readers >= 0);
                    if(0 == task->data[i].data_in->readers) {
                        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                             "GPU[%s]:\tMake read-only copy %p [ref_count %d] available",
                                             gpu_device->super.name, task->data[i].data_in,
                                             task->data[i].data_in->super.super.obj_reference_count);
                        parsec_list_item_ring_chop((parsec_list_item_t*)task->data[i].data_in);
                        PARSEC_LIST_ITEM_SINGLETON(task->data[i].data_in);
                        parsec_list_push_back(&src_device->gpu_mem_lru, (parsec_list_item_t*)task->data[i].data_in);
                        src_device->data_avail_epoch++;
                    }
                    parsec_atomic_unlock( &task->data[i].data_in->original->lock );
                    /* Notify any waiting thread that we're done messing with that device structure */
                    rc = parsec_atomic_cas_int32(&src_device->mutex, -1, 0); (void)rc;
                    assert(rc);
                } else {
                    PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                         "GPU[%s]:\tSending D2D transfer complete command to %s for copy %p "
                                         "[ref_count %d] -- readers is still %d",
                                         gpu_device->super.name, src_device->super.name, task->data[i].data_in,
                                         task->data[i].data_in->super.super.obj_reference_count, task->data[i].data_in->readers);
                    parsec_gpu_send_transfercomplete_cmd_to_device(task->data[i].data_in,
                                                                   (parsec_device_module_t*)gpu_device,
                                                                   (parsec_device_module_t*)src_device);
                }
            }
            continue;
        }
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s]:\tparsec_gpu_callback_complete_push, PUSH of %s: task->data[%d].data_out = %p [ref_count = %d], and push_task is %s, %s because transfer_status is %d",
                             gpu_device->super.name, parsec_task_snprintf(task_str, MAX_TASK_STRLEN, task),
                             i, task->data[i].data_out, task->data[i].data_out->super.super.obj_reference_count,
                             (NULL != task->data[i].data_out->push_task) ? parsec_task_snprintf(task_str2, MAX_TASK_STRLEN, task->data[i].data_out->push_task) : "(null)",
                             (task->data[i].data_out->data_transfer_status == PARSEC_DATA_STATUS_COMPLETE_TRANSFER) ? "all is good" : "Assertion",
                             task->data[i].data_out->data_transfer_status);
        assert(task->data[i].data_out->data_transfer_status == PARSEC_DATA_STATUS_COMPLETE_TRANSFER);
        if( task->data[i].data_out->data_transfer_status != PARSEC_DATA_STATUS_COMPLETE_TRANSFER ) {  /* data is not ready */
            /**
             * As long as we have only one stream to push the data on the GPU we should never
             * end up in this case. Remove previous assert if changed.
             */
            return -1;
        }
    }
    gtask->complete_stage = NULL;

    if( GPU_TASK_TYPE_PREFETCH == gtask->task_type ) {
        parsec_data_copy_t *gpu_copy = task->data[0].data_out;
#if defined(PARSEC_DEBUG_NOISIER)
        char tmp[MAX_TASK_STRLEN];
        assert(NULL != gpu_copy);
        if( NULL != gpu_copy->original->dc )
            gpu_copy->original->dc->key_to_string(gpu_copy->original->dc, gpu_copy->original->key, tmp, MAX_TASK_STRLEN);
        else
            snprintf(tmp, MAX_TASK_STRLEN, "unbound data");
#endif
        PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                             "GPU[%s]:\tPrefetch for data copy %p [ref_count %d] (%s) done. readers = %d, device_index = %d, version = %d, flags = %d, state = %d, data_transfer_status = %d",
                             gpu_device->super.name, gpu_copy, gpu_copy->super.super.obj_reference_count,
                             tmp,
                             gpu_copy->readers, gpu_copy->device_index, gpu_copy->version,
                             gpu_copy->flags, gpu_copy->coherency_state, gpu_copy->data_transfer_status);
        gpu_copy->readers--;
        if( 0 == gpu_copy->readers ) {
            parsec_list_item_ring_chop((parsec_list_item_t*)gpu_copy);
            PARSEC_LIST_ITEM_SINGLETON(gpu_copy);
            PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                                 "GPU[%s]:\tMake copy %p [ref_count %d] available after prefetch from gpu_task %p, ec %p",
                                 gpu_device->super.name, gpu_copy, gpu_copy->super.super.obj_reference_count, gtask, gtask->ec);
            parsec_list_push_back(&gpu_device->gpu_mem_lru, (parsec_list_item_t*)gpu_copy);
        }
        return parsec_level_zero_destroy_task(gpu_device, gpu_task);
    }

    return 0;
}

/**
 * This function tries to progress a stream, by picking up a ready task
 * and applying the progress function. The task to be progresses is
 * always the highest priority in the waiting queue, even when a task
 * has been specified as an input argument.
 * The progress function is either specified by the caller via the
 * upstream_progress_fct input argument or by the next task to be progresses
 * via the submit function associated with the task. In any case, this
 * function progresses a single task, which is then returned as the
 * out_task parameter.
 *
 * Beware: this function does not generate errors by itself, instead
 * it propagates upward the return code of the progress function.
 * However, by convention the error code follows the parsec_hook_return_e
 * enum.
 */
static inline int
progress_stream( parsec_device_gpu_module_t* gpu_device,
                 parsec_gpu_exec_stream_t* stream,
                 parsec_advance_task_function_t upstream_progress_fct,
                 parsec_gpu_task_t* task,
                 parsec_gpu_task_t** out_task )
{
    parsec_advance_task_function_t progress_fct;
    int saved_rc = 0, rc;
#if defined(PARSEC_DEBUG_NOISIER)
    char task_str[MAX_TASK_STRLEN];
#endif
    parsec_level_zero_exec_stream_t *level_zero_stream = (parsec_level_zero_exec_stream_t *)stream;

    /* We always handle the tasks in order. Thus if we got a new task, add it to the
     * local list (possibly by reordering the list). Also, as we can return a single
     * task first try to see if anything completed. */
    if( NULL != task ) {
        PARSEC_PUSH_TASK(stream->fifo_pending, (parsec_list_item_t*)task);
        task = NULL;
    }
    *out_task = NULL;
    progress_fct = upstream_progress_fct;

    if( NULL != stream->tasks[stream->end] ) {
        rc = zeEventQueryStatus(level_zero_stream->events[stream->end]);
        if( ZE_RESULT_SUCCESS == rc ) {
            /* Save the task for the next step */
            task = *out_task = stream->tasks[stream->end];
            PARSEC_DEBUG_VERBOSE(19, parsec_gpu_output_stream,
                                 "GPU[%s]: Completed %s priority %d on stream %s{%p}",
                                 gpu_device->super.name,
                                 parsec_task_snprintf(task_str, MAX_TASK_STRLEN, task->ec),
                                 task->ec->priority, stream->name, (void*)stream);
            stream->tasks[stream->end]    = NULL;
            stream->end = (stream->end + 1) % stream->max_events;

#if defined(PARSEC_PROF_TRACE)
            if( stream->prof_event_track_enable ) {
                if( task->prof_key_end != -1 ) {
                    PARSEC_PROFILING_TRACE(stream->profiling, task->prof_key_end, task->prof_event_id, task->prof_tp_id, NULL);
                } 
            } 
#endif /* (PARSEC_PROF_TRACE) */

            rc = PARSEC_HOOK_RETURN_DONE;
            if (task->complete_stage)
                rc = task->complete_stage(gpu_device, out_task, stream);
            /* the task can be withdrawn by the system */
            return rc;
        }
        if( ZE_RESULT_NOT_READY != rc ) {
            PARSEC_LEVEL_ZERO_CHECK_ERROR( "(progress_stream) zeEventQueryStatus ", rc,
                                     {return PARSEC_HOOK_RETURN_AGAIN;} );
        }
    }

 grab_a_task:
    if( NULL == stream->tasks[stream->start] ) {  /* there is room on the stream */
        task = (parsec_gpu_task_t*)parsec_list_pop_front(stream->fifo_pending);  /* get the best task */
    }
    if( NULL == task ) {  /* No tasks, we're done */
        return saved_rc;
    }
    PARSEC_LIST_ITEM_SINGLETON((parsec_list_item_t*)task);

    assert( NULL == stream->tasks[stream->start] );
    /**
     * In case the task is succesfully progressed, the corresponding profiling
     * event is triggered.
     */
    if ( NULL == upstream_progress_fct ) {
        /* Grab the submit function */
        progress_fct = task->submit;
#if defined(PARSEC_DEBUG_PARANOID)
        int i;
        const parsec_flow_t *flow;
        for( i = 0; i < task->ec->task_class->nb_flows; i++ ) {
            /* Make sure data_in is not NULL */
            if( NULL == task->ec->data[i].data_in ) continue;

            flow = task->flow[i];
            if(PARSEC_FLOW_ACCESS_NONE == (PARSEC_FLOW_ACCESS_MASK & flow->flow_flags)) continue;
            if( 0 == (task->ec->data[i].data_out->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) continue;
            assert(task->ec->data[i].data_out->data_transfer_status == PARSEC_DATA_STATUS_COMPLETE_TRANSFER);
        }
#endif /* defined(PARSEC_DEBUG_PARANOID) */
    }
    rc = progress_fct( gpu_device, task, stream );
    if( 0 > rc ) {
        if( PARSEC_HOOK_RETURN_AGAIN != rc &&
            PARSEC_HOOK_RETURN_ASYNC != rc ) {
            *out_task = task;
            return rc;
        }

        if( PARSEC_HOOK_RETURN_ASYNC == rc ) {
            PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                 "GPU[%s]: GPU task %p has been removed by the progress function",
                                 gpu_device->super.name, (void*)task);
        } else {
            parsec_fifo_push(stream->fifo_pending, (parsec_list_item_t*)task);
            PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,
                                 "GPU[%s]: Reschedule task %p: no room available on the GPU for data",
                                 gpu_device->super.name, (void*)task->ec);
        }
        *out_task = NULL;
        return PARSEC_HOOK_RETURN_DONE;
    }
    /**
     * Do not skip the level_zero event generation. The problem is that some of the inputs
     * might be in the pipe of being transferred to the GPU. If we activate this task
     * too early, it might get executed before the data is available on the GPU.
     * Obviously, this lead to incorrect results.
     */
    rc = zeCommandListAppendSignalEvent( level_zero_stream->level_zero_cl, level_zero_stream->events[stream->start] );
    assert(ZE_RESULT_SUCCESS == rc);
    ze_result_t ze_rc = zeCommandListClose(level_zero_stream->level_zero_cl);
    PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandListClose ", ze_rc, { } );
    ze_rc = zeCommandQueueExecuteCommandLists(level_zero_stream->level_zero_cq, 1, &level_zero_stream->level_zero_cl, NULL);
    PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandQueueExecuteCommandLists ", ze_rc, { } );
    ze_rc = zeCommandListReset(level_zero_stream->level_zero_cl);
    PARSEC_LEVEL_ZERO_CHECK_ERROR( "zeCommandListReset ", ze_rc, { } );

    stream->tasks[stream->start]    = task;
    stream->start = (stream->start + 1) % stream->max_events;
    PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                         "GPU[%s]: Submitted %s(task %p) priority %d on stream %s{%p}",
                         gpu_device->super.name,
                         task->ec->task_class->name, (void*)task->ec, task->ec->priority,
                         stream->name, (void*)stream);

    task = NULL;
    goto grab_a_task;
}

/**
 *  This function schedule the move of all the data required for a
 *  specific task from the main memory into the GPU memory.
 *
 *  Returns:
 *     a positive number: the number of data to be moved.
 *     -1: data cannot be moved into the GPU.
 *     -2: No more room on the GPU to move this data.
 */
static int
parsec_gpu_kernel_push( parsec_device_gpu_module_t      *gpu_device,
                        parsec_gpu_task_t               *gpu_task,
                        parsec_gpu_exec_stream_t        *gpu_stream)
{
    parsec_device_level_zero_module_t *level_zero_device = (parsec_device_level_zero_module_t*)gpu_device;
    parsec_task_t *this_task = gpu_task->ec;
    const parsec_flow_t *flow;
    int i, ret = 0;
#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[MAX_TASK_STRLEN];
#endif

#if 0
    if( gpu_task->last_data_check_epoch == gpu_device->data_avail_epoch )
        return PARSEC_HOOK_RETURN_AGAIN;
#endif
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                         "GPU[%s]: Try to Push %s",
                         gpu_device->super.name,
                         parsec_gpu_describe_gpu_task(tmp, MAX_TASK_STRLEN, gpu_task) );

    if( GPU_TASK_TYPE_PREFETCH == gpu_task->task_type ) {
        if( NULL == gpu_task->ec->data[0].data_in->original ) {
            /* The PREFETCH order comes after the copy was detached and released, ignore it */
            PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                                 "GPU[%s]: %s has been released already, destroying prefetch request",
                                 gpu_device->super.name,
                                 parsec_gpu_describe_gpu_task(tmp, MAX_TASK_STRLEN, gpu_task));
            parsec_level_zero_destroy_task(gpu_device, &gpu_task);
            return PARSEC_HOOK_RETURN_ASYNC;
        }
        if( NULL != gpu_task->ec->data[0].data_in->original->device_copies[gpu_device->super.device_index] &&
            gpu_task->ec->data[0].data_in->original->owner_device == gpu_device->super.device_index ) {
            /* There is already a copy of this data in the GPU */
            PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,
                                 "GPU[%s]: %s data_copy at index %d is %p, destroying prefetch request",
                                 gpu_device->super.name,
                                 parsec_gpu_describe_gpu_task(tmp, MAX_TASK_STRLEN, gpu_task),
                                 gpu_device->super.device_index,
                                 gpu_task->ec->data[0].data_in->original->device_copies[gpu_device->super.device_index]);
            parsec_level_zero_destroy_task(gpu_device, &gpu_task);
            return PARSEC_HOOK_RETURN_ASYNC;
        }
    }

    /* Do we have enough available memory on the GPU to hold the input and output data ? */
    ret = parsec_gpu_data_reserve_device_space( level_zero_device, gpu_task );
    if( ret < 0 ) {
        gpu_task->last_data_check_epoch = gpu_device->data_avail_epoch;
        return ret;
    }

    for( i = 0; i < this_task->task_class->nb_flows; i++ ) {

        flow = gpu_task->flow[i];
        /* Skip CTL flows */
        if(PARSEC_FLOW_ACCESS_NONE == (PARSEC_FLOW_ACCESS_MASK & flow->flow_flags)) continue;

        /* Make sure data_in is not NULL */
        if( NULL == this_task->data[i].data_in ) continue;

        /* If there is already a GPU data copy (set by reserve_device_space), and this copy
         * is not parsec-owned, don't stage in */
        if( NULL != this_task->data[i].data_out &&
            (0 == (this_task->data[i].data_out->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) ) continue;

        assert( NULL != parsec_data_copy_get_ptr(this_task->data[i].data_in) );

        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "GPU[%s]:\t\tIN  Data of %s <%x> on GPU",
                             gpu_device->super.name, flow->name,
                             this_task->data[i].data_out->original->key);
        ret = parsec_gpu_data_stage_in( level_zero_device, flow,
                                        &(this_task->data[i]), gpu_task, gpu_stream );
        if( ret < 0 ) {
            return ret;
        }
    }

    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                         "GPU[%s]: Push task %s DONE",
                         gpu_device->super.name,
                         parsec_task_snprintf(tmp, MAX_TASK_STRLEN, this_task) );
    gpu_task->complete_stage = parsec_gpu_callback_complete_push;
#if defined(PARSEC_PROF_TRACE)
    gpu_task->prof_key_end = -1; /* We do not log that event as the completion of this task */
#endif
    return ret;
}

/**
 *  This function schedule the move of all the modified data for a
 *  specific task from the GPU memory into the main memory.
 *
 *  Returns: negative number if any error occured.
 *           positive: the number of data to be moved.
 */
static int
parsec_gpu_kernel_pop( parsec_device_gpu_module_t   *gpu_device,
                       parsec_gpu_task_t            *gpu_task,
                       parsec_gpu_exec_stream_t     *gpu_stream)
{
    parsec_task_t *this_task = gpu_task->ec;
    parsec_gpu_data_copy_t     *gpu_copy;
    parsec_data_t              *original;
    uint32_t                    nb_elts;
    const parsec_flow_t        *flow;
    int return_code = 0, how_many = 0, i, update_data_epoch = 0;
#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[MAX_TASK_STRLEN];
#endif

    if (gpu_task->task_type == GPU_TASK_TYPE_D2HTRANSFER) {
        for( i = 0; i < this_task->locals[0].value; i++ ) {
            gpu_copy = this_task->data[i].data_out;
            /* If the gpu copy is not owned by parsec, we don't manage it at all */
            if( 0 == (gpu_copy->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) continue;
            original = gpu_copy->original;
            if(PARSEC_SUCCESS != gpu_task->stage_out(gpu_task, (1U << i), gpu_stream)){
                parsec_warning( "%s:%d %s", __FILE__, __LINE__,
                                "gpu_task->stage_out from device ");
                parsec_warning("data %s <<%p>> -> <<%p>>\n", this_task->task_class->out[i]->name,
                                gpu_copy->device_private, original->device_copies[0]->device_private);
                return_code = -2;
                goto release_and_return_error;
            }
        }
        return return_code;
    }

    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                        "GPU[%s]: Try to Pop %s",
                        gpu_device->super.name,
                        parsec_task_snprintf(tmp, MAX_TASK_STRLEN, this_task) );

    for( i = 0; i < this_task->task_class->nb_flows; i++ ) {
        /* We need to manage all data that has been used as input, even if they were read only */

        /* Make sure data_in is not NULL */
        if( NULL == this_task->data[i].data_in ) continue;

        flow = gpu_task->flow[i];
        if( PARSEC_FLOW_ACCESS_NONE == (PARSEC_FLOW_ACCESS_MASK & flow->flow_flags) )  continue;  /* control flow */

        gpu_copy = this_task->data[i].data_out;

        /* If the gpu copy is not owned by parsec, we don't manage it at all */
        if( 0 == (gpu_copy->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) continue;

        original = gpu_copy->original;
        nb_elts = gpu_task->flow_nb_elts[i];

        assert( this_task->data[i].data_in == NULL || original == this_task->data[i].data_in->original );

        if( !(flow->flow_flags & PARSEC_FLOW_ACCESS_WRITE) ) {
            /* Do not propagate GPU copies to successors (temporary solution) */
            this_task->data[i].data_out = original->device_copies[0];
        }
        parsec_atomic_lock(&original->lock);
        if( flow->flow_flags & PARSEC_FLOW_ACCESS_READ ) {
            gpu_copy->readers--;
            if( gpu_copy->readers < 0 ) {
                PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                     "GPU[%s]: While trying to Pop %s, gpu_copy %p [ref_count %d] on flow %d with original %p had already 0 readers",
                                     gpu_device->super.name,
                                     parsec_task_snprintf(tmp, MAX_TASK_STRLEN, this_task),
                                     gpu_copy, gpu_copy->super.super.obj_reference_count,
                                     i, original);

            }
            assert(gpu_copy->readers >= 0);
            if( (0 == gpu_copy->readers) &&
                !(flow->flow_flags & PARSEC_FLOW_ACCESS_WRITE) ) {
                PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                     "GPU[%s]:\tMake read-only copy %p [ref_count %d] available on flow %s",
                                     gpu_device->super.name, gpu_copy, gpu_copy->super.super.obj_reference_count, flow->name);
                parsec_list_item_ring_chop((parsec_list_item_t*)gpu_copy);
                PARSEC_LIST_ITEM_SINGLETON(gpu_copy); /* TODO: singleton instead? */
                parsec_list_push_back(&gpu_device->gpu_mem_lru, (parsec_list_item_t*)gpu_copy);
                update_data_epoch = 1;
                parsec_atomic_unlock(&original->lock);
                continue;  /* done with this element, go for the next one */
            }
            PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                 "GPU[%s]:\tread copy %p [ref_count %d] on flow %s has readers (%i)",
                                 gpu_device->super.name, gpu_copy, gpu_copy->super.super.obj_reference_count, flow->name, gpu_copy->readers);
        }
        if( flow->flow_flags & PARSEC_FLOW_ACCESS_WRITE ) {
            assert( gpu_copy == parsec_data_get_copy(gpu_copy->original, gpu_device->super.device_index) );

            PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                "GPU[%s]:\tOUT Data copy %p [ref_count %d] for flow %s",
                                 gpu_device->super.name, gpu_copy, gpu_copy->super.super.obj_reference_count, flow->name);

            /* Stage the transfer of the data back to main memory */
            gpu_device->super.required_data_out += nb_elts;
            assert( ((parsec_list_item_t*)gpu_copy)->list_next == (parsec_list_item_t*)gpu_copy );
            assert( ((parsec_list_item_t*)gpu_copy)->list_prev == (parsec_list_item_t*)gpu_copy );

            assert( PARSEC_DATA_COHERENCY_OWNED == gpu_copy->coherency_state );
            if( gpu_task->pushout & (1 << i) ) {
                /* TODO: make sure no readers are working on the CPU version */
                original = gpu_copy->original;
                PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                                    "GPU[%s]:\tMove D2H data <%s:%x> copy %p [ref_count %d] -- D:%p -> H:%p requested",
                                     gpu_device->super.name, flow->name, original->key, gpu_copy, gpu_copy->super.super.obj_reference_count,
                                     (void*)gpu_copy->device_private, original->device_copies[0]->device_private);
#if defined(PARSEC_PROF_TRACE)
                if( gpu_stream->prof_event_track_enable ) {
                    if(parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_DATA_OUT) {
                        parsec_profile_data_collection_info_t info;
                        if( NULL != original->dc ) {
                            info.desc    = original->dc;
                            info.data_id = original->key;
                        } else {
                            info.desc    = (parsec_dc_t*)original;
                            info.data_id = -1;
                        }
                        gpu_task->prof_key_end = parsec_gpu_moveout_key_end;
                        gpu_task->prof_tp_id   = this_task->taskpool->taskpool_id;
                        gpu_task->prof_event_id = this_task->task_class->key_functions->key_hash(this_task->task_class->make_key(this_task->taskpool, this_task->locals), NULL);
                        PARSEC_PROFILING_TRACE(gpu_stream->profiling,
                                               parsec_gpu_moveout_key_start,
                                               gpu_task->prof_event_id,
                                               gpu_task->prof_tp_id,
                                               &info);
                    } else {
                        gpu_task->prof_key_end = -1;
                    }
                }
#endif
                /* Move the data back into main memory */
                if( PARSEC_SUCCESS != gpu_task->stage_out(gpu_task, (1U << flow->flow_index), gpu_stream)){
                    parsec_warning( "%s:%d %s", __FILE__, __LINE__,
                                    "gpu_task->stage_out from device ");
                    parsec_warning("data %s <<%p>> -> <<%p>>\n", this_task->task_class->out[i]->name,
                                   gpu_copy->device_private, original->device_copies[0]->device_private);
                    return_code = -2;
                    parsec_atomic_unlock(&original->lock);
                    goto release_and_return_error;
                }
                gpu_device->super.data_out_to_host += nb_elts; /* TODO: not hardcoded, use datatype size */
                how_many++;
            } else {
                assert( 0 == gpu_copy->readers );
            }
        }
        parsec_atomic_unlock(&original->lock);
    }

  release_and_return_error:
    if( update_data_epoch ) {
        gpu_device->data_avail_epoch++;
    }
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                         "GPU[%s]: Pop %s DONE (return %d data epoch %"PRIu64")",
                         gpu_device->super.name,
                         parsec_task_snprintf(tmp, MAX_TASK_STRLEN, this_task), return_code, gpu_device->data_avail_epoch );

    return (return_code < 0 ? return_code : how_many);
}

/**
 * Make sure all data on the device is correctly put back into the queues.
 */
static int
parsec_gpu_kernel_epilog( parsec_device_gpu_module_t *gpu_device,
                          parsec_gpu_task_t          *gpu_task )
{
    parsec_task_t *this_task = gpu_task->ec;
    parsec_gpu_data_copy_t     *gpu_copy, *cpu_copy;
    parsec_data_t              *original;
    int i;

#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[MAX_TASK_STRLEN];
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                         "GPU[%s]: Epilog of %s",
                         gpu_device->super.name,
                         parsec_task_snprintf(tmp, MAX_TASK_STRLEN, this_task) );
#endif

    for( i = 0; i < this_task->task_class->nb_flows; i++ ) {
        /* Make sure data_in is not NULL */
        if( NULL == this_task->data[i].data_in ) continue;

        /* Don't bother if there is no real data (aka. CTL or no output) */
        if(NULL == this_task->data[i].data_out) continue;


        if( !(gpu_task->flow[i]->flow_flags & PARSEC_FLOW_ACCESS_WRITE) ) {
            /* Warning data_out for read only flows has been overwritten in pop */
            continue;
        }

        gpu_copy = this_task->data[i].data_out;
        original = gpu_copy->original;
        cpu_copy = original->device_copies[0];

        /* If it is a copy managed by the user, don't bother either */
        if( 0 == (gpu_copy->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) continue;
        
        /**
         * There might be a race condition here. We can't assume the first CPU
         * version is the corresponding CPU copy, as a new CPU-bound data
         * might have been created meanwhile.
         *
         * WARNING: For now we always forward the cpu_copy to the next task, to
         * do that, we lie to the engine by updating the CPU copy to the same
         * status than the GPU copy without updating the data itself. Thus, the
         * cpu copy is really invalid. this is related to Issue #88, and the
         * fact that:
         *      - we don't forward the gpu copy as output
         *      - we always take a cpu copy as input, so it has to be in the
         *        same state as the GPU to prevent an extra data movement.
         */
        assert( PARSEC_DATA_COHERENCY_OWNED == gpu_copy->coherency_state );
        gpu_copy->coherency_state = PARSEC_DATA_COHERENCY_SHARED;
        cpu_copy->coherency_state = PARSEC_DATA_COHERENCY_SHARED;

        /**
         *  The cpu_copy will be updated in the completion, and at that moment
         *  the two versions will be identical.
         */
        cpu_copy->version = gpu_copy->version;
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                             "GPU[%s]: CPU copy %p [ref_count %d] gets the same version %d as GPU copy %p [ref_count %d] at %s:%d",
                             gpu_device->super.name,
                             cpu_copy, cpu_copy->super.super.obj_reference_count, cpu_copy->version, gpu_copy, gpu_copy->super.super.obj_reference_count,
                             __FILE__, __LINE__);

        /**
         * Let's lie to the engine by reporting that working version of this
         * data (aka. the one that GEMM worked on) is now on the CPU.
         */
        this_task->data[i].data_out = cpu_copy;

        assert( 0 == gpu_copy->readers );

        if( gpu_task->pushout & (1 << i) ) {
            PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                 "LEVEL_ZERO copy %p [ref_count %d] moved to the read LRU in %s",
                                 gpu_copy, gpu_copy->super.super.obj_reference_count, __func__);
            parsec_list_item_ring_chop((parsec_list_item_t*)gpu_copy);
            PARSEC_LIST_ITEM_SINGLETON(gpu_copy);
            parsec_list_push_back(&gpu_device->gpu_mem_lru, (parsec_list_item_t*)gpu_copy);
        } else {
            PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                                 "LEVEL_ZERO copy %p [ref_count %d] moved to the owned LRU in %s",
                                 gpu_copy, gpu_copy->super.super.obj_reference_count, __func__);
            parsec_list_push_back(&gpu_device->gpu_mem_owned_lru, (parsec_list_item_t*)gpu_copy);
        }
    }
    return 0;
}

/** @brief Release the LEVEL_ZERO copies of the data used in WRITE mode.
 *
 * @details This function can be used when the LEVEL_ZERO task didn't run
 *          to completion on the device (either due to an error, or
 *          simply because the body requested a reexecution on a
 *          different location). It releases the LEVEL_ZERO copies of the
 *          output data, allowing them to be reused by the runtime.
 *          This function has the drawback of kicking in too late,
 *          after all data transfers have been completed toward the
 *          device.
 *
 * @param [IN] gpu_device, the GPU device the the task has been
 *             supposed to execute.
 * @param [IN] gpu_task, the task that has been cancelled, and which
 *             needs it's data returned to the runtime.
 * @return Currently only success.
 */
static int
parsec_gpu_kernel_cleanout( parsec_device_gpu_module_t *gpu_device,
                            parsec_gpu_task_t          *gpu_task )
{
    parsec_task_t *this_task = gpu_task->ec;
    parsec_gpu_data_copy_t     *gpu_copy, *cpu_copy;
    parsec_data_t              *original;
    int i, data_avail_epoch = 0;

#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[MAX_TASK_STRLEN];
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                        "GPU[%s]: Cleanup of %s",
                        gpu_device->super.name,
                        parsec_task_snprintf(tmp, MAX_TASK_STRLEN, this_task) );
#endif

    for( i = 0; i < this_task->task_class->nb_flows; i++ ) {
        /* Make sure data_in is not NULL */
        if( NULL == this_task->data[i].data_in ) continue;

        /* Don't bother if there is no real data (aka. CTL or no output) */
        if(NULL == this_task->data[i].data_out) continue;
        if( !(gpu_task->flow[i]->flow_flags & PARSEC_FLOW_ACCESS_WRITE) ) {
            /* Warning data_out for read only flows has been overwritten in pop */
            continue;
        }

        gpu_copy = this_task->data[i].data_out;
        original = gpu_copy->original;
        parsec_atomic_lock(&original->lock);
        assert(gpu_copy->super.super.obj_reference_count > 1);
        /* Issue #134 */
        parsec_data_copy_detach(original, gpu_copy, gpu_device->super.device_index);
        gpu_copy->coherency_state = PARSEC_DATA_COHERENCY_SHARED;
        cpu_copy = original->device_copies[0];

        /**
         * Let's lie to the engine by reporting that working version of this
         * data (aka. the one that GEMM worked on) is now on the CPU.
         */
        this_task->data[i].data_out = cpu_copy;
        if( 0 != (gpu_copy->flags & PARSEC_DATA_FLAG_PARSEC_OWNED) ) {
            parsec_list_push_back(&gpu_device->gpu_mem_lru, (parsec_list_item_t*)gpu_copy);
        }
        parsec_atomic_unlock(&original->lock);
        data_avail_epoch++;
        PARSEC_DEBUG_VERBOSE(20, parsec_gpu_output_stream,
                             "LEVEL_ZERO copy %p [ref_count %d] moved to the read LRU in %s\n",
                             gpu_copy, gpu_copy->super.super.obj_reference_count, __func__);
    }
    if( data_avail_epoch )  /* Update data availability epoch */
        gpu_device->data_avail_epoch++;
    return 0;
}

/**
 * This version is based on 4 streams: one for transfers from the memory to
 * the GPU, 2 for kernel executions and one for transfers from the GPU into
 * the main memory. The synchronization on each stream is based on LEVEL_ZERO events,
 * such an event indicate that a specific epoch of the lifetime of a task has
 * been completed. Each type of stream (in, exec and out) has a pending FIFO,
 * where tasks ready to jump to the respective step are waiting.
 */
parsec_hook_return_t
parsec_level_zero_kernel_scheduler( parsec_execution_stream_t *es,
                                    parsec_gpu_task_t         *gpu_task,
                                    int which_gpu )
{
    parsec_device_gpu_module_t* gpu_device;
    int rc, exec_stream = 0;
    parsec_gpu_task_t *progress_task, *out_task_submit = NULL, *out_task_pop = NULL;
#if defined(PARSEC_DEBUG_NOISIER)
    char tmp[MAX_TASK_STRLEN];
#endif
    int pop_null = 0;

    gpu_device = (parsec_device_gpu_module_t*)parsec_mca_device_get(which_gpu);

#if defined(PARSEC_PROF_TRACE)
    PARSEC_PROFILING_TRACE_FLAGS( es->es_profile,
                                  PARSEC_PROF_FUNC_KEY_END(gpu_task->ec->taskpool,
                                                           gpu_task->ec->task_class->task_class_id),
                                  gpu_task->ec->task_class->key_functions->key_hash(gpu_task->ec->task_class->make_key(gpu_task->ec->taskpool, gpu_task->ec->locals), NULL),
                                  gpu_task->ec->taskpool->taskpool_id, NULL,
                                  PARSEC_PROFILING_EVENT_RESCHEDULED );
#endif /* defined(PARSEC_PROF_TRACE) */

    /* Check the GPU status -- three kinds of values for rc:
     *   - rc < 0: somebody is doing a short atomic operation while there is no manager,
     *             so wait.
     *   - rc == 0: there is no manager, and at the exit of the while, this thread
     *             made rc go from 0 to 1, so it is the new manager of the GPU and
     *             needs to deal with gpu_task
     *   - rc > 0: there is a manager, and at the exit of the while, this thread has
     *             committed new work that the manager will need to do, but the work is
     *             not in the queue yet.
     */
    while(1) {
        rc = gpu_device->mutex;
        struct timespec delay;
        if( rc >= 0 ) {
            if( parsec_atomic_cas_int32( &gpu_device->mutex, rc, rc+1 ) ) {
                break;
            }
        } else {
            delay.tv_nsec = 100;
            delay.tv_sec = 0;
            nanosleep(&delay, NULL);
        }
    }
    if( 0 < rc ) {
        parsec_fifo_push( &(gpu_device->pending), (parsec_list_item_t*)gpu_task );
        return PARSEC_HOOK_RETURN_ASYNC;
    }
    PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,"GPU[%s]: Entering GPU management at %s:%d",
                         gpu_device->super.name, __FILE__, __LINE__);

#if defined(PARSEC_PROF_TRACE)
    if( parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_OWN )
        PARSEC_PROFILING_TRACE( es->es_profile, parsec_gpu_own_GPU_key_start,
                                (unsigned long)es, PROFILE_OBJECT_ID_NULL, NULL );
#endif  /* defined(PARSEC_PROF_TRACE) */

 check_in_deps:
    if( NULL != gpu_task ) {
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,
                             "GPU[%s]:\tUpload data (if any) for %s priority %d",
                             gpu_device->super.name,
                             parsec_gpu_describe_gpu_task(tmp, MAX_TASK_STRLEN, gpu_task),
                             gpu_task->ec->priority );
    }
    rc = progress_stream( gpu_device,
                          gpu_device->exec_stream[0],
                          parsec_gpu_kernel_push,
                          gpu_task, &progress_task );
    if( rc < 0 ) {  /* In case of error progress_task is the task that raised it */
        if( -1 == rc )
            goto disable_gpu;
        /* We are in the early stages, and if there no room on the GPU for a task we need to
         * delay all retries for the same task for a little while. Meanwhile, put the task back
         * trigger a device flush, and keep executing tasks that have their data on the device.
         */
        if( NULL != progress_task ) {
            PARSEC_PUSH_TASK(gpu_device->exec_stream[0]->fifo_pending, (parsec_list_item_t*)progress_task);
            progress_task = NULL;
        }
        /* If we can extract data go for it, otherwise try to drain the pending tasks */
        gpu_task = parsec_gpu_create_w2r_task(gpu_device, es);
        if( NULL != gpu_task )
            goto get_data_out_of_device;
    }
    gpu_task = progress_task;

    /* Stage-in completed for this task: it is ready to be executed */
    exec_stream = (exec_stream + 1) % (gpu_device->max_exec_streams - 2);  /* Choose an exec_stream */
    if( NULL != gpu_task ) {
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,  "GPU[%s]:\tExecute %s priority %d", gpu_device->super.name,
                             parsec_task_snprintf(tmp, MAX_TASK_STRLEN, gpu_task->ec),
                             gpu_task->ec->priority );
    }
    rc = progress_stream( gpu_device,
                          gpu_device->exec_stream[2+exec_stream],
                          NULL,
                          gpu_task, &progress_task );
    if( rc < 0 ) {
        if( PARSEC_HOOK_RETURN_DISABLE == rc )
            goto disable_gpu;
        if( PARSEC_HOOK_RETURN_ASYNC != rc ) {
            /* Reschedule the task. As the chore_id has been modified,
               another incarnation of the task will be executed. */
            if( NULL != progress_task ) {
                parsec_gpu_kernel_cleanout(gpu_device, progress_task);
                __parsec_reschedule(es, progress_task->ec);
                gpu_task = progress_task;
                progress_task = NULL;
                goto remove_gpu_task;
            }
            gpu_task = NULL;
            goto fetch_task_from_shared_queue;
        }
        progress_task = NULL;
    }
    gpu_task = progress_task;
    out_task_submit = progress_task;

 get_data_out_of_device:
    if( NULL != gpu_task ) {  /* This task has completed its execution */
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,  "GPU[%s]:\tRetrieve data (if any) for %s priority %d", gpu_device->super.name,
                            parsec_task_snprintf(tmp, MAX_TASK_STRLEN, gpu_task->ec),
                            gpu_task->ec->priority );
    }
    /* Task is ready to move the data back to main memory */
    rc = progress_stream( gpu_device,
                          gpu_device->exec_stream[1],
                          parsec_gpu_kernel_pop,
                          gpu_task, &progress_task );
    if( rc < 0 ) {
        if( -1 == rc )
            goto disable_gpu;
    }
    if( NULL != progress_task ) {
        /* We have a succesfully completed task. However, it is not gpu_task, as
         * it was just submitted into the data retrieval system. Instead, the task
         * ready to move into the next level is the progress_task.
         */
        gpu_task = progress_task;
        progress_task = NULL;
        goto complete_task;
    }
    gpu_task = progress_task;
    out_task_pop = progress_task;

 fetch_task_from_shared_queue:
    assert( NULL == gpu_task );
    if (1 == parsec_level_zero_sort_pending && out_task_submit == NULL && out_task_pop == NULL) {
        parsec_gpu_sort_pending_list(gpu_device);
    }
    gpu_task = (parsec_gpu_task_t*)parsec_fifo_try_pop( &(gpu_device->pending) );
    if( NULL != gpu_task ) {
        pop_null = 0;
        gpu_task->last_data_check_epoch = gpu_device->data_avail_epoch - 1;  /* force at least one tour */
        PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,  "GPU[%s]:\tGet from shared queue %s priority %d", gpu_device->super.name,
                             parsec_gpu_describe_gpu_task(tmp, MAX_TASK_STRLEN, gpu_task),
                             gpu_task->ec->priority);
        if( GPU_TASK_TYPE_D2D_COMPLETE == gpu_task->task_type ) {
            goto get_data_out_of_device;
        }
    } else {
        pop_null++;
        if( pop_null % 1024 == 1023 ) {
            PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,  "GPU[%s]:\tStill waiting for %d tasks to execute, but poped NULL the last %d times I tried to pop something...",
                                 gpu_device->super.name, gpu_device->mutex, pop_null);
        }
    }
    goto check_in_deps;

 complete_task:
    assert( NULL != gpu_task );
    PARSEC_DEBUG_VERBOSE(10, parsec_gpu_output_stream,  "GPU[%s]:\tComplete %s",
                         gpu_device->super.name,
                         parsec_task_snprintf(tmp, MAX_TASK_STRLEN, gpu_task->ec));
    /* Everything went fine so far, the result is correct and back in the main memory */
    PARSEC_LIST_ITEM_SINGLETON(gpu_task);
    if (gpu_task->task_type == GPU_TASK_TYPE_D2HTRANSFER) {
        parsec_gpu_complete_w2r_task(gpu_device, gpu_task, es);
        gpu_task = progress_task;
        goto fetch_task_from_shared_queue;
    }
    if (gpu_task->task_type == GPU_TASK_TYPE_D2D_COMPLETE) {
        free( gpu_task->ec );
        gpu_task->ec = NULL;
        goto remove_gpu_task;
    }
    parsec_gpu_kernel_epilog( gpu_device, gpu_task );
    __parsec_complete_execution( es, gpu_task->ec );
    gpu_device->super.executed_tasks++;
 remove_gpu_task:
    // Load problem: was parsec_device_load[gpu_device->super.device_index] -= gpu_task->load;
    parsec_device_load[gpu_device->super.device_index] -= parsec_device_sweight[gpu_device->super.device_index];
    PARSEC_DEBUG_VERBOSE(3, parsec_gpu_output_stream,"GPU[%s]: gpu_task %p freed at %s:%d", gpu_device->super.name, 
                         gpu_task, __FILE__, __LINE__);
    free( gpu_task );
    rc = parsec_atomic_fetch_dec_int32( &(gpu_device->mutex) );
    if( 1 == rc ) {  /* I was the last one */
#if defined(PARSEC_PROF_TRACE)
        if( parsec_gpu_trackable_events & PARSEC_PROFILE_LEVEL_ZERO_TRACK_OWN )
            PARSEC_PROFILING_TRACE( es->es_profile, parsec_gpu_own_GPU_key_end,
                                    (unsigned long)es, PROFILE_OBJECT_ID_NULL, NULL );
#endif  /* defined(PARSEC_PROF_TRACE) */
        PARSEC_DEBUG_VERBOSE(2, parsec_gpu_output_stream,"GPU[%s]: Leaving GPU management at %s:%d", 
                             gpu_device->super.name, __FILE__, __LINE__);

        return PARSEC_HOOK_RETURN_ASYNC;
    }
    gpu_task = progress_task;
    goto fetch_task_from_shared_queue;

 disable_gpu:
    /* Something wrong happened. Push all the pending tasks back on the
     * cores, and disable the gpu.
     */
    parsec_warning("Critical issue related to the GPU discovered. Giving up\n");
    return PARSEC_HOOK_RETURN_DISABLE;
}

#endif /* PARSEC_HAVE_LEVEL_ZERO */