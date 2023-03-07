#ifndef _NVMEVIRT_SSD_CONFIG_H
#define _NVMEVIRT_SSD_CONFIG_H

/* SSD Configuration*/
#define SAMSUNG_970PRO 0
#define SAMSUNG_ZNS_970PRO 1
#define SKHYNIX_ZNS_PROTOTYPE 2

#define BASE_SSD   (SAMSUNG_ZNS_970PRO)

/* Macros for specific setting. Modify these macros for your target */
#if (BASE_SSD == SKHYNIX_ZNS_PROTOTYPE)
#define SSD_INSTANCES        1
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     16
#define SSD_INSTANCE_BITS    1
#define READ_PAGE_SIZE      (64*1024)
#define PGM_PAGE_SIZE        (READ_PAGE_SIZE * 3)
#define PLNS_PER_LUN         1 /* not used*/

#define CH_MAX_XFER_SIZE  (64*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (192*1024)

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3200ull) //MB/s

#define NAND_4KB_READ_LATENCY 25485
#define NAND_READ_LATENCY 40950
#define NAND_PROG_LATENCY 1913640
#define NAND_ERASE_LATENCY 0

#define FW_READ0_LATENCY (37540 - 7390)
#define FW_READ1_LATENCY  (0)
#define FW_READ0_SIZE (0)
#define FW_PROG0_LATENCY  (0)
#define FW_PROG1_LATENCY (0)
#define FW_XFER_LATENCY 413
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

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * PGM_PAGE_SIZE * 2)

#elif  (BASE_SSD == SAMSUNG_970PRO)
#define SSD_INSTANCES        4 
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     2
#define PLNS_PER_LUN         1
#define SSD_INSTANCE_BITS    2
#define READ_PAGE_SIZE      (32*1024)
#define PGM_PAGE_SIZE        (READ_PAGE_SIZE * 1)
#define BLKS_PER_PLN         10240
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
#define FW_XFER_LATENCY (0)
#define OP_AREA_PERCENT      (0.07)

//Not used
#define ZONE_SIZE       (0) //byte
#define DIES_PER_ZONE   (0)

#define MAX_ZRWA_ZONES (0) /* 0 : Not support ZRWA */
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * PGM_PAGE_SIZE * 2)

#elif  (BASE_SSD == SAMSUNG_ZNS_970PRO)
#define SSD_INSTANCES        1
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     2
#define PLNS_PER_LUN         1
#define SSD_INSTANCE_BITS    2
#define READ_PAGE_SIZE      (32*1024)
#define PGM_PAGE_SIZE        (READ_PAGE_SIZE * 1)

#define CH_MAX_XFER_SIZE  (16*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (512) 

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			(3360ull) //MB/s

#define NAND_4KB_READ_LATENCY (35760 - 10000)
#define NAND_READ_LATENCY (36013 - 10000)
#define NAND_PROG_LATENCY (185000 + 5000)
#define NAND_ERASE_LATENCY 0

#define FW_READ0_LATENCY (25510 - 17010)
#define FW_READ1_LATENCY (30326 - 19586)
#define FW_READ0_SIZE (16*1024)
#define FW_PROG0_LATENCY  (4000+2000)
#define FW_PROG1_LATENCY (460)
#define FW_XFER_LATENCY (0)
#define OP_AREA_PERCENT      (0.07)

#define ZONE_SIZE       (16*1024*1024) //byte
#define DIES_PER_ZONE   (NAND_CHANNELS*LUNS_PER_NAND_CH)

#define MAX_ZRWA_ZONES (0xFFFFFFFF) // No limit
#define ZRWAFG_SIZE (PGM_PAGE_SIZE)
#define ZRWA_SIZE   (MB(4))
#define ZRWA_BUFFER_SIZE   (ZRWA_SIZE * 2)
/*One of the two must be set to zero(BLKS_PER_PLN, BLK_SIZE)*/
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * PGM_PAGE_SIZE * 3)
#endif // BASE_SSD == SAMSUNG_970_PRO

#define NAND_CH_PER_SSD_INS  (NAND_CHANNELS/SSD_INSTANCES)
#define LPN_TO_SSD_ID(lpn) ((lpn) % SSD_INSTANCES)     
#define LPN_TO_LOCAL_LPN(lpn)  ((lpn) >> SSD_INSTANCE_BITS)
#endif
