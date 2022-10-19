#ifndef INTERFACE_DPCPP_H
#define INTERFACE_DPCPP_H

#if defined(c_plusplus) || defined(__cplusplus)
#include "sycl/ext/oneapi/backend/level_zero.hpp"

extern "C" {
#endif

void * sycl_queue_create(ze_driver_handle_t ze_driver,
                         ze_device_handle_t ze_device,
                         ze_context_handle_t ze_context,
                         ze_command_queue_handle_t ze_queue);
int sycl_queue_destroy(void *_queue);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif //INTERFACE_DPCPP_H
