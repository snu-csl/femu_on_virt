#ifndef _NVMEVIRT_FTL_H
#define _NVMEVIRT_FTL_H

#include <linux/types.h>
#include "queue.h"
#include "pqueue.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

/*
    Default malloc size
    Channel = 40 * 8 = 320
    LUN     = 40 * 8 = 320
    Plane   = 16 * 1 = 16
    Block   = 32 * 256 = 8192
    Page    = 16 * 256 = 4096
    Sector  = 4 * 8 = 32

    Line    = 40 * 256 = 10240
    maptbl  = 8 * 4194304 = 33554432
    rmap    = 8 * 4194304 = 33554432
*/



enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,
    NAND_NOP = 3,

    #if SUPPORT_ZNS
    NAND_READ_LATENCY = 40950,
    NAND_PROG_LATENCY = 10000,
    NAND_ERASE_LATENCY = 0,
    FW_READ_LATENCY = (37540 - 7390),
    FW_PROG_LATENCY = 0,
    FW_XFER_LATENCY = 413,
    #else
    NAND_READ_LATENCY = 10000,
    NAND_PROG_LATENCY = 10000,
    NAND_ERASE_LATENCY = 2000000,
    FW_READ_LATENCY = 0,
    FW_PROG_LATENCY = 0,
    FW_XFER_LATENCY = 0,
    #endif
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

#define PG_OFFS_BITS   (4) // 16 pages -> 1 wordline
#define WORDLINE_BITS  (PG_BITS - PG_OFFS_BITS)
/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t pg  : PG_BITS;
            uint64_t blk : BLK_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        struct {
            uint64_t pg_offs : PG_OFFS_BITS;
            uint64_t wordline : WORDLINE_BITS;
            uint64_t blk_in_die : BLK_BITS + PL_BITS + LUN_BITS + CH_BITS;
            uint64_t rsv : 1;
        } h;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t gc_endtime;
    struct channel_model * perf_model;
};

struct ssd_pcie {
    struct channel_model * perf_model;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_flash_pg; /* # of pgs per flash page */
    int flash_pgs_per_blk; /* # of flash pages per block */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */
    int fw_rd_lat;       /* Firmware overhead of read in nanoseconds */
    int fw_wr_lat;       /* Firmware overhead of write in nanoseconds */
    int fw_xfer_lat;     /* Firmware overhead of data transfer in nanoseconds */

    uint64_t ch_bandwidth; /*NAND CH Maximum bandwidth in MB/s*/
    uint64_t pcie_bandwidth; /*PCIE Maximum bandwidth in MB/s*/

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    double op_area_pcent;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */

    int pba_pcent;    /* (physical space / logical space) * 100*/    
};

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

struct nand_cmd {
    int type;
    int cmd;
    int64_t xfer_size; // byte
    int64_t stime; /* Coperd: request arrival time */
};

struct ssd {
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ssd_pcie *pcie;

    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;
    unsigned int cpu_nr_dispatcher;

    // /* lockless ring for communication with NVMe IO thread */
    // struct rte_ring **to_ftl;
    // struct rte_ring **to_poller;
    // bool *dataplane_started_ptr;
    // QemuThread ftl_thread;
};

unsigned long ssd_init(unsigned int cpu_nr_dispatcher, unsigned long memmap_size);
uint64_t ssd_read(struct nvme_command *cmd, unsigned long long nsecs_start);
uint64_t ssd_write(struct nvme_command *cmd, unsigned long long nsecs_start);
bool should_gc(void);
int do_gc(bool force);
void adjust_ftl_latency(int target, int lat);

/* Macros for specific setting. Modify these macros for your target */
#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3200ull) //MB/s

struct nvme_request {
    struct nvme_command * cmd;
    __u64 nsecs_start;
};

struct nvme_result {
    __u32 status;
    __u64 nsecs_target;
    __u64 wp; // only for zone append
};

extern struct ssd ssd;

uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd);
inline uint64_t ssd_advance_pcie(struct ssd *ssd, __u64 request_time, __u64 length) ;
#endif
