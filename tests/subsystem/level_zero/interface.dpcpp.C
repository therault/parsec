#include "level_zero/ze_api.h"
#include "interface.dpcpp.h"

sycl_wrapper_t *sycl_wrapper_create(ze_driver_handle_t ze_driver,
                         ze_device_handle_t ze_device,
                         ze_context_handle_t ze_context,
                         ze_command_queue_handle_t ze_queue)
{
    sycl_wrapper_t *res = new sycl_wrapper_t;
    std::vector<sycl::device>devices;

    res->platform = sycl::make_platform<sycl::backend::ext_oneapi_level_zero>(ze_driver);
    res->device = sycl::make_device<sycl::backend::ext_oneapi_level_zero>(ze_device);
    devices.push_back(res->device);
    sycl::backend_input_t<sycl::backend::ext_oneapi_level_zero, sycl::context> hContextInteropInput = {ze_context, devices};
    res->context = sycl::make_context<sycl::backend::ext_oneapi_level_zero>(hContextInteropInput);
    sycl::backend_input_t<sycl::backend::ext_oneapi_level_zero, sycl::queue> hQueueInteropInput = {ze_queue, res->device};
    res->queue = sycl::make_queue<sycl::backend::ext_oneapi_level_zero>(hQueueInteropInput, res->context);

    return res;
}

int sycl_wrapper_destroy(sycl_wrapper_t *sw)
{
    delete sw;
    return 0;
}
