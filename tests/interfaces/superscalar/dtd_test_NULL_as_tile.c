#include "parsec_config.h"

/* system and io */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
/* parsec things */
#include "parsec.h"
#include "parsec/profiling.h"
#ifdef PARSEC_VTRACE
#include "parsec/vt_user.h"
#endif

#include "data_dist/matrix/two_dim_rectangle_cyclic.h"
#include "parsec/interfaces/superscalar/insert_function_internal.h"
#include "dplasma/testing/common_timing.h"

double time_elapsed = 0.0;

int
call_to_kernel_type( parsec_execution_unit_t    *context,
                     parsec_execution_context_t *this_task )
{
    (void)context;
    int *data;

    this_task->data[0].data_out = (parsec_data_copy_t *)context;
    return 0;
}

int main(int argc, char ** argv)
{
    parsec_context_t* parsec;

    int ncores = 8, m, n;
    int no_of_tasks = 2;

    if(argv[1] != NULL){
        ncores = atoi(argv[1]);
    }

    parsec = parsec_init(ncores, &argc, &argv);

    two_dim_block_cyclic_t ddescDATA;
    two_dim_block_cyclic_init( &ddescDATA, matrix_Integer, matrix_Tile, 1/*nodes*/, 0/*rank*/,
                                1, 1,/* tile_size*/ no_of_tasks, no_of_tasks,
                                /* Global matrix size*/ 0, 0, /* starting point */ no_of_tasks,
                                no_of_tasks, 1, 1, 1);

    ddescDATA.mat = calloc((size_t)ddescDATA.super.nb_local_tiles * (size_t) ddescDATA.super.bsiz,
                           (size_t) parsec_datadist_getsizeoftype(ddescDATA.super.mtype));
    parsec_ddesc_set_key ((parsec_ddesc_t *)&ddescDATA, "ddescDATA");

    parsec_ddesc_t *ddesc = &(ddescDATA.super.super);

    /*
    printf("---Starting--- \n");
    for( m = 0; m < no_of_tasks; m++ ) {
        for( n = 0; n < no_of_tasks; n++ ) {
            parsec_data_copy_t *gdata = ddesc->data_of_key(ddesc, ddesc->data_key(ddesc, m, n))->device_copies[0];
            int *data = PARSEC_DATA_COPY_GET_PTR((parsec_data_copy_t *) gdata);
            printf("At index [%d, %d]:\t%d\n", m, n, *data);
        }
    }
    */

    parsec_dtd_init();

    two_dim_block_cyclic_t *__ddescDATA = &ddescDATA;

    TIME_START();

    parsec_dtd_handle_t* PARSEC_dtd_handle = parsec_dtd_handle_new (parsec); /* 1 = arena_count */
    /* Registering the dtd_handle with PARSEC context */
    parsec_enqueue(parsec, (parsec_handle_t*) PARSEC_dtd_handle);
#if defined (OVERLAP)
    parsec_context_start(parsec);
#endif

    for( m = 0; m < no_of_tasks; m++ ) {
        for( n = 0; n < no_of_tasks; n++ ) {
            parsec_insert_task( PARSEC_dtd_handle, call_to_kernel_type,     "Test_Task",
                                   PASSED_BY_REF,    NULL,                 INOUT | REGION_FULL,
                                   0 );
        }
    }

    parsec_dtd_handle_wait( parsec, PARSEC_dtd_handle );
    parsec_dtd_context_wait_on_handle( parsec, PARSEC_dtd_handle );

    parsec_dtd_handle_destruct(PARSEC_dtd_handle);

    TIME_STOP();

    /*
    printf("---Finally--- \n");
    for( m = 0; m < no_of_tasks; m++ ) {
        for( n = 0; n < no_of_tasks; n++ ) {
            parsec_data_copy_t *gdata = ddesc->data_of_key(ddesc, ddesc->data_key(ddesc, m, n))->device_copies[0];
            int *data = PARSEC_DATA_COPY_GET_PTR((parsec_data_copy_t *) gdata);
            printf("At index [%d, %d]:\t%d\n", m, n, *data);
        }
    }
    */
    printf("Time Elapsed:\t");
    printf("\n%lf\n", time_elapsed);

    parsec_dtd_fini();
    parsec_fini(&parsec);

    return 0;
}
