#include "ftl.h"
#include "nvmev.h"

struct ssd ssd;

static inline void ftl_assert(bool true, int num_line) {
    if (!true)
        printk("assert happend at line %d", num_line);
}

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    if(!(lpn < ssd->sp.tt_pgs)) {
        NVMEV_ERROR("lpn is not smaller than tt_pages");
    }
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    printk("[JH_LOG] ppa2pgidx: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
        ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    if (!(pgidx < spp->tt_pgs)) {
        NVMEV_ERROR("pgidx < spp->tt_pgs");
    }

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

// static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
// {
//     return (next > curr);
// }

// static inline pqueue_pri_t victim_line_get_pri(void *a)
// {
//     return ((struct line *)a)->vpc;
// }

// static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
// {
//     ((struct line *)a)->vpc = pri;
// }

// static inline size_t victim_line_get_pos(void *a)
// {
//     return ((struct line *)a)->pos;
// }

// static inline void victim_line_set_pos(void *a, size_t pos)
// {
//     ((struct line *)a)->pos = pos;
// }

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    int i;

    lm->tt_lines = spp->blks_per_pl;
    if (!(lm->tt_lines == spp->tt_lines)) {
        NVMEV_ERROR("tt_lines config doesn't match");
    }

    lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);

    // QTAILQ_INIT(&lm->free_line_list);
    // lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
    //         victim_line_get_pri, victim_line_set_pri,
    //         victim_line_get_pos, victim_line_set_pos);
    // QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        // QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    // ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    // curline = QTAILQ_FIRST(&lm->free_line_list);
    // QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    if (!(a >= 0 && a < max)) {
        NVMEV_ERROR("a >= 0 && a < max");
    }
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    // curline = QTAILQ_FIRST(&lm->free_line_list);
    // if (!curline) {
    //     ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
    //     return NULL;
    // }

    // QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    printk("[JH_LOG] current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", 
        wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            // if (wpp->pg == spp->pgs_per_blk) {
            //     wpp->pg = 0;
            //     /* move current line to {victim,full} line list */
            //     if (wpp->curline->vpc == spp->pgs_per_line) {
            //         /* all pgs are still valid, move to full line list */
            //         ftl_assert(wpp->curline->ipc == 0);
            //         QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
            //         lm->full_line_cnt++;
            //     } else {
            //         ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
            //         /* there must be some invalid pages in this line */
            //         ftl_assert(wpp->curline->ipc > 0);
            //         pqueue_insert(lm->victim_line_pq, wpp->curline);
            //         lm->victim_line_cnt++;
            //     }
            //     /* current line is used up, pick another empty line */
            //     check_addr(wpp->blk, spp->blks_per_pl);
            //     wpp->curline = NULL;
            //     wpp->curline = get_next_free_line(ssd);
            //     if (!wpp->curline) {
            //         /* TODO */
            //         // abort();
            //         printk("curline is NULL -> normal");
            //     }
            //     wpp->blk = wpp->curline->id;
            //     check_addr(wpp->blk, spp->blks_per_pl);
            //     /* make sure we are starting from page 0 in the super block */
            //     ftl_assert(wpp->pg == 0, __LINE__);
            //     ftl_assert(wpp->lun == 0, __LINE__);
            //     ftl_assert(wpp->ch == 0, __LINE__);
            //     /* TODO: assume # of pl_per_lun is 1, fix later */
            //     ftl_assert(wpp->pl == 0, __LINE__);
            // }
        }
    }

    printk("[JH_LOG] advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", 
        wpp->ch, wpp->lun, wpp->pg, wpp->blk, wpp->pg);
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0, __LINE__);

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

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 256;
    spp->blks_per_pl = 256; /* 16GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

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
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = 1;
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
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
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

void ssd_init(void)
{

    struct ssdparams *spp = &(ssd.sp);
    int i;

    ssd_init_params(spp);

    /* initialize ssd internal layout architecture */
    ssd.ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); // 40 * 8 = 320
    for (i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&(ssd.ch[i]), spp);
    }

    /* initialize maptbl */
    printk("initialize maptbl");
    ssd_init_maptbl(&ssd); // mapping table

    /* initialize rmap */
    printk("initialize rmap");
    ssd_init_rmap(&ssd); // reverse mapping table (?)

    /* initialize all the lines */
    printk("initialize lines");
    ssd_init_lines(&ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    printk("initialize write pointer");
    ssd_init_write_pointer(&ssd);

    // qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
    //                    QEMU_THREAD_JOINABLE);
}
