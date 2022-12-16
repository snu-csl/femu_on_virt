#include "nvmev.h"
#include "ftl.h"
#include "zns.h"

#if SUPPORT_ZNS

struct zns_ssd g_zns_ssd;
extern struct buffer global_write_buffer;

void zns_init_descriptor(struct zns_ssd *zns_ssd)
{
	struct zone_descriptor *zone_descs;
	__u32 zone_size = zns_ssd->zone_size;
	__u32 nr_zones = zns_ssd->nr_zones;
	__u64 zslba = 0;
	__u32 i = 0;
	const __u32 zrwa_buffer_size = zns_ssd->zrwa_buffer_size;

	zns_ssd->zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_ATOMIC);
	zns_ssd->report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (nr_zones - 1), GFP_ATOMIC);
	zns_ssd->zwra_buffer = (struct buffer *) kmalloc(sizeof(struct buffer) * nr_zones, GFP_ATOMIC);
	
	zone_descs = zns_ssd->zone_descs;
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * zns_ssd->nr_zones);

	for (i = 0; i < nr_zones; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zns_ssd->zone_descs[i].wp = zslba;
		zslba += BYTE_TO_LBA(zone_size);
		zone_descs[i].zone_capacity = BYTE_TO_LBA(zone_size);
		
		buffer_init(&(zns_ssd->zwra_buffer[i]), zrwa_buffer_size); 

		NVMEV_ZNS_DEBUG("[i] zslba 0x%llx zone capacity 0x%llx\n", zone_descs[i].zslba, zone_descs[i].zone_capacity);
	}
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

	zns_ssd->ns = namespace;
	zns_ssd->storage_base_addr = storage_base_addr;
	
	/* It should be 4KB aligned, according to lpn size */
	NVMEV_ASSERT((zns_ssd->zone_size % zns_ssd->ssd.sp.chunksz) == 0); 
	
	zns_init_descriptor(zns_ssd);
	zns_init_resource(zns_ssd);

	buffer_init(&global_write_buffer, WRITE_BUFFER_SIZE);
}

void zns_exit(void)
{
	struct zns_ssd * zns_ssd = get_zns_ssd_instance();
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zns_ssd->zone_descs);
	kfree(zns_ssd->report_buffer);
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
            ssd_flush(req, ret);
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