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
    struct qs_plan output={.nout=2}; output.outputs[1].index=1;
    CHECK(qp_result_capacity(&output,0)==256);
    CHECK(qp_result_capacity(&output,1)==2048);
    output.exprs[0].type=Q_STRING; output.outputs[0].typmod=259;
    CHECK(qp_result_capacity(&output,1)==257); /* bounded 64KiB text */
    output.exprs[0].type=Q_INTEGER; output.exprs[3].type=Q_STRING;
    output.outputs[0].aggregate=1; output.aggs[0]=(struct qs_agg){.kind=Q_MIN,.expr=3};
    CHECK(qp_result_capacity(&output,1)==257);
    output.aggs[0].kind=Q_COUNT;
    CHECK(qp_result_capacity(&output,1)==2048);
    /* Typed integer columns fit many more rows than 32-byte cell output.
     * Compact a partial batch, including its final partial NULL byte. */
    struct qs_plan typed={.nout=2}; typed.outputs[0].type_oid=23; typed.outputs[1].type_oid=20;
    unsigned capacity=qp_host_capacity(&typed); CHECK(capacity>10000);
    struct qs_result *packet=calloc(1,sizeof(*packet)); CHECK(packet);
    packet->rows=513;
    for(unsigned col=0;col<2;++col) for(unsigned row=0;row<packet->rows;++row) {
        int64_t value=-(int64_t)row;
        memcpy(qp_host_values(packet,&typed,capacity,col)+(size_t)row*qp_host_width(typed.outputs[col].type_oid),&value,qp_host_width(typed.outputs[col].type_oid));
        if(row%17==0) qp_host_nulls(packet,&typed,capacity,col)[row/8]|=1U<<(row%8);
    }
    qp_host_compact(packet,&typed,capacity);
    for(unsigned row=0;row<packet->rows;++row) {
        CHECK(qp_host_isnull(packet,&typed,row,0)==(row%17==0));
        int32_t narrow; int64_t wide;
        memcpy(&narrow,qp_host_values(packet,&typed,packet->rows,0)+(size_t)row*4,4);
        memcpy(&wide,qp_host_values(packet,&typed,packet->rows,1)+(size_t)row*8,8);
        CHECK(narrow==-(int32_t)row && wide==-(int64_t)row);
    }
    struct qs_result *slice=calloc(1,sizeof(*slice)); CHECK(slice);
    CHECK(!qp_host_slice(slice,packet,&typed,7,256));
    for(unsigned row=0;row<256;++row) {
        CHECK(qp_host_isnull(slice,&typed,row,1)==((row+7)%17==0));
        int64_t value; memcpy(&value,qp_host_values(slice,&typed,256,1)+(size_t)row*8,8);
        CHECK(value==-(int64_t)(row+7));
    }
    /* Native varlena values survive a claim with non-byte-aligned NULLs;
     * consumers copy only the claimed strings, not the complete pool. */
    typed.outputs[1].type_oid=25; packet->str_bytes=0;
    for(unsigned row=0;row<packet->rows;++row) {
        unsigned offset=(packet->str_bytes+3)&~3U,bytes=4+row%97;
        uint32_t header=bytes<<2;
        memcpy(packet->str_data+offset,&header,4);
        memset(packet->str_data+offset+4,'a'+row%26,bytes-4);
        memcpy(qp_host_values(packet,&typed,packet->rows,1)+(size_t)row*4,&offset,4);
        packet->str_bytes=offset+bytes;
    }
    memset(qp_host_nulls(packet,&typed,packet->rows,1),0,(packet->rows+7)/8);
    for(unsigned row=0;row<packet->rows;row+=17) qp_host_nulls(packet,&typed,packet->rows,1)[row/8]|=1U<<(row%8);
    CHECK(!qp_host_slice(slice,packet,&typed,7,256));
    CHECK(slice->str_bytes<packet->str_bytes);
    for(unsigned row=0;row<slice->rows;++row) {
        CHECK(qp_host_isnull(slice,&typed,row,1)==((row+7)%17==0));
        if(qp_host_isnull(slice,&typed,row,1)) continue;
        unsigned bytes;
        const char *value=qp_host_var(slice,qp_host_values(slice,&typed,slice->rows,1)+(size_t)row*4,&bytes);
        CHECK(value && bytes==4+(row+7)%97);
        for(unsigned i=4;i<bytes;++i) CHECK(value[i]==(char)('a'+(row+7)%26));
    }
    const unsigned starts[]={0,1,7,8,255,512},lengths[]={1,7,8,9,256,513};
    for(unsigned i=0;i<sizeof(starts)/sizeof(*starts);++i)
        for(unsigned j=0;j<sizeof(lengths)/sizeof(*lengths);++j) {
            unsigned first=starts[i],rows=lengths[j];
            if(rows>packet->rows-first) rows=packet->rows-first;
            CHECK(!qp_host_slice(slice,packet,&typed,first,rows));
            for(unsigned row=0;row<rows;++row)
                CHECK(qp_host_isnull(slice,&typed,row,1)==((row+first)%17==0));
            if(rows%8) CHECK(!(qp_host_nulls(slice,&typed,rows,1)[rows/8]&~((1U<<(rows%8))-1)));
        }
    free(slice);
    free(packet);
    struct qp_plan scan={.count=1};
    scan.nodes[0].kind=QP_SCAN; scan.nodes[0].left=scan.nodes[0].right=-1; scan.nodes[0].bytes=8*1024*1024;
    qp_parallel_boundaries(&scan,0); CHECK(scan.nodes[0].parallel);
    scan.nodes[0].parallel=0; scan.nodes[0].ordered_index=1;
    qp_parallel_boundaries(&scan,0); CHECK(!scan.nodes[0].parallel);
    memset(&scan,0,sizeof(scan)); scan.count=4;
    scan.nodes[0].left=1; scan.nodes[0].expr.nexprs=1;
    scan.nodes[0].expr.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0};
    scan.nodes[1].kind=QP_INDEX_JOIN; scan.nodes[1].left=2; scan.nodes[1].right=3;
    scan.nodes[1].expr.nout=scan.nodes[1].expr.nexprs=2;
    scan.nodes[1].expr.outputs[1].index=1;
    scan.nodes[1].expr.exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0};
    scan.nodes[1].expr.exprs[1]=(struct qs_expr){.op=Q_COLUMN,.column=1};
    scan.nodes[2].expr.nout=scan.nodes[3].expr.nout=1;
    CHECK(cost_probe_expression(&scan,0,0,1));
    scan.nodes[0].expr.exprs[0].column=1;
    CHECK(!cost_probe_expression(&scan,0,0,1));
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
    CHECK(low.record_copies==8*mib*(low.passes+1));
    CHECK(cost_partial_rows(1000,100,100,0)<1000);
    CHECK(cost_partial_rows(1000,100,10,0)>cost_partial_rows(1000,100,100,0));
    CHECK(cost_partial_rows(1000,100,10,1)==100);
    CHECK(cost_partial_rows(1000,100,10,-1)==100);
    CHECK(cost_partial_rows(0,100,10,0)==0);
    CHECK(cost_spill_io(1024*1024,5000,1000)>cost_spill_io(1024*1024,5000,5000));
    puts("planner cost: parallel waves, packed hash/group layouts and external sort passed");
    return EXIT_SUCCESS;
}
