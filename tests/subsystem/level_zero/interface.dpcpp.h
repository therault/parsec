#ifndef INTERFACE_DPCPP_H
#define INTERFACE_DPCPP_H

typedef struct sycl_wrapper_s sycl_wrapper_t;

#if defined(c_plusplus) || defined(__cplusplus)
#include "sycl/ext/oneapi/backend/level_zero.hpp"

struct sycl_wrapper_s {
    sycl::platform platform;
    sycl::device   device;
    sycl::context  context;
    sycl::queue    queue;
};

extern "C" {
#endif


sycl_wrapper_t *sycl_wrapper_create(ze_driver_handle_t ze_driver,
                         ze_device_handle_t ze_device,
                         ze_context_handle_t ze_context,
                         ze_command_queue_handle_t ze_queue);
int sycl_wrapper_destroy(sycl_wrapper_t *_queue);

void *sycl_malloc(sycl_wrapper_t *sw, size_t size);
void sycl_free(sycl_wrapper_t *sw, void *ptr);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif //INTERFACE_DPCPP_H
