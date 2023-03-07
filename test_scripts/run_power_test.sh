#!/bin/bash

#!/bin/bash
ZNS_DEV=nvme2n2
CONV_DEV=nvme2n1
MOUNT_PATH=/mnt/virt

ZNS_DEV_PATH=/dev/$ZNS_DEV
CONV_DEV_PATH=/dev/$CONV_DEV

delay=2
READ_ONLY_DISABLE()
{
   echo 1 1 1 > /proc/nvmev/write_times
   sleep $delay  
}

READ_ONLY_ENABLE()
{
   echo 0 0 1 > /proc/nvmev/write_times
   sleep $delay   
}

RECOVERY_DEVICE()
{
   echo 0 0 1 > /proc/nvmev/read_times 
   sleep $delay   
}

PREPARE()
{
    mount_point="/mnt/virt"
    aioengine="libaio"
    bs="1M"
    qd=32
    cmd="write"
    time=30  
    direct=1
    fio_size="2G"
    pattern=0xdeadface

    fio --directory=$mount_point --direct=$direct --iodepth=$qd --rw=$cmd \
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --buffer_pattern=$pattern\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --bs=$bs \
 
}

THREAD()
{
    filebench -f varmail_t16.f
}

THREAD2()
{
    mount_point="/mnt/virt"
    aioengine="sync"
    bs="4k"
    qd=32
    cmd="write"
    time=30  
    direct=1
    fio_size="2G"
    pattern=0xdeadface

    fio --directory=$mount_point --direct=$direct --iodepth=$qd --rw=$cmd \
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --buffer_pattern=$pattern\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --fsync=1 --bs=$bs --time_based=1 --runtime=$time\
 
}

VERIFY()
{
    mount_point="/mnt/virt"
    aioengine="sync"
    bs="1M"
    qd=32
    cmd="read"
    direct=1
    fio_size="2G"
    pattern=0xdeadface

    fio --directory=$mount_point --direct=$direct --iodepth=$qd --rw=$cmd \
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --verify_pattern=$pattern --verify=pattern\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --fsync=1 --bs=$bs \
 
}

READ_ONLY_DISABLE

PREPARE

echo "CALL 1st thread - Background Operating"
THREAD first&
THREAD_1st_PID=$!

echo "THREAD PID : $THREAD_1st_PID"

THREAD2 second&
THREAD_2st_PID=$!

echo "THREAD PID : $THREAD_2st_PID"

sleep 20

READ_ONLY_ENABLE

kill -9 $THREAD_1st_PID $THREAD_2st_PID
killall filebench
killall fio

umount $MOUNT_PATH

RECOVERY_DEVICE

READ_ONLY_DISABLE

sudo mount -t f2fs -o fsync_mode=nobarrier $CONV_DEV_PATH $MOUNT_PATH 

VERIFY