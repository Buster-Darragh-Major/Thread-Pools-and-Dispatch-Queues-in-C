#include <stdio.h>
#include <sys/sysinfo.h>

int main(int argc, char *argv[])
{
    printf("This machine has %d cores.\n", get_nprocs());
}