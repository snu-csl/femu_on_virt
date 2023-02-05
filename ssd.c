#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"

static inline unsigned long long __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}

void buffer_init(struct buffer * buf, __u32 size)
{
    spin_lock_init(&buf->lock);
    buf->initial = size;
    buf->remaining = size;
}

uint32_t buffer_allocate(struct buffer * buf, __u32 size)
{
    #if 1
    while(!spin_trylock(&buf->lock));
    
    if (buf->remaining < size) {
        spin_unlock(&buf->lock);
        return 0;
    } 

    buf->remaining-=size;
    spin_unlock(&buf->lock);
    return size;
    #elif 0
    uint32_t num;
    while(!spin_trylock(&buffer_lock));
    num = min(remaining_buf_size, nr_buffers);
    num = min(num, 16);
    remaining_buf_size -= num;
    spin_unlock(&buffer_lock);
    return num;
    #else 
    return nr_buffers;
    #endif
}

bool buffer_release(struct buffer * buf, __u32 size)
{
    while(!spin_trylock(&buf->lock));
    buf->remaining += size;
    spin_unlock(&buf->lock);

    return true;
}

void buffer_refill(struct buffer * buf) 
{
    while(!spin_trylock(&buf->lock));
    buf->remaining = buf->initial;
    spin_unlock(&buf->lock);
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

void ssd_init_params(struct ssdparams *spp, __u64 capacity, __u32 nparts)
{
    __u64 blk_size, total_size;

    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgsz = spp->secsz * spp->secs_per_pg;

    spp->nchs = NAND_CHANNELS;
    spp->pls_per_lun = PLNS_PER_LUN;
    spp->luns_per_ch = LUNS_PER_NAND_CH;

    /* partitioning SSD by dividing channel*/
    NVMEV_ASSERT((spp->nchs % nparts) == 0);
    spp->nchs /= nparts;
    capacity /= nparts; 

    if (BLKS_PER_PLN > 0) {
        /* flashpgs_per_blk depends on capacity */
        spp->blks_per_pl = BLKS_PER_PLN; 
        blk_size = DIV_ROUND_UP(capacity, spp->blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs);
    } else {
        NVMEV_ASSERT(BLK_SIZE > 0);
        blk_size = BLK_SIZE;
        spp->blks_per_pl = DIV_ROUND_UP(capacity, blk_size * spp->pls_per_lun * spp->luns_per_ch * spp->nchs);
    }

    spp->pgs_per_oneshotpg = ONESHOT_PAGE_SIZE / (spp->pgsz);
    spp->oneshotpgs_per_blk = DIV_ROUND_UP(blk_size, ONESHOT_PAGE_SIZE); 

    spp->pgs_per_flashpg = FLASH_PAGE_SIZE / (spp->pgsz);
    spp->flashpgs_per_blk = (ONESHOT_PAGE_SIZE / FLASH_PAGE_SIZE) * spp->oneshotpgs_per_blk;  
   
    spp->pgs_per_blk = spp->pgs_per_oneshotpg * spp->oneshotpgs_per_blk;

    spp->write_unit_size = WRITE_UNIT_SIZE;

    spp->pg_4kb_rd_lat = NAND_4KB_READ_LATENCY;
    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;
    spp->ch_max_xfer_size = CH_MAX_XFER_SIZE;

    spp->fw_rd0_lat = FW_READ0_LATENCY;
    spp->fw_rd1_lat = FW_READ1_LATENCY;
    spp->fw_rd0_size = FW_READ0_SIZE;
    spp->fw_wr0_lat = FW_PROG0_LATENCY;
    spp->fw_wr1_lat = FW_PROG1_LATENCY;
    spp->fw_4kb_xfer_lat = FW_4KB_XFER_LATENCY; 

    spp->ch_bandwidth = NAND_CHANNEL_BANDWIDTH; 
    spp->pcie_bandwidth = PCIE_BANDWIDTH; 

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */ // lun size is super-block(line) size

    //spp->gc_thres_pcent = 0.75;
    //spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    //spp->gc_thres_pcent_high = 0.997; /* Mot used */
    spp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
    spp->enable_gc_delay = 1;

    spp->op_area_pcent = OP_AREA_PERCENT;
    spp->pba_pcent = (int)((1 + spp->op_area_pcent) * 100);

    spp->write_buffer_size = WRITE_BUFFER_SIZE;
    check_params(spp);

    total_size = (unsigned long)spp->tt_luns * spp->blks_per_lun * spp->pgs_per_blk * spp->secsz * spp->secs_per_pg;
    blk_size = spp->pgs_per_blk *  spp->secsz * spp->secs_per_pg;
    NVMEV_INFO("Total Capacity=%lu(GB), %lu(MB) Block Size=%lu(Byte) luns=%lu lines=%lu pgs_per_line=%lu pgs_per_blk=%u gc_thresh_line=%d spp->gc_thres_lines_high=%d n", 
                    BYTE_TO_GB(total_size), BYTE_TO_MB(total_size), blk_size, 
                    spp->tt_luns, spp->tt_lines, spp->pgs_per_line, spp->pgs_per_blk, spp->gc_thres_lines, spp->gc_thres_lines_high);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    int i;
    pg->nsecs = spp->secs_per_pg;
    pg->sec = kmalloc(sizeof(nand_sec_status_t) * pg->nsecs, GFP_KERNEL);
    for (i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    int i;
    blk->npgs = spp->pgs_per_blk;
    blk->pg = kmalloc(sizeof(struct nand_page) * blk->npgs, GFP_KERNEL);
    for (i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    int i;
    pl->nblks = spp->blks_per_pl;
    pl->blk = kmalloc(sizeof(struct nand_block) * pl->nblks, GFP_KERNEL);
    for (i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    int i;
    lun->npls = spp->pls_per_lun;
    lun->pl = kmalloc(sizeof(struct nand_plane) * lun->npls, GFP_KERNEL);
    for (i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    int i;
    ch->nluns = spp->luns_per_ch;
    ch->lun = kmalloc(sizeof(struct nand_lun) * ch->nluns, GFP_KERNEL);
    for (i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }

    ch->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
    chmodel_init(ch->perf_model, spp->ch_bandwidth);

    /* Add firmware overhead */
    ch->perf_model->xfer_lat+=(spp->fw_4kb_xfer_lat *UNIT_XFER_SIZE/4096);
}

void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
    pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
    chmodel_init(pcie->perf_model, spp->pcie_bandwidth);
}

void ssd_init(struct ssd * ssd, struct ssdparams *spp, __u32 cpu_nr_dispatcher)
{
    __u32 i;
    /*copy spp*/
    ssd->sp = *spp;

        /* initialize conv_ssd internal layout architecture */
    ssd->ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); // 40 * 8 = 320
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&(ssd->ch[i]), spp);
    }

    /* Set CPU number to use same cpuclock as io.c */
    ssd->cpu_nr_dispatcher = cpu_nr_dispatcher;

    ssd->pcie = (struct ssd_pcie *)kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
    ssd_init_pcie(ssd->pcie, spp);

    ssd->write_buffer = (struct buffer *) kmalloc(sizeof(struct buffer), GFP_ATOMIC); 
    buffer_init(ssd->write_buffer, spp->write_buffer_size);

    return;
}

inline uint64_t ssd_advance_pcie(struct ssd *ssd, __u64 request_time, __u64 length) 
{
    struct channel_model * perf_model = ssd->pcie->perf_model;
    return chmodel_request(perf_model, request_time, length);
}

uint64_t ssd_advance_write_buffer(struct ssd *ssd, __u64 request_time, __u64 length) 
{
    uint64_t nsecs_latest = request_time;
    struct ssdparams *spp = &ssd->sp;

    nsecs_latest += spp->fw_wr0_lat;
	nsecs_latest += spp->fw_wr1_lat * DIV_ROUND_UP(length, KB(4));

    nsecs_latest = ssd_advance_pcie(ssd, nsecs_latest, length);

    return nsecs_latest;
}

uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        __get_ioclock(ssd) : ncmd->stime;
    uint64_t nand_stime, nand_etime;
    uint64_t chnl_stime, chnl_etime;
    uint64_t remaining, xfer_size, completed_time;
    struct ssdparams *spp;
    struct nand_lun *lun;
    struct ssd_channel * ch;
    struct ppa *ppa = ncmd->ppa;
    NVMEV_DEBUG("SSD: %p, Enter stime: %lld, ch %lu lun %lu blk %lu page %lu command %d ppa 0x%llx\n",
                            ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);
	
    if (ppa->ppa == UNMAPPED_PPA) {
		NVMEV_INFO("Error ppa 0x%llx\n", ppa->ppa);
		return cmd_stime;
	}

    spp = &ssd->sp;
    lun = get_lun(ssd, ppa);
    ch = get_ch(ssd, ppa); 
    remaining = ncmd->xfer_size;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                    lun->next_lun_avail_time;

        if (ncmd->xfer_size == 4096)
            nand_etime = nand_stime + spp->pg_4kb_rd_lat;
        else       
            nand_etime = nand_stime + spp->pg_rd_lat;
   
        /* read: then data transfer through channel */
        chnl_stime = nand_etime;

        while (remaining) {
            xfer_size = min(remaining, (uint64_t)spp->ch_max_xfer_size);
            chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size);
            
            if (ncmd->interleave_pci_dma) /* overlap pci transfer with nand ch transfer*/
                completed_time = ssd_advance_pcie(ssd, chnl_etime, xfer_size);
            else
                completed_time = chnl_etime;
            
            remaining -= xfer_size;
            chnl_stime = chnl_etime;
        }
    
        lun->next_lun_avail_time = chnl_etime;
        break;

    case NAND_WRITE:
        NVMEV_DEBUG("SSD: %p, Enter stime: %lld, ch %lu lun %lu blk %lu page %lu\n",
                            ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);

        /* write: transfer data through channel first */
        chnl_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;

        chnl_etime = chmodel_request(ch->perf_model, chnl_stime, ncmd->xfer_size);

        /* write: then do NAND program */
        nand_stime = chnl_etime;
        nand_etime = nand_stime + spp->pg_wr_lat;   
        lun->next_lun_avail_time = nand_etime;
        completed_time = nand_etime;
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        nand_etime = nand_stime + spp->blk_er_lat;
        lun->next_lun_avail_time = nand_etime;
        completed_time = nand_etime;
        break;

    case NAND_NOP:
        /* no operation: just return last completed time of lun */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime;
        completed_time = nand_stime;
        break;

    default:
        NVMEV_ERROR("Unsupported NAND command: 0x%x\n", c);
    }

    return completed_time;
}

void adjust_ftl_latency(int target, int lat)
{
/*TODO ..*/
#if 0
    struct ssdparams *spp;
    int i;

    for (i = 0; i < SSD_PARTITIONS; i++) {
        spp = &(g_conv_ssds[i].sp); 
        printk("Before latency: %d %d %d, change to %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat, lat);
        switch (target) {
            case NAND_READ:
                spp->pg_rd_lat = lat;
                break;

            case NAND_WRITE:
                spp->pg_wr_lat = lat;
                break;

            case NAND_ERASE:
                spp->blk_er_lat = lat;
                break;

            default:
                NVMEV_ERROR("Unsupported NAND command\n");
        }
        printk("After latency: %d %d %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat);
    }
#endif
}