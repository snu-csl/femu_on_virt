#include "nvmev.h"
#include "ftl.h"
#include "zns.h"
#include <linux/ktime.h>
#include <linux/sched/clock.h>
#if SUPPORT_ZNS

struct zns_ssd g_zns_ssd;
struct buffer zns_write_buffer;
extern struct nvmev_dev *vdev;
unsigned long long latest_flush_nsecs = 0;
const unsigned long long flush_interval = 5ULL*1000*1000*1000;

void zns_reset_desc_durable(__u32 zid)
{
	#if PERF_MEASURE == 0
	struct zns_ssd *zns_ssd = &g_zns_ssd;
	zns_ssd->zone_descs_durable[zid].wp = zns_ssd->zone_descs[zid].zslba; 

	NVMEV_DEBUG("%s zid=%d\n",__FUNCTION__, zid);

	memset(zns_ssd->wl_state[zid], 0, sizeof(__u16)*zns_ssd->wl_per_zone);
	#endif
}

void zns_flush_desc_durable(void)
{
	#if PERF_MEASURE == 0
	struct zns_ssd *zns_ssd = &g_zns_ssd;
	__u32 nr_zones = zns_ssd->nr_zones;

	memcpy(zns_ssd->zone_descs_durable, zns_ssd->zone_descs, sizeof(struct zone_descriptor) * nr_zones);	
	#endif	
}

void zns_flush_desc_durable_bg(void)
{
	#if PERF_MEASURE == 0
	struct zns_ssd *zns_ssd = &g_zns_ssd;
	__u32 nr_zones = zns_ssd->nr_zones;
	unsigned long long curr_nsecs_local = cpu_clock(zns_ssd->cpu_nr_dispatcher);
	
	if (curr_nsecs_local - latest_flush_nsecs > flush_interval) {
		if (vdev->config.write_time > 0) {
			memcpy(zns_ssd->zone_descs_durable, zns_ssd->zone_descs, sizeof(struct zone_descriptor) * nr_zones);
			latest_flush_nsecs = curr_nsecs_local;
		}
	}
	#endif
}

static inline unsigned long long __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}

void zns_flush(struct nvme_request * req, struct nvme_result * ret)
{   
  	struct zns_ssd *zns_ssd = &g_zns_ssd;
	__u32 nr_zones = zns_ssd->nr_zones;

	struct ssdparams *spp = &zns_ssd->ssd.sp;
    uint32_t i, j;
    uint64_t latest = __get_ioclock(&zns_ssd->ssd);
	uint64_t lba,lpn;
	struct ppa ppa;
	struct nand_cmd swr;

	NVMEV_DEBUG("%s \n",__FUNCTION__);

	for (i = 0; i < zns_ssd->nr_zones; i++) {
		lba = zns_ssd->zone_descs[i].wp;
		lpn = lba / spp->secs_per_chunk;
		ppa = __lpn_to_ppa(zns_ssd, lpn);

		if (zns_ssd->zone_descs[i].state != ZONE_STATE_FULL && ppa.g.chunk > 0) {
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE;
			swr.stime = latest;
			swr.xfer_size = spp->chunks_per_pgm_pg * spp->chunksz;

			ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);	
		}
		
	}

    for (i = 0; i < spp->nchs; i++) {
		struct ssd_channel *ch = &zns_ssd->ssd.ch[i];

        for (j = 0; j < spp->luns_per_ch; j++) {
			struct nand_lun *lun = &ch->lun[j];
            latest = max(latest, lun->next_lun_avail_time);
        }
    }

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

void zns_recover_metadata(void)
{
	struct zns_ssd *zns_ssd = &g_zns_ssd;
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	__u32 nr_zones = zns_ssd->nr_zones;
	int i, j;
	int secs;
	__u32 secs_per_pgm_pg = spp->chunks_per_pgm_pg * spp->secs_per_chunk;
	__u32 wl_off;
	__u64 elba;
	bool error;
	NVMEV_DEBUG("%s\n",__FUNCTION__);

	buffer_refill(&zns_write_buffer);

	for (i = 0; i < nr_zones; i++) {
		if (zns_ssd->zone_descs[i].wp != zns_ssd->zone_descs_durable[i].wp) {
			
			NVMEV_ERROR("%s recover zone=%d wp=0x%llx->0x%llx state=%d\n",__FUNCTION__, i, 
					zns_ssd->zone_descs[i].wp, zns_ssd->zone_descs_durable[i].wp, zns_ssd->zone_descs[i].state);
			NVMEV_ASSERT(zns_ssd->zone_descs_durable[i].wp < zns_ssd->zone_descs[i].wp);

			zns_ssd->zone_descs[i].wp = zns_ssd->zone_descs_durable[i].wp;

			if (zns_ssd->zone_descs[i].state == ZONE_STATE_FULL) 
				zns_ssd->zone_descs[i].state = ZONE_STATE_OPENED_IMPL;
			
			elba = zns_ssd->zone_descs[i].zslba + zns_ssd->zone_descs[i].zone_capacity;

			memset(vdev->ns_mapped[1] + (zns_ssd->zone_descs[i].wp * spp->secsz), 0,  (elba - zns_ssd->zone_descs[i].wp) * spp->secsz);
		}

		wl_off = (zns_ssd->zone_descs[i].wp - zns_ssd->zone_descs[i].zslba) / secs_per_pgm_pg;
		
		if ((zns_ssd->zone_descs[i].wp % secs_per_pgm_pg) > 0) {
			wl_off++;

			//error = buffer_allocate(&zns_write_buffer, (zns_ssd->zone_descs[i].wp % secs_per_pgm_pg) * spp->secsz);
			//NVMEV_ASSERT(error == ((zns_ssd->zone_descs[i].wp % secs_per_pgm_pg) * spp->secsz));
		}

		for (j = wl_off; j < zns_ssd->wl_per_zone; j++) {
			zns_ssd->wl_state[i][j] = 0;
		}
	}
}

void zns_advance_durable_write_pointer(__u64 lba)
{
	#if PERF_MEASURE == 0
	struct zns_ssd *zns_ssd = get_zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	struct zone_descriptor *zone_descs_durable = zns_ssd->zone_descs_durable;
	int i;
	__u64 lpn = lba / spp->secs_per_chunk;
	__u32 secs_per_pgm_pg = spp->chunks_per_pgm_pg * spp->secs_per_chunk;
	// get zone from start_lba
	__u32 zid = lpn_to_zone(zns_ssd, lpn);
	__u64 slpn = zone_to_slpn(zns_ssd, zid);
	__u64 llpn = zone_to_slpn(zns_ssd, zid+1);

	__u64 old = zone_descs_durable[zid].wp / spp->secs_per_chunk;
	__u64 old_wl_off = (old - slpn) / spp->chunks_per_pgm_pg;

	__u64 wl_off = (lpn - slpn) / spp->chunks_per_pgm_pg;
	__u64 nr_wls = DIV_ROUND_UP(zns_ssd->zone_size/spp->chunksz,  spp->chunks_per_pgm_pg);
	__u64 lba_off = (lba - (slpn * spp->secs_per_chunk))% (secs_per_pgm_pg);
	__u64 new_wp, sectors_last_wl;

	//NVMEV_ASSERT(zns_ssd->wl_state[zid][wl_off] < (lba_off + 1));

	/*
	if (zns_ssd->wl_state[zid][wl_off] >= (lba_off + 1))
		NVMEV_ERROR("zid=%d wl_state=%d lba_off=0x%llx lba=0x%llx wl_off=%llu\n", 
		zid, zns_ssd->wl_state[zid][wl_off], lba_off, lba, wl_off);
	*/
	zns_ssd->wl_state[zid][wl_off] = lba_off + 1;

	for (i = old_wl_off; i < nr_wls; i++) {
		if (zns_ssd->wl_state[zid][i] == 0)
			break;

		if ((i == (nr_wls - 1)) && (zns_ssd->zone_size % (secs_per_pgm_pg * spp->secsz))) {
			sectors_last_wl = BYTE_TO_LBA(zns_ssd->zone_size) % secs_per_pgm_pg;

			if (zns_ssd->wl_state[zid][i] != sectors_last_wl)
				break;

		}
		else {
			if (zns_ssd->wl_state[zid][i] != secs_per_pgm_pg)
				break;

			
		}

		new_wp = zone_descs_durable[zid].zslba + 
										(i * secs_per_pgm_pg) + 
										zns_ssd->wl_state[zid][i];
		//NVMEV_ASSERT(zone_descs_durable[zid].wp < new_wp);

		NVMEV_DEBUG("New WP zid=%d wl=%d wl_state=%d lba_off=0x%llx lba=0x%llx wl_off=%llu wp_old=0x%llx wl_new=0x%llx remaining=0x%llx\n", 
		zid, i, zns_ssd->wl_state[zid][i], lba_off, lba, wl_off, zone_descs_durable[zid].wp, new_wp,zns_write_buffer.remaining);

		zone_descs_durable[zid].wp = new_wp;	
	}
	#endif
}

void zns_init_descriptor(struct zns_ssd *zns_ssd)
{
	struct zone_descriptor *zone_descs;
	__u32 zone_size = zns_ssd->zone_size;
	__u32 nr_zones = zns_ssd->nr_zones;
	__u64 zslba = 0;
	__u32 i = 0;
	
	const __u32 zrwa_buffer_size = zns_ssd->zrwa_buffer_size;

	zns_ssd->zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_ATOMIC);
	zns_ssd->zone_descs_durable = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_ATOMIC);
	zns_ssd->report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (nr_zones - 1), GFP_ATOMIC);
	zns_ssd->zwra_buffer = (struct buffer *) kmalloc(sizeof(struct buffer) * nr_zones, GFP_ATOMIC);
	zns_ssd->write_buffer = (struct buffer *) kmalloc(sizeof(struct buffer) * nr_zones, GFP_ATOMIC);
#if MEASURE_QD	
	zns_ssd->lock = (spinlock_t *) kmalloc(sizeof(spinlock_t) * nr_zones, GFP_ATOMIC);
#endif
	zone_descs = zns_ssd->zone_descs;
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * zns_ssd->nr_zones);

	zns_ssd->wl_state = (__u16 **) kmalloc(sizeof(__u16 *) * nr_zones, GFP_ATOMIC);
	
	for (i = 0; i < nr_zones; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zns_ssd->zone_descs[i].wp = zslba;
		zslba += BYTE_TO_LBA(zone_size);
		zone_descs[i].zone_capacity = BYTE_TO_LBA(zone_size);
		
		buffer_init(&(zns_ssd->zwra_buffer[i]), zrwa_buffer_size); 
		buffer_init(&(zns_ssd->write_buffer[i]), WRITE_BUFFER_SIZE); 

		zns_ssd->wl_state[i] = (__u16 *) kmalloc(sizeof(__u16) * zns_ssd->wl_per_zone, GFP_ATOMIC);
#if MEASURE_QD	
		spin_lock_init(&zns_ssd->lock[i]);
#endif
		memset(zns_ssd->wl_state[i], 0, sizeof(__u16) * zns_ssd->wl_per_zone);
		NVMEV_ZNS_DEBUG("[i] zslba 0x%llx zone capacity 0x%llx\n", zone_descs[i].zslba, zone_descs[i].zone_capacity);
	}

	memcpy(zns_ssd->zone_descs_durable, zns_ssd->zone_descs, sizeof(struct zone_descriptor) * zns_ssd->nr_zones);
}

void zns_init_resource(struct zns_ssd *zns_ssd)
{
	struct zone_resource_info *res_infos = zns_ssd->res_infos;

	res_infos[ACTIVE_ZONE].total_cnt = zns_ssd->nr_zones;
	res_infos[ACTIVE_ZONE].acquired_cnt = 0;

	res_infos[OPEN_ZONE].total_cnt = zns_ssd->nr_zones;
	res_infos[OPEN_ZONE].acquired_cnt = 0;

	res_infos[ZRWA_ZONE].total_cnt = zns_ssd->nr_zones;
	res_infos[ZRWA_ZONE].acquired_cnt = 0;

}

void zns_init(unsigned int cpu_nr_dispatcher, void * storage_base_addr, unsigned long capacity, unsigned int namespace)
{
	struct zns_ssd *zns_ssd = get_zns_ssd_instance();
	struct ssd_pcie *pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
	struct ssdparams *spp = &(zns_ssd->ssd.sp);

	ssd_init_ftl_instance(&(zns_ssd->ssd), cpu_nr_dispatcher, capacity);
    ssd_init_pcie(pcie, &(zns_ssd->ssd.sp));

	zns_ssd->ssd.pcie = pcie;
	zns_ssd->zone_size = ZONE_SIZE;
	zns_ssd->nr_zones = capacity / ZONE_SIZE;
	zns_ssd->dies_per_zone = DIES_PER_ZONE;
	zns_ssd->nr_active_zones = zns_ssd->nr_zones; // max
	zns_ssd->nr_open_zones = zns_ssd->nr_zones; // max
	zns_ssd->nr_zrwa_zones = MAX_ZRWA_ZONES;
	zns_ssd->zrwa_size = ZRWA_SIZE;
	zns_ssd->zrwafg_size = ZRWAFG_SIZE;
	zns_ssd->zrwa_buffer_size = ZRWA_BUFFER_SIZE;
	zns_ssd->lbas_per_zrwa = zns_ssd->zrwa_size / zns_ssd->ssd.sp.secsz;
	zns_ssd->lbas_per_zrwafg = zns_ssd->zrwafg_size / zns_ssd->ssd.sp.secsz;
	zns_ssd->wl_per_zone = DIV_ROUND_UP(zns_ssd->zone_size,  spp->chunks_per_pgm_pg * spp->secs_per_chunk * spp->secsz);

	zns_ssd->ns = namespace;
	zns_ssd->storage_base_addr = storage_base_addr;
	
	/* It should be 4KB aligned, according to lpn size */
	NVMEV_ASSERT((zns_ssd->zone_size % zns_ssd->ssd.sp.chunksz) == 0); 
	
	zns_init_descriptor(zns_ssd);
	zns_init_resource(zns_ssd);


	buffer_init(&zns_write_buffer, WRITE_BUFFER_SIZE);
}

void zns_exit(void)
{
	struct zns_ssd * zns_ssd = get_zns_ssd_instance();
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zns_ssd->zone_descs);
	kfree(zns_ssd->report_buffer);
}
extern struct nvmev_dev *vdev;

 bool zns_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret)
 {
    struct nvme_command *cmd = req->cmd;
    size_t csi = NS_CSI(cmd->common.nsid - 1);
    NVMEV_ASSERT(csi == NVME_CSI_ZNS);
    switch(cmd->common.opcode) {
        case nvme_cmd_write:
			if (vdev->config.write_time == 0) {
				ret->status = NVME_SC_SUCCESS;
				ret->nsecs_target = req->nsecs_start;
				ret->ignore = true;
			} else {
				ret->ignore = false;
				if (!zns_write(req, ret))
					return false;
			}
			break;
        case nvme_cmd_read:
            if (!zns_read(req, ret))
                return false;
            break;
        case nvme_cmd_flush:
			if (vdev->config.write_time == 0) {
				ret->status = NVME_SC_SUCCESS;
				ret->nsecs_target = req->nsecs_start;
				ret->ignore = true;
			} else {
				ret->ignore = false;
            	zns_flush(req, ret);
			}
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
        case nvme_cmd_zone_mgmt_send:
			if (vdev->config.write_time == 0) {
				ret->status = NVME_SC_SUCCESS;
				ret->nsecs_target = req->nsecs_start;
				ret->ignore = true;
			} else {
				ret->ignore = false;
				zns_zmgmt_send(req, ret);
			}
			break;
		case nvme_cmd_zone_mgmt_recv:
			zns_zmgmt_recv(req, ret);
			break;
		case nvme_cmd_zone_append:
		default:
            break;
	}

    return true;
 }
#endif