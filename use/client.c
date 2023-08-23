#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#define RSS_RANGE 0
#define NEXT_VMA_PTR 1

static struct module_values {
    pid_t pid;
    unsigned long addr_start;
    unsigned long addr_end;
};


int main(int argc, char* argv)
{
    struct module_values values;
    // scanf("%d", &values.pid);
    // scanf("%lu", &values.addr_start);
    // scanf("%lu", &values.addr_end);
    values.pid = 1704;

    // Kbytes 160
    values.addr_start = 0x00007f421ec28000;
    values.addr_end = values.addr_start + 10;
    // values.addr_end = 0x00007fc00d9bd000-100;

    int fd;
    if ((fd = open("/dev/rss_range", O_RDWR)) < 0) perror("open");
    
    int ret;
    values.addr_start = 0x100;

        if ((ret = ioctl(fd, NEXT_VMA_PTR, &values)) < 0) perror("can't get next_vma_ptr");

        values.addr_start = 0x00007f31ba1b4000;
        values.addr_end   = values.addr_start + 0x10000;

        ret = ioctl(fd, RSS_RANGE, &values);
        if (ret < 0) perror("can't get rss_range");

        printf("Rss = %d\n", ret);
        values.addr_start = values.addr_end;

    if (close(fd) != 0) perror("close");
    return 0;
}
