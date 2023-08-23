#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define PAGE_SIZE 0x1000

static void print_page(uint64_t address, uint64_t data) {
    printf("0x%-16lx : pfn %-16lx soft-dirty %ld file/shared %ld "
        "swapped %ld present %ld\n",
        address,
        data & 0x7fffffffffffff,
        (data >> 55) & 1,
        (data >> 61) & 1,
        (data >> 62) & 1,
        (data >> 63) & 1);
}

struct mapping_addr {
    uint64_t start_addr;
    uint64_t end_addr;
    struct mapping_addr *next;
};

struct page_info {
    uint64_t pfn;
    int soft_dirty;
    int shared;
    int swapped;
    int present;
};

void parse_page(struct page_info* pinfo, uint64_t data) {
    pinfo->pfn = data & 0x7fffffffffffff;
    pinfo->soft_dirty = (data >> 55) & 1;
    pinfo->shared = (data >> 61) & 1;
    pinfo->swapped = (data >> 62) & 1;
    pinfo->present = (data >> 63) & 1;
}


int main(int argc, char *argv[]) {
    char filename[BUFSIZ];
    if(argc != 4) {
        printf("Usage: %s pid start_address end_address\n",
            argv[0]);
        return 1;
    }

    errno = 0;
    int pid = (int)strtol(argv[1], NULL, 0);
    if(errno) {
        perror("strtol");
        return 1;
    }
    snprintf(filename, sizeof filename, "/proc/%d/pagemap", pid);

    int pagemap_fd = open(filename, O_RDONLY);
    if(pagemap_fd < 0) {
        perror("open pagemap");
        return 1;
    }

    int kpagecount_fd = open("/proc/kpagecount", O_RDONLY);
    if(kpagecount_fd < 0) {
        perror("open kpagecount");
        return 1;
    }

    // TODO: /proc/pid/mapsを使ってmappingされてない部分の探索を省く
    // snprintf(filename, sizeof filename, "/proc/%d/maps", pid);
    // FILE* maps_stream = fopen(filename, O_RDONLY);
    // if (maps_stream == NULL) {
    //     perror("open maps");
    //     return 1;
    // }
    // struct mapping_addr* list_maps = NULL;
    // struct mapping_addr* current_maps;
    // while( !feof(maps_stream) ) {
    // 
    // }


    uint64_t start_address = strtoul(argv[2], NULL, 0);
    uint64_t end_address = strtoul(argv[3], NULL, 0);

    int vss = 0;
    int rss = 0;
    int pss = 0;
    int uss = 0;
    int swap = 0;

    for(uint64_t i = start_address; i < end_address; i += PAGE_SIZE) {
        uint64_t data;
        uint64_t index = (i / PAGE_SIZE) * sizeof(data);
        if(pread(pagemap_fd, &data, sizeof(data), index) != sizeof(data)) {
            perror("pread");
            break;
        }
        struct page_info pinfo;
        parse_page(&pinfo, data);

        uint64_t count = 1;
        if(pread(kpagecount_fd, &count, sizeof(count), pinfo.pfn*sizeof(uint64_t)) != sizeof(count)) {
            perror("pread kpagecount");
            break;
        }

        vss += PAGE_SIZE;
        rss += pinfo.present ? PAGE_SIZE : 0;
        pss += pinfo.present ? PAGE_SIZE/count : 0;
        uss += !pinfo.shared ? PAGE_SIZE : 0;
        swap += pinfo.swapped ? PAGE_SIZE : 0;
        
        // print_page(i, data);
    }

    printf("Vss: %dKB\n", vss/1024);
    printf("Rss: %dKB\n", rss/1024);
    printf("Pss: %dKB\n", pss/1024);
    printf("Swp: %dKB\n", swap/1024);

    close(pagemap_fd);
    return 0;
}
