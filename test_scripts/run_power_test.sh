#!/bin/bash

#!/bin/bash
ZNS_DEV=nvme2n2
CONV_DEV=nvme2n1
MOUNT_PATH=/mnt/virt

ZNS_DEV_PATH=/dev/$ZNS_DEV
CONV_DEV_PATH=/dev/$CONV_DEV
MAX_TIME=30
MAX_RAND_DELAY=25
RAND_DELAY="$(($RANDOM% $MAX_RAND_DELAY+1))"
#RAND_DELAY=20
REMAINING_DELAY=$(($MAX_TIME - $RAND_DELAY + 2))

delay=3
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
    direct=1
    fio_size="4G"
    pattern=0xdeadface

    fio --directory=$mount_point --direct=$direct --iodepth=$qd --rw=$cmd \
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --buffer_pattern=$pattern\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --bs=$bs --create_on_open=1 \
 
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
    time=$MAX_TIME  
    direct=1
    fio_size="4G"
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
    fio_size="4G"
    pattern=0xdeadface

    fio --directory=$mount_point --direct=$direct --iodepth=$qd --rw=$cmd \
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --verify_pattern=$pattern --verify=pattern --do_verify=1\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --fsync=1 --bs=$bs --unlink=1\
 
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

sleep $RAND_DELAY

echo "Wake Up $RAND_DELAY $REMAINING_DELAY"

READ_ONLY_ENABLE

kill -9 $THREAD_1st_PID $THREAD_2st_PID
killall filebench
killall fio

umount $MOUNT_PATH

sleep $REMAINING_DELAY 

RECOVERY_DEVICE

READ_ONLY_DISABLE

echo "Mount.."
sudo mount -t f2fs -o fsync_mode=nobarrier $CONV_DEV_PATH $MOUNT_PATH 

VERIFY