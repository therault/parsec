#include <oneapi/mkl.hpp>
#include "sycl/ext/oneapi/backend/level_zero.hpp"
#include "interface.dpcpp.h"

extern "C" {
 int dpcpp_kernel_GEMM(void *_sw,
                       const double *A,
                       double *C,
                       int mb);
}

int dpcpp_kernel_GEMM(void *_sw,
                      const double *A,
                      double *C,
                      int mb)
{
    sycl_wrapper_t *sw = reinterpret_cast<sycl_wrapper_t *>(_sw);

    double alpha=0.0;
    double beta=1.0;
    try {
      oneapi::mkl::blas::gemm(sw->queue, oneapi::mkl::transpose::N, oneapi::mkl::transpose::N,
         mb, mb, mb,
         alpha, A, mb,
         A, mb,
         beta, C, mb);
    } catch (const oneapi::mkl::invalid_argument &e) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws invalid argument exception");
    } catch (const oneapi::mkl::unsupported_device &e) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws unsuported device exception");
    } catch (const oneapi::mkl::host_bad_alloc &e) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws host bad allocation exception");
    } catch (const oneapi::mkl::device_bad_alloc &e) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws device bad allocation exception");
    } catch (const oneapi::mkl::unimplemented &e) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws unimplemented exception");
    } catch (const std::exception& e) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws unexpected exception");
    } catch (...) {
      fprintf(stderr, "OneAPI MKL BLAS GEMM throws unexpected exception that is also badly formatted...");
    }
    fprintf(stderr, "kernel has been scheduled on OneAPI MKL BLAS using the DPC++ driver\n");

    return 0;
}
