#ifndef _ZNS_H
#define _ZNS_H

#include <linux/types.h>
#include "nvmev.h"
#include "nvme_zns.h"

extern struct nvmev_dev *vdev;

#define NVMEV_ZNS_DEBUG(string, args...) //printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)

// Zoned Namespace Command Set Specification Revision 1.1a
#define NR_MAX_ZONE (vdev->config.nr_zones)
#define NR_MAX_ACTIVE_ZONE	(vdev->config.nr_active_zones) //0xFFFFFFFF : No limit
#define NR_MAX_OPEN_ZONE (vdev->config.nr_open_zones) //0xFFFFFFFF : No limit
#define BYTES_PER_ZONE (vdev->config.zone_size)
#define LBAS_PER_ZONE (BYTE_TO_LBA(BYTES_PER_ZONE))

#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

#define LBA_TO_ZONE(lba) ((lba) / LBAS_PER_ZONE)
#define ZONE_TO_SLBA(zid) ((zid) * LBAS_PER_ZONE)

#define NR_MAX_ZRWA_ZONE (vdev->config.nr_zrwa_zones) //0xFFFFFFFF : No limit
#define LBAS_PER_ZRWAFG	(BYTE_TO_LBA(KB(1))) // ZRWA Flush Granurity 
#define LBAS_PER_ZRWA (BYTE_TO_LBA(MB(1))) // ZRWA Size

/* zns extern global variables*/
extern struct zone_resource_info res_infos[RES_TYPE_COUNT];
extern struct zone_descriptor * zone_descs;
extern struct zone_report * report_buffer;
extern __u32 zns_nsid;

/* zns internal functions */
#define ZONE_CAPACITY(zid) (zone_descs[zid].zone_capacity)
#define IS_ZONE_SEQUENTIAL(zid) (zone_descs[zid].type == ZONE_TYPE_SEQ_WRITE_REQUIRED)
#define IS_ZONE_RESOURCE_FULL(type) (res_infos[type].acquired_cnt == res_infos[type].total_cnt)
#define IS_ZONE_RESOURCE_AVAIL(type) (res_infos[type].acquired_cnt < res_infos[type].total_cnt)

void * __get_zns_media_addr_from_lba(__u64 lba);
void * __get_zns_media_addr_from_zid(__u64 zid);
bool __acquire_zone_resource(__u32 type);
void __release_zone_resource(__u32 type);
void __change_zone_state(__u32 zid, enum zone_state state);

// data transfer
__u64 __prp_transfer_data(__u64 prp1, __u64 prp2, void * buffer, __u64 length, __u32 io);

/* zns external interface */
__u32 zns_proc_mgmt_recv(struct nvme_zone_mgmt_recv * cmd);
__u32 zns_proc_mgmt_send(struct nvme_zone_mgmt_send * cmd);
__u32 zns_proc_nvme_write(struct nvme_rw_command * cmd);
__u32 zns_proc_nvme_read(struct nvme_rw_command * cmd);
__u32 zns_proc_append(struct nvme_zone_append * cmd,  __u64 * wp);

void ZNS_INIT(void);
void zns_exit(void);
void zns_proc_io_cmd(struct nvmev_proc_table *pe);
#endif
