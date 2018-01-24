/*
 * Copyright (c) 2018      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */


/**
 * @file
 *
 * Dijsktra-Matter Termination Detection Algorithm, four-counters variant
 *   (see TBD)
 *
 */

#ifndef MCA_TERMDET_FOURCOUNTER_H
#define MCA_TERMDET_FOURCOUNTER_H

#include "parsec/parsec_config.h"
#include "parsec/mca/mca.h"
#include "parsec/mca/termdet/termdet.h"

#include "parsec/parsec_remote_dep.h"

BEGIN_C_DECLS

/**
 * Globally exported variable
 */
PARSEC_DECLSPEC extern const parsec_termdet_base_component_t parsec_termdet_fourcounter_component;
PARSEC_DECLSPEC extern const parsec_termdet_module_t parsec_termdet_fourcounter_module;

int parsec_termdet_fourcounter_msg_dispatch(parsec_comm_engine_t *ce, long unsigned int tag,  void *msg, long unsigned int size, int src,  void *module);

typedef enum {
    PARSEC_TERMDET_FOURCOUNTER_MSG_TYPE_DOWN,
    PARSEC_TERMDET_FOURCOUNTER_MSG_TYPE_UP
} parsec_termdet_fourcounter_msg_type_t;

typedef struct {
    parsec_termdet_fourcounter_msg_type_t msg_type;
    uint32_t tp_id;
    uint32_t nb_sent;
    uint32_t nb_received;
} parsec_termdet_fourcounter_msg_up_t;

typedef struct {
    parsec_termdet_fourcounter_msg_type_t msg_type;
    uint32_t tp_id;
    uint32_t result;
} parsec_termdet_fourcounter_msg_down_t;

// This needs to be kept in sync with all possible messages
#define TERMDET_FOURCOUNTER_MAX_MSG_SIZE (sizeof(parsec_termdet_fourcounter_msg_down_t))

typedef struct {
    parsec_list_item_t list_item;
    unsigned char msg[TERMDET_FOURCOUNTER_MAX_MSG_SIZE];
    parsec_comm_engine_t *ce;
    void *module;
    long unsigned int tag;
    long unsigned int size;
    int src;
} parsec_termdet_fourcounter_delayed_msg_t;

#define PARSEC_TERMDET_FOURCOUNTER_MAX_MSG_SIZE sizeof(parsec_termdet_fourcounter_msg_up_t)

extern parsec_list_t parsec_termdet_fourcounter_delayed_messages;

/* static accessor */
mca_base_component_t *termdet_fourcounter_static_component(void);

END_C_DECLS
#endif /* MCA_TERMDET_FOURCOUNTER_H */
