#ifndef _NVMEVIRT_SSD_CONFIG_H
#define _NVMEVIRT_SSD_CONFIG_H

/* SSD Configuration*/
#define SAMSUNG_970PRO 0
#define SKHYNIX_ZNS_PROTOTYPE 1

#define BASE_SSD   (SAMSUNG_970PRO)

/* Macros for specific setting. Modify these macros for your target */
#if (BASE_SSD == SKHYNIX_ZNS_PROTOTYPE)
#define SSD_INSTANCES        1
#define NAND_CHANNELS        8
#define LUNS_PER_NAND_CH     16
#define SSD_INSTANCE_BITS    1
#define READ_PAGE_SIZE      (64*1024)
#define PGM_PAGE_SIZE        (READ_PAGE_SIZE * 3)
#define PLNS_PER_LUN         1 /* not used*/         
#define MAX_NAND_XFER_SIZE  (64*1024) /* to overlap with pcie transfer */

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

/*One of the two must be set to zero(BLKS_PER_PLN, BLK_SIZE)*/
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)
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
#define MAX_NAND_XFER_SIZE  (16*1024) /* to overlap with pcie transfer */

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

#if SUPPORT_ZNS
    #undef SSD_INSTANCES
    #undef BLKS_PER_PLN
    #undef BLK_SIZE
    #undef OP_AREA_PERCENT

    #define SSD_INSTANCES        1
    #define ZONE_SIZE       (128*1024*1024) //byte
    #define DIES_PER_ZONE   (NAND_CHANNELS*LUNS_PER_NAND_CH)

    /*One of the two must be set to zero(BLKS_PER_PLN, BLK_SIZE)*/
    #define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
    #define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)
    #define OP_AREA_PERCENT      (0)
#endif // SUPPORT_ZNS 
#endif // BASE_SSD == SAMSUNG_970_PRO

#define NAND_CH_PER_SSD_INS  (NAND_CHANNELS/SSD_INSTANCES)
#define LPN_TO_SSD_ID(lpn) ((lpn) % SSD_INSTANCES)     
#define LPN_TO_LOCAL_LPN(lpn)  ((lpn) >> SSD_INSTANCE_BITS)

#define LPN_TO_BYTE(lpn) ((lpn) << 12)
#define BYTE_TO_LPN(byte) ((byte) >> 12)

#define LBA_TO_LPN(lba) (BYTE_TO_LPN(LBA_TO_BYTE(lba)))

#define WRITE_BUFFER_SIZE 4096

#endif
