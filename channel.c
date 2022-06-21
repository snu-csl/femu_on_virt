#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "channel.h"

#define ASSERT(X)

struct channel channels[NR_TOTAL_CHANNELS];

extern struct nvmev_dev *vdev;

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

void CHANNEL_INIT(void)
{
	size_t i = 0;

    for (i = 0; i < NR_PCIE_CHANNELS; i++) {
        channels[PCIE_CH(i)].head = 0;
        channels[PCIE_CH(i)].valid_len = 0;
        channels[PCIE_CH(i)].cur_time = 0;
        channels[PCIE_CH(i)].initial_credit = INITIAL_CREDIT_PCIE_GEN3X4;
        channels[PCIE_CH(i)].credits_per_4KB_transfer = CREDITS_PER_4KB_TRANSFER;
        channels[PCIE_CH(i)].credits_per_command = 1;

        MEMSET(&channels[PCIE_CH(i)].avail_credits[0], channels[PCIE_CH(i)].initial_credit, NR_CREDIT_ENTRIES);
    }
    
	for (i = 0; i < NR_NAND_CHANNELS; i++) {
		channels[NAND_CH(i)].head = 0;
		channels[NAND_CH(i)].valid_len = 0;
		channels[NAND_CH(i)].cur_time = 0;
		channels[NAND_CH(i)].initial_credit = INITIAL_CREDIT_NAND_CHANNEL;
		channels[NAND_CH(i)].credits_per_4KB_transfer = CREDITS_PER_4KB_TRANSFER;
		channels[NAND_CH(i)].credits_per_command = 0;

		MEMSET(&channels[NAND_CH(i)].avail_credits[0], channels[NAND_CH(i)].initial_credit, NR_CREDIT_ENTRIES);
	}
}

__u64 request_ch(__u32 id, __u64 request_time, __u64 length)
{
	unsigned long long cur_time = __get_wallclock();
	unsigned int request_time_offs;
    
    struct channel * ch = &channels[id];

	unsigned int pos;
	unsigned int remaining_credits;
	unsigned int default_delay; 
	unsigned int delay = 0;
	unsigned int consumed_credits;
	unsigned int next_pos;
	unsigned long long valid_len;

	// Search current time index and move head to it
	unsigned int cur_time_offs = (cur_time/UNIT_TIME_INTERVAL) - (ch->cur_time/UNIT_TIME_INTERVAL);
	cur_time_offs  = (cur_time_offs < ch->valid_len) ? cur_time_offs : ch->valid_len;

	if (ch->head + cur_time_offs >= NR_CREDIT_ENTRIES) {
		MEMSET(&(ch->avail_credits[ch->head]), ch->initial_credit, NR_CREDIT_ENTRIES - ch->head);
		MEMSET(&(ch->avail_credits[0]), ch->initial_credit, cur_time_offs - (NR_CREDIT_ENTRIES - ch->head));
	}
	else {
		MEMSET(&(ch->avail_credits[ch->head]), ch->initial_credit, cur_time_offs);
	}

	ch->head = (ch->head + cur_time_offs) %  NR_CREDIT_ENTRIES;
	ch->cur_time = cur_time;
	ch->valid_len = ch->valid_len - cur_time_offs; 

	if (ch->valid_len > NR_CREDIT_ENTRIES) {
		NVMEV_ERROR("[%s] Invalid valid_len 0x%x\n",__FUNCTION__, ch->valid_len);
		ASSERT(0);
	}

	if (request_time < cur_time) {
		NVMEV_DEBUG("[%s] Reqeust time is before the current time 0x%llx 0x%llx\n", 
									__FUNCTION__, request_time, cur_time);
		return 0; // return minimum delay
	}

	//Search request time index
	request_time_offs = (request_time/UNIT_TIME_INTERVAL) - (cur_time/UNIT_TIME_INTERVAL);

	if (request_time_offs >= NR_CREDIT_ENTRIES) {
		NVMEV_ERROR("[%s] Need to increase allay size 0x%llx 0x%llx 0x%x\n", __FUNCTION__, request_time, cur_time, request_time_offs);
		return 0; // return minimum delay
	}

	pos = (ch->head + request_time_offs) %  NR_CREDIT_ENTRIES;
	remaining_credits = DIV_ROUND_UP(length, 4096) * ch->credits_per_4KB_transfer;
	remaining_credits += ch->credits_per_command;

	default_delay = remaining_credits / ch->initial_credit; 
	delay = 0;
	
	while (1) {
		consumed_credits = (remaining_credits <= ch->avail_credits[pos]) ? 
												remaining_credits : ch->avail_credits[pos];  
		ch->avail_credits[pos] -= consumed_credits;
		remaining_credits -= consumed_credits;

		if (remaining_credits) {
			next_pos = (pos + 1) % NR_CREDIT_ENTRIES;	
			// If array is full
			if (next_pos != ch->head) {
				delay++;
				pos = next_pos;
			}
			else {
				NVMEV_ERROR("[%s] No free entry 0x%llx 0x%llx 0x%x\n", __FUNCTION__, request_time, cur_time, request_time_offs);
				break;
			}
		}
		else
			break;
	}
	
	valid_len = (pos >= ch->head) ? 
								   (pos - ch->head + 1) : (NR_CREDIT_ENTRIES - (ch->head - pos - 1));

	if (valid_len > ch->valid_len)
		ch->valid_len = valid_len;
	
	// check if array is small..
	delay = (delay > default_delay) ? (delay - default_delay) : 0;

	return delay *  UNIT_TIME_INTERVAL;  
}