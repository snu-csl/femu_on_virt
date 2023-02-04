#ifndef _SSD_H
#define _SSD_H

#include <linux/types.h>
#include "queue.h"
#include "pqueue.h"
#include "ssd_config.h"
#include "channel.h"
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

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

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

#define TOTAL_PPA_BITS (64)
#define BLK_BITS    (16)
#define CHUNK_BITS  (16)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (8)
#define RSB_BITS    (TOTAL_PPA_BITS - (BLK_BITS + CHUNK_BITS + PL_BITS + LUN_BITS + CH_BITS))

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t chunk  : CHUNK_BITS; // chunk == 4KB
            uint64_t blk : BLK_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : RSB_BITS;
        } g;

        struct {
            uint64_t : CHUNK_BITS;
            uint64_t blk_in_ssd : BLK_BITS + PL_BITS + LUN_BITS + CH_BITS;
            uint64_t rsv : RSB_BITS;
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

struct nand_cmd {
    int type;
    int cmd;
    uint64_t xfer_size; // byte
    uint64_t stime; /* Coperd: request arrival time */
    bool interleave_pci_dma;
};

struct buffer {
    __u32 initial;
    __u32 remaining;
    spinlock_t lock;
};

/*
chunk : Mapping unit (4KB)
page (flash page) : Nand sensing unit,tR 
wordline (flash wordline) : Nand Program unit, tPROG
blk : Nand Erase unit
*/
struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_chunk;  /* # of sectors per page */
    int chunksz;
    int chunks_per_page; /* # of pgs per flash page */
    int pages_per_blk; /* # of flash pages per block */
    int chunks_per_wordline; /* # of pgs per oneshot program page */
    int wordlines_per_blk; /* # of pgm page pages per block */
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
    int fw_4kb_xfer_lat;     /* Firmware overhead of data transfer in nanoseconds */

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

    unsigned long long write_buffer_size;
};

#define STRUCT_SSD_ENTRY  struct ssdparams sp; \
                          struct ssd_channel *ch; \
                          struct ssd_pcie *pcie; \
                          struct buffer *write_buffer; \
                          unsigned int cpu_nr_dispatcher; \

struct ssd {
    STRUCT_SSD_ENTRY
};

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct nand_chunk *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->chunk[ppa->g.chunk]);
}

void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp);
void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp);
void ssd_init_params(struct ssdparams *spp, __u64 capacity, __u32 nparts);
void ssd_init(struct ssd * ssd, struct ssdparams *spp, __u32 cpu_nr_dispatcher);

void adjust_ftl_latency(int target, int lat);
uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd);
uint64_t ssd_advance_pcie(struct ssd *ssd, __u64 request_time, __u64 length);

void buffer_init(struct buffer * buf, __u32 size);
uint32_t buffer_allocate(struct buffer * buf, __u32 size);
bool buffer_release(struct buffer * buf, __u32 size);
void buffer_refill(struct buffer * buf);
#endif
