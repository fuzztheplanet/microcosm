#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
    const char *value=getenv("MC_PACK_CHECK");
    if(argc!=2 || strcmp(argv[1],"forwarded") || !value || strcmp(value,"present"))return 99;
    puts("packed launch: arguments/environment preserved");
    return 17;
}
