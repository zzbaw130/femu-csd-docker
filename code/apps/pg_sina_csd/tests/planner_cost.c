/* Resource-model invariants, without a running PostgreSQL or CSD device. */
#include "cost_model.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(test) do { if(!(test)) { fprintf(stderr,"line %d: %s\n",__LINE__,#test); return EXIT_FAILURE; } } while(0)
int main(void)
{
    /* A single block cannot speed up on eight CUs. Three blocks leave five
     * idle; an incomplete last wave costs more than ideal work/CU. */
    CHECK(cost_parallel(80,1,8,0.02)==80);
    CHECK(cost_parallel(30,3,8,0)==10);
    CHECK(cost_parallel(90,9,4,0)>90.0/4);
    CHECK(cost_parallel(80,8,4,0.02)>20);
    CHECK(cost_parallel(80,8,4,0.02)<cost_parallel(80,8,2,0.02));
    /* 4-byte PG integer + NULL, 8-byte integer + NULL, 128-bit numeric with
     * scale/dscale/NULL: packed 33 bytes, not three 32-byte cells. */
    struct qs_plan row={.nout=3};
    row.outputs[0].type_oid=23; row.outputs[1].type_oid=20; row.outputs[2].type_oid=1700;
    CHECK(cost_hash_row(&row)==33);
    CHECK(cost_hash_state(128,33)==128*45);
    CHECK(cost_hash_state(129,33)==256*45);
    struct qp_node group={.nkeys=1}; group.expr.naggs=1;
    group.expr.exprs[0].type=Q_INTEGER; group.expr.keys[0]=0;
    CHECK(cost_group_stride(&group)==64);
    CHECK(cost_group_state(&group,112)==128*(8+56)+128);
    CHECK(cost_group_state(&group,113)>cost_group_state(&group,112));
    /* Only supported >=64MiB conditions. More workmem reduces spill, and a
     * dataset larger than all memory still requires external merge. */
    double mib=1024*1024;
    struct cost_sort small=cost_sort_state(1000,32,64*mib);
    struct cost_sort low=cost_sort_state(8*mib,32,64*mib);
    struct cost_sort high=cost_sort_state(8*mib,32,256*mib);
    CHECK(small.spill==0);
    CHECK(low.spill>high.spill);
    CHECK(low.fan>=2 && low.fan<=32);
    CHECK(low.comparisons>0 && low.merge>0);
    CHECK(low.state==high.state);
    CHECK(isfinite(low.spill));
    puts("planner cost: parallel waves, packed hash/group layouts and external sort passed");
    return EXIT_SUCCESS;
}
