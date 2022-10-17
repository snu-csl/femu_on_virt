#ifndef _NVMEVIRT_FTL_H
#define _NVMEVIRT_FTL_H

#include <linux/types.h>
#include "queue.h"
#include "pqueue.h"
#include "ssd_config.h"

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
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    CHUNK_FREE = 0,
    CHUNK_INVALID = 1,
    CHUNK_VALID = 2
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

#define TOTAL_PPA_BITS (64)
#define BLK_BITS    (16)
#define CHUNK_BITS  (16)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (8)
#define RSB_BITS    (TOTAL_PPA_BITS - (BLK_BITS + CHUNK_BITS + PL_BITS + LUN_BITS + CH_BITS))

#if (READ_PAGE_SIZE == (64*1024))
#define CHUNK_OFFS_BITS   (4) // 16 pages -> 1 wordline
#elif (READ_PAGE_SIZE == (32*1024))
#define CHUNK_OFFS_BITS   (3) // 8 pages -> 1 wordline
#endif
#define WORDLINE_BITS  (CHUNK_BITS - CHUNK_OFFS_BITS)
/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t chunk  : CHUNK_BITS;
            uint64_t blk : BLK_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : RSB_BITS;
        } g;

        struct {
            uint64_t chunk_offs : CHUNK_OFFS_BITS;
            uint64_t wordline : WORDLINE_BITS;
            uint64_t blk_in_die : BLK_BITS + PL_BITS + LUN_BITS + CH_BITS;
            uint64_t rsv : 1;
        } h;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_chunk {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_chunk *chunk;
    int nchunks;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    uint64_t next_pln_avail_time;
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
    int secs_per_chunk;  /* # of sectors per page */
    int chunksz;
    int chunks_per_read_pg; /* # of pgs per flash page */
    int read_pgs_per_blk; /* # of flash pages per block */
    int chunks_per_pgm_pg; /* # of pgs per oneshot program page */
    int pgm_pgs_per_blk; /* # of pgm page pages per block */
    int chunks_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    /* Unit size of NVMe write command 
       Transfer size should be multiple of it */
    int write_unit_size;  

    int pg_4kb_rd_lat;/* NAND page 4KB read latency in nanoseconds */
    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */
    int ch_max_xfer_size;

    int fw_rd0_lat;       /* Firmware overhead of read 0 of read in nanoseconds */
    int fw_rd1_lat;       /* Firmware overhead of read 1 of read in nanoseconds */
    int fw_rd0_size;
    int fw_wr0_lat;       /* Firmware overhead of write in nanoseconds */
    int fw_wr1_lat;       /* Firmware overhead of write in nanoseconds */
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
    unsigned long secs_per_blk; /* # of sectors per block */
    unsigned long secs_per_pl;  /* # of sectors per plane */
    unsigned long secs_per_lun; /* # of sectors per LUN */
    unsigned long secs_per_ch;  /* # of sectors per channel */
    unsigned long tt_secs;      /* # of sectors in the SSD */

    unsigned long chunks_per_pl;   /* # of pages per plane */
    unsigned long chunks_per_lun;  /* # of pages per LUN (Die) */
    unsigned long chunks_per_ch;   /* # of pages per channel */
    unsigned long tt_chunks;       /* total # of pages in the SSD */

    unsigned long blks_per_lun; /* # of blocks per LUN */
    unsigned long blks_per_ch;  /* # of blocks per channel */
    unsigned long tt_blks;      /* total # of blocks in the SSD */

    unsigned long secs_per_line;
    unsigned long chunks_per_line;
    unsigned long blks_per_line;
    unsigned long tt_lines;

    unsigned long pls_per_ch;   /* # of planes per channel */
    unsigned long tt_pls;       /* total # of planes in the SSD */

    unsigned long tt_luns;      /* total # of LUNs in the SSD */

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

struct nand_cmd {
    int type;
    int cmd;
    int64_t xfer_size; // byte
    int64_t stime; /* Coperd: request arrival time */
};

struct write_flow_control {
    int write_credits;
    int credits_to_refill;
};

struct ssd {
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ssd_pcie *pcie;

    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct write_pointer gc_wp;
    struct line_mgmt lm;
    struct write_flow_control wfc;
    unsigned int cpu_nr_dispatcher;

    // /* lockless ring for communication with NVMe IO thread */
    // struct rte_ring **to_ftl;
    // struct rte_ring **to_poller;
    // bool *dataplane_started_ptr;
    // QemuThread ftl_thread;
};

struct nvme_request {
    struct nvme_command * cmd;
    __u32 sq_id;
    __u64 nsecs_start;
};

struct nvme_result {
    __u32 status;
    __u64 nsecs_target;
    __u32 early_completion;
    __u64 nsecs_target_early;
    __u64 wp; // only for zone append
};

extern struct ssd ssd[SSD_INSTANCES];

unsigned long ssd_init(unsigned int cpu_nr_dispatcher, unsigned long memmap_size);
void ssd_init_ftl_instance(struct ssd *ssd, unsigned int cpu_nr_dispatcher, unsigned long capacity);
void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp);
bool ssd_read(struct nvme_request * req, struct nvme_result * ret);
bool ssd_write(struct nvme_request * req, struct nvme_result * ret);
bool ssd_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret);
void ssd_flush(struct nvme_request * req, struct nvme_result * ret);
void ssd_gc_bg(void);
void ssd_gc(void);
void ssd_gc2(struct ssd *ssd);
void adjust_ftl_latency(int target, int lat);
bool release_write_buffer(uint32_t nr_buffers);
uint32_t allocate_write_buffer(uint32_t nr_buffers);
uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd);
uint64_t ssd_advance_pcie(struct ssd *ssd, __u64 request_time, __u64 length);
#endif
