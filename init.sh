insmod mem_swap.ko npages=250000 # * 4K swap space
mkswap -f /dev/mem_swap
swapon /dev/mem_swap
