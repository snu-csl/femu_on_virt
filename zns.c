#include "nvmev.h"
#include "ftl.h"
#include "zns.h"

#if SUPPORT_ZNS 

struct zone_resource_info res_infos[RES_TYPE_COUNT];
struct zone_descriptor * zone_descs;

void zns_init(void)
{
	__u64 zslba = 0;
	__u32 i = 0;

	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * NR_MAX_ZONE, GFP_ATOMIC);
	report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (NR_MAX_ZONE - 1), GFP_ATOMIC);
	
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * NR_MAX_ZONE);

	for (i = 0; i < NR_MAX_ZONE; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zone_descs[i].wp = zslba;
		zslba += LBAS_PER_ZONE;
		zone_descs[i].zone_capacity = LBAS_PER_ZONE;

		NVMEV_ZNS_DEBUG("[i] zslba 0x%llx zone capacity 0x%llx\n", zone_descs[i].zslba, zone_descs[i].zone_capacity);
	}

	res_infos[ACTIVE_ZONE].total_cnt = NR_MAX_ACTIVE_ZONE;
	res_infos[ACTIVE_ZONE].acquired_cnt = 0;

	res_infos[OPEN_ZONE].total_cnt = NR_MAX_OPEN_ZONE;
	res_infos[OPEN_ZONE].acquired_cnt = 0;

	res_infos[ZRWA_ZONE].total_cnt = NR_MAX_OPEN_ZONE;
	res_infos[ZRWA_ZONE].acquired_cnt = 0;
}

void zns_exit(void)
{
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zone_descs);
	kfree(report_buffer);
}
#endif