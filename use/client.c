#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

static struct module_values {
    pid_t pid;
    unsigned long addr_start;
    unsigned long addr_end;
};

int main()
{
    int fd;
    struct module_values values;
    scanf("%d", &values.pid);
    // scanf("%lu", &values.addr_start);
    // scanf("%lu", &values.addr_end);
    values.addr_start = 0x00007ff023c1b000;
    values.addr_end   = values.addr_start + 100;

    if ((fd = open("/dev/rss_range", O_RDWR)) < 0) perror("open");

    int ret;
    if (ret = ioctl(fd, 0, &values) < 0) perror("ioctl_set");

    printf("Rss = %d\n", ret);

    if (close(fd) != 0) perror("close");
    return 0;
}
