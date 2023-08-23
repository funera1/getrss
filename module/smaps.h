void smap_gather_stats_range(struct vm_area_struct *vma,
        struct mem_size_stats *mss, unsigned long start, unsigned long end);
int get_smaps_range(struct task_struct* task, struct mem_size_stats* mss, unsigned long start, unsigned long end);
void __show_smap(const struct mem_size_stats *mss);
