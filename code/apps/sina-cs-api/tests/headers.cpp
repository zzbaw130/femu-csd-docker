#include "cs.sh"
#include "cs_kernel.h"
static_assert(sizeof(femu_csd_args) == 52, "original FEMU ABI");
int main()
{
    int length=0;
    return csQueryLibrarySupport(CS_FILE_SYSTEMS_SUPPORTED,&length,nullptr)==CS_INVALID_LENGTH && length==1 ? 0 : 1;
}
