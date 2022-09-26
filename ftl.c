#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ftl.h"
#include "channel.h"

struct ssd ssd[SSD_INSTANCES];
uint32_t nr_write_buffers = 256;
spinlock_t buffer_lock;

static inline unsigned long long __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}

static inline bool last_pg_in_wordline(struct ssd *ssd, struct ppa *ppa)
{
    return ppa->h.pg_offs == (ssd->sp.pgs_per_flash_pg - 1);
}

static inline bool first_pg_in_wordline(struct ssd *ssd, struct ppa *ppa)
{
    return ppa->h.pg_offs == 0;
}

uint32_t allocate_write_buffer(uint32_t nr_buffers)
{
    #if 1
    while(!spin_trylock(&buffer_lock));
    
    if (nr_write_buffers < nr_buffers) {
        spin_unlock(&buffer_lock);
        return 0;
    } 

    nr_write_buffers-=nr_buffers;
    spin_unlock(&buffer_lock);
    return nr_buffers;
    #elif 0
    uint32_t num;
    while(!spin_trylock(&buffer_lock));
    num = min(nr_write_buffers, nr_buffers);
    num = min(num, 16);
    nr_write_buffers -= num;
    spin_unlock(&buffer_lock);
    return num;
    #else 
    return nr_buffers;
    #endif
}

bool release_write_buffer(uint32_t nr_buffers)
{
    #if 1
    while(!spin_trylock(&buffer_lock));
    nr_write_buffers += nr_buffers;
    spin_unlock(&buffer_lock);
    #endif
    return true;
}

bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ssd * get_ssd_ins_from_lpn(uint64_t lpn)
{
    return &ssd[LPN_TO_SSD_ID(lpn)];
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    NVMEV_ASSERT(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    NVMEV_JH("ppa2pgidx: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
        ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    NVMEV_ASSERT(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct ssd *ssd)
{
    ssd->wfc.write_credits--;
}

static inline void check_and_refill_write_credit(struct ssd *ssd)
{
    struct write_flow_control * wfc = &(ssd->wfc);
    if (wfc->write_credits <= 0) {
        ssd_gc2(ssd);

        wfc->write_credits += wfc->credits_to_refill; 
    } 
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    int i;

    lm->tt_lines = spp->blks_per_pl;
    NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
    lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd, uint32_t io_type)
{
    struct write_pointer *wpp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    if (io_type == USER_IO)
        wpp = &ssd->wp;
    else if (io_type == GC_IO)
        wpp = &ssd->gc_wp;
    else
        NVMEV_ASSERT(0);

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
}

static void ssd_init_write_flow_control(struct ssd *ssd) {
    struct write_flow_control * wfc = &(ssd->wfc);

    wfc->write_credits = ssd->sp.pgs_per_line;
    wfc->credits_to_refill = ssd->sp.pgs_per_line; 
}

static inline void check_addr(int a, int max)
{
    NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        NVMEV_ERROR("No free lines left in VIRT !!!!\n");
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    NVMEV_DEBUG("[%s] free_line_cnt %d\n",__FUNCTION__, lm->free_line_cnt);
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd, uint32_t io_type)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp;
    if (io_type == USER_IO)
        wpp = &ssd->wp;
    else if (io_type == GC_IO)
        wpp = &ssd->gc_wp;
    else
        NVMEV_ASSERT(0);

    NVMEV_DEBUG("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", 
        wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

    check_addr(wpp->pg, spp->pgs_per_blk);
    wpp->pg++;
    if ((wpp->pg % spp->pgs_per_flash_pg) == 0) {
        wpp->pg -= spp->pgs_per_flash_pg;
            check_addr(wpp->ch, spp->nchs);
            wpp->ch++;
            if (wpp->ch == spp->nchs) {
                wpp->ch = 0;
                check_addr(wpp->lun, spp->luns_per_ch);
                wpp->lun++;
                /* in this case, we should go to next lun */
                if (wpp->lun == spp->luns_per_ch) {
                    wpp->lun = 0;
                    /* go to next wordline in the block */
                    wpp->pg += spp->pgs_per_flash_pg;
                    if (wpp->pg == spp->pgs_per_blk) {
                        wpp->pg = 0;
                        /* move current line to {victim,full} line list */
                        if (wpp->curline->vpc == spp->pgs_per_line) {
                            /* all pgs are still valid, move to full line list */
                            NVMEV_ASSERT(wpp->curline->ipc == 0);
                            QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                            lm->full_line_cnt++;
                            NVMEV_JH("wpp: move line to full_line_list\n");
                        } else {
                            NVMEV_JH("wpp: line is moved to victim list\n");
                            NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                            /* there must be some invalid pages in this line */
                            NVMEV_ASSERT(wpp->curline->ipc > 0);
                            pqueue_insert(lm->victim_line_pq, wpp->curline);
                            lm->victim_line_cnt++;
                        }
                        /* current line is used up, pick another empty line */
                        check_addr(wpp->blk, spp->blks_per_pl);
                        wpp->curline = NULL;
                        wpp->curline = get_next_free_line(ssd);
                        if (!wpp->curline) {
                            /* TODO */
                            NVMEV_ERROR("curline is NULL!");
                        }
                        NVMEV_DEBUG("wpp: got new clean line %d\n", wpp->curline->id);
                        wpp->blk = wpp->curline->id;
                        check_addr(wpp->blk, spp->blks_per_pl);
                        /* make sure we are starting from page 0 in the super block */
                        NVMEV_ASSERT(wpp->pg == 0);
                        NVMEV_ASSERT(wpp->lun == 0);
                        NVMEV_ASSERT(wpp->ch == 0);
                        /* TODO: assume # of pl_per_lun is 1, fix later */
                        NVMEV_ASSERT(wpp->pl == 0);
                    }
                }
            }
    }
    NVMEV_JH("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n", 
        wpp->ch, wpp->lun, wpp->pg, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct ssd *ssd, uint32_t io_type)
{
    struct write_pointer *wpp;
    struct ppa ppa;

    if (io_type == USER_IO)
        wpp = &ssd->wp;
    else if (io_type == GC_IO)
        wpp = &ssd->gc_wp;
    else
        NVMEV_ASSERT(0);

    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    NVMEV_ASSERT(ppa.g.pl == 0);

    return ppa;
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

static void ssd_init_params(struct ssdparams *spp, unsigned long capacity)
{
    unsigned long blk_size, total_size;

    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->blks_per_pl = BLKS_PER_PLN; 
    spp->pls_per_lun = PLNS_PER_LUN;
    spp->luns_per_ch = LUNS_PER_NAND_CH;
    spp->nchs = NAND_CH_PER_SSD_INS;

    //capacity = (unsigned long)(capacity * spp->pba_pcent / 100);

    /* flash_pgs_per_blk depends on capacity */
    blk_size = DIV_ROUND_UP(capacity, spp->blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs);
    spp->pgs_per_flash_pg = FLASH_PAGE_SIZE / (spp->secsz * spp->secs_per_pg); 
    spp->flash_pgs_per_blk = DIV_ROUND_UP(blk_size, FLASH_PAGE_SIZE);  
    spp->pgs_per_blk = spp->pgs_per_flash_pg * spp->flash_pgs_per_blk;

    spp->pg_4kb_rd_lat = NAND_4KB_READ_LATENCY;
    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;
    spp->ch_max_xfer_size = MAX_NAND_XFER_SIZE;

    spp->fw_rd0_lat = FW_READ0_LATENCY;
    spp->fw_rd1_lat = FW_READ1_LATENCY;
    spp->fw_rd0_size = FW_READ0_SIZE;
    spp->fw_wr0_lat = FW_PROG0_LATENCY;
    spp->fw_wr1_lat = FW_PROG1_LATENCY;
    spp->fw_xfer_lat = FW_XFER_LATENCY; 

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

    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.997; /* Mot used */
    spp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
    spp->enable_gc_delay = 1;

    spp->op_area_pcent = 0.07;
    spp->pba_pcent = (int)((1 + spp->op_area_pcent) * 100);

    check_params(spp);

    total_size = (unsigned long)spp->tt_luns * spp->blks_per_lun * spp->pgs_per_blk * spp->secsz * spp->secs_per_pg;
    blk_size = spp->pgs_per_blk *  spp->secsz * spp->secs_per_pg;
    NVMEV_INFO("Total Capacity=%lu(GB), %lu(MB) Block Size=%lu(Byte) luns=%u lines=%u pgs_per_line=%u pgs_per_blk=%u gc_thresh_line=%d spp->gc_thres_lines_high=%d n", 
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

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
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
    ch->perf_model->xfer_lat+=spp->fw_xfer_lat;
}

static void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
    pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
    chmodel_init(pcie->perf_model, spp->pcie_bandwidth);
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    int i;
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init_ftl_instance(struct ssd *ssd, unsigned int cpu_nr_dispatcher, unsigned long capacity)
{
    struct ssdparams *spp = &(ssd->sp);
    int i;

    /* Set CPU number to use same cpuclock as io.c */
    ssd->cpu_nr_dispatcher = cpu_nr_dispatcher;

    ssd_init_params(spp, capacity);

    NVMEV_INFO("Init FTL Instance with %d channels(%d pages), CPU %d\n", spp->nchs, spp->tt_pgs, cpu_nr_dispatcher);

    /* initialize ssd internal layout architecture */
    ssd->ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); // 40 * 8 = 320
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&(ssd->ch[i]), spp);
    }

    ssd->pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
    ssd_init_pcie(ssd->pcie, spp);

    /* initialize maptbl */
    NVMEV_INFO("initialize maptbl\n");
    ssd_init_maptbl(ssd); // mapping table

    /* initialize rmap */
    NVMEV_INFO("initialize rmap\n");
    ssd_init_rmap(ssd); // reverse mapping table (?)

    /* initialize all the lines */
    NVMEV_INFO("initialize lines\n");
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    NVMEV_INFO("initialize write pointer\n");
    ssd_init_write_pointer(ssd, USER_IO);
    ssd_init_write_pointer(ssd, GC_IO);

    ssd_init_write_flow_control(ssd);

    return;
}

unsigned long ssd_init(unsigned int cpu_nr_dispatcher, unsigned long memmap_size)
{
    struct ssdparams *spp;
    int i;
    unsigned long logical_space;

    for (i = 0; i < SSD_INSTANCES; i++) 
        ssd_init_ftl_instance(&ssd[i], cpu_nr_dispatcher, 
                                DIV_ROUND_UP(memmap_size, SSD_INSTANCES));

    spp = &(ssd[0].sp);

    logical_space = (unsigned long)((memmap_size * 100) / spp->pba_pcent);

    NVMEV_INFO("FTL physical space: %ld, logical space: %ld (physical/logical * 100 = %d)\n", memmap_size, logical_space, spp->pba_pcent);

    spin_lock_init(&buffer_lock);

    return logical_space;
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    //int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

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

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

inline uint64_t ssd_advance_pcie(__u64 request_time, __u64 length) 
{
    struct channel_model * perf_model = ssd[0].pcie->perf_model;
    return chmodel_request(perf_model, request_time, length);
}

uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        __get_ioclock(ssd) : ncmd->stime;
    uint64_t nand_stime, nand_etime;
    uint64_t chnl_stime, chnl_etime;
    uint64_t pcie_stime, pcie_etime;
    uint64_t remaining, xfer_size, completed_time;
    
    NVMEV_DEBUG("SSD: %p, Enter stime: %lld, ch %lu lun %lu blk %lu page %lu command %d ppa 0x%llx\n",
                            ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);
	
	if (!mapped_ppa(ppa)) {
		NVMEV_INFO("Error ppa 0x%llx\n", ppa->ppa);
		return cmd_stime;
	}
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    struct ssd_channel * ch = get_ch(ssd, ppa); 
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
            xfer_size = min(remaining, spp->ch_max_xfer_size);
            chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size);
            
            if (ncmd->type == USER_IO) /* overlap pci transfer with nand ch transfer*/
                completed_time = 
					ssd_advance_pcie(chnl_etime, xfer_size);
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

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    NVMEV_ASSERT(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        NVMEV_ASSERT(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    } 
    
    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    NVMEV_ASSERT(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    NVMEV_ASSERT(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;
    int i;

    for (i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        gcr.xfer_size = LPN_TO_BYTE(1);
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    NVMEV_ASSERT(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, GC_IO);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, GC_IO);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_NOP;
        gcw.stime = 0;

        if (first_pg_in_wordline(ssd, &new_ppa)) {
            gcw.cmd = NAND_WRITE;
            gcw.xfer_size = LPN_TO_BYTE(ssd->sp.pgs_per_flash_pg);
        }

        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && (victim_line->vpc > (ssd->sp.pgs_per_line / 8))) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int pg;

    for (pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        NVMEV_ASSERT(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    NVMEV_ASSERT(get_blk(ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flash_page(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int pg;
    uint64_t completed_time = 0;

    for (pg = 0; pg < spp->pgs_per_flash_pg; pg++) {
        ppa->h.pg_offs = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        NVMEV_ASSERT(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) 
            cnt++;
    }

    if (cnt > 0) {
        if (ssd->sp.enable_gc_delay) {
            struct nand_cmd gcr;
            gcr.type = GC_IO;
            gcr.cmd = NAND_READ;
            gcr.stime = 0;
            gcr.xfer_size = LPN_TO_BYTE(cnt);
            completed_time = ssd_advance_status(ssd, ppa, &gcr);
        }

        for (pg = 0; pg < spp->pgs_per_flash_pg; pg++) {
            ppa->h.pg_offs = pg;
            pg_iter = get_pg(ssd, ppa);

            /* there shouldn't be any free page in victim blocks */
            if (pg_iter->status == PG_VALID) 
                /* delay the maptbl update until "write" happens */
                gc_write_page(ssd, ppa);
        }
    }
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

int do_gc(struct ssd *ssd, bool force)
{   
    struct line *victim_line = NULL;
    struct ssdparams *spp = &(ssd->sp);
    struct nand_lun *lunp;
    struct nand_block *blkp;
    struct ppa ppa;
    int ch, lun, flash_pg;

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    NVMEV_DEBUG("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc,victim_line->vpc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    ssd->wfc.credits_to_refill = victim_line->ipc;

    /* copy back valid data */
    for (flash_pg = 0; flash_pg < spp->flash_pgs_per_blk; flash_pg++) {
        ppa.h.wordline = flash_pg; 
        for (ch = 0; ch < spp->nchs; ch++) {
            for (lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(ssd, &ppa);
                clean_one_flash_page(ssd, &ppa);

                if (flash_pg == (spp->flash_pgs_per_blk - 1)){
                    mark_block_free(ssd, &ppa);

                    if (spp->enable_gc_delay) {
                        struct nand_cmd gce;
                        gce.type = GC_IO;
                        gce.cmd = NAND_ERASE;
                        gce.stime = 0;
                        ssd_advance_status(ssd, &ppa, &gce);
                    }

                    lunp->gc_endtime = lunp->next_lun_avail_time;
                }
            }
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

void ssd_gc_bg(void) {
    unsigned int i = 0;
    struct ssd *ssd_ins;

    for (i = 0; i < SSD_INSTANCES; i++) {
        ssd_ins = &ssd[i];

        if (should_gc(ssd_ins)) {
			NVMEV_DEBUG("NEED GC!\n");
			do_gc(ssd_ins, false); // 782336
		}
    }
}

void ssd_gc(void) {
    unsigned int i = 0, r;
    struct ssd *ssd_ins;

    for (i = 0; i < SSD_INSTANCES; i++) {
        ssd_ins = &ssd[i];

        while (should_gc_high(ssd_ins)) {
            NVMEV_DEBUG("should_gc_high passed");
            /* perform GC here until !should_gc(ssd) */
            r = do_gc(ssd_ins, true);
            if (r == -1)
                break;
        }
    }
}

void ssd_gc2(struct ssd *ssd) {
    unsigned int i = 0, r;
    if (should_gc_high(ssd)) {
        NVMEV_DEBUG("should_gc_high passed");
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
    }
}

bool is_same_flash_page(struct ppa ppa1, struct ppa ppa2) 
{
    return (ppa1.h.blk_in_die == ppa2.h.blk_in_die) &&
           (ppa1.h.wordline == ppa2.h.wordline);
}

bool ssd_read(struct nvme_request * req, struct nvme_result * ret)
{
    struct nvme_command * cmd = req->cmd;
    uint64_t nsecs_start = req->nsecs_start;
    struct ssd *ssd_ins;
    struct ssdparams *spp = &(ssd[0].sp);  
    uint64_t lba = cmd->rw.slba;
    int nr_lba = (cmd->rw.length + 1);
    struct ppa cur_ppa;
    struct ppa prev_ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
    uint64_t lpn, local_lpn;
    uint64_t sublat, maxlat = nsecs_start;
    uint32_t xfer_size, i;
    struct nand_cmd srd;
    srd.type = USER_IO;
    srd.cmd = NAND_READ;
    srd.stime = nsecs_start;

    NVMEV_DEBUG("ssd_read: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
    if (LPN_TO_LOCAL_LPN(end_lpn) >= spp->tt_pgs) {
        NVMEV_ERROR("ssd_read: lpn passed FTL range(start_lpn=%lld,tt_pgs=%d)\n", start_lpn, spp->tt_pgs);
        return false;
    }

    if (LBA_TO_BYTE(nr_lba) <= spp->fw_rd0_size)
        srd.stime += spp->fw_rd0_lat;
    else
        srd.stime += spp->fw_rd1_lat;


    for (i = 0; (i < SSD_INSTANCES) && (start_lpn <= end_lpn); i++, start_lpn++) {
        ssd_ins = get_ssd_ins_from_lpn(start_lpn);
        xfer_size = 0;
        prev_ppa = get_maptbl_ent(ssd_ins, LPN_TO_LOCAL_LPN(start_lpn));
        
        NVMEV_DEBUG("[%s] ssd_ins=%p, ftl_ins=%lld, local_lpn=%lld",__FUNCTION__, ssd_ins, LPN_TO_SSD_ID(lpn), LPN_TO_LOCAL_LPN(lpn));

        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn+=SSD_INSTANCES) {
            local_lpn = LPN_TO_LOCAL_LPN(lpn);
            cur_ppa = get_maptbl_ent(ssd_ins, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(ssd_ins, &cur_ppa))
            {
                NVMEV_DEBUG("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
                NVMEV_DEBUG("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
                cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk, cur_ppa.g.pl, cur_ppa.g.pg);
                continue;
            }

            // aggregate read io in same flash page
            if (mapped_ppa(&prev_ppa) && is_same_flash_page(cur_ppa, prev_ppa)) {
                xfer_size += LPN_TO_BYTE(1);
                continue;
            }

            if (xfer_size > 0) {
                srd.xfer_size = xfer_size;
                sublat = ssd_advance_status(ssd_ins, &prev_ppa, &srd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
            }
            
            xfer_size = LPN_TO_BYTE(1);
            prev_ppa = cur_ppa;
        }

        // issue remaining io
        if (xfer_size > 0) {
            srd.xfer_size = xfer_size;
            sublat = ssd_advance_status(ssd_ins, &prev_ppa, &srd);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
    }

    ret->early_completion = false;
    ret->nsecs_target = maxlat;
    ret->status = NVME_SC_SUCCESS; 
    return true;
}

void enqueue_io_req3(int sqid, unsigned long long nsecs_start, unsigned long long nsecs_target, unsigned int pgs_to_release);
bool ssd_write(struct nvme_request * req, struct nvme_result * ret)
{
    struct nvme_command * cmd = req->cmd;
    uint64_t nsecs_start = req->nsecs_start;
    uint64_t lba = cmd->rw.slba;
    struct ssd *ssd_ins;
    struct ssdparams *spp = &(ssd[0].sp);
    int nr_lba = (cmd->rw.length + 1);
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn, local_lpn;
    uint64_t curlat = 0, maxlat = 0;
    uint32_t pgs_to_pgm = spp->pgs_per_flash_pg;
    uint32_t i = 0;
    uint32_t buffers_allocated = 0;
    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.stime = nsecs_start;

    NVMEV_DEBUG("ssd_write: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
    if (LPN_TO_LOCAL_LPN(end_lpn)  >= spp->tt_pgs) {
        NVMEV_ERROR("ssd_write: lpn passed FTL range(start_lpn=%lld,tt_pgs=%d)\n", start_lpn, spp->tt_pgs);
        return false;
    }
    
    //swr.stime = swr.stime > temp_latest_early_completed[req->sq_id] ? swr.stime : temp_latest_early_completed[req->sq_id];  
    buffers_allocated = allocate_write_buffer(end_lpn - start_lpn + 1); 
	
	if (buffers_allocated < (end_lpn - start_lpn + 1))
		return false;

	swr.stime += spp->fw_wr0_lat;
	swr.stime += spp->fw_wr1_lat * buffers_allocated;
    swr.stime = ssd_advance_pcie(swr.stime, LPN_TO_BYTE(buffers_allocated));
 
    for (lpn = start_lpn; lpn <= end_lpn && buffers_allocated > 0; lpn++, buffers_allocated--) {
        ssd_ins = get_ssd_ins_from_lpn(lpn);
        local_lpn = LPN_TO_LOCAL_LPN(lpn); 
        ppa = get_maptbl_ent(ssd_ins, local_lpn); // 현재 LPN에 대해 전에 이미 쓰인 PPA가 있는지 확인
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd_ins, &ppa);
            set_rmap_ent(ssd_ins, INVALID_LPN, &ppa);
            NVMEV_JH("ssd_write: %lld is invalid, ", ppa2pgidx(ssd_ins, &ppa));
        }

        /* new write */
        ppa = get_new_page(ssd_ins, USER_IO);
        /* update maptbl */
        set_maptbl_ent(ssd_ins, local_lpn, &ppa);
        NVMEV_JH("ssd_write: got new ppa %lld, ", ppa2pgidx(ssd_ins, &ppa));
        /* update rmap */
        set_rmap_ent(ssd_ins, local_lpn, &ppa);

        mark_page_valid(ssd_ins, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd_ins, USER_IO);

        /* Aggregate write io in flash page */
        if (last_pg_in_wordline(ssd_ins, &ppa)) {
            swr.cmd = NAND_WRITE;
            swr.xfer_size = LPN_TO_BYTE(pgs_to_pgm);

            curlat = ssd_advance_status(ssd_ins, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;

            enqueue_io_req3(req->sq_id, swr.stime, curlat, pgs_to_pgm);
        } else {            
            swr.cmd = NAND_NOP;
            swr.xfer_size = LPN_TO_BYTE(1);

            curlat = ssd_advance_status(ssd_ins, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;   
        }
        
        consume_write_credit(ssd_ins);
        check_and_refill_write_credit(ssd_ins);
    }
    
        ret->nsecs_target = maxlat;
        ret->early_completion = true;
        ret->nsecs_target_early = swr.stime;
        ret->status = NVME_SC_SUCCESS;

        NVMEV_ASSERT(ret->nsecs_target_early <= ret->nsecs_target); 
        return true;
    
}

void adjust_ftl_latency(int target, int lat)
{
    struct ssdparams *spp;
    int i;

    for (i = 0; i < SSD_INSTANCES; i++) {
        spp = &(ssd[i].sp); 
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
}
