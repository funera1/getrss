cd ../module
make
sudo rmmod rss_range
sudo insmod rss_range.ko
cd ../use

sudo mknod /dev/rss_range c 64 1
sudo chmod 666 /dev/rss_range
