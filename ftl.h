#ifndef _NVMEVIRT_FTL_H
#define _NVMEVIRT_FTL_H

#include <linux/types.h>
#include "queue.h"
#include "pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int chunk;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;

    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    // //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;

    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};


struct write_flow_control {
    int write_credits;
    int credits_to_refill;
};

struct conv_ssd {
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ssd_pcie *pcie;
    struct buffer *write_buffer;
    unsigned int cpu_nr_dispatcher;

    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct write_pointer gc_wp;
    struct line_mgmt lm;
    struct write_flow_control wfc;
};

extern struct conv_ssd g_conv_ssd[SSD_PARTITIONS];

unsigned long ssd_init(unsigned int cpu_nr_dispatcher, unsigned long memmap_size);

bool ssd_read(struct nvme_request * req, struct nvme_result * ret);
bool ssd_write(struct nvme_request * req, struct nvme_result * ret);
bool ssd_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret);
void ssd_flush(struct nvme_request * req, struct nvme_result * ret);
void ssd_gc_bg(void);
void ssd_gc(struct conv_ssd *ssd);

#endif
