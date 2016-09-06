/*
 * Copyright (c) 2011-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2013      Inria. All rights reserved.
 *
 * @precisions normal z -> c d s
 *
 */

#include <lapacke.h>
#include "dplasma.h"
#include "dplasma/lib/dplasmatypes.h"
#include "data_dist/matrix/two_dim_rectangle_cyclic.h"
#include "data_dist/matrix/vector_two_dim_cyclic.h"

#include "map.h"
#include "zpltmg_chebvand.h"
#include "zpltmg_fiedler.h"
#include "zpltmg_hankel.h"
#include "zpltmg_toeppd.h"

/**
 *******************************************************************************
 *
 *  Generic case
 *
 *******************************************************************************
 */
struct zpltmg_args_s {
    PLASMA_enum            mtxtype;
    unsigned long long int seed;
    dague_complex64_t     *W;
};
typedef struct zpltmg_args_s zpltmg_args_t;

static int
dplasma_zpltmg_generic_operator( dague_execution_unit_t *eu,
                                 const tiled_matrix_desc_t *descA,
                                 void *_A,
                                 PLASMA_enum uplo, int m, int n,
                                 void *op_data )
{
    int tempmm, tempnn, ldam;
    zpltmg_args_t     *args = (zpltmg_args_t*)op_data;
    dague_complex64_t *A    = (dague_complex64_t*)_A;
    (void)eu;
    (void)uplo;

    tempmm = (m == (descA->mt-1)) ? (descA->m - m * descA->mb) : descA->mb;
    tempnn = (n == (descA->nt-1)) ? (descA->n - n * descA->nb) : descA->nb;
    ldam   = BLKLDD( *descA, m );

    if ( args->mtxtype == PlasmaMatrixCircul ) {
        return CORE_zpltmg_circul(
            tempmm, tempnn, A, ldam,
            descA->m, m*descA->mb, n*descA->nb, args->W );
    } else {
        return CORE_zpltmg(
            args->mtxtype, tempmm, tempnn, A, ldam,
            descA->m, descA->n, m*descA->mb, n*descA->nb, args->seed );
    }
}

/**
 *******************************************************************************
 *
 * @ingroup dplasma_complex64
 *
 * dplasma_zpltmg_generic - Generic wrapper for cases that are based on the map
 * function. This is the default for many test matrices generation.
 *
 * See dplasma_map() for further information.
 *
 *******************************************************************************
 *
 * @param[in,out] dague
 *          The dague context of the application that will run the operation.
 *
 * @param[in] mtxtype
 *          Type of matrix to be generated.
 *
 * @param[in,out] A
 *          Descriptor of the distributed matrix A to generate. Any tiled matrix
 *          descriptor can be used.
 *
 * @param[out] W
 *          Workspace required by some generators.
 *
 * @param[in] seed
 *          The seed used in the random generation.
 *
 *******************************************************************************
 *
 * @return
 *          \retval -i if the ith parameters is incorrect.
 *          \retval 0 on success.
 *
 *******************************************************************************
 *
 * @sa dplasma_cpltmg_genvect
 * @sa dplasma_dpltmg_genvect
 * @sa dplasma_spltmg_genvect
 *
 ******************************************************************************/
static inline int
dplasma_zpltmg_generic( dague_context_t *dague,
                        PLASMA_enum mtxtype,
                        tiled_matrix_desc_t *A,
                        dague_complex64_t *W,
                        unsigned long long int seed)
{
    dague_handle_t *dague_zpltmg = NULL;
    zpltmg_args_t *params = (zpltmg_args_t*)malloc(sizeof(zpltmg_args_t));

    params->mtxtype = mtxtype;
    params->seed    = seed;
    params->W       = W;

    dague_zpltmg = dplasma_map_New( PlasmaUpperLower, A, dplasma_zpltmg_generic_operator, params );
    if ( dague_zpltmg != NULL )
    {
        dague_enqueue(dague, (dague_handle_t*)dague_zpltmg);
        dplasma_progress(dague);
        dplasma_map_Destruct( dague_zpltmg );
        return 0;
    } else {
        return -101;
    }
}

/**
 *******************************************************************************
 *
 * @ingroup dplasma_complex64
 *
 * dplasma_zpltmg_genvect - Generic wrapper for cases that are using two
 * datatypes: the default one, and one describing a vector.
 *
 *******************************************************************************
 *
 * @param[in,out] dague
 *          The dague context of the application that will run the operation.
 *
 * @param[in] mtxtype
 *          Type of matrix to be generated.
 *
 * @param[in,out] A
 *          Descriptor of the distributed matrix A to generate. Any tiled matrix
 *          descriptor can be used.
 *
 * @param[in] seed
 *          The seed used in the random generation.
 *
 *******************************************************************************
 *
 * @return
 *          \retval -i if the ith parameters is incorrect.
 *          \retval 0 on success.
 *
 *******************************************************************************
 *
 * @sa dplasma_cpltmg_genvect
 * @sa dplasma_dpltmg_genvect
 * @sa dplasma_spltmg_genvect
 *
 ******************************************************************************/
static inline int
dplasma_zpltmg_genvect( dague_context_t *dague,
                        PLASMA_enum mtxtype,
                        tiled_matrix_desc_t *A,
                        unsigned long long int seed )
{
    size_t vectorsize = 0;
    dague_handle_t* handle;

    switch( mtxtype ) {
    case PlasmaMatrixChebvand:
        handle = (dague_handle_t*)dague_zpltmg_chebvand_new( seed,
                                                             (dague_ddesc_t*)A );
        vectorsize = 2 * A->nb * sizeof(dague_complex64_t);
        break;

    case PlasmaMatrixFiedler:
        handle = (dague_handle_t*)dague_zpltmg_fiedler_new( seed,
                                                            (dague_ddesc_t*)A );
        vectorsize = A->mb * sizeof(dague_complex64_t);
        break;

    case PlasmaMatrixHankel:
        handle = (dague_handle_t*)dague_zpltmg_hankel_new( seed,
                                                           (dague_ddesc_t*)A );
        vectorsize = A->mb * sizeof(dague_complex64_t);
        break;

    case PlasmaMatrixToeppd:
        handle = (dague_handle_t*)dague_zpltmg_toeppd_new( seed,
                                                           (dague_ddesc_t*)A );
        vectorsize = 2 * A->mb * sizeof(dague_complex64_t);
        break;

    default:
        return -2;
    }

    if (handle != NULL) {
        dague_zpltmg_hankel_handle_t *handle_zpltmg = (dague_zpltmg_hankel_handle_t*)handle;

        /* Default type */
        dplasma_add2arena_tile( handle_zpltmg->arenas[DAGUE_zpltmg_hankel_DEFAULT_ARENA],
                                A->mb*A->nb*sizeof(dague_complex64_t),
                                DAGUE_ARENA_ALIGNMENT_SSE,
                                dague_datatype_double_complex_t, A->mb );

        /* Vector type */
        dplasma_add2arena_tile( handle_zpltmg->arenas[DAGUE_zpltmg_hankel_VECTOR_ARENA],
                                vectorsize,
                                DAGUE_ARENA_ALIGNMENT_SSE,
                                dague_datatype_double_complex_t, A->mb );

        dague_enqueue(dague, handle);
        dplasma_progress(dague);

        dague_matrix_del2arena( handle_zpltmg->arenas[DAGUE_zpltmg_hankel_DEFAULT_ARENA] );
        dague_matrix_del2arena( handle_zpltmg->arenas[DAGUE_zpltmg_hankel_VECTOR_ARENA ] );
        dague_handle_free(handle);
        return 0;
    } else {
        return -101;
    }
}

/**
 *******************************************************************************
 *
 * @ingroup dplasma_complex64
 *
 * dplasma_zpltmg_circul - Generates a Circulant test matrix by tiles.
 *
 *******************************************************************************
 *
 * @param[in,out] dague
 *          The dague context of the application that will run the operation.
 *
 * @param[in,out] A
 *          Descriptor of the distributed matrix A to generate. Any tiled matrix
 *          descriptor can be used.
 *
 * @param[in] seed
 *          The seed used in the random generation.
 *
 *******************************************************************************
 *
 * @return
 *          \retval -i if the ith parameters is incorrect.
 *          \retval 0 on success.
 *
 *******************************************************************************
 *
 * @sa dplasma_cpltmg_circul
 * @sa dplasma_dpltmg_circul
 * @sa dplasma_spltmg_circul
 *
 ******************************************************************************/
static inline int
dplasma_zpltmg_circul( dague_context_t *dague,
                       tiled_matrix_desc_t *A,
                       unsigned long long int seed )
{
    int info;
    dague_complex64_t *V = (dague_complex64_t*) malloc( A->m * sizeof(dague_complex64_t) );

    CORE_zplrnt( A->m, 1, V, A->m, A->m, 0, 0, seed );

    info = dplasma_zpltmg_generic(dague, PlasmaMatrixCircul, A, V, seed);

    free(V);
    return info;
}

/**
 *******************************************************************************
 *
 * @ingroup dplasma_internal
 * @ingroup dplasma_complex64
 *
 * dplasma_zpltmg_condex - Generates a Condex test matrix by tiles.
 *
 *******************************************************************************
 *
 * @param[in,out] dague
 *          The dague context of the application that will run the operation.
 *
 * @param[in,out] A
 *          Descriptor of the distributed matrix A to generate. Any tiled matrix
 *          descriptor can be used.
 *
 *******************************************************************************
 *
 * @return
 *          \retval -i if the ith parameters is incorrect.
 *          \retval 0 on success.
 *
 *******************************************************************************
 *
 * @sa dplasma_cpltmg_condex
 * @sa dplasma_dpltmg_condex
 * @sa dplasma_spltmg_condex
 *
 ******************************************************************************/
static inline int
dplasma_zpltmg_condex( dague_context_t *dague,
                       tiled_matrix_desc_t *A )
{
    /* gallery('condex', A->m, 4, 100.) */
    dague_complex64_t theta = 100.;
    two_dim_block_cyclic_t *twodA = (two_dim_block_cyclic_t *)A;
    two_dim_block_cyclic_t Q;
    two_dim_block_cyclic_init( &Q, matrix_ComplexDouble, matrix_Tile,
                               1, A->super.myrank,
                               A->mb, A->nb, A->mb*A->mt, 3, 0, 0, A->m, 3, twodA->grid.strows, twodA->grid.stcols, 1 );
    Q.mat = dague_data_allocate((size_t)Q.super.nb_local_tiles *
                                (size_t)Q.super.bsiz *
                                (size_t)dague_datadist_getsizeoftype(Q.super.mtype));
    dague_ddesc_set_key((dague_ddesc_t*)&Q, "Q");

    if (A->super.myrank == 0) {
        dague_complex64_t *Qmat;

        Qmat = (dague_complex64_t*)(Q.mat);

        /* Initialize the Q matrix */
        CORE_zpltmg_condexq( A->m, A->n, Qmat, Q.super.lm );

        /*
         * Conversion to tile layout
         */
        {
            dague_complex64_t *W = (dague_complex64_t*) malloc (A->mb * sizeof(dague_complex64_t) );
            int *leaders = NULL;
            int i, nleaders;

            /* Get all the cycles leaders and length
             * They are the same for each independent problem (each panel) */
            GKK_getLeaderNbr( Q.super.lmt, A->nb, &nleaders, &leaders );

            /* shift cycles. */
            for(i=0; i<nleaders; i++) {

                /* cycle #i belongs to this thread, so shift it */
                memcpy(W, Qmat + leaders[i*3] * A->mb, A->mb * sizeof(dague_complex64_t) );
                CORE_zshiftw(leaders[i*3], leaders[i*3+1], A->mt, A->nb, A->mb, Qmat, W);
            }

            free(leaders); free(W);
        }
    }

    dplasma_zlaset( dague, PlasmaUpperLower, 0., 1. + theta, A );
    dplasma_zgemm( dague, PlasmaNoTrans, PlasmaConjTrans,
                   -theta, (tiled_matrix_desc_t*)&Q,
                           (tiled_matrix_desc_t*)&Q,
                   1.,     A );

    dague_data_free(Q.mat);
    tiled_matrix_desc_destroy((tiled_matrix_desc_t*)&Q);
    return 0;
}

/**
 *******************************************************************************
 *
 * @ingroup dplasma_complex64
 *
 * dplasma_zpltmg_house - Generates a Householder test matrix by tiles.
 *
 *******************************************************************************
 *
 * @param[in,out] dague
 *          The dague context of the application that will run the operation.
 *
 * @param[in,out] A
 *          Descriptor of the distributed matrix A to generate. Any tiled matrix
 *          descriptor can be used.
 *
 * @param[in] seed
 *          The seed used in the random generation.
 *
 *******************************************************************************
 *
 * @return
 *          \retval -i if the ith parameters is incorrect.
 *          \retval 0 on success.
 *
 *******************************************************************************
 *
 * @sa dplasma_cpltmg_house
 * @sa dplasma_dpltmg_house
 * @sa dplasma_spltmg_house
 *
 ******************************************************************************/
static inline int
dplasma_zpltmg_house( dague_context_t *dague,
                      tiled_matrix_desc_t *A,
                      unsigned long long int seed )
{
    /* gallery('house', random, 0 ) */
    vector_two_dim_cyclic_t V;
    dague_complex64_t *Vmat, tau;

    vector_two_dim_cyclic_init( &V, matrix_ComplexDouble, PlasmaVectorDiag,
                                1, A->super.myrank,
                                A->mb, A->m, 0, A->m, 1 );
    V.mat = dague_data_allocate((size_t)V.super.nb_local_tiles *
                                (size_t)V.super.bsiz *
                                (size_t)dague_datadist_getsizeoftype(V.super.mtype));
    dague_ddesc_set_key((dague_ddesc_t*)&V, "V");
    Vmat = (dague_complex64_t*)(V.mat);

    /* Initialize Householder vector */
    if (A->super.myrank == 0) {
        CORE_zplrnt( A->m, 1, Vmat, A->m, A->m, 0, 0, seed );
        LAPACKE_zlarfg_work( A->m, Vmat, Vmat+1, 1, &tau );
        Vmat[0] = 1.;
    }

#if defined(DAGUE_HAVE_MPI)
    MPI_Bcast( &tau, 1, dague_datatype_double_complex_t, 0, MPI_COMM_WORLD );
#endif

    /* Compute the Householder matrix I - tau v * v' */
    dplasma_zlaset( dague, PlasmaUpperLower, 0., 1., A);
    dplasma_zgerc( dague, -tau,
                   (tiled_matrix_desc_t*)&V,
                   (tiled_matrix_desc_t*)&V,
                   A );

    dague_data_free(V.mat);
    tiled_matrix_desc_destroy((tiled_matrix_desc_t*)&V);

    return 0;
}

/**
 *******************************************************************************
 *
 * @ingroup dplasma_complex64
 *
 * dplasma_zpltmg - Generates a special test matrix by tiles.
 *
 *******************************************************************************
 *
 * @param[in,out] dague
 *          The dague context of the application that will run the operation.
 *
 * @param[in] mtxtype
 *          See PLASMA_zpltmg() for possible values and information on generated
 *          matrices.
 *
 * @param[in,out] A
 *          Descriptor of the distributed matrix A to generate. Any tiled matrix
 *          descriptor can be used.
 *          On exit, the matrix A generated.
 *
 * @param[in] seed
 *          The seed used in the random generation.
 *
 *******************************************************************************
 *
 * @return
 *          \retval -i if the ith parameters is incorrect.
 *          \retval 0 on success.
 *
 *******************************************************************************
 *
 * @sa dplasma_cpltmg
 * @sa dplasma_dpltmg
 * @sa dplasma_spltmg
 *
 ******************************************************************************/
int
dplasma_zpltmg( dague_context_t *dague,
                PLASMA_enum mtxtype,
                tiled_matrix_desc_t *A,
                unsigned long long int seed)
{

    switch( mtxtype ) {
    case PlasmaMatrixCircul:
        return dplasma_zpltmg_circul(dague, A, seed);
        break;

    case PlasmaMatrixChebvand:
        return dplasma_zpltmg_genvect(dague, mtxtype,
                                      A, seed);
        break;

    case PlasmaMatrixCondex:
        return dplasma_zpltmg_condex(dague, A);
        break;

    case PlasmaMatrixFiedler:
        return dplasma_zpltmg_genvect(dague, mtxtype,
                                      A, seed);
        break;

    case PlasmaMatrixHankel:
        return dplasma_zpltmg_genvect(dague, mtxtype,
                                      A, seed);
        break;

    case PlasmaMatrixHouse:
        return dplasma_zpltmg_house(dague, A, seed);
        break;

    case PlasmaMatrixToeppd:
        return dplasma_zpltmg_genvect(dague, mtxtype,
                                      A, seed);
        break;

    case PlasmaMatrixCauchy:
    case PlasmaMatrixCompan:
    case PlasmaMatrixDemmel:
    case PlasmaMatrixDorr:
    case PlasmaMatrixFoster:
    case PlasmaMatrixHadamard:
    case PlasmaMatrixHilb:
    case PlasmaMatrixInvhess:
    case PlasmaMatrixKms:
    case PlasmaMatrixLangou:
    case PlasmaMatrixLehmer:
    case PlasmaMatrixLotkin:
    case PlasmaMatrixMinij:
    case PlasmaMatrixMoler:
    case PlasmaMatrixOrthog:
    case PlasmaMatrixParter:
    case PlasmaMatrixRandom:
    case PlasmaMatrixRiemann:
    case PlasmaMatrixRis:
    case PlasmaMatrixWilkinson:
    case PlasmaMatrixWright:
        return dplasma_zpltmg_generic(dague, mtxtype, A, NULL, seed);
        break;
    default:
        return -2;
    }

    return 0;
}
