/* See rand.h. */
#include "magitrickle/rand.h"

#include <fcntl.h>
#include <unistd.h>

mt_err_t mt_random_bytes(uint8_t *buf, size_t len) {
    /* /dev/urandom works from kernel 2.6 onward; getrandom(2) needs 3.17+
     * and some Entware targets run 3.2/3.4, so read the device directly
     * (matches config/id.c's mt_id_random, which this now backs). */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) { return MT_ERR_SYS; }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) { break; }
        got += (size_t)n;
    }
    close(fd);
    return got == len ? MT_OK : MT_ERR_SYS;
}
