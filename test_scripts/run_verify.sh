#!/bin/bash

THREAD()
{
    filebench -f varmail_t16.f
}

THREAD2()
{
    mount_point="/mnt/virt"
    aioengine="sync"
    bs="1M"
    qd=32
    cmd="write"
    time=30  
    direct=1
    fio_size="4G"
    pattern=0xdeadface

    fio --directory=$mount_point --direct=$direct --iodepth=$qd --rw=$cmd \
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --buffer_pattern=$pattern\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --fsync=0 --bs=$bs \
 
}

Verify()
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
        --ioengine=$aioengine --numjobs=1 --size=$fio_size --group_reporting --verify_pattern=$pattern --verify=pattern\
        --name=write --filename=a.mp3 --invalidate=1 --end_fsync=1 --fsync=1 --bs=$bs \
 
}

THREAD2
Verify