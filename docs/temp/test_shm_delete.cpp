#include <errno.h>
#include <fcntl.h> /* For O_* constants */
#include <sys/mman.h>
#include <sys/stat.h> /* For mode constants */

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main() {
    int test_fd = shm_unlink("/libatbus-test-shm.bus");
    if (test_fd < 0) {
        printf("shm_unlink failed, err: %d, %s", errno, strerror(errno));
        return 1;
    }

    return 0;
}
