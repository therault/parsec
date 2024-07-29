#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mpi.h>
#include <cuda_runtime_api.h>

#define cudaErrorCheck(call)                                                              \
do {                                                                                      \
    cudaError_t cuErr = call;                                                             \
    if(cudaSuccess != cuErr){                                                             \
        fprintf(stderr, "rank %d: CUDA Error - %s:%d: '%s'\n", rank, __FILE__, __LINE__, cudaGetErrorString(cuErr));\
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);                                          \
        exit(EXIT_FAILURE);                                                               \
    }                                                                                     \
} while(0)

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
 
    int size;
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if(size != 2){
        if(rank == 0){
            fprintf(stderr, "This program requires exactly 2 MPI ranks, but you are attempting to use %d! Exiting...\n", size);
        }
        MPI_Finalize();
        exit(EXIT_FAILURE);
    }

    int num_devices = 0;
    int did = 0;
    cudaErrorCheck( cudaGetDeviceCount(&num_devices) );
    if(0 == num_devices) {
        fprintf(stderr, "rank %d doesn't see any CUDA device\n", rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    cudaErrorCheck( cudaSetDevice(did) );
    struct cudaDeviceProp prop;
    cudaErrorCheck( cudaGetDeviceProperties ( &prop, did ) );
    char hn[256];
    gethostname(hn, 256);
    printf("#Rank %d on %s uses device %d -- %s on BusID/DeviceID/DomainID: %d/%d/%d\n", rank, hn, did, prop.name, prop.pciBusID, prop.pciDeviceID, prop.pciDomainID);
    for(int i=0; i<=27; i++){
        long int N = 1 << i;
   
        // Allocate memory for A on CPU
        double *A, *B;
        cudaErrorCheck( cudaHostAlloc((void**)&A, N*sizeof(double), cudaHostAllocPortable));
        cudaErrorCheck( cudaHostAlloc((void**)&B, N*sizeof(double), cudaHostAllocPortable));

        // Initialize all elements of A to some known constant, and B to 0
        for(int i=0; i<N; i++){
            A[i] = (double)N * (double)(rank+1);
            B[i] = 0.0;
        }

        // Allocate memory for A and B on GPU
        double *d_A, *d_B;
        cudaErrorCheck( cudaMalloc((void**)&d_A, N*sizeof(double)) );
        cudaErrorCheck( cudaMemcpy(d_A, A, N*sizeof(double), cudaMemcpyHostToDevice) );
        cudaErrorCheck( cudaMalloc((void**)&d_B, N*sizeof(double)) );
        cudaErrorCheck( cudaMemcpy(d_B, B, N*sizeof(double), cudaMemcpyHostToDevice) );

        double start_time, stop_time, elapsed_time;
        int loop_count = 50;

        // D2H2H2D
        start_time = MPI_Wtime();
        for(int i=1; i<=loop_count; i++){
            MPI_Request req[2];
            MPI_Status stat[2];
            cudaErrorCheck( cudaMemcpy(A, d_A, N*sizeof(double), cudaMemcpyDeviceToHost) );
            MPI_Irecv(B, N, MPI_DOUBLE, 1-rank, 0, MPI_COMM_WORLD, &req[0]);
            MPI_Isend(A, N, MPI_DOUBLE, 1-rank, 0, MPI_COMM_WORLD, &req[1]);
            MPI_Waitall(2, req, stat);
            cudaErrorCheck( cudaMemcpy(d_B, B, N*sizeof(double), cudaMemcpyHostToDevice) );
        }
        stop_time = MPI_Wtime();
        elapsed_time = stop_time - start_time;
        if(0 == rank) {
            printf("D2H2H2D\t%ld bytes\t%g seconds\t(average over %d transfers)\t\t%g GB/s\n", 
                   N, elapsed_time/loop_count, loop_count, N*sizeof(double)*loop_count/elapsed_time/1e9);
        }

        start_time = MPI_Wtime();
        for(int i=1; i<=loop_count; i++){
            MPI_Request req[2];
            MPI_Status stat[2];
            MPI_Irecv(d_B, N, MPI_DOUBLE, 1-rank, 0, MPI_COMM_WORLD, &req[0]);
            MPI_Isend(d_A, N, MPI_DOUBLE, 1-rank, 0, MPI_COMM_WORLD, &req[1]);
            MPI_Waitall(2, req, stat);
        }
        stop_time = MPI_Wtime();
        elapsed_time = stop_time - start_time;

        if(0 == rank) {
            printf("D2D\t%ld bytes\t%g seconds\t(average over %d transfers)\t\t%g GB/s\n", 
                   N, elapsed_time/loop_count, loop_count, N*sizeof(double)*loop_count/elapsed_time/1e9);
        }

        cudaErrorCheck( cudaFree(d_A) );
        cudaErrorCheck( cudaFree(d_B) );
        cudaFreeHost(A);
        cudaFreeHost(B);
    }

    MPI_Finalize();
}