#ifndef PINS_TASK_STUBS_H
#define PINS_TASK_STUBS_H

#include "parsec/parsec_config.h"
#include "parsec/mca/mca.h"
#include "parsec/mca/pins/pins.h"
#include "parsec/runtime.h"

BEGIN_C_DECLS

/**
 * Globally exported variable
 */
PARSEC_DECLSPEC extern const parsec_pins_base_component_t parsec_pins_task_stubs_component;
PARSEC_DECLSPEC extern parsec_pins_module_t parsec_pins_task_stubs_module;
/* static accessor */
mca_base_component_t * pins_task_stubs_static_component(void);

END_C_DECLS

#endif // PINS_TASK_STUBS_H
