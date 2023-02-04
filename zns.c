#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"
#include "zns.h"

#if SUPPORT_ZNS

struct zns_ssd *g_zns_ssd = NULL;

static void zns_init_descriptor(struct zns_ssd *zns_ssd)
{
	struct zone_descriptor *zone_descs;
	__u32 zone_size = zns_ssd->zp.zone_size;
	__u32 nr_zones = zns_ssd->zp.nr_zones;
	__u64 zslba = 0;
	__u32 i = 0;
	const __u32 zrwa_buffer_size = zns_ssd->zp.zrwa_buffer_size;

	zns_ssd->zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_ATOMIC);
	zns_ssd->report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (nr_zones - 1), GFP_ATOMIC);
	zns_ssd->zwra_buffer = (struct buffer *) kmalloc(sizeof(struct buffer) * nr_zones, GFP_ATOMIC);
	
	zone_descs = zns_ssd->zone_descs;
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * zns_ssd->zp.nr_zones);

	for (i = 0; i < nr_zones; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zone_descs[i].wp = zslba;
		zslba += BYTE_TO_LBA(zone_size);
		zone_descs[i].zone_capacity = BYTE_TO_LBA(zone_size);
		
		buffer_init(&(zns_ssd->zwra_buffer[i]), zrwa_buffer_size); 

		NVMEV_ZNS_DEBUG("[i] zslba 0x%llx zone capacity 0x%llx\n", zone_descs[i].zslba, zone_descs[i].zone_capacity);
	}
}

static void zns_init_resource(struct zns_ssd *zns_ssd)
{
	struct zone_resource_info *res_infos = zns_ssd->res_infos;

	res_infos[ACTIVE_ZONE].total_cnt = zns_ssd->zp.nr_zones;
	res_infos[ACTIVE_ZONE].acquired_cnt = 0;

	res_infos[OPEN_ZONE].total_cnt = zns_ssd->zp.nr_zones;
	res_infos[OPEN_ZONE].acquired_cnt = 0;

	res_infos[ZRWA_ZONE].total_cnt = zns_ssd->zp.nr_zones;
	res_infos[ZRWA_ZONE].acquired_cnt = 0;

}

static void zns_init_params(struct znsparams *zpp, struct ssdparams *spp, __u64 capacity)
{
	zpp->zone_size = ZONE_SIZE;
	zpp->nr_zones = capacity / ZONE_SIZE;
	zpp->dies_per_zone = DIES_PER_ZONE;
	zpp->nr_active_zones = zpp->nr_zones; // max
	zpp->nr_open_zones = zpp->nr_zones; // max
	zpp->nr_zrwa_zones = MAX_ZRWA_ZONES;
	zpp->zrwa_size = ZRWA_SIZE;
	zpp->zrwafg_size = ZRWAFG_SIZE;
	zpp->zrwa_buffer_size = ZRWA_BUFFER_SIZE;
	zpp->lbas_per_zrwa = zpp->zrwa_size / spp->secsz;
	zpp->lbas_per_zrwafg = zpp->zrwafg_size / spp->secsz;

	NVMEV_INFO("zone_size=%d(KB), # zones=%d # die/zone=%d \n", zpp->zone_size, zpp->nr_zones, zpp->dies_per_zone);
}

void zns_init(__u64 capacity, __u32 cpu_nr_dispatcher, void * storage_base_addr, __u32 namespace)
{
	struct zns_ssd *zns_ssd;
	struct ssdparams spp;

	const __u32 nparts = 1; /* Not support multi partitions for zns*/

	zns_ssd = kmalloc(sizeof(struct zns_ssd) * nparts, GFP_KERNEL);

	ssd_init_params(&spp, capacity, nparts);
	zns_init_params(&zns_ssd->zp, &spp, capacity);
	
	ssd_init(&zns_ssd->ssd, &spp, cpu_nr_dispatcher);

	zns_ssd->ns = namespace;
	zns_ssd->storage_base_addr = storage_base_addr;
	
	/* It should be 4KB aligned, according to lpn size */
	NVMEV_ASSERT((zns_ssd->zp.zone_size % spp.chunksz) == 0); 
	
	zns_init_descriptor(zns_ssd);
	zns_init_resource(zns_ssd);

	NVMEV_ASSERT(g_zns_ssd == NULL);
	g_zns_ssd = zns_ssd;
}

void zns_exit(void)
{
	struct zns_ssd * zns_ssd = zns_ssd_instance();
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zns_ssd->zone_descs);
	kfree(zns_ssd->report_buffer);
}

void zns_flush(struct nvme_request * req, struct nvme_result * ret)
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

 bool zns_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret)
 {
    struct nvme_command *cmd = req->cmd;
    size_t csi = NS_CSI(cmd->common.nsid - 1);
    NVMEV_ASSERT(csi == NVME_CSI_ZNS);
    switch(cmd->common.opcode) {
        case nvme_cmd_write:
            if (!zns_write(req, ret))
                return false;
            break;
        case nvme_cmd_read:
            if (!zns_read(req, ret))
                return false;
            break;
        case nvme_cmd_flush:
            zns_flush(req, ret);
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
			zns_zmgmt_send(req, ret);
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