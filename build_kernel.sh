#!/bin/sh -e
NR_CPUS=$(grep "processor"</proc/cpuinfo | wc -l)
PWD=$(pwd)

export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-

cd $PWD/linux-3.12.20

#------ recovery original source and update. -----
#git checkout master
#git pull
#-------------------------------------------------

if [ ! -e $PWD/linux-3.12.20/.config ]; then
    make exynos_defconfig
fi

make -j$NR_CPUS

#
# uncommnet your tagging tool.
# ex) gtags, tags, and cscope.

if [ -e /usr/local/bin/gtags ]; then
    make gtags
fi

if [ -e /usr/bin/ctags ]; then
    make tags
fi 

if [ -e /usr/bin/cscope ]; then
    make cscope
fi

