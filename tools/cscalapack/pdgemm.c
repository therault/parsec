/*
 * Copyright (c) 2009-2015 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2010      University of Denver, Colorado.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <mpi.h>
#include <math.h>
#include "myscalapack.h"
#include "../../dplasma/testing/flops.h"

#ifndef max
#define max(_a, _b) ( (_a) < (_b) ? (_b) : (_a) )
#define min(_a, _b) ( (_a) > (_b) ? (_b) : (_a) )
#endif

static int i0=0, i1=1;
static double m1=-1e0, p1=1e0;

typedef enum {
    PARAM_BLACS_CTX,
    PARAM_RANK,
    PARAM_M,
    PARAM_N,
    PARAM_NB,
    PARAM_SEED,
    PARAM_VALIDATE,
    PARAM_NRHS
} params_enum_t;

static void setup_params( int params[], int argc, char* argv[] );
static double check_solution( int params[], double *Alu, double *tau );

#define Rnd64_A 6364136223846793005ULL
#define Rnd64_C 1ULL
#define RndF_Mul 5.4210108624275222e-20f
#define RndD_Mul 5.4210108624275222e-20
#define NBELEM 1

static unsigned long long int
Rnd64_jump(unsigned long long int n, unsigned long long int seed ) {
    unsigned long long int a_k, c_k, ran;
    int i;

    a_k = Rnd64_A;
    c_k = Rnd64_C;

    ran = seed;
    for (i = 0; n; n >>= 1, ++i) {
        if (n & 1)
            ran = a_k * ran + c_k;
        c_k *= (a_k + 1);
        a_k *= a_k;
    }

    return ran;
}

void CORE_dplrnt( int m, int n, double *A, int lda,
                  int bigM, int m0, int n0, unsigned long long int seed )
{
    double *tmp = A;
    int64_t i, j;
    unsigned long long int ran, jump;

    jump = (unsigned long long int)m0 + (unsigned long long int)n0 * (unsigned long long int)bigM;

    for (j=0; j<n; ++j ) {
        ran = Rnd64_jump( NBELEM*jump, seed );
        for (i = 0; i < m; ++i) {
            *tmp = 0.5f - ran * RndF_Mul;
            ran  = Rnd64_A * ran + Rnd64_C;
            tmp++;
        }
        tmp  += lda-i;
        jump += bigM;
    }
}

static void init_random_matrix(double *A,
                               int m, int n,
                               int mb, int nb,
                               int myrow, int mycol,
                               int nprow, int npcol,
                               int mloc,
                               int seed)
{
    int i, j;
    int idum1, idum2, iloc, jloc, i0=0;
    int tempm, tempn;
    double *Ab;

    for (i = 1; i <= m; i += mb) {
        for (j = 1; j <= n; j += nb) {
            if ( ( myrow == indxg2p_( &i, &mb, &idum1, &i0, &nprow ) ) &&
                 ( mycol == indxg2p_( &j, &nb, &idum1, &i0, &npcol ) ) ){
                iloc = indxg2l_( &i, &mb, &idum1, &idum2, &nprow );
                jloc = indxg2l_( &j, &nb, &idum1, &idum2, &npcol );

                Ab =  &A[ (jloc-1)*mloc + (iloc-1) ];
                tempm = (i+mb > m) ? (m%mb) : (mb);
                tempn = (j+nb > n) ? (n%nb) : (nb);
                tempm = (m - i +1) > mb ? mb : (m-i + 1);
                tempn = (n - j +1) > nb ? nb : (n-j + 1);
                CORE_dplrnt( tempm, tempn, Ab, mloc,
                             m, mb*( (i-1)/mb ), nb*( (j-1)/nb ), seed);
            }
        }
    }
}

int main( int argc, char **argv ) {
    int params[8];
    int info;
    int ictxt, nprow, npcol, myrow, mycol, iam;
    int m, n, nb, s, mloc, nloc, verif, iseed;
    double *A=NULL, *B=NULL, *C=NULL; int descA[9];
    double resid = NAN;
    double telapsed, gflops, pgflops;

    setup_params( params, argc, argv );
    ictxt = params[PARAM_BLACS_CTX];
    iam   = params[PARAM_RANK];
    m     = params[PARAM_M];
    n     = params[PARAM_N];
    nb    = params[PARAM_NB];
    s     = params[PARAM_NRHS];
    iseed = params[PARAM_SEED];
    verif = params[PARAM_VALIDATE];

    Cblacs_gridinfo( ictxt, &nprow, &npcol, &myrow, &mycol );
    mloc = numroc_( &m, &nb, &myrow, &i0, &nprow );
    nloc = numroc_( &n, &nb, &mycol, &i0, &npcol );
    descinit_( descA, &m, &n, &nb, &nb, &i0, &i0, &ictxt, &mloc, &info );
    assert( 0 == info );
    A = malloc( sizeof(double)*mloc*nloc );
    B = malloc( sizeof(double)*mloc*nloc );
    C = malloc( sizeof(double)*mloc*nloc );

    init_random_matrix(A,
                       m, n,
                       nb, nb,
                       myrow, mycol,
                       nprow, npcol,
                       mloc,
                       iseed);
    init_random_matrix(B,
                       m, n,
                       nb, nb,
                       myrow, mycol,
                       nprow, npcol,
                       mloc,
                       iseed);
    init_random_matrix(C,
                       m, n,
                       nb, nb,
                       myrow, mycol,
                       nprow, npcol,
                       mloc,
                       iseed);

    {
        double t1, t2;
        t1 = MPI_Wtime();
        pdgemm_( "n", "n", &m, &n, &n, &p1, A, &i1, &i1, descA, 
                                            B, &i1, &i1, descA, 
                                       &p1, C, &i1, &i1, descA );
        t2 = MPI_Wtime();
        telapsed = t2-t1;
    }

    if( 0 != iam ) {
        MPI_Reduce( &telapsed, NULL, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD );
    }
    else {
        MPI_Reduce( MPI_IN_PLACE, &telapsed, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD );
        gflops = FLOPS_DGEMM((double)m, (double)n, (double)n)/1e+9/telapsed;
        pgflops = gflops/(((double)nprow)*((double)npcol));
        printf( "### PDGEMM ###\n"
                "#%4sx%-4s %7s %7s %4s %4s # %10s %10s %10s %11s\n", "P", "Q", "M", "N", "NB", "NRHS", "resid", "time(s)", "gflops", "gflops/pxq" );
        printf( " %4d %-4d %7d %7d %4d %4d   %10.3e %10.3g %10.3g %11.3g\n", nprow, npcol, m, n, nb, s, resid, telapsed, gflops, pgflops );
    }

    free( A ); A = NULL;
    free( B ); B = NULL;
    free( C ); C = NULL;

    Cblacs_exit( 0 );
    return 0;
}


static void setup_params( int params[], int argc, char* argv[] ) {
    int i;
    int ictxt, iam, nprocs, p, q;
    MPI_Init( &argc, &argv );
    Cblacs_pinfo( &iam, &nprocs );
    Cblacs_get( -1, 0, &ictxt );

    p = 1;
    q = 1;
    params[PARAM_M]         = 0;
    params[PARAM_N]         = 1000;
    params[PARAM_NB]        = 64;
    params[PARAM_SEED]      = 3872;
    params[PARAM_VALIDATE]  = 1;
    params[PARAM_NRHS]      = 1;

    for( i = 1; i < argc; i++ ) {
        if( strcmp( argv[i], "-p" ) == 0 ) {
            p = atoi(argv[i+1]);
            i++;
            continue;
        }
        if( strcmp( argv[i], "-q" ) == 0 ) {
            q = atoi(argv[i+1]);
            i++;
            continue;
        }
        if( strcmp( argv[i], "-n" ) == 0 ) {
            params[PARAM_N] = atoi(argv[i+1]);
            i++;
            continue;
        }
        if( strcmp( argv[i], "-b" ) == 0 ) {
            params[PARAM_NB] = atoi(argv[i+1]);
            i++;
            continue;
        }
        if( strcmp( argv[i], "-x" ) == 0 ) {
            params[PARAM_VALIDATE] = 0;
            continue;
        }
        if(( strcmp( argv[i], "-s" ) == 0 ) ||
           ( strcmp( argv[i], "-nrhs" ) == 0 )) {
            params[PARAM_NRHS] = atoi(argv[i+1]);
            i++;
            continue;
        }
        if( strcmp( argv[i], "-seed" ) == 0 ) {
            params[PARAM_SEED] = atoi(argv[i+1]);
            i++;
        }
        fprintf( stderr, "### USAGE: %s [-p NUM][-q NUM][-n NUM][-b NUM][-x][-s NUM]\n"
                         "#     -p         : number of rows in the PxQ process grid\n"
                         "#     -q         : number of columns in the PxQ process grid\n"
                         "#     -n         : dimension of the matrix (NxN)\n"
                         "#     -b         : block size (NB)\n"
                         "#     -x         : disable verification\n"
                         "#          -seed : Change the seed\n", argv[0] );
        Cblacs_abort( ictxt, i );
    }
    /* Validity checks etc. */
    if( params[PARAM_NB] > params[PARAM_N] )
        params[PARAM_NB] = params[PARAM_N];
    if( 0 == params[PARAM_M] )
        params[PARAM_M] = params[PARAM_N];
    if( p*q > nprocs ) {
        if( 0 == iam )
            fprintf( stderr, "### ERROR: we do not have enough processes available to make a p-by-q process grid ###\n"
                             "###   Bye-bye                                                                      ###\n" );
        Cblacs_abort( ictxt, 1 );
    }
    if( params[PARAM_VALIDATE] && (params[PARAM_M] != params[PARAM_N]) ) {
        if( 0 == iam )
            fprintf( stderr, "### WARNING: Unable to validate on a non-square matrix. Canceling validation.\n" );
        params[PARAM_VALIDATE] = 0;
    }
    Cblacs_gridinit( &ictxt, "Row", p, q );
    params[PARAM_BLACS_CTX] = ictxt;
    params[PARAM_RANK] = iam;
}