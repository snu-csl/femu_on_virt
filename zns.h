#ifndef _NVMEVIRT_ZNS_H
#define _NVMEVIRT_ZNS_H

#include <linux/types.h>
#include "nvmev.h"
#include "nvme_zns.h"

extern struct zns_ssd * g_zns_ssd;

#define NVMEV_ZNS_DEBUG(string, args...) //printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)

// Zoned Namespace Command Set Specification Revision 1.1a
#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))
struct znsparams {
    uint32_t nr_zones;
    uint32_t nr_active_zones;
    uint32_t nr_open_zones;
    uint32_t dies_per_zone;
    uint32_t zone_size; //bytes

    /*related to zrwa*/
    uint32_t nr_zrwa_zones;
    uint32_t zrwafg_size;
    uint32_t zrwa_size;
    uint32_t zrwa_buffer_size;
    uint32_t lbas_per_zrwafg;
    uint32_t lbas_per_zrwa;
};

struct zns_ssd {
     union {
        struct ssd ssd;

        struct {
            STRUCT_SSD_ENTRIES
        };
    };

    struct znsparams zp;

    uint32_t ns;
    void * storage_base_addr;
    
    struct zone_resource_info res_infos[RES_TYPE_COUNT];
    struct zone_descriptor *zone_descs;
    struct zone_report *report_buffer;
    struct buffer * zwra_buffer;
};

/* zns internal functions */
static inline void * get_storage_addr_from_zid(struct zns_ssd *zns_ssd, uint64_t zid) {
    return (void *) ((char*)zns_ssd->storage_base_addr + zid*zns_ssd->zp.zone_size);
}

static inline bool is_zone_resource_avail(struct zns_ssd *zns_ssd, uint32_t type)
{
	return zns_ssd->res_infos[type].acquired_cnt < zns_ssd->res_infos[type].total_cnt;
}

static inline bool is_zone_resource_full(struct zns_ssd *zns_ssd, uint32_t type)
{
	return zns_ssd->res_infos[type].acquired_cnt == zns_ssd->res_infos[type].total_cnt;
}

static inline bool acquire_zone_resource(struct zns_ssd *zns_ssd, uint32_t type)
{
	if(is_zone_resource_avail(zns_ssd, type)) {
		zns_ssd->res_infos[type].acquired_cnt++;
		return true;
	}

	return false;
}

static inline void release_zone_resource(struct zns_ssd *zns_ssd, uint32_t type)
{	
	ASSERT(zns_ssd->res_infos[type].acquired_cnt > 0);

	zns_ssd->res_infos[type].acquired_cnt--;
}

static inline void change_zone_state(struct zns_ssd * zns_ssd, uint32_t zid, enum zone_state state)
{
	NVMEV_ZNS_DEBUG("change state zid %d from %d to %d \n",zid, zns_ssd->zone_descs[zid].state, state);

	// check if transition is correct
	zns_ssd->zone_descs[zid].state = state;
}

static inline uint32_t lpn_to_zone(struct zns_ssd *zns_ssd, uint64_t lpn) {
    return (lpn) / (zns_ssd->zp.zone_size / zns_ssd->sp.pgsz);
}

static inline uint64_t zone_to_slpn(struct zns_ssd *zns_ssd, uint32_t zid) {
    return (zid) * (zns_ssd->zp.zone_size / zns_ssd->sp.pgsz);
}

static inline uint32_t lba_to_zone(struct zns_ssd *zns_ssd, uint64_t lba) {
    return (lba) / (BYTE_TO_LBA(zns_ssd->zp.zone_size));
}

static inline uint64_t zone_to_slba(struct zns_ssd *zns_ssd, uint32_t zid) {
    return (zid) * (BYTE_TO_LBA(zns_ssd->zp.zone_size));
}

static inline  uint64_t zone_to_elba(struct zns_ssd *zns_ssd, uint32_t zid) 
{
	return zone_to_slba(zns_ssd, zid + 1) - 1; 
}

static inline uint32_t die_to_channel(struct zns_ssd *zns_ssd, uint32_t die) {
    return (die) % zns_ssd->sp.nchs;
}

static inline uint32_t die_to_lun(struct zns_ssd *zns_ssd, uint32_t die) {
    return (die) / zns_ssd->sp.nchs;
}

static inline uint64_t lba_to_lpn(struct zns_ssd *zns_ssd, uint64_t lba) 
{
	return lba / zns_ssd->sp.secs_per_pg;
}

static inline struct zns_ssd * zns_ssd_instance(void) {
    return g_zns_ssd;
}

/* zns external interface */
void zns_zmgmt_recv(struct nvme_request * req, struct nvme_result * ret);
void zns_zmgmt_send(struct nvme_request * req, struct nvme_result * ret);
bool zns_write(struct nvme_request * req, struct nvme_result * ret);
bool zns_read(struct nvme_request * req, struct nvme_result * ret);
bool zns_proc_nvme_io_cmd(struct nvme_request * req, struct nvme_result * ret);

void zns_init(uint64_t capacity, uint32_t cpu_nr_dispatcher, void * storage_base_addr, uint32_t namespace);
void zns_exit(void);
#endif
