#include "syscall.h"
#include "error.h"
#include "string.h"


// This function takes in one argument: the thing to print

void main(int argc, char** argv) {
    char * msg = argv[0]; 
    _print(msg);
    _print(msg);
}