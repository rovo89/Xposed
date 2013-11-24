#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

static void replaceAsm(uintptr_t function, unsigned const char* newCode, size_t len) {
#ifdef __arm__
    function = function & ~1;
#endif
    uintptr_t pageStart = function & ~(PAGESIZE-1);
    size_t pageProtectSize = PAGESIZE;
    if (function+len > pageStart+pageProtectSize)
        pageProtectSize += PAGESIZE;

    mprotect((void*)pageStart, pageProtectSize, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy((void*)function, newCode, len);
    mprotect((void*)pageStart, pageProtectSize, PROT_READ | PROT_EXEC);

    __clear_cache((void*)function, (void*)(function+len));
}

static int asmReplaceTest()
{
    return 123;
}

int main(int argc, const char* const argv[]) {
    int result;
    
    result = asmReplaceTest();
    if (result != 123) {
        printf("ERROR: Result of first call is %d instead of 123\n", result);
        return 1;
    }
    
#ifdef __arm__
    unsigned const char asmReturn42[] = { 42, 0x20, 0x70, 0x47 };
#else
    unsigned const char asmReturn42[] = { 0xB8, 42, 0x00, 0x00, 0x00, 0xC3 };
#endif
    replaceAsm((uintptr_t) asmReplaceTest,  asmReturn42, sizeof(asmReturn42));
    
    result = asmReplaceTest();
    if (result != 42) {
        printf("ERROR: Result of second call is %d instead of 42\n", result);
        return 1;
    }
    
    printf("OK\n");
    return 0;
}

