#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "channel.h"

extern struct nvmev_dev *vdev;

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

void chmodel_init(struct channel_model * ch, __u64 bandwidth/*MB/s*/)  
{
	ch->head = 0;
	ch->valid_len = 0;
	ch->cur_time = 0;
	ch->max_credits = BANDWIDTH_TO_MAX_CREDITS(bandwidth);
	ch->command_credits = 0;
	ch->xfer_lat = BANDWIDTH_TO_TX_TIME(bandwidth);

	MEMSET(&(ch->avail_credits[0]), ch->max_credits, NR_CREDIT_ENTRIES);

	NVMEV_DEBUG("[%s] max_credits %d tx_time %lld\n",__FUNCTION__, ch->max_credits, ch->xfer_lat);
}

__u64 chmodel_request( struct channel_model * ch, __u64 request_time, __u64 length)
{
	__u64 cur_time = __get_wallclock();
	__u32 pos, next_pos;
	__u32 remaining_credits, consumed_credits;
	__u32 default_delay, delay = 0; 
	__u32 valid_length;
	__u64 total_latency;
	__u32 units_to_xfer = DIV_ROUND_UP(length, UNIT_XFER_SIZE);
	__u32 cur_time_offs, request_time_offs;	
	
	// Search current time index and move head to it
	cur_time_offs  = (cur_time/UNIT_TIME_INTERVAL) - (ch->cur_time/UNIT_TIME_INTERVAL);
	cur_time_offs  = (cur_time_offs < ch->valid_len) ? cur_time_offs : ch->valid_len;

	if (ch->head + cur_time_offs >= NR_CREDIT_ENTRIES) {
		MEMSET(&(ch->avail_credits[ch->head]), ch->max_credits, NR_CREDIT_ENTRIES - ch->head);
		MEMSET(&(ch->avail_credits[0]), ch->max_credits, cur_time_offs - (NR_CREDIT_ENTRIES - ch->head));
	}
	else {
		MEMSET(&(ch->avail_credits[ch->head]), ch->max_credits, cur_time_offs);
	}

	ch->head = (ch->head + cur_time_offs) %  NR_CREDIT_ENTRIES;
	ch->cur_time = cur_time;
	ch->valid_len = ch->valid_len - cur_time_offs; 

	if (ch->valid_len > NR_CREDIT_ENTRIES) {
		NVMEV_ERROR("[%s] Invalid valid_len 0x%x\n",__FUNCTION__, ch->valid_len);
		NVMEV_ASSERT(0);
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
	remaining_credits = units_to_xfer * UNIT_XFER_CREDITS;
	remaining_credits += ch->command_credits;

	default_delay = remaining_credits / ch->max_credits; 
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
	
	valid_length = (pos >= ch->head) ? 
								   (pos - ch->head + 1) : (NR_CREDIT_ENTRIES - (ch->head - pos - 1));

	if (valid_length > ch->valid_len)
		ch->valid_len = valid_length;
	
	// check if array is small..
	delay = (delay > default_delay) ? (delay - default_delay) : 0;
	
	total_latency = (ch->xfer_lat * units_to_xfer) + (delay * UNIT_TIME_INTERVAL);

	return request_time + total_latency;  
}
