#!/bin/bash

sudo su -c "echo 1 > /proc/nvmev/read_latency"
sudo su -c "echo 1 > /proc/nvmev/write_latency"
sudo su -c "echo 5 > /proc/nvmev/slot"
