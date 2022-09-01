#ifndef _CHANNEL_H
#define _CHANNEL_H

/* Macros for channel model */
#define NR_CREDIT_ENTRIES 		(1024*8)
#define UNIT_TIME_INTERVAL 		(100000ULL) //ns
#define UNIT_XFER_SIZE 			(4096ULL)	//bytes
#define UNIT_XFER_CREDITS		(1)    //credits needed to transfer data(UNIT_XFER_SIZE) 

#define SIZE_OF_CREDIT_T 1

#if (SIZE_OF_CREDIT_T == 1)
typedef __u8 credit_t;
#define MEMSET(dest, value, length) memset(dest, value, length)

#elif (SIZE_OF_CREDIT_T == 2)
typedef __u16 credit_t;
#define MEMSET(dest, value, length) memset16(dest, value, length)

#elif (SIZE_OF_CREDIT_T == 4)
typedef __u32 credit_t;
#define MEMSET(dest, value, length) memset32(dest, value, length)
#else
#error "Invalid credit size"
#endif

struct channel_model {
	__u64 cur_time;
	__u32 head;
	__u32 valid_len;
	__u32 max_credits;
	__u32 command_credits;
	__u32 xfer_lat; /*XKB NAND CH transfer time in nanoseconds*/
    
	credit_t avail_credits[NR_CREDIT_ENTRIES];
};

#define BANDWIDTH_TO_TX_TIME(MB_S) (((UNIT_XFER_SIZE)* NS_PER_SEC(1)) / (MB(MB_S)))
#define BANDWIDTH_TO_MAX_CREDITS(MB_S) (MB(MB_S) * UNIT_TIME_INTERVAL / NS_PER_SEC(1) / UNIT_XFER_SIZE * UNIT_XFER_CREDITS)

__u64 chmodel_request( struct channel_model * ch, __u64 request_time, __u64 length);
void chmodel_init(struct channel_model * ch, __u64 bandwidth/*MB/s*/);
#endif
