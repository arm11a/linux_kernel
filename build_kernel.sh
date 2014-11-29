#!/bin/sh -e

export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-

#cd linux_kernel
##git checkout master
#git pull #iamrootArm11A master
cd linux-3.12.20
#make exynos_defconfig
#kmake menuconfig

#make -j4

export KBUILD_SRC=${pwd}

make tags
make cscope

