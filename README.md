# OS Jackfruit — Multi-Container Runtime

## Team Information
- Name: Prithvi Aithal SRN: PES1UG24CS909
- Name: S.Hemanth Kumar  SRN: PES1UG24CS418

## Build Instructions
cd boilerplate
make

## Load Kernel Module
sudo insmod monitor.ko
ls /dev/container_monitor

## Run
sudo ./engine supervisor ./rootfs
sudo ./engine start alpha ./rootfs /bin/ls
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo rmmod monitor
