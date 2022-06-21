#include "zns.h"

#if SUPPORT_ZNS 

struct zone_report * report_buffer;

void __fill_zone_report(struct nvme_zone_mgmt_recv * cmd, struct zone_report * report)
{
	__u64 slba = cmd->slba;
	__u64 start_zid = LBA_TO_ZONE(slba);
	
	__u64 bytes_transfer = (cmd->nr_dw + 1) * sizeof(__u32);

	__u64 nr_zone_to_report; 

	if (cmd->zra_specific_features == 0) // all
		nr_zone_to_report = NR_MAX_ZONE - start_zid;
	else // partial. # of zone desc transferred
		nr_zone_to_report = (bytes_transfer / sizeof(struct zone_descriptor))  - 1;

	report->nr_zones = nr_zone_to_report;

	memcpy(report->zd, &(zone_descs[start_zid]), sizeof(struct zone_descriptor) * nr_zone_to_report);
}

bool __check_mgmt_rcv_option_supported(struct nvme_zone_mgmt_recv * cmd)
{
	if (cmd->zra != 0) {
		NVMEV_ERROR("Currently, Not support Extended Report Zones\n");
		return false;
	}

	if (cmd->zra_specific_field != 0) {
		NVMEV_ERROR("Currently, Only support listing all zone\n");
		return false;
	}

	return true;
}

__u32 zns_proc_mgmt_recv(struct nvme_zone_mgmt_recv * cmd)
{
	__u64 prp1 = (__u64)cmd->prp1;
	__u64 prp2 = (__u64)cmd->prp2;
	__u64 length = (cmd->nr_dw + 1) * sizeof(__u32);
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_dw 0x%lx  action %u partial %u action_specific 0x%x\n",
					__FUNCTION__, cmd->slba, length, cmd->zra, cmd->zra_specific_features, cmd->zra_specific_field);
	
	
	if (__check_mgmt_rcv_option_supported(cmd) == false)
		return NVME_SC_INVALID_FIELD;

	__fill_zone_report(cmd, report_buffer);

	__prp_transfer_data(prp1, prp2, report_buffer, length, 0);

	return NVME_SC_SUCCESS;
}
#endif