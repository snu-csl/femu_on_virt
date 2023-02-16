#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

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
    int pg;
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

struct conv_ftl {

    union {
        struct ssd ssd;

        struct {
            STRUCT_SSD_ENTRIES
        };
    };

    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct write_pointer gc_wp;
    struct line_mgmt lm;
    struct write_flow_control wfc;
};

struct conv_ftl * conv_create_and_init(uint64_t capacity, uint32_t cpu_nr_dispatcher);

bool conv_proc_nvme_io_cmd(struct conv_ftl conv_ftls[], struct nvme_request *req, struct nvme_result *ret);
bool conv_read(struct conv_ftl conv_ftls[], struct nvme_request *req, struct nvme_result *ret);
bool conv_write(struct conv_ftl conv_ftls[], struct nvme_request *req, struct nvme_result *ret);
void conv_flush(struct conv_ftl conv_ftls[], struct nvme_request *req, struct nvme_result *ret);

#endif
