#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "conv_ssd.h"

struct conv_ssd *g_conv_ssds = NULL;

static inline bool last_pg_in_wordline(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    return (ppa->g.pg % conv_ssd->sp.pgs_per_oneshotpg)== (conv_ssd->sp.pgs_per_oneshotpg - 1);
}

bool should_gc(struct conv_ssd *conv_ssd)
{
    return (conv_ssd->lm.free_line_cnt <= conv_ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ssd *conv_ssd)
{
    return (conv_ssd->lm.free_line_cnt <= conv_ssd->sp.gc_thres_lines_high);
}

static inline struct conv_ssd * conv_ssd_instance(uint32_t id)
{
    return &g_conv_ssds[id % SSD_PARTITIONS];
}

static inline struct ppa get_maptbl_ent(struct conv_ssd *conv_ssd, uint64_t lpn)
{
    return conv_ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ssd *conv_ssd, uint64_t lpn, struct ppa *ppa)
{
    NVMEV_ASSERT(lpn < conv_ssd->sp.tt_pgs);
    conv_ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ssd->sp;
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

static inline uint64_t get_rmap_ent(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(conv_ssd, ppa);

    return conv_ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ssd *conv_ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(conv_ssd, ppa);

    conv_ssd->rmap[pgidx] = lpn;
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

static inline void consume_write_credit(struct conv_ssd *conv_ssd)
{
    conv_ssd->wfc.write_credits--;
}

static inline void check_and_refill_write_credit(struct conv_ssd *conv_ssd)
{
    struct write_flow_control * wfc = &(conv_ssd->wfc);
    if (wfc->write_credits <= 0) {
        conv_gc(conv_ssd);

        wfc->write_credits += wfc->credits_to_refill; 
    } 
}

static void init_lines(struct conv_ssd *conv_ssd)
{
    struct ssdparams *spp = &conv_ssd->sp;
    struct line_mgmt *lm = &conv_ssd->lm;
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

static void init_write_pointer(struct conv_ssd *conv_ssd, uint32_t io_type)
{
    struct write_pointer *wpp;
    struct line_mgmt *lm = &conv_ssd->lm;
    struct line *curline = NULL;

    if (io_type == USER_IO)
        wpp = &conv_ssd->wp;
    else if (io_type == GC_IO)
        wpp = &conv_ssd->gc_wp;
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

static void init_write_flow_control(struct conv_ssd *conv_ssd) {
    struct write_flow_control * wfc = &(conv_ssd->wfc);

    wfc->write_credits = conv_ssd->sp.pgs_per_line;
    wfc->credits_to_refill = conv_ssd->sp.pgs_per_line; 
}

static inline void check_addr(int a, int max)
{
    NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ssd *conv_ssd)
{
    struct line_mgmt *lm = &conv_ssd->lm;
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

static void advance_write_pointer(struct conv_ssd *conv_ssd, uint32_t io_type)
{
    struct ssdparams *spp = &conv_ssd->sp;
    struct line_mgmt *lm = &conv_ssd->lm;
    struct write_pointer *wpp;
    if (io_type == USER_IO)
        wpp = &conv_ssd->wp;
    else if (io_type == GC_IO)
        wpp = &conv_ssd->gc_wp;
    else
        NVMEV_ASSERT(0);

    NVMEV_DEBUG("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", 
        wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

    check_addr(wpp->pg, spp->pgs_per_blk);
    wpp->pg++;
    if ((wpp->pg % spp->pgs_per_oneshotpg) == 0) {
        wpp->pg -= spp->pgs_per_oneshotpg;
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
                    wpp->pg += spp->pgs_per_oneshotpg;
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
                        wpp->curline = get_next_free_line(conv_ssd);
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
        wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct conv_ssd *conv_ssd, uint32_t io_type)
{
    struct write_pointer *wpp;
    struct ppa ppa;

    if (io_type == USER_IO)
        wpp = &conv_ssd->wp;
    else if (io_type == GC_IO)
        wpp = &conv_ssd->gc_wp;
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

static void init_maptbl(struct conv_ssd *conv_ssd)
{
    int i;
    struct ssdparams *spp = &conv_ssd->sp;

    conv_ssd->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        conv_ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void init_rmap(struct conv_ssd *conv_ssd)
{
    int i;
    struct ssdparams *spp = &conv_ssd->sp;

    conv_ssd->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
    for (i = 0; i < spp->tt_pgs; i++) {
        conv_ssd->rmap[i] = INVALID_LPN;
    }
}

static void conv_init_ftl(struct conv_ssd *conv_ssd)
{
    struct ssdparams *spp = &(conv_ssd->sp);
    
    NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n", spp->nchs, spp->tt_pgs);

    /* initialize maptbl */
    NVMEV_INFO("initialize maptbl\n");
    init_maptbl(conv_ssd); // mapping table

    /* initialize rmap */
    NVMEV_INFO("initialize rmap\n");
    init_rmap(conv_ssd); // reverse mapping table (?)

    /* initialize all the lines */
    NVMEV_INFO("initialize lines\n");
    init_lines(conv_ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    NVMEV_INFO("initialize write pointer\n");
    init_write_pointer(conv_ssd, USER_IO);
    init_write_pointer(conv_ssd, GC_IO);

    init_write_flow_control(conv_ssd);

    return;
}

__u64 conv_init(__u64 capacity, __u32 cpu_nr_dispatcher)
{
    struct ssdparams spp;
    struct conv_ssd * conv_ssds;
    __u32 i;
    __u64 logical_space;
    const __u32 nparts =  SSD_PARTITIONS;
    
    ssd_init_params(&spp, capacity, nparts);
    
    conv_ssds = kmalloc(sizeof(struct conv_ssd) * nparts, GFP_KERNEL);
    
    for (i = 0; i < nparts; i++) { 
        ssd_init(&conv_ssds[i].ssd, &spp, cpu_nr_dispatcher);
        conv_init_ftl(&conv_ssds[i]);
    }

     /* PCIe, Write buffer are shared by all instances*/
    for (i = 1; i < nparts; i++) {
        kfree(conv_ssds[i].ssd.pcie);
        kfree(conv_ssds[i].ssd.write_buffer);

        conv_ssds[i].ssd.pcie = conv_ssds[0].ssd.pcie;
        conv_ssds[i].ssd.write_buffer = conv_ssds[0].ssd.write_buffer;
    }

    NVMEV_ASSERT(g_conv_ssds == NULL);
    g_conv_ssds = conv_ssds; 

    logical_space = (__u64)((capacity * 100) / spp.pba_pcent);
    logical_space = logical_space - (logical_space % PAGE_SIZE);
    NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d) cpu: %d\n", 
                    capacity, logical_space, spp.pba_pcent, cpu_nr_dispatcher);

    return logical_space;
}

static inline bool valid_ppa(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ssd->sp;
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

static inline bool valid_lpn(struct conv_ssd *conv_ssd, uint64_t lpn)
{
    return (lpn < conv_ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    return &(conv_ssd->lm.lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &conv_ssd->lm;
    struct ssdparams *spp = &conv_ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(&conv_ssd->ssd, ppa);
    NVMEV_ASSERT(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(&conv_ssd->ssd, ppa);
    NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(conv_ssd, ppa);
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

static void mark_page_valid(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;
  
    /* update page status */
    pg = get_pg(&conv_ssd->ssd, ppa);
    NVMEV_ASSERT(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(&conv_ssd->ssd, ppa);
    NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < conv_ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(conv_ssd, ppa);
    NVMEV_ASSERT(line->vpc >= 0 && line->vpc < conv_ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ssd->sp;
    struct nand_block *blk = get_blk(&conv_ssd->ssd, ppa);
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

static void gc_read_page(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    /* advance conv_ssd status, we don't care about how long it takes */
    if (conv_ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        gcr.xfer_size = conv_ssd->sp.pgsz;
        gcr.interleave_pci_dma = false;
        ssd_advance_status(&(conv_ssd->ssd), ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ssd *conv_ssd, struct ppa *old_ppa)
{
    struct ssdparams *spp = &conv_ssd->sp;
    struct ppa new_ppa;
    uint64_t lpn = get_rmap_ent(conv_ssd, old_ppa);

    NVMEV_ASSERT(valid_lpn(conv_ssd, lpn));
    new_ppa = get_new_page(conv_ssd, GC_IO);
    /* update maptbl */
    set_maptbl_ent(conv_ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(conv_ssd, lpn, &new_ppa);

    mark_page_valid(conv_ssd, &new_ppa);

    /* need to advance the write pointer here */
    advance_write_pointer(conv_ssd, GC_IO);

    if (conv_ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_NOP;
        gcw.stime = 0;
        gcw.interleave_pci_dma = false;

        if (last_pg_in_wordline(conv_ssd, &new_ppa)) {
            gcw.cmd = NAND_WRITE;
            gcw.xfer_size = spp->pgsz * conv_ssd->sp.pgs_per_oneshotpg;
        }

        ssd_advance_status(&conv_ssd->ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(conv_ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;

    new_lun = get_lun(conv_ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

    return 0;
}

static struct line *select_victim_line(struct conv_ssd *conv_ssd, bool force)
{
    struct line_mgmt *lm = &conv_ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && (victim_line->vpc > (conv_ssd->sp.pgs_per_line / 8))) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;
    int pg;

    for (pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(&conv_ssd->ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        NVMEV_ASSERT(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(conv_ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(conv_ssd, ppa);
            cnt++;
        }
    }

    NVMEV_ASSERT(get_blk(&conv_ssd->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &conv_ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0, i = 0;
    uint64_t completed_time = 0;
    struct ppa ppa_copy = *ppa;

    for (i = 0; i < spp->pgs_per_flashpg; i++) {
        pg_iter = get_pg(&conv_ssd->ssd, &ppa_copy);
        /* there shouldn't be any free page in victim blocks */
        NVMEV_ASSERT(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) 
            cnt++;
        
        ppa_copy.g.pg++;
    }

    ppa_copy = *ppa;

    if (cnt > 0) {
        if (conv_ssd->sp.enable_gc_delay) {
            struct nand_cmd gcr;
            gcr.type = GC_IO;
            gcr.cmd = NAND_READ;
            gcr.stime = 0;
            gcr.xfer_size = spp->pgsz * cnt;
            gcr.interleave_pci_dma = false;
            completed_time = ssd_advance_status(&conv_ssd->ssd, &ppa_copy, &gcr);
        }

        for (i = 0; i < spp->pgs_per_flashpg; i++) {
            pg_iter = get_pg(&conv_ssd->ssd, &ppa_copy);

            /* there shouldn't be any free page in victim blocks */
            if (pg_iter->status == PG_VALID) 
                /* delay the maptbl update until "write" happens */
                gc_write_page(conv_ssd, &ppa_copy);
            
            ppa_copy.g.pg++;
        }
    }
}

static void mark_line_free(struct conv_ssd *conv_ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &conv_ssd->lm;
    struct line *line = get_line(conv_ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

int do_gc(struct conv_ssd *conv_ssd, bool force)
{   
    struct line *victim_line = NULL;
    struct ssdparams *spp = &(conv_ssd->sp);
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun, flashpg;

    victim_line = select_victim_line(conv_ssd, force);
    if (!victim_line) {
        return -1;
    }

    ppa.g.blk = victim_line->id;
    NVMEV_DEBUG("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc,victim_line->vpc, conv_ssd->lm.victim_line_cnt, conv_ssd->lm.full_line_cnt,
              conv_ssd->lm.free_line_cnt);

    conv_ssd->wfc.credits_to_refill = victim_line->ipc;

    /* copy back valid data */
    for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
        ppa.g.pg = flashpg * spp->pgs_per_flashpg; 
        for (ch = 0; ch < spp->nchs; ch++) {
            for (lun = 0; lun < spp->luns_per_ch; lun++) {
                ppa.g.ch = ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                lunp = get_lun(&conv_ssd->ssd, &ppa);
                clean_one_flashpg(conv_ssd, &ppa);

                if (flashpg == (spp->flashpgs_per_blk - 1)){
                    mark_block_free(conv_ssd, &ppa);

                    if (spp->enable_gc_delay) {
                        struct nand_cmd gce;
                        gce.type = GC_IO;
                        gce.cmd = NAND_ERASE;
                        gce.stime = 0;
                        gce.interleave_pci_dma = false;
                        ssd_advance_status(&conv_ssd->ssd, &ppa, &gce);
                    }

                    lunp->gc_endtime = lunp->next_lun_avail_time;
                }
            }
        }
    }

    /* update line status */
    mark_line_free(conv_ssd, &ppa);

    return 0;
}

void conv_gc_bg(void) {
    unsigned int i = 0;
    struct conv_ssd *conv_ssd;

    for (i = 0; i < SSD_PARTITIONS; i++) {
        conv_ssd = conv_ssd_instance(i);

        if (should_gc(conv_ssd)) {
			NVMEV_DEBUG("NEED GC!\n");
			do_gc(conv_ssd, false); // 782336
		}
    }
}

void conv_gc(struct conv_ssd *conv_ssd) {
    if (should_gc_high(conv_ssd)) {
        NVMEV_DEBUG("should_gc_high passed");
        /* perform GC here until !should_gc(conv_ssd) */
        do_gc(conv_ssd, true);
    }
}

bool is_same_flash_page(struct conv_ssd * conv_ssd, struct ppa ppa1, struct ppa ppa2) 
{   
    uint32_t ppa1_page = ppa1.g.pg / conv_ssd->sp.pgs_per_flashpg;
    uint32_t ppa2_page = ppa2.g.pg / conv_ssd->sp.pgs_per_flashpg;
    
    return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) &&
           (ppa1_page == ppa2_page);
}

bool conv_read(struct nvme_request * req, struct nvme_result * ret)
{
    struct nvme_command * cmd = req->cmd;
    uint64_t nsecs_start = req->nsecs_start;
    struct conv_ssd *conv_ssd;
    struct ssdparams *spp = &(conv_ssd_instance(0)->sp);  
    uint64_t lba = cmd->rw.slba;
    int nr_lba = (cmd->rw.length + 1);
    struct ppa cur_ppa;
    struct ppa prev_ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
    uint64_t lpn, local_lpn;
    uint64_t nsecs_completed, nsecs_latest = nsecs_start;
    uint32_t xfer_size, i;
    struct nand_cmd srd;
    srd.type = USER_IO;
    srd.cmd = NAND_READ;
    srd.stime = nsecs_start;
    srd.interleave_pci_dma = true;

    NVMEV_DEBUG("conv_read: start_lpn=%lld, len=%d, end_lpn=%ld", start_lpn, nr_lba, end_lpn);
    if (LPN_TO_LOCAL_LPN(end_lpn) >= spp->tt_pgs) {
        NVMEV_ERROR("conv_read: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
        return false;
    }

    if (LBA_TO_BYTE(nr_lba) <= spp->fw_rd0_size)
        srd.stime += spp->fw_rd0_lat;
    else
        srd.stime += spp->fw_rd1_lat;


    for (i = 0; (i < SSD_PARTITIONS) && (start_lpn <= end_lpn); i++, start_lpn++) {
        conv_ssd = conv_ssd_instance(start_lpn);
        xfer_size = 0;
        prev_ppa = get_maptbl_ent(conv_ssd, LPN_TO_LOCAL_LPN(start_lpn));
        
        NVMEV_DEBUG("[%s] conv_ssd=%p, ftl_ins=%lld, local_lpn=%lld",__FUNCTION__, conv_ssd, LPN_TO_SSD_ID(lpn), LPN_TO_LOCAL_LPN(lpn));

        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn+=SSD_PARTITIONS) {
            local_lpn = LPN_TO_LOCAL_LPN(lpn);
            cur_ppa = get_maptbl_ent(conv_ssd, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ssd, &cur_ppa))
            {
                NVMEV_DEBUG("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
                NVMEV_DEBUG("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
                cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk, cur_ppa.g.pl, cur_ppa.g.pg);
                continue;
            }

            // aggregate read io in same flash page
            if (mapped_ppa(&prev_ppa) && is_same_flash_page(conv_ssd, cur_ppa, prev_ppa)) {
                xfer_size += spp->pgsz;
                continue;
            }

            if (xfer_size > 0) {
                srd.xfer_size = xfer_size;
                nsecs_completed = ssd_advance_status(&conv_ssd->ssd, &prev_ppa, &srd);
                nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
            }
            
            xfer_size = spp->pgsz;
            prev_ppa = cur_ppa;
        }

        // issue remaining io
        if (xfer_size > 0) {
            srd.xfer_size = xfer_size;
            nsecs_completed = ssd_advance_status(&conv_ssd->ssd, &prev_ppa, &srd);
            nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
        }
    }

    ret->nsecs_target = nsecs_latest;
    ret->status = NVME_SC_SUCCESS; 
    return true;
}

void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, struct buffer * write_buffer, unsigned int buffs_to_release);
bool conv_write(struct nvme_request * req, struct nvme_result * ret)
{
    struct nvme_command * cmd = req->cmd;
    uint64_t nsecs_start = req->nsecs_start;
    uint64_t lba = cmd->rw.slba;
    struct conv_ssd *conv_ssd;
    struct ssdparams *spp = &conv_ssd_instance(0)->sp;
    struct buffer * wbuffer = conv_ssd_instance(0)->write_buffer;

    int nr_lba = (cmd->rw.length + 1);
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn, local_lpn;
    uint64_t nsecs_completed = 0, nsecs_latest;
    uint64_t nsecs_xfer_completed;
    uint32_t allocated_buf_size;
    struct nand_cmd swr;

    NVMEV_DEBUG("conv_write: start_lpn=%lld, len=%d, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
    if (LPN_TO_LOCAL_LPN(end_lpn)  >= spp->tt_pgs) {
        NVMEV_ERROR("conv_write: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n", start_lpn, spp->tt_pgs);
        return false;
    }
    
    //swr.stime = swr.stime > temp_latest_early_completed[req->sq_id] ? swr.stime : temp_latest_early_completed[req->sq_id];  
    allocated_buf_size = buffer_allocate(wbuffer, LBA_TO_BYTE(nr_lba)); 
	
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

    nsecs_latest = nsecs_start;
	nsecs_latest += spp->fw_wr0_lat;
	nsecs_latest += spp->fw_wr1_lat * (end_lpn - start_lpn + 1);
    nsecs_latest = ssd_advance_pcie(&conv_ssd_instance(0)->ssd, 
                                        nsecs_latest, LBA_TO_BYTE(nr_lba));
    nsecs_xfer_completed = nsecs_latest;

    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = nsecs_latest;
    swr.interleave_pci_dma = false;

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        conv_ssd = conv_ssd_instance(lpn);
        local_lpn = LPN_TO_LOCAL_LPN(lpn); 
        ppa = get_maptbl_ent(conv_ssd, local_lpn); // 현재 LPN에 대해 전에 이미 쓰인 PPA가 있는지 확인
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(conv_ssd, &ppa);
            set_rmap_ent(conv_ssd, INVALID_LPN, &ppa);
            NVMEV_JH("conv_write: %lld is invalid, ", ppa2pgidx(conv_ssd, &ppa));
        }

        /* new write */
        ppa = get_new_page(conv_ssd, USER_IO);
        /* update maptbl */
        set_maptbl_ent(conv_ssd, local_lpn, &ppa);
        NVMEV_JH("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ssd, &ppa));
        /* update rmap */
        set_rmap_ent(conv_ssd, local_lpn, &ppa);

        mark_page_valid(conv_ssd, &ppa);

        /* need to advance the write pointer here */
        advance_write_pointer(conv_ssd, USER_IO);

        /* Aggregate write io in flash page */
        if (last_pg_in_wordline(conv_ssd, &ppa)) {
            swr.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;

            nsecs_completed = ssd_advance_status(&conv_ssd->ssd, &ppa, &swr);
            nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

            enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuffer, spp->pgs_per_oneshotpg * spp->pgsz);
        } 
        
        consume_write_credit(conv_ssd);
        check_and_refill_write_credit(conv_ssd);
    }
    
    if (cmd->rw.control & NVME_RW_FUA) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;
    ret->status = NVME_SC_SUCCESS;

    return true;
    
}

/*TODO*/
void conv_flush(struct nvme_request * req, struct nvme_result * ret)
{   
	unsigned long long latest = 0;

	NVMEV_DEBUG("qid %d entry %d\n", sqid, sq_entry);

    latest = local_clock();
    #if 0
	for (i = 0; i < vdev->config.nr_io_units; i++) {
		latest = max(latest, vdev->io_unit_stat[i]);
	}
    #endif

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool conv_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret)
{
    struct nvme_command *cmd = req->cmd;
    size_t csi = NS_CSI(cmd->common.nsid - 1);
    NVMEV_ASSERT(csi == NVME_CSI_NVM);

    //printk("%s csi=%d opcode=%d\n",__FUNCTION__, csi, cmd->common.opcode);
    switch(cmd->common.opcode) {
        case nvme_cmd_write:
            if (!conv_write(req, ret))
                return false;
            break;
        case nvme_cmd_read:
            if (!conv_read(req, ret))
                return false;
            break;
        case nvme_cmd_flush:
            conv_flush(req, ret);
            break;
        case nvme_cmd_write_uncor:
        case nvme_cmd_compare:
        case nvme_cmd_write_zeroes:
        case nvme_cmd_dsm:
        case nvme_cmd_resv_register:
        case nvme_cmd_resv_report:
        case nvme_cmd_resv_acquire:
        case nvme_cmd_resv_release:
            break;
        default:
            break;
    }

    return true;
}