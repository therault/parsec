/*
 * Copyright (c) 2009-2019 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */

#ifndef _pins_dot_grapher_h
#define _pins_dot_grapher_h

#include "parsec/parsec_config.h"
#include "parsec/mca/mca.h"
#include "parsec/mca/pins/pins.h"
#include "parsec/runtime.h"

BEGIN_C_DECLS

/**
 * Globally exported variable
 */
PARSEC_DECLSPEC extern const parsec_pins_base_component_t parsec_pins_dot_grapher_component;
PARSEC_DECLSPEC extern const parsec_pins_module_t parsec_pins_dot_grapher_module;
/* static accessor */
mca_base_component_t * pins_dot_grapher_static_component(void);


END_C_DECLS

#endif /* _pins_dot_grapher_h */
