#include "level_zero/ze_api.h"
#include "interface.dpcpp.h"

void * sycl_queue_create(ze_driver_handle_t ze_driver,
                         ze_device_handle_t ze_device,
                         ze_context_handle_t ze_context,
                         ze_command_queue_handle_t ze_queue)
{
    sycl::platform platform;
    sycl::device   device;
    sycl::context  context;
    sycl::queue    queue;

    std::vector<sycl::device>devices;

    platform = sycl::level_zero::make<sycl::platform>(ze_driver);
    device = sycl::level_zero::make<sycl::device>(platform, ze_device);
    devices.push_back(device);
    context = sycl::level_zero::make<sycl::context>(devices, ze_context);
    queue = sycl::level_zero::make<sycl::queue>(context, ze_queue);

    return static_cast<void*>(queue);
}

int sycl_queue_destroy(void *_queue)
{
    return 0;
}
