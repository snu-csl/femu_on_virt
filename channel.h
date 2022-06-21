#ifndef _CHANNEL_H
#define _CHANNEL_H

/* Macros for channel model */
#define NR_CREDIT_ENTRIES (1024*8)
#define UNIT_TIME_INTERVAL (25000) //ns
#define CREDITS_PER_4KB_TRANSFER	(10)

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

struct channel {
	unsigned long long cur_time;
	unsigned int head;
	unsigned int valid_len;
	unsigned int initial_credit;
	unsigned int credits_per_4KB_transfer;
	unsigned int credits_per_command;
	credit_t avail_credits[NR_CREDIT_ENTRIES];
};

/* Macros for specific setting. Modify these macros for your target */
#define NR_PCIE_CHANNELS	(1)
#define NR_NAND_CHANNELS	(8)
#define NR_TOTAL_CHANNELS	(NR_NAND_CHANNELS + NR_PCIE_CHANNELS)

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3200ull) //MB/s

#define NAND_CHANNEL_OCCUPANCY_TIME_4KB	 ((4096ull*1000*1000*1000)/(NAND_CHANNEL_BANDWIDTH*1024*1024))
#define PCIE_OCCUPANCY_TIME_4KB 		 ((4096ull*1000*1000*1000)/(PCIE_BANDWIDTH*1024*1024))

#define INITIAL_CREDIT_NAND_CHANNEL	 (50) // it will be calculated by UNIT_TIME_INTERVAL, NAND_CHANNEL_BANDWIDTH
#define INITIAL_CREDIT_PCIE_GEN3X4	 (200) // it will be calculated by UNIT_TIME_INTERVAL, PCIE_BANDWIDTH

enum {
	PCIE_CHANNEL = 0,
	PCIE_CH_0 = PCIE_CHANNEL,
	NAND_CHANNEL = PCIE_CH_0 + NR_PCIE_CHANNELS,
	NAND_CH_0 = NAND_CHANNEL,
};

#define PCIE_CH(c) (PCIE_CH_0 + (c))
#define NAND_CH(c) (NAND_CH_0 + (c)) 

__u64 request_ch(__u32 id, __u64 request_time, __u64 length);
void CHANNEL_INIT(void);
#endif
