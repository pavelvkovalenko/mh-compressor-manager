#include "async_io.h"
#include <unistd.h>
#include <cerrno>

ssize_t AsyncIO::sync_read_full(int fd, void* buffer, size_t size) {
    ssize_t total_read = 0;
    uint8_t* buf_ptr = static_cast<uint8_t*>(buffer);

    while (total_read < static_cast<ssize_t>(size)) {
        ssize_t bytes = read(fd, buf_ptr + total_read, size - total_read);
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return -1;
        }
        if (bytes == 0) {
            break;  // EOF
        }
        total_read += bytes;
    }

    return total_read;
}
