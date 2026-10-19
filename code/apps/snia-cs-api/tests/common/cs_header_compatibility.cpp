#include "cs.h"
int main()
{
    int length=0;
    return csQueryLibrarySupport(CS_FILE_SYSTEMS_SUPPORTED,&length,nullptr)==CS_INVALID_LENGTH && length==1 ? 0 : 1;
}
