#ifndef _ZNS_H
#define _ZNS_H

#include <linux/types.h>
#include "nvmev.h"
#include "nvme_zns.h"

extern struct zns_ssd g_zns_ssd;

#define NVMEV_ZNS_DEBUG(string, args...) //printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)

// Zoned Namespace Command Set Specification Revision 1.1a
#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

struct zns_ssd {
    struct ssd ssd;

    __u32 nr_zones;
    __u32 nr_active_zones;
    __u32 nr_open_zones;
    __u32 dies_per_zone;
    __u32 zone_size; //bytes

    /*related to zrwa*/
    __u32 nr_zrwa_zones;
    __u32 zrwafg_size;
    __u32 zrwa_size;
    __u32 lbas_per_zrwafg;
    __u32 lbas_per_zrwa;
    
    __u32 ns;
    void * storage_base_addr;
    
    struct zone_resource_info res_infos[RES_TYPE_COUNT];
    struct zone_descriptor *zone_descs;
    struct zone_report *report_buffer;
    struct buffer * zwra_buffer;

    unsigned int cpu_nr_dispatcher;
};

/* zns internal functions */
static inline void * get_storage_addr_from_zid(struct zns_ssd *zns_ssd, __u64 zid) {
    return (void *) ((char*)zns_ssd->storage_base_addr + zid*zns_ssd->zone_size);
}

static inline bool is_zone_resource_avail(struct zns_ssd *zns_ssd, __u32 type)
{
	return zns_ssd->res_infos[type].acquired_cnt < zns_ssd->res_infos[type].total_cnt;
}

static inline bool is_zone_resource_full(struct zns_ssd *zns_ssd, __u32 type)
{
	return zns_ssd->res_infos[type].acquired_cnt == zns_ssd->res_infos[type].total_cnt;
}

static inline bool acquire_zone_resource(struct zns_ssd *zns_ssd, __u32 type)
{
	if(is_zone_resource_avail(zns_ssd, type)) {
		zns_ssd->res_infos[type].acquired_cnt++;
		return true;
	}

	return false;
}

static inline void release_zone_resource(struct zns_ssd *zns_ssd, __u32 type)
{	
	ASSERT(zns_ssd->res_infos[type].acquired_cnt > 0);

	zns_ssd->res_infos[type].acquired_cnt--;
}

static inline void change_zone_state(struct zns_ssd * zns_ssd, __u32 zid, enum zone_state state)
{
	NVMEV_ZNS_DEBUG("change state zid %d from %d to %d \n",zid, zns_ssd->zone_descs[zid].state, state);

	// check if transition is correct
	zns_ssd->zone_descs[zid].state = state;
}

static inline __u32 lpn_to_zone(struct zns_ssd *zns_ssd, __u64 lpn) {
    return (lpn) / (zns_ssd->zone_size / zns_ssd->ssd.sp.chunksz);
}

static inline __u64 zone_to_slpn(struct zns_ssd *zns_ssd, __u32 zid) {
    return (zid) * (zns_ssd->zone_size / zns_ssd->ssd.sp.chunksz);
}

static inline __u32 lba_to_zone(struct zns_ssd *zns_ssd, __u64 lba) {
    return (lba) / (BYTE_TO_LBA(zns_ssd->zone_size));
}

static inline __u64 zone_to_slba(struct zns_ssd *zns_ssd, __u32 zid) {
    return (zid) * (BYTE_TO_LBA(zns_ssd->zone_size));
}

static inline __u32 die_to_channel(struct zns_ssd *zns_ssd, __u32 die) {
    return (die) % zns_ssd->ssd.sp.nchs;
}

static inline __u32 die_to_lun(struct zns_ssd *zns_ssd, __u32 die) {
    return (die) / zns_ssd->ssd.sp.nchs;
}

static inline struct zns_ssd * get_zns_ssd_instance(void) {
    return &(g_zns_ssd);
}

/* zns external interface */
void zns_zmgmt_recv(struct nvme_request * req, struct nvme_result * ret);
void zns_zmgmt_send(struct nvme_request * req, struct nvme_result * ret);
bool zns_write(struct nvme_request * req, struct nvme_result * ret);
bool zns_read(struct nvme_request * req, struct nvme_result * ret);
bool zns_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret);

void zns_init(unsigned int cpu_nr_dispatcher, void * storage_base_addr, unsigned long capacity, unsigned int namespace);
void zns_exit(void);
#endif
