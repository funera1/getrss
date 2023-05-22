#include <linux/ioctl.h>
#include <unistd.h>

static struct module_values {
    pid_t pid;
    unsigned long addr_start;
    unsigned long addr_end;
};
