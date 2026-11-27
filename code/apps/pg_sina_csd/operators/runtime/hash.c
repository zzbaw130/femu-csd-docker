#include "graph_state.h"
#include "graph_batch.h"

uint64_t hash_key(uint64_t hash,struct qs_cell v)
{
    while(v.scale && !(v.value%10)) { v.value/=10; --v.scale; }
    return mix_hash(hash^(uint64_t)v.value^(uint64_t)((unsigned __int128)v.value>>64)^((uint64_t)v.scale<<56));
}
uint64_t row_hash(const struct qp_node *n,const struct qs_cell *row,int right)
{
    uint64_t hash=0x9e3779b97f4a7c15ULL;
    for(unsigned i=0;i<n->nkeys;++i) hash=hash_key(hash,row[right?n->right_keys[i]:n->left_keys[i]]);
    return hash;
}
void hash_key_layout(struct hash_key_layout *layout,const struct qp_node *n,const struct qs_plan *p,int right)
{
    layout->count=n->nkeys;
    for(unsigned k=0;k<n->nkeys;++k) {
        unsigned col=right?n->right_keys[k]:n->left_keys[k],offset=0;
        for(unsigned c=0;c<col;++c) { unsigned bytes=hash_value_bytes(p,c); offset+=bytes+(bytes<16?1:3); }
        layout->offsets[k]=offset; layout->widths[k]=hash_value_bytes(p,col);
    }
}
void hash_release(cs_work_context *ctx,struct operator_state *o)
{
    if(o->hash_blocks) {
        for(unsigned row=0;row<o->hash_capacity;row+=HASH_RELOAD_BLOCK_ROWS)
            cs_work_free(ctx,o->hash_blocks[row/HASH_RELOAD_BLOCK_ROWS]);
        cs_work_free(ctx,o->hash_blocks); o->hash_blocks=NULL;
    }
    if(o->hash_data) cs_work_free(ctx,o->hash_data);
    o->hash_data=NULL;
}
