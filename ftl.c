#include "ftl.h"
#include "nvmev.h"
#include <assert.h>

struct ssd ssd;






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

// static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
// {
//     pg->nsecs = spp->secs_per_pg;
//     pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
//     for (int i = 0; i < pg->nsecs; i++) {
//         pg->sec[i] = SEC_FREE;
//     }
//     pg->status = PG_FREE;
// }

// static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
// {
//     blk->npgs = spp->pgs_per_blk;
//     blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
//     for (int i = 0; i < blk->npgs; i++) {
//         ssd_init_nand_page(&blk->pg[i], spp);
//     }
//     blk->ipc = 0;
//     blk->vpc = 0;
//     blk->erase_cnt = 0;
//     blk->wp = 0;
// }

// static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
// {
//     pl->nblks = spp->blks_per_pl;
//     pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
//     for (int i = 0; i < pl->nblks; i++) {
//         ssd_init_nand_blk(&pl->blk[i], spp);
//     }
// }

// static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
// {
//     lun->npls = spp->pls_per_lun;
//     lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
//     for (int i = 0; i < lun->npls; i++) {
//         ssd_init_nand_plane(&lun->pl[i], spp);
//     }
//     lun->next_lun_avail_time = 0;
//     lun->busy = false;
// }

// static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
// {
//     ch->nluns = spp->luns_per_ch;
//     ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
//     for (int i = 0; i < ch->nluns; i++) {
//         ssd_init_nand_lun(&ch->lun[i], spp);
//     }
//     ch->next_ch_avail_time = 0;
//     ch->busy = 0;
// }

// static void ssd_init_maptbl(struct ssd *ssd)
// {
//     struct ssdparams *spp = &ssd->sp;

//     ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
//     for (int i = 0; i < spp->tt_pgs; i++) {
//         ssd->maptbl[i].ppa = UNMAPPED_PPA;
//     }
// }

// static void ssd_init_rmap(struct ssd *ssd)
// {
//     struct ssdparams *spp = &ssd->sp;

//     ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
//     for (int i = 0; i < spp->tt_pgs; i++) {
//         ssd->rmap[i] = INVALID_LPN;
//     }
// }

void ssd_init(void)
{

    struct ssdparams *spp = &(ssd.sp);

    // assert(ssd);

    ssd_init_params(spp);
    printk("init ssd done: %d", ssd.sp.tt_pgs);

}