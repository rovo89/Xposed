#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>

static void replaceAsm(void* function, char* newCode, int len) {
    function = (void*)((int)function & ~1);
    void* pageStart = (void*)((int)function & ~(PAGESIZE-1));
    mprotect(pageStart, PAGESIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(function, newCode, len);
    mprotect(pageStart, PAGESIZE, PROT_READ | PROT_EXEC);
    __clear_cache(function, (char*)function+len);
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
    
    char asmReturn42[] = { 42, 0x20, 0x70, 0x47 };
    replaceAsm((void*) asmReplaceTest,  asmReturn42, sizeof(asmReturn42));
    
    result = asmReplaceTest();
    if (result != 42) {
        printf("ERROR: Result of second call is %d instead of 42\n", result);
        return 1;
    }
    
    printf("OK\n");
    return 0;
}

