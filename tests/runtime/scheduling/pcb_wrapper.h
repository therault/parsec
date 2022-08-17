/*
 * Copyright (c) 2014-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#ifndef _pcb_wrapper_h
#define _pcb_wrapper_h

#include "parsec/runtime.h"
#include "parsec/data_distribution.h"

/**
 * @param [IN] A     the data, already distributed and allocated
 * @param [IN] nt    number of tasks at a given level
 * @param [IN] level number of levels
 *
 * @return the parsec object to schedule.
 */
parsec_taskpool_t *pcb_new(parsec_data_collection_t *A, int nt, int level);

#endif
