/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */
#include "redistribute_internal.h"
#include "redistribute.h"
#include "redistribute_reshuffle.h"

static inline int parsec_imin(int a, int b)
{
    return (a <= b) ? a : b;
};

static inline int parsec_imax(int a, int b)
{
    return (a >= b) ? a : b;
};

/**
 * @brief New function for redistribute
 *
 * @param [in] dcY: the data, already distributed and allocated
 * @param [out] dcT: the data, redistributed and allocated
 * @param [in] size_row: row size to be redistributed
 * @param [in] size_col: column size to be redistributed
 * @param [in] disi_Y: row displacement in dcY
 * @param [in] disj_Y: column displacement in dcY
 * @param [in] disi_T: row displacement in dcT
 * @param [in] disj_T: column displacement in dcT
 * @return the parsec object to schedule.
 */
parsec_taskpool_t*
parsec_redistribute_New(parsec_tiled_matrix_dc_t *dcY,
                        parsec_tiled_matrix_dc_t *dcT,
                        int size_row, int size_col,
                        int disi_Y, int disj_Y,
                        int disi_T, int disj_T)
{
    parsec_taskpool_t* redistribute_taskpool;

    if( size_row < 1 || size_col < 1 ) {
        if( 0 == dcY->super.myrank )
            fprintf(stderr, "ERROR: Submatrix size should be bigger than 1\n");
        exit(1);
    }

    if( disi_Y < 0 || disj_Y < 0 ) {
        if( 0 == dcY->super.myrank )
            fprintf(stderr, "ERROR: Source displacement should not be negative\n");
        exit(1);
    }

    if( disi_T < 0 || disj_T < 0 ) {
        if( 0 == dcY->super.myrank )
            fprintf(stderr, "ERROR: Target displacement should not be negative\n");
        exit(1);
    }

    if( (disi_Y+size_row > dcY->lmt*dcY->mb)
        || (disj_Y+size_col > dcY->lnt*dcY->nb) ){
        if( 0 == dcY->super.myrank )
            fprintf(stderr, "ERROR: Submatrix exceed SOURCE size\n");
        exit(1);
    }

    if( (disi_T+size_row > dcT->lmt*dcT->mb)
        || (disj_T+size_col > dcT->lnt*dcT->nb) ){
        if( 0 == dcY->super.myrank )
            fprintf(stderr, "ERROR: Submatrix exceed TARGET size\n");
        exit(1);
    }

    /* Optimized version: tile sizes of source and target ar the same,
     * displacements in both source and target are at the start of tiles */
    if( (dcY->mb == dcT->mb) && (dcY->nb == dcT->nb) && (disi_Y % dcY->mb == 0)
        && (disj_Y % dcY->nb == 0) && (disi_T % dcT->mb == 0) && (disj_T % dcT->nb == 0) ) {
        parsec_redistribute_reshuffle_taskpool_t* taskpool = NULL;
        /* new taskpool */
        taskpool = parsec_redistribute_reshuffle_new(dcY, dcT, size_row, size_col, disi_Y, disj_Y, disi_T, disj_T);
        redistribute_taskpool = (parsec_taskpool_t*)taskpool;

        /* Check distribution, and detarmine batch size: num_col */
        if( (dcY->dtype & two_dim_tabular_type) && (dcT->dtype & two_dim_tabular_type) ) {
            taskpool->_g_num_col = parsec_imin( ceil(size_col/dcY->nb), dcY->super.nodes );
        } else if( (dcY->dtype & two_dim_tabular_type) && (dcT->dtype & two_dim_block_cyclic_type) ) {
            taskpool->_g_num_col = ((two_dim_block_cyclic_t *)dcT)->grid.cols * ((two_dim_block_cyclic_t *)dcT)->grid.kcols;
        } else if( (dcY->dtype & two_dim_block_cyclic_type) && (dcT->dtype & two_dim_tabular_type) ) {
            taskpool->_g_num_col = ((two_dim_block_cyclic_t *)dcY)->grid.cols * ((two_dim_block_cyclic_t *)dcY)->grid.kcols;
        } else if( (dcY->dtype & two_dim_block_cyclic_type) && (dcT->dtype & two_dim_block_cyclic_type) ) {
            taskpool->_g_num_col = parsec_imax( ((two_dim_block_cyclic_t *)dcY)->grid.cols * ((two_dim_block_cyclic_t *)dcY)->grid.kcols, ((two_dim_block_cyclic_t *)dcT)->grid.cols * ((two_dim_block_cyclic_t *)dcT)->grid.kcols );
        } else {
            fprintf(stderr, "Only support two_dim_block_cyclic_type and two_dim_tabular_type\n");
        }

        /* Calculate NT, need to update !!! */
        int n_T_START = disj_T / dcT->nb;
        int n_T_END = (size_col+disj_T-1) / dcT->nb;
        taskpool->_g_NT = (n_T_END-n_T_START)/taskpool->_g_num_col;

        parsec_matrix_add2arena(&taskpool->arenas_datatypes[PARSEC_redistribute_reshuffle_DEFAULT_ADT_IDX],
                                MY_TYPE, matrix_UpperLower,
                                1, dcY->mb, dcY->nb, dcY->mb,
                                PARSEC_ARENA_ALIGNMENT_SSE, -1 );
    /* General version */
    } else {
        parsec_redistribute_taskpool_t* taskpool = NULL;
        /* R will be used for padding tiles like in AMR,
         * here for a normal redistribution problem, R is set to 0.
         */
        int R = 0;

        /* new taskpool */
        taskpool = parsec_redistribute_new(dcY, dcT, size_row, size_col, disi_Y, disj_Y, disi_T, disj_T, R);
        redistribute_taskpool = (parsec_taskpool_t*)taskpool;

        /* Check distribution, and detarmine batch size: num_col */
        if( (dcY->dtype & two_dim_tabular_type) && (dcT->dtype & two_dim_tabular_type) ) {
            taskpool->_g_num_col = parsec_imin( ceil(size_col/dcY->nb), dcY->super.nodes );
        } else if( (dcY->dtype & two_dim_tabular_type) && (dcT->dtype & two_dim_block_cyclic_type) ) {
            taskpool->_g_num_col = ((two_dim_block_cyclic_t *)dcT)->grid.cols * ((two_dim_block_cyclic_t *)dcT)->grid.kcols;
        } else if( (dcY->dtype & two_dim_block_cyclic_type) && (dcT->dtype & two_dim_tabular_type) ) {
            taskpool->_g_num_col = ((two_dim_block_cyclic_t *)dcY)->grid.cols * ((two_dim_block_cyclic_t *)dcY)->grid.kcols;
        } else if( (dcY->dtype & two_dim_block_cyclic_type) && (dcT->dtype & two_dim_block_cyclic_type) ) {
            taskpool->_g_num_col = parsec_imax( ((two_dim_block_cyclic_t *)dcY)->grid.cols * ((two_dim_block_cyclic_t *)dcY)->grid.kcols, ((two_dim_block_cyclic_t *)dcT)->grid.cols * ((two_dim_block_cyclic_t *)dcT)->grid.kcols );
        } else {
            fprintf(stderr, "Only support two_dim_block_cyclic_type and two_dim_tabular_type\n");
        }

        /* Calculate NT, need to update !!! */
        int n_T_START = disj_T / (dcT->nb-2*R);
        int n_T_END = (size_col+disj_T-1) / (dcT->nb-2*R);
        taskpool->_g_NT = (n_T_END-n_T_START)/taskpool->_g_num_col;

        parsec_matrix_add2arena(&taskpool->arenas_datatypes[PARSEC_redistribute_DEFAULT_ADT_IDX],
                                MY_TYPE, matrix_UpperLower,
                                1, 1, 1, 1,
                                PARSEC_ARENA_ALIGNMENT_SSE, -1 );

        int Y_LDA = dcY->storage == matrix_Lapack ? dcY->llm : dcY->mb;
        int T_LDA = dcT->storage == matrix_Lapack ? dcT->llm : dcT->mb;

        parsec_matrix_add2arena(&taskpool->arenas_datatypes[PARSEC_redistribute_TARGET_ADT_IDX],
                                MY_TYPE, matrix_UpperLower,
                                1, dcT->mb, dcT->nb, T_LDA,
                                PARSEC_ARENA_ALIGNMENT_SSE, -1 );

        // parsec_matrix_add2arena(&taskpool->arenas_datatypes[PARSEC_redistribute_SOURCE_ADT_IDX],
        //                         MY_TYPE, matrix_UpperLower,
        //                         1, dcY->mb, dcY->nb, Y_LDA,
        //                         PARSEC_ARENA_ALIGNMENT_SSE, -1 );

        parsec_matrix_add2arena(&taskpool->arenas_datatypes[PARSEC_redistribute_INNER_ADT_IDX],
                                MY_TYPE, matrix_UpperLower,
                                1, dcY->mb-2*R, dcY->nb-2*R, Y_LDA,
                                PARSEC_ARENA_ALIGNMENT_SSE, -1 );
    }

    return redistribute_taskpool;
}

/**
 * @param [inout] the parsec object to destroy
 */
void parsec_redistribute_Destruct(parsec_taskpool_t *taskpool)
{
    parsec_redistribute_taskpool_t *redistribute_taskpool = (parsec_redistribute_taskpool_t *)taskpool;

    /* Optimized version: tile sizes of source and target ar the same,
     * displacements in both source and target are at the start of tiles */
    if( (redistribute_taskpool->_g_descY->mb == redistribute_taskpool->_g_descT->mb)
        && (redistribute_taskpool->_g_descY->nb == redistribute_taskpool->_g_descT->nb)
        && (redistribute_taskpool->_g_disi_Y % redistribute_taskpool->_g_descY->mb == 0)
        && (redistribute_taskpool->_g_disj_Y % redistribute_taskpool->_g_descY->nb == 0)
        && (redistribute_taskpool->_g_disi_T % redistribute_taskpool->_g_descT->mb == 0)
        && (redistribute_taskpool->_g_disj_T % redistribute_taskpool->_g_descT->nb == 0) )
    {
        parsec_redistribute_reshuffle_taskpool_t *redistribute_reshuffle_taskpool = (parsec_redistribute_reshuffle_taskpool_t *)taskpool;
        parsec_matrix_del2arena(&redistribute_reshuffle_taskpool->arenas_datatypes[PARSEC_redistribute_reshuffle_DEFAULT_ADT_IDX]);
    } else {
        parsec_matrix_del2arena(&redistribute_taskpool->arenas_datatypes[PARSEC_redistribute_DEFAULT_ADT_IDX]);
        parsec_matrix_del2arena(&redistribute_taskpool->arenas_datatypes[PARSEC_redistribute_TARGET_ADT_IDX]);
        // parsec_matrix_del2arena(&redistribute_taskpool->arenas_datatypes[PARSEC_redistribute_SOURCE_ADT_IDX]);
        parsec_matrix_del2arena(&redistribute_taskpool->arenas_datatypes[PARSEC_redistribute_INNER_ADT_IDX]);
    }

    parsec_taskpool_free(taskpool);
}

/**
 * @brief Redistribute dcY to dcT in PTG
 *
 * @param [in] dcY: source distribution, already distributed and allocated
 * @param [out] dcT: target distribution, redistributed and allocated
 * @param [in] size_row: row size to be redistributed
 * @param [in] size_col: column size to be redistributed
 * @param [in] disi_Y: row displacement in dcY
 * @param [in] disj_Y: column displacement in dcY
 * @param [in] disi_T: row displacement in dcT
 * @param [in] disj_T: column displacement in dcT
 */
int parsec_redistribute(parsec_context_t *parsec,
                        parsec_tiled_matrix_dc_t *dcY,
                        parsec_tiled_matrix_dc_t *dcT,
                        int size_row, int size_col,
                        int disi_Y, int disj_Y,
                        int disi_T, int disj_T)
{
    parsec_taskpool_t *parsec_redistribute_ptg = NULL;

    parsec_redistribute_ptg = parsec_redistribute_New(
                              dcY, dcT, size_row, size_col, disi_Y,
                              disj_Y, disi_T, disj_T);

    if( NULL != parsec_redistribute_ptg ){
        parsec_context_add_taskpool(parsec, parsec_redistribute_ptg);
        parsec_context_start(parsec);
        parsec_context_wait(parsec);
        parsec_redistribute_Destruct(parsec_redistribute_ptg);
    }

    return 0;
}

/**
 * @brief Copy from Y to T
 *
 * @param [out] T: target
 * @param [in] Y: source
 * @param [in] mb: row size to be copied
 * @param [in] nb: column size to be copied
 * @param [in] T_LDA: LDA of T
 * @param [in] Y_LDA: LDA of Y
 */
void CORE_redistribute_reshuffle_copy(DTYPE *T, DTYPE *Y, const int mb,
                                      const int nb, const int T_LDA, const int Y_LDA)
{
    MOVE_SUBMATRIX(mb, nb, Y, 0, 0, Y_LDA, T, 0, 0, T_LDA);
}
