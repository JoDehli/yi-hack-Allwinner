## Content

This procedure allows you to unbrick your cam using a backup file (which you did previously).

Please note: bootloader saves the hash of the file and if it matches the hash of the file you are trying to load, this procedure will not work.

In this case, you have to use a different file: for example you can use "factory" if you have previously used "hacked" and vice versa (see below).

In the "hacked" version I added a random file to force the hash to change.


## How to use

1. Clone this repo on a linux environment.
2. Copy your partition (mtdblockX.bin) in the folder corresponding to your model. mtdblock2.bin for rootfs partition and mtdblock3.bin for home partition.
3. Enter to the unbrick folder
   `cd unbrick`
4. Run the build command with the desired option.

   If you want to create an original unbrick partition:
   
   `./build.sh factory`
   
   If you want to create a hacked partition:
   
   `./build.sh hacked`

   The last option allows you to run the hack after the unbrick (but you need to install the hack separately). The backup partition is already modified to run the hack.
5. You will find the file home_XXX.gz (and/or rootfs_XXX.gz) in the folder corresponding to your model.
6. Check if there were any errors in the procedure: the size of the resulting file must be similar to the corresponding mtdblockX.bin.
7. Unzip it in the root folder of your sd card.
8. Switch on the cam and wait for the cam to come online.

To run this script correctly you have to comply some dependencies depending on your OS.
For example if you are using a Debian distro, install:
- mtd-utils
- u-boot-tools

## DISCLAIMER
**NOBODY BUT YOU IS RESPONSIBLE FOR ANY USE OR DAMAGE THIS SOFTWARE MAY CAUSE. THIS IS INTENDED FOR EDUCATIONAL PURPOSES ONLY. USE AT YOUR OWN RISK.**

# Donation
If you like this project, you can buy Roleo a beer :)
[![paypal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=JBYXDMR24FW7U&currency_code=EUR&source=url)
