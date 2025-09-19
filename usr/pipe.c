#include "syscall.h"
#include "error.h"
#include "string.h"

void main(void) {
    char buf[6];
    int wfd = -1;
    int rfd = -1;
    _pipe(&wfd, &rfd);
    if (_fork()) { // parent
        _close(rfd);
        strncpy(buf, "hello", 6);
        _write(wfd, buf, 6);
        _wait(0); // wait for child
    } else { // child
        _close(wfd);
        _read(rfd, buf, 6);
        buf[5] = '\0'; // null-terminate
        _print(buf);
    }
}