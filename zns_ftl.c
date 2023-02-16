#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"

static void zns_init_descriptor(struct zns_ftl *zns_ftl)
{
	struct zone_descriptor *zone_descs;
	uint32_t zone_size = zns_ftl->zp.zone_size;
	uint32_t nr_zones = zns_ftl->zp.nr_zones;
	uint64_t zslba = 0;
	uint32_t i = 0;
	const uint32_t zrwa_buffer_size = zns_ftl->zp.zrwa_buffer_size;

	zns_ftl->zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_ATOMIC);
	zns_ftl->report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (nr_zones - 1), GFP_ATOMIC);
	zns_ftl->zwra_buffer = (struct buffer *) kmalloc(sizeof(struct buffer) * nr_zones, GFP_ATOMIC);
	
	zone_descs = zns_ftl->zone_descs;
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * zns_ftl->zp.nr_zones);

	for (i = 0; i < nr_zones; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zone_descs[i].wp = zslba;
		zslba += BYTE_TO_LBA(zone_size);
		zone_descs[i].zone_capacity = BYTE_TO_LBA(zone_size);
		
		buffer_init(&(zns_ftl->zwra_buffer[i]), zrwa_buffer_size); 

		NVMEV_ZNS_DEBUG("[i] zslba 0x%llx zone capacity 0x%llx\n", zone_descs[i].zslba, zone_descs[i].zone_capacity);
	}
}

static void zns_init_resource(struct zns_ftl *zns_ftl)
{
	struct zone_resource_info *res_infos = zns_ftl->res_infos;

	res_infos[ACTIVE_ZONE].total_cnt = zns_ftl->zp.nr_zones;
	res_infos[ACTIVE_ZONE].acquired_cnt = 0;

	res_infos[OPEN_ZONE].total_cnt = zns_ftl->zp.nr_zones;
	res_infos[OPEN_ZONE].acquired_cnt = 0;

	res_infos[ZRWA_ZONE].total_cnt = zns_ftl->zp.nr_zones;
	res_infos[ZRWA_ZONE].acquired_cnt = 0;

}

static void zns_init_params(struct znsparams *zpp, struct ssdparams *spp, uint64_t capacity)
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

struct zns_ftl * zns_create_and_init(uint64_t capacity, uint32_t cpu_nr_dispatcher, void * storage_base_addr, uint32_t namespace)
{
	struct zns_ftl *zns_ftl;
	struct ssdparams spp;

	const uint32_t nparts = 1; /* Not support multi partitions for zns*/

	zns_ftl = kmalloc(sizeof(struct zns_ftl) * nparts, GFP_KERNEL);

	ssd_init_params(&spp, capacity, nparts);
	zns_init_params(&zns_ftl->zp, &spp, capacity);
	
	ssd_init(&zns_ftl->ssd, &spp, cpu_nr_dispatcher);

	zns_ftl->ns = namespace;
	zns_ftl->storage_base_addr = storage_base_addr;
	
	/* It should be 4KB aligned, according to lpn size */
	NVMEV_ASSERT((zns_ftl->zp.zone_size % spp.pgsz) == 0); 
	
	zns_init_descriptor(zns_ftl);
	zns_init_resource(zns_ftl);

	return zns_ftl;
}

void zns_exit(struct zns_ftl *zns_ftl)
{
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zns_ftl->zone_descs);
	kfree(zns_ftl->report_buffer);
}

void zns_flush(struct zns_ftl *zns_ftl, struct nvme_request *req, struct nvme_result *ret)
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

 bool zns_proc_nvme_io_cmd(struct zns_ftl *zns_ftl, struct nvme_request * req, struct nvme_result * ret)
 {
    struct nvme_command *cmd = req->cmd;
    size_t csi = NS_CSI(cmd->common.nsid - 1);
    NVMEV_ASSERT(csi == NVME_CSI_ZNS);
    switch(cmd->common.opcode) {
        case nvme_cmd_write:
            if (!zns_write(zns_ftl, req, ret))
                return false;
            break;
        case nvme_cmd_read:
            if (!zns_read(zns_ftl, req, ret))
                return false;
            break;
        case nvme_cmd_flush:
            zns_flush(zns_ftl, req, ret);
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
			zns_zmgmt_send(zns_ftl, req, ret);
			break;
		case nvme_cmd_zone_mgmt_recv:
			zns_zmgmt_recv(zns_ftl, req, ret);
			break;
		case nvme_cmd_zone_append:
		default:
            break;
	}

    return true;
 }