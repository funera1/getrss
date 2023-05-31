cd ../module
make
sudo rmmod rss_range.ko
sudo insmod rss_range.ko

sudo mknod /dev/rss_range c 64 1 
sudo chmod 666 /dev/rss_range
