#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pid.h>
#include <linux/pagewalk.h>
#include <linux/pagemap.h>
#include <linux/kallsyms.h>
#include <linux/compiler-gcc.h>

static struct module_values {
    pid_t pid;
    unsigned long addr_start;
    unsigned long addr_end;
};
