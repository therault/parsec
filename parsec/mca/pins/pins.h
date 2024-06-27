/*
 * Copyright (c) 2009-2023 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#ifndef PARSEC_MCA_PINS_H
#define PARSEC_MCA_PINS_H
/* PaRSEC Performance Instrumentation Callback System */

#include "parsec/runtime.h"
#include "parsec/mca/mca.h"

#define PARSEC_PINS_SEPARATOR ";"

struct parsec_pins_next_callback_s;
struct parsec_flow_s;
struct parsec_data_s;

typedef void (*parsec_pins_empty_callback_t)(struct parsec_pins_next_callback_s* cb_data,
                                             struct parsec_execution_stream_s*   es);
typedef void (*parsec_pins_task_callback_t)(struct parsec_pins_next_callback_s* cb_data,
                                            struct parsec_execution_stream_s*   es,
                                            struct parsec_task_s*               task);
typedef void (*parsec_pins_dc_to_dag_callback_t)(struct parsec_pins_next_callback_s* cb_data,
                                                 struct parsec_execution_stream_s*   es,
                                                 const struct parsec_task_s*         task,
                                                 const struct parsec_data_s         *data,
                                                 const struct parsec_flow_s         *flow,
                                                 int                                 direct_reference);
typedef void (*parsec_pins_dag_to_dc_callback_t)(struct parsec_pins_next_callback_s* cb_data,
                                                 struct parsec_execution_stream_s*   es,
                                                 const struct parsec_task_s*         task,
                                                 const struct parsec_data_s         *data,
                                                 const struct parsec_flow_s         *flow);
typedef void (*parsec_pins_dependency_callback_t)(struct parsec_pins_next_callback_s* cb_data,
                                                  struct parsec_execution_stream_s*   es,
                                                  const struct parsec_task_s*         src_task,
                                                  const struct parsec_task_s*         dst_task,
                                                  int                                 activation,
                                                  const struct parsec_flow_s*         src_flow,
                                                  const struct parsec_flow_s*         dst_flow);
typedef union {
    parsec_pins_empty_callback_t      parsec_pins_empty_callback;
    parsec_pins_task_callback_t       parsec_pins_task_callback;
    parsec_pins_dependency_callback_t parsec_pins_dependency_callback;
    parsec_pins_dc_to_dag_callback_t  parsec_pins_dc_to_dag_callback;
    parsec_pins_dag_to_dc_callback_t  parsec_pins_dag_to_dc_callback;
} parsec_pins_callback;

typedef struct parsec_pins_next_callback_s {
    parsec_pins_callback                cb_func;
    struct parsec_pins_next_callback_s* cb_data;
} parsec_pins_next_callback_t;

typedef enum PARSEC_PINS_FLAG {
        SELECT_BEGIN = 0,    // called before scheduler begins looking for an available task
#define SELECT_BEGIN_PINS_FCT_TYPE parsec_pins_empty_callback
        SELECT_END,          // called after scheduler has finished looking for an available task. Parameter: the selected task (or NULL if none found)
#define SELECT_END_PINS_FCT_TYPE parsec_pins_task_callback
        PREPARE_INPUT_BEGIN, // called before the data_lookup() step. Parameter: the task for which data_lookup() is called
#define PREPARE_INPUT_BEGIN_PINS_FCT_TYPE parsec_pins_task_callback
        PREPARE_INPUT_END,   // called after the data_lookup() step. Parameter: the task for which data_lookup() was called
#define PREPARE_INPUT_END_PINS_FCT_TYPE parsec_pins_task_callback
        RELEASE_DEPS_BEGIN,  // called before the release_deps() step. Parameter: the task for which release_deps() is called
#define RELEASE_DEPS_BEGIN_PINS_FCT_TYPE parsec_pins_task_callback
        RELEASE_DEPS_END,    // called after the release_deps() step. Parameter: the task for which release_deps() was called
#define RELEASE_DEPS_END_PINS_FCT_TYPE parsec_pins_task_callback
        ACTIVATE_CB_BEGIN,   // called before handling an active message that sends task activation orders
#define ACTIVATE_CB_BEGIN_PINS_FCT_TYPE parsec_pins_empty_callback
        ACTIVATE_CB_END,     // called after handling an active message that sends task activation orders
#define ACTIVATE_CB_END_PINS_FCT_TYPE parsec_pins_empty_callback
        DATA_FLUSH_BEGIN,    // called before flushing data tracking in the DTD DSL (all currently tracked data)
#define DATA_FLUSH_BEGIN_PINS_FCT_TYPE parsec_pins_empty_callback
        DATA_FLUSH_END,      // called after flushing data tracking in the DTD DSL (all currently tracked data)
#define DATA_FLUSH_END_PINS_FCT_TYPE parsec_pins_empty_callback
        EXEC_BEGIN,          // called before thread executes a task. Parameter: the task that starts executing
#define EXEC_BEGIN_PINS_FCT_TYPE parsec_pins_task_callback
        EXEC_END,            // called after thread executes a task. Parameter: the task that is done executing
#define EXEC_END_PINS_FCT_TYPE parsec_pins_task_callback
        COMPLETE_EXEC_BEGIN, // called at the beginning of the task execution completion (will discover new tasks). Parameter: the task that completes
#define COMPLETE_EXEC_BEGIN_PINS_FCT_TYPE parsec_pins_task_callback
        COMPLETE_EXEC_END,   // called after the task execution completion (all new tasks have been discovered). Parameter: the task that completed
#define COMPLETE_EXEC_END_PINS_FCT_TYPE parsec_pins_task_callback
        SCHEDULE_BEGIN,      // called before scheduling a ring of tasks. Parameter: the ring of tasks to schedule
#define SCHEDULE_BEGIN_PINS_FCT_TYPE parsec_pins_task_callback
        SCHEDULE_END,        // called after scheduling a ring of tasks.
#define SCHEDULE_END_PINS_FCT_TYPE parsec_pins_empty_callback
        THREAD_INIT,         // Provided as an option for modules to run work during thread init without using the MCA module registration system.
#define THREAD_INIT_PINS_FCT_TYPE parsec_pins_empty_callback
        THREAD_FINI,         // Similar to above, for thread finalization.
#define THREAD_FINI_PINS_FCT_TYPE parsec_pins_empty_callback
        TASK_DEPENDENCY,     // Called when a task discovers it provides data (or control) to another task. Parameters:
#define TASK_DEPENDENCY_PINS_FCT_TYPE parsec_pins_dependency_callback
        DATA_COLLECTION_INPUT, // called when a data from a data collection flows in the DAG for the first time. Parameters: task, data, flow, and an int that says it's a direct_reference
#define DATA_COLLECTION_INPUT_PINS_FCT_TYPE parsec_pins_dc_to_dag_callback
        DATA_COLLECTION_OUTPUT, // called when a data flows from the DAG into the data collection. Parameters: task, data, flow
#define DATA_COLLECTION_OUTPUT_PINS_FCT_TYPE parsec_pins_dag_to_dc_callback
    /* PARSEC_PINS_FLAG_COUNT is not an event at all */
    PARSEC_PINS_FLAG_COUNT
} PARSEC_PINS_FLAG;

extern uint64_t parsec_pins_enable_mask;
extern const char *parsec_pins_enable_default_names;

#define PARSEC_PINS_FLAG_MASK(_flag) (1<<(_flag))
#define PARSEC_PINS_FLAG_ENABLED(_flag) (parsec_pins_enable_mask & PARSEC_PINS_FLAG_MASK(_flag))

BEGIN_C_DECLS

/*
 * These functions should see the profiling dictionary and register properties inside
 */
typedef int (*parsec_pins_profiling_register_func_t)(void);

/*
 * Structure for function pointers to register profiling stuff
 */
typedef struct parsec_pins_base_module_profiling_s {
    parsec_pins_profiling_register_func_t     register_properties;
} parsec_pins_base_module_profiling_t;

/*
 * Structures for pins components
 */
struct parsec_pins_base_component_2_0_0 {
    mca_base_component_2_0_0_t        base_version;
    mca_base_component_data_2_0_0_t   base_data;
};

typedef struct parsec_pins_base_component_2_0_0 parsec_pins_base_component_2_0_0_t;
typedef struct parsec_pins_base_component_2_0_0 parsec_pins_base_component_t;

/*
 * Structure for sched modules
 */

/*
 These functions should each be called once at the appropriate lifecycle of the PaRSEC Context
 except that taskpool functions should be called once per taskpool, and thread functions once per thread
 */
typedef void (*parsec_pins_base_module_init_fn_t)(struct parsec_context_s * master);
typedef void (*parsec_pins_base_module_fini_fn_t)(struct parsec_context_s * master);
typedef void (*parsec_pins_base_module_taskpool_init_fn_t)(struct parsec_taskpool_s * tp);
typedef void (*parsec_pins_base_module_taskpool_fini_fn_t)(struct parsec_taskpool_s * tp);
typedef void (*parsec_pins_base_module_thread_init_fn_t)(struct parsec_execution_stream_s * es);
typedef void (*parsec_pins_base_module_thread_fini_fn_t)(struct parsec_execution_stream_s * es);

struct parsec_pins_base_module_1_0_0_t {
    parsec_pins_base_module_init_fn_t          init;
    parsec_pins_base_module_fini_fn_t          fini;
    parsec_pins_base_module_taskpool_init_fn_t taskpool_init;
    parsec_pins_base_module_taskpool_fini_fn_t taskpool_fini;
    parsec_pins_base_module_thread_init_fn_t   thread_init;
    parsec_pins_base_module_thread_fini_fn_t   thread_fini;
};

typedef struct parsec_pins_base_module_1_0_0_t parsec_pins_base_module_1_0_0_t;
typedef struct parsec_pins_base_module_1_0_0_t parsec_pins_base_module_t;

typedef struct {
    const parsec_pins_base_component_t      *component;
    parsec_pins_base_module_t                module;
    parsec_pins_base_module_profiling_t      init_profiling;
} parsec_pins_module_t;

/*
 * Macro for use in components that are of type pins, version 2.0.0
 */
#define PARSEC_PINS_BASE_VERSION_2_0_0           \
    MCA_BASE_VERSION_2_0_0,                     \
        "pins", 2, 0, 0

END_C_DECLS

void parsec_pins_init(struct parsec_context_s * master);
void parsec_pins_fini(struct parsec_context_s * master);
void parsec_pins_taskpool_init(struct parsec_taskpool_s * tp);
void parsec_pins_taskpool_fini(struct parsec_taskpool_s * tp);
void parsec_pins_thread_init(struct parsec_execution_stream_s * es);
void parsec_pins_thread_fini(struct parsec_execution_stream_s * es);

/*
 the following functions are intended for public use wherever they are necessary
 */

void parsec_pins_disable_registration(int disable);

int parsec_pins_is_module_enabled(char * module_name);

int parsec_pins_nb_modules_enabled(void);

int parsec_pins_register_callback(struct parsec_execution_stream_s* es,
                           PARSEC_PINS_FLAG method_flag,
                           parsec_pins_callback cb,
                           parsec_pins_next_callback_t* cb_data);

int parsec_pins_unregister_callback(struct parsec_execution_stream_s* es,
                             PARSEC_PINS_FLAG method_flag,
                             parsec_pins_callback cb,
                             parsec_pins_next_callback_t** cb_data);


PARSEC_PINS_FLAG parsec_pins_name_to_begin_flag(const char *name);

#ifdef PARSEC_PROF_PINS

/* These are just macro helpers to make the preprocessor do what we want */
#define PARSEC_PINS_FIRST_ARG_(N, ...) N
#define PARSEC_PINS_FIRST_ARG(args) PARSEC_PINS_FIRST_ARG_ args
#define PARSEC_PINS_FCT_TYPE(method_flag) method_flag ## _PINS_FCT_TYPE
#define PARSEC_PINS_FCT_TYPE_TO_CALLBACK_(src_type) parsec_pins_make_callback_ ## src_type
#define PARSEC_PINS_FCT_TYPE_TO_CALLBACK(src_type) PARSEC_PINS_FCT_TYPE_TO_CALLBACK_(src_type)
#define PARSEC_PINS_MAKE_FCT_TO_CALLBACK(src_type) \
  static inline parsec_pins_callback PARSEC_PINS_FCT_TYPE_TO_CALLBACK(src_type)(src_type ## _t cb) { \
    parsec_pins_callback ret; \
    ret.src_type = cb; \
    return ret; \
  }
/* We need one such converter per type used, to allow for type-checking */
PARSEC_PINS_MAKE_FCT_TO_CALLBACK(parsec_pins_empty_callback);
PARSEC_PINS_MAKE_FCT_TO_CALLBACK(parsec_pins_task_callback);
PARSEC_PINS_MAKE_FCT_TO_CALLBACK(parsec_pins_dependency_callback);
PARSEC_PINS_MAKE_FCT_TO_CALLBACK(parsec_pins_dc_to_dag_callback);
PARSEC_PINS_MAKE_FCT_TO_CALLBACK(parsec_pins_dag_to_dc_callback);
/* If you do not want to use the enum name but the enum value you need to
 * use this explicit macro and pass the name of the field that describes the
 * type of function expected in the PARSEC_PINS call */
#define PARSEC_PINS_EXPLICIT(_method_flag, _pname, _args...)                             \
    do {                                                                                 \
        if (PARSEC_PINS_FLAG_ENABLED(_method_flag)) {                                    \
            assert( _method_flag < PARSEC_PINS_FLAG_COUNT );                             \
            parsec_execution_stream_t *_es;                                              \
            _es = PARSEC_PINS_FIRST_ARG((_args));                                        \
            parsec_pins_next_callback_t* cb_event = &_es->pins_events_cb[_method_flag];  \
            while( NULL != cb_event->cb_func._pname ) {                                  \
                cb_event->cb_func._pname(cb_event->cb_data, _args);                      \
                cb_event = cb_event->cb_data;                                            \
            }                                                                            \
        }                                                                                \
    } while (0)
/* If you use the enum name to denote a PINS event, you can use this macro directly */
#define PARSEC_PINS(method_flag, args...)              \
    PARSEC_PINS_EXPLICIT(method_flag, PARSEC_PINS_FCT_TYPE(method_flag), args)
#define PARSEC_PINS_DISABLE_REGISTRATION(boolean)      \
    parsec_pins_disable_registration(boolean)
/* Use the enum name when calling PARSEC_PINS_[UN]REGISTER, or create your union to call parsec_pins_[un]register */
#define PARSEC_PINS_REGISTER(unit, method_flag, cb, data)       \
    parsec_pins_register_callback(unit, method_flag, PARSEC_PINS_FCT_TYPE_TO_CALLBACK(PARSEC_PINS_FCT_TYPE(method_flag))( cb ), data)
#define PARSEC_PINS_UNREGISTER(unit, method_flag, cb, data)       \
    parsec_pins_unregister_callback(unit, method_flag, PARSEC_PINS_FCT_TYPE_TO_CALLBACK(PARSEC_PINS_FCT_TYPE(method_flag))( cb ), data)
#define PARSEC_PINS_INIT(master_context)               \
    parsec_pins_init(master_context)
#define PARSEC_PINS_FINI(master_context)               \
    parsec_pins_fini(master_context)
#define PARSEC_PINS_THREAD_INIT(exec_unit)             \
    parsec_pins_thread_init(exec_unit)
#define PARSEC_PINS_TASKPOOL_INIT(parsec_tp)           \
    parsec_pins_taskpool_init(parsec_tp)
#define PARSEC_PINS_THREAD_FINI(exec_unit)             \
    parsec_pins_thread_fini(exec_unit)
#define PARSEC_PINS_TASKPOOL_FINI(parsec_tp)           \
    parsec_pins_taskpool_fini(parsec_tp)

#else // NOT PARSEC_PROF_PINS
#define PARSEC_PINS_EXPLICIT(_method_flag, _pname, _args...)   \
    do {} while (0)
#define PARSEC_PINS(method_flag, args...)              \
    do {} while (0)
#define PARSEC_PINS_DISABLE_REGISTRATION(boolean)      \
    do {} while(0)
#define PARSEC_PINS_REGISTER(method_flag, cb, data)    \
    do {} while (0)
#define PARSEC_PINS_UNREGISTER(method_flag, cb, data)  \
    do {} while (0)
#define PARSEC_PINS_INIT(master_context)               \
    do {} while (0)
#define PARSEC_PINS_FINI(master_context)               \
    do {} while (0)
#define PARSEC_PINS_THREAD_INIT(exec_unit)             \
    do {} while (0)
#define PARSEC_PINS_TASKPOOL_INIT(parsec_tp)           \
    do {} while (0)
#define PARSEC_PINS_THREAD_FINI(exec_unit)             \
    do {} while (0)
#define PARSEC_PINS_TASKPOOL_FINI(parsec_tp)           \
    do {} while (0)

#endif // PARSEC_PROF_PINS

#endif // PARSEC_PINS_H
