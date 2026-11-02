/* Resource-model invariants, without a running PostgreSQL or CSD device. */
#include "cost_model.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(test) do { if(!(test)) { fprintf(stderr,"line %d: %s\n",__LINE__,#test); return EXIT_FAILURE; } } while(0)
int main(void)
{
    struct qs_plan expressions={.nexprs=4};
    expressions.exprs[0].op=Q_COLUMN; expressions.exprs[1].op=Q_CONST;
    expressions.exprs[2].op=Q_PARAMETER; expressions.exprs[3].op=Q_AGGREF;
    CHECK(cost_expression_ops(&expressions)==0);
    expressions.exprs[3].column=-1; /* Candidate operation, not an aggregate reference. */
    CHECK(cost_expression_ops(&expressions)==1);
    expressions.exprs[3].op=Q_MUL;
    CHECK(cost_expression_ops(&expressions)==1);
    CHECK(cost_population_fraction(true,true,1,1)==0);
    CHECK(cost_population_fraction(true,true,1,0)==1);
    CHECK(cost_population_fraction(false,true,0,1)==0);
    CHECK(cost_population_fraction(true,false,0,1)==1);
    struct cost_sort bounded=cost_topn_state(3000000,128,64*1024*1024,4,10);
    CHECK(bounded.spill==0 && bounded.state<1024*1024);
    CHECK(cost_topn_state(3000000,128,64*1024*1024,4,-1).spill>0);
    CHECK(cost_topn_state(3000000,128,64*1024*1024,4,3000000).spill>0);
    /* Shared hits remove I/O, not CPU. File-only hits still copy pages;
     * overlapping cache fractions must not discount the same I/O twice. */
    CHECK(30+cost_host_cached_io(100,0,0,0.1)==130);
    CHECK(30+cost_host_cached_io(100,1,0,0.1)==30);
    CHECK(30+cost_host_cached_io(100,1,1,0.1)==30);
    CHECK(30+cost_host_cached_io(100,0,1,0.1)==40);
    CHECK(cost_host_cached_io(100,0.5,0.5,0.1)==50);
    CHECK(cost_host_cached_io(100,0.5,1,0.1)==5);
    CHECK(cost_host_scan_io(100,10,20,5,0,0)==35);
    CHECK(cost_host_scan_io(100,10,20,5,0,1)==5);
    CHECK(cost_host_scan_io(100,10,20,5,1,1)==0);
    CHECK(cost_host_scan_io(100,10,20,5,0.5,1)==2.5);
    CHECK(cost_host_scan_io(0,10,20,5,0,1)==0);
    CHECK(cost_host_scan_io(10,10,20,5,0,0)==10);
    /* Bounded index caches must not make repeated random I/O free. A small
     * table can fit; clustered matching tuples share a Heap page. */
    CHECK(cost_first_match_visits(4,0)==4);
    CHECK(cost_first_match_visits(4,1)==1);
    CHECK(cost_first_match_visits(0,0.5)==0);
    CHECK(cost_first_match_visits(4,0.25)>2.73 && cost_first_match_visits(4,0.25)<2.74);
    CHECK(cost_first_match_visits(128,0.1)>9.99 && cost_first_match_visits(128,0.1)<10);
    CHECK(cost_index_pages(0,10000,64,256)==0);
    CHECK(cost_index_pages(257,100000,0,256)<258);
    CHECK(cost_index_pages(100000,1,64,256)<=1);
    CHECK(cost_index_pages(1000000,10000,64,256)>10000);
    CHECK(cost_index_pages(1000000,10000,64,256)>cost_index_pages(10000,10000,64,256));
    CHECK(cost_index_heap_visits(100,4,50,1)==100);
    CHECK(cost_index_heap_visits(100,4,50,0)==400);
    CHECK(cost_index_heap_visits(0,4,50,0)==0);
    CHECK(cost_index_window_misses(100,4,50,512,0)==100);
    CHECK(cost_index_window_misses(100,4,50,0,1)==100);
    CHECK(cost_index_window_misses(100,4,50,512,1)>0);
    CHECK(cost_index_window_misses(100,4,50,512,1)<1);
    CHECK(cost_index_window_misses(100,100000,50,512,1)==100);
    CHECK(cost_anti_fallback(0)==0);
    CHECK(cost_anti_fallback(1000)>1 && cost_anti_fallback(1000)<1000);
    /* A single block cannot speed up on eight CUs. Three blocks leave five
     * idle; an incomplete last wave costs more than ideal work/CU. */
    CHECK(cost_parallel(80,1,8,0.02)==80);
    CHECK(cost_parallel(30,3,8,0)==10);
    CHECK(cost_parallel(90,9,4,0)>90.0/4);
    CHECK(cost_parallel(80,8,4,0.02)>20);
    CHECK(cost_parallel(80,8,4,0.02)<cost_parallel(80,8,2,0.02));
    CHECK(cost_scan_overlap(40,80,8,4)==30);
    CHECK(cost_scan_overlap(40,80,1,4)==0);
    CHECK(cost_scan_overlap(40,80,8,1)==0);
    CHECK(cost_scan_overlap(40,0,8,4)==0);
    /* 4-byte PG integer + NULL, 8-byte integer + NULL, 128-bit numeric with
     * scale/dscale/NULL: packed 33 bytes, not three 32-byte cells. */
    struct qs_plan row={.nout=3};
    row.outputs[0].type_oid=23; row.outputs[1].type_oid=20; row.outputs[2].type_oid=1700;
    CHECK(cost_hash_row(&row)==33);
    CHECK(cost_hash_state(128,33)==128*45);
    CHECK(cost_hash_state(129,33)==256*45);
    /* A repeated-key existence build fits after compaction; unique or
     * unknown keys still need the general external path. Limits >=64MiB. */
    double existence_full=cost_hash_state(8000000,33);
    CHECK(cost_hash_existence_state(8000000,131072,33,64*1024*1024)<64*1024*1024);
    CHECK(cost_hash_existence_state(8000000,0,33,64*1024*1024)==existence_full);
    CHECK(cost_hash_existence_state(8000000,8000000,33,64*1024*1024)==existence_full);
    CHECK(cost_hash_existence_state(1000,10,33,64*1024*1024)==cost_hash_state(1000,33));
    struct qp_node group={.nkeys=1}; group.expr.naggs=1;
    group.expr.exprs[0].type=Q_INTEGER; group.expr.keys[0]=0;
    CHECK(cost_group_stride(&group)==64);
    CHECK(cost_group_state(&group,112)==128*(8+56)+128);
    CHECK(cost_group_state(&group,113)>cost_group_state(&group,112));
    CHECK(cost_group_resident(&group,10000000,256*1024*1024)==0x1p21*64+128);
    CHECK(cost_group_resident(&group,100,64*1024*1024)==cost_group_state(&group,100));
    /* Sort-child quota under a 256MiB query, not a device workmem setting. */
    CHECK(cost_sort_memory(128*1024*1024,16*1024*1024)==96*1024*1024);
    struct cost_sort small_merge=cost_sort_state(12000000,96,16*1024*1024,1);
    struct cost_sort borrowed_merge=cost_sort_state(12000000,96,96*1024*1024,1);
    CHECK(small_merge.passes>borrowed_merge.passes && small_merge.spill>borrowed_merge.spill);
    struct qs_plan output={.nout=2}; output.outputs[1].index=1;
    CHECK(qp_result_capacity(&output,0)==256);
    uint64_t completion=QP_TYPED_OUTPUT|QP_OUTPUT_READY|QP_INPUT_DRAINED|
        (UINT64_C(116508)<<5)|(UINT64_C(65536)<<22);
    CHECK(qp_output_rows(completion)==116508 && qp_output_strings(completion)==65536);
    CHECK((completion&31)==(QP_OUTPUT_READY|QP_INPUT_DRAINED));
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
    /* A wide packet's packed payload crosses the old str_bytes position.
     * The completion length, not that overwritten field, must drive unpack. */
    struct qs_plan wide={.nout=QS_OUT};
    for(unsigned col=0;col<QS_OUT;++col) wide.outputs[col].type_oid=col+1==QS_OUT?25:20;
    packet->rows=1000;
    unsigned fixed=qp_host_bytes(&wide,packet->rows),strings=32768;
    CHECK(fixed<=sizeof(packet->data));
    memset(packet->data,0x5a,fixed);
    for(unsigned i=0;i<strings;++i) packet->str_data[i]=(unsigned char)i;
    qp_host_pack(packet,&wide,strings);
    memcpy(slice,packet,offsetof(struct qs_result,data)+fixed+strings);
    qp_host_unpack(slice,&wide,strings);
    CHECK(slice->str_bytes==strings && !memcmp(packet->data,slice->data,fixed));
    for(unsigned i=0;i<strings;++i) CHECK((unsigned char)slice->str_data[i]==(unsigned char)i);
    free(slice);
    free(packet);
    /* Filtering borrowed Heap text need not intern the output. Numeric
     * conversion and stateful predicates retain the old projection path. */
    struct qp_plan *filtered=calloc(1,sizeof(*filtered)); CHECK(filtered);
    filtered->packed_result=QP_DIRECT_RESULT; filtered->count=1;
    CHECK(qp_plan_constant_bytes(filtered)<sizeof(*filtered)/32);
    /* Runtime-only file maps and timings must not travel in PG's Const. */
    filtered->files[0].count=17; filtered->pipelines[0].wall_ns=42;
    struct qp_plan *expanded=calloc(1,sizeof(*expanded)); CHECK(expanded);
    memcpy(expanded,filtered,qp_plan_constant_bytes(filtered));
    CHECK(expanded->count==1 && expanded->packed_result==QP_DIRECT_RESULT);
    CHECK(!expanded->files[0].count && !expanded->pipelines[0].wall_ns);
    free(expanded);
    filtered->nparams=3; filtered->param_nodes[2]=QP_HOST_PARAMETER;
    CHECK(qp_host_parameter_bytes(filtered)==3*sizeof(struct qs_cell));
    filtered->param_nodes[2]=0;
    CHECK(!qp_host_parameter_bytes(filtered)); /* Device-owned InitPlans need no HOST snapshot. */
    struct qs_plan *projection=&filtered->nodes[0].expr;
    projection->nattrs=2; projection->nexprs=4; projection->nout=1; projection->predicate=3;
    projection->cols[0].type=Q_DECIMAL; projection->cols[1].type=Q_INTEGER;
    projection->exprs[0]=(struct qs_expr){.op=Q_COLUMN,.column=0};
    projection->exprs[1]=(struct qs_expr){.op=Q_COLUMN,.column=1};
    projection->exprs[2]=(struct qs_expr){.op=Q_CONST,.type=Q_INTEGER,.constant=100};
    projection->exprs[3]=(struct qs_expr){.op=Q_LE,.left=1,.right=2};
    projection->outputs[0].type_oid=1700;
    CHECK(qp_host_raw_projection(filtered));
    projection->cols[1].type=Q_DECIMAL; CHECK(!qp_host_raw_projection(filtered));
    projection->cols[1].type=Q_STRING; CHECK(qp_host_raw_projection(filtered));
    projection->exprs[3].op=Q_LIKE; CHECK(qp_host_raw_projection(filtered));
    projection->exprs[3].op=Q_INSET; CHECK(!qp_host_raw_projection(filtered));
    free(filtered);
    struct qp_plan scan={.count=1};
    scan.nodes[0].kind=QP_SCAN; scan.nodes[0].left=scan.nodes[0].right=-1; scan.nodes[0].bytes=8*1024*1024;
    qp_parallel_boundaries(&scan,0); CHECK(scan.nodes[0].parallel);
    CHECK(qp_task_slots(&scan,4)==4); /* Scan-only needs no extra I/O contexts. */
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
    CHECK(cost_join_expression(&scan,0,0,1,false));
    scan.nodes[0].expr.exprs[0].column=1;
    CHECK(!cost_join_expression(&scan,0,0,1,false));
    CHECK(cost_join_expression(&scan,0,0,1,true));
    scan.nodes[1].kind=QP_JOIN; scan.nodes[1].join_type=QP_INNER; scan.nodes[1].nkeys=1;
    CHECK(cost_join_expression(&scan,0,0,1,false)); /* Equal key forwarded from the other side. */
    scan.nodes[1].join_type=QP_LEFT;
    CHECK(!cost_join_expression(&scan,0,0,1,false)); /* NULL extension breaks that equality. */
    scan.nodes[1].kind=QP_INDEX_JOIN;
    scan.nodes[1].parallel=1; scan.nodes[2].left=-1;
    CHECK(qp_task_slots(&scan,4)==8);
    CHECK(qp_task_slots(&scan,8)==16);
    CHECK(qp_task_slots(&scan,64)==QS_WORKERS);
    scan.nodes[1].parallel=0;
    CHECK(qp_task_slots(&scan,4)==4); /* A serial index branch changes nothing. */
    /* Hide scalar probe waits without multiplying grouped-state workspaces. */
    scan.nodes[0].kind=QP_AGGREGATE;
    scan.nodes[1].kind=QP_JOIN; scan.nodes[1].hash=scan.nodes[1].parallel=1;
    CHECK(qp_task_slots(&scan,4)==8);
    scan.nodes[0].nkeys=1;
    CHECK(qp_task_slots(&scan,4)==4);
    scan.nodes[0].nkeys=0; scan.nodes[3].kind=QP_AGGREGATE; scan.nodes[3].nkeys=1;
    CHECK(qp_task_slots(&scan,4)==4); /* Inner grouping also retains its budget. */
    /* Only supported >=64MiB conditions. More workmem reduces spill, and a
     * dataset larger than all memory still requires external merge. */
    double mib=1024*1024;
    CHECK(cost_sort_workspace()>5*mib && cost_sort_workspace()<8*mib);
    struct cost_sort small=cost_sort_state(1000,32,64*mib,1);
    struct cost_sort low=cost_sort_state(8*mib,32,64*mib,1);
    struct cost_sort high=cost_sort_state(8*mib,32,256*mib,1);
    CHECK(small.spill==0);
    CHECK(low.spill>high.spill);
    CHECK(low.fan>32 && high.fan>low.fan);
    struct cost_sort text=cost_sort_state(8*mib,32,64*mib,2);
    CHECK(text.fan<low.fan && text.spill>=low.spill);
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
