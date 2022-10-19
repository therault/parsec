#include "sycl/ext/oneapi/backend/level_zero.hpp"

extern "C" {
 int dpcpp_kernel_GEMM(void *_queue,
                       const double *A,
                       double *C,
                       int mb);
}

int dpcpp_kernel_GEMM(void *_queue,
                      const double *A,
                      double *C,
                      int mb)
{
    sycl::queue queue = reinterpret_cast<sycl::queue>(_queue);

    double alpha=0.0;
    double beta=1.0;
    try {
      oneapi::mkl::blas::gemm(queue, oneapi::mkl::transpose::N, oneapi::mkl::transpose::N,
         mb, mb, mb,
         alpha, A, mb,
         A, mb,
         beta, C, mb);
    } catch (const oneapi::mkl::invalid_argument &e) {
      parsec_warning("OneAPI MKL BLAS GEMM throws invalid argument exception");
    } catch (const oneapi::mkl::unsupported_device &e) {
      parsec_warning("OneAPI MKL BLAS GEMM throws unsuported device exception");
    } catch (const oneapi::mkl::host_bad_alloc &e) {
      parsec_warning("OneAPI MKL BLAS GEMM throws host bad allocation exception");
    } catch (const oneapi::mkl::device_bad_alloc &e) {
      parsec_warning("OneAPI MKL BLAS GEMM throws device bad allocation exception");
    } catch (const oneapi::mkl::unimplemented &e) {
      parsec_warning("OneAPI MKL BLAS GEMM throws unimplemented exception");
    } catch (const std::exception& e) {
      parsec_warning("OneAPI MKL BLAS GEMM throws unexpected exception");
    } catch (...) {
      parsec_warning("OneAPI MKL BLAS GEMM throws unexpected exception that is also badly formatted...");
    }
    fprintf(stderr, "kernel has been scheduled on OneAPI MKL BLAS using the DPC++ driver\n");

    return 0;
}
