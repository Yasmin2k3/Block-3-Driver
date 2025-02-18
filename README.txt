*IMPORTANT - when you make a change, make is what compiles your changes into code.

HOW TO LOAD DRIVER:

This will load the driver into the kernel:
sudo insmod <driver-name>.ko

This will remove/ unload driver into the kernel:
sudo rmmod <driver-name>.ko

To view its message to make sure tasks are done successfully:
sudo dmesg | tail
