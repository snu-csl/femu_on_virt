#include "nvmev.h"
#include "ftl.h"
#include "zns.h"

#if SUPPORT_ZNS

struct zns_ssd g_zns_ssd;

void zns_init_descriptor(struct zns_ssd *zns_ssd)
{
	struct zone_descriptor *zone_descs;
	__u32 zone_size = zns_ssd->zone_size;
	__u32 nr_zones = zns_ssd->nr_zones;
	__u64 zslba = 0;
	__u32 i = 0;
	
	zns_ssd->zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * nr_zones, GFP_ATOMIC);
	zns_ssd->report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (nr_zones - 1), GFP_ATOMIC);
	
	zone_descs = zns_ssd->zone_descs;
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * zns_ssd->nr_zones);

	for (i = 0; i < nr_zones; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zns_ssd->zone_descs[i].wp = zslba;
		zslba += BYTE_TO_LBA(zone_size);
		zone_descs[i].zone_capacity = BYTE_TO_LBA(zone_size);

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

void zns_init(unsigned int cpu_nr_dispatcher, unsigned long capacity)
{
	struct zns_ssd *zns_ssd = get_zns_ssd_instance();
	ssd_init_ftl_instance(&(zns_ssd->ssd), cpu_nr_dispatcher, capacity);
	
	struct ssd_pcie *pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
    ssd_init_pcie(pcie, &(zns_ssd->ssd.sp));

	zns_ssd->ssd.pcie = pcie;
	zns_ssd->zone_size = ZONE_SIZE;
	zns_ssd->nr_zones = capacity / ZONE_SIZE;
	zns_ssd->dies_per_zone = DIES_PER_ZONE;
	zns_ssd->nr_active_zones = zns_ssd->nr_zones; // max
	zns_ssd->nr_open_zones = zns_ssd->nr_zones; // max
	
	zns_init_descriptor(zns_ssd);
	zns_init_resource(zns_ssd);
}

void zns_exit(void)
{
	struct zns_ssd * zns_ssd = get_zns_ssd_instance();
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zns_ssd->zone_descs);
	kfree(zns_ssd->report_buffer);
}
#endif