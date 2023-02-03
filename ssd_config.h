#ifndef _NVMEVIRT_SSD_CONFIG_H
#define _NVMEVIRT_SSD_CONFIG_H

/* SSD Configuration*/
#define SAMSUNG_970PRO 0
#define SAMSUNG_ZNS_970PRO 1
#define SKHYNIX_ZNS_PROTOTYPE 2
#define SKHYNIX_ZNS_PROTOTYPE2 3

#define BASE_SSD   (SAMSUNG_970PRO)

/* Macros for specific setting. Modify these macros for your target */
#if (BASE_SSD == SKHYNIX_ZNS_PROTOTYPE)
#define SSD_PARTITIONS        1
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     16
#define SSD_PARTITION_BITS    0
#define FLASH_PAGE_SIZE      (64*1024)
#define WORDLINE_SIZE        (FLASH_PAGE_SIZE * 3)
#define PLNS_PER_LUN         1 /* not used*/

#define CH_MAX_XFER_SIZE  (FLASH_PAGE_SIZE) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (WORDLINE_SIZE)

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3200ull) //MB/s

#define NAND_4KB_READ_LATENCY 25485
#define NAND_READ_LATENCY 40950
#define NAND_PROG_LATENCY 1913640
#define NAND_ERASE_LATENCY 0

#define FW_READ0_LATENCY (37540 - 7390 + 2000)
#define FW_READ1_LATENCY  (0)
#define FW_READ0_SIZE (0)
#define FW_PROG0_LATENCY  (0)
#define FW_PROG1_LATENCY (0)
#define FW_4KB_XFER_LATENCY 413
#define OP_AREA_PERCENT      (0)

#define ZONE_SIZE       (96*1024*1024) //byte
#define DIES_PER_ZONE   (1)//NAND_CHANNELS*LUNS_PER_NAND_CH)

#define MAX_ZRWA_ZONES (0) /* 0 : Not support ZRWA */
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)

/*One of the two must be set to zero(BLKS_PER_PLN, BLK_SIZE)*/
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)

#define WRITE_BUFFER_SIZE   (WORDLINE_SIZE)

/* Modify configuration  */
#define NR_NAMESPACE	1 // Still.. only support single namespace.

/* NVME_CSI_NVM : Conv
   NVME_CSI_ZNS : ZNS
   NS_CAPACITY : MB (0 -> Full capacity) */

#define NS_CSI_0 NVME_CSI_ZNS
#define NS_CAPACITY_0 (0) 
#define NS_CSI_1 NVME_CSI_NVM 
#define NS_CAPACITY_1 (0)

/* Macros for specific setting. Modify these macros for your target */
#elif (BASE_SSD == SKHYNIX_ZNS_PROTOTYPE2)
#define SSD_PARTITIONS        1
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     4
#define SSD_PARTITION_BITS    0
#define FLASH_PAGE_SIZE      (64*1024)
#define WORDLINE_SIZE        (FLASH_PAGE_SIZE)
#define PLNS_PER_LUN         1 /* not used*/

#define CH_MAX_XFER_SIZE  (64*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (512)

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3200ull) //MB/s

#define NAND_4KB_READ_LATENCY 25485
#define NAND_READ_LATENCY 40950
#define NAND_PROG_LATENCY ((1913640/3) - 78125)
#define NAND_ERASE_LATENCY 0

#define FW_READ0_LATENCY (37540 - 7390)
#define FW_READ1_LATENCY  (0)
#define FW_READ0_SIZE (0)
#define FW_PROG0_LATENCY  (0)
#define FW_PROG1_LATENCY (0)
#define FW_4KB_XFER_LATENCY 413
#define OP_AREA_PERCENT      (0.7)

#define ZONE_SIZE       (512*1024*1024) //byte
#define DIES_PER_ZONE   (NAND_CHANNELS*LUNS_PER_NAND_CH)

#define MAX_ZRWA_ZONES (0) /* 0 : Not support ZRWA */
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)

/*One of the two must be set to zero(BLKS_PER_PLN, BLK_SIZE)*/
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * WORDLINE_SIZE * 4)

/* Modify configuration  */
#define NR_NAMESPACE	1 // Still.. only support single namespace.

/* NVME_CSI_NVM : Conv
   NVME_CSI_ZNS : ZNS
   NS_CAPACITY : MB (0 -> Full capacity) */

#define NS_CSI_0 NVME_CSI_ZNS
#define NS_CAPACITY_0 (0) 
#define NS_CSI_1 NVME_CSI_NVM 
#define NS_CAPACITY_1 (0)

#elif  (BASE_SSD == SAMSUNG_970PRO)
#define SSD_PARTITIONS        4 
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     2
#define PLNS_PER_LUN         1
#define SSD_PARTITION_BITS    2
#define FLASH_PAGE_SIZE      (32*1024)
#define WORDLINE_SIZE        (FLASH_PAGE_SIZE * 1)
#define BLKS_PER_PLN         8192
#define BLK_SIZE             0 /*BLKS_PER_PLN should not be 0 */

#define CH_MAX_XFER_SIZE  (16*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (512) 

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3360ull) //MB/s

#define NAND_4KB_READ_LATENCY (35760)
#define NAND_READ_LATENCY (36013)
#define NAND_PROG_LATENCY (185000 + 5000)
#define NAND_ERASE_LATENCY 0

#define FW_READ0_LATENCY (25510 - 17010)
#define FW_READ1_LATENCY (30326 - 19586)
#define FW_READ0_SIZE (16*1024)
#define FW_PROG0_LATENCY  (4000)
#define FW_PROG1_LATENCY (460)
#define FW_4KB_XFER_LATENCY (0)
#define OP_AREA_PERCENT      (0.07)

//Not used
#define ZONE_SIZE       (0) //byte
#define DIES_PER_ZONE   (0)

#define MAX_ZRWA_ZONES (0) /* 0 : Not support ZRWA */
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * WORDLINE_SIZE * 2)

/* Modify configuration  */
#define NR_NAMESPACE	1 // Still.. only support single namespace.

/* NVME_CSI_NVM : Conv
   NVME_CSI_ZNS : ZNS
   NS_CAPACITY : MB (0 -> Full capacity) */

#define NS_CSI_0 NVME_CSI_NVM
#define NS_CAPACITY_0 (0) 
#define NS_CSI_1 NVME_CSI_NVM 
#define NS_CAPACITY_1 (0)

#elif  (BASE_SSD == SAMSUNG_ZNS_970PRO)
#define SSD_PARTITIONS        1
#define SSD_PARTITION_BITS    0
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     2
#define PLNS_PER_LUN         1
#define FLASH_PAGE_SIZE      (32*1024)
#define WORDLINE_SIZE        (FLASH_PAGE_SIZE * 1)

#define CH_MAX_XFER_SIZE  (16*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (512) 

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3360ull) //MB/s

#define NAND_4KB_READ_LATENCY (35760)
#define NAND_READ_LATENCY (36013)
#define NAND_PROG_LATENCY (185000 + 5000)
#define NAND_ERASE_LATENCY 0

#define FW_READ0_LATENCY (25510 - 17010)
#define FW_READ1_LATENCY (30326 - 19586)
#define FW_READ0_SIZE (16*1024)
#define FW_PROG0_LATENCY  (4000)
#define FW_PROG1_LATENCY (460)
#define FW_4KB_XFER_LATENCY (0)
#define OP_AREA_PERCENT      (0.07)

#define ZONE_SIZE       (32*1024*1024) //byte
#define DIES_PER_ZONE   (NAND_CHANNELS*LUNS_PER_NAND_CH)

#define MAX_ZRWA_ZONES (0xFFFFFFFF) // No limit
#define ZRWAFG_SIZE (WORDLINE_SIZE)
#define ZRWA_SIZE   (MB(1))
#define ZRWA_BUFFER_SIZE   (ZRWA_SIZE * 2)
/*One of the two must be set to zero(BLKS_PER_PLN, BLK_SIZE)*/
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * WORDLINE_SIZE * 4)

/* Modify configuration  */
#define NR_NAMESPACE	1 // Still.. only support single namespace.

/* NVME_CSI_NVM : Conv
   NVME_CSI_ZNS : ZNS
   NS_CAPACITY : MB (0 -> Full capacity) */

#define NS_CSI_0 NVME_CSI_ZNS
#define NS_CAPACITY_0 (0) 
#define NS_CSI_1 NVME_CSI_NVM 
#define NS_CAPACITY_1 (0)
#endif // BASE_SSD == SAMSUNG_970_PRO

#define NCHS_PER_PARTITON  (NAND_CHANNELS/SSD_PARTITIONS)
#define LPN_TO_SSD_ID(lpn) ((lpn) % SSD_PARTITIONS)     
#define LPN_TO_LOCAL_LPN(lpn)  ((lpn) >> SSD_PARTITION_BITS)
#endif
