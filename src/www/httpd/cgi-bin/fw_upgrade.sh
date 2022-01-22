#!/bin/sh

export PATH=$PATH:/home/base/tools:/home/yi-hack/bin:/home/yi-hack/sbin:/tmp/sd/yi-hack/bin:/tmp/sd/yi-hack/sbin
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/lib:/home/yi-hack/lib:/tmp/sd/yi-hack/lib

YI_HACK_PREFIX="/home/yi-hack"

. $YI_HACK_PREFIX/www/cgi-bin/validate.sh

if ! $(validateQueryString $QUERY_STRING); then
    printf "Content-type: application/json\r\n\r\n"
    printf "{\n"
    printf "\"%s\":\"%s\"\\n" "error" "true"
    printf "}"
    exit
fi

NAME="$(echo $QUERY_STRING | cut -d'=' -f1)"
VAL="$(echo $QUERY_STRING | cut -d'=' -f2)"

if [ "$NAME" != "get" ] ; then
    exit
fi

if [ "$VAL" == "info" ] ; then
    printf "Content-type: application/json\r\n\r\n"

    FW_VERSION=`cat /home/yi-hack/version`
    LATEST_FW=`/home/yi-hack/usr/bin/wget -O -  https://api.github.com/repos/roleoroleo/yi-hack-Allwinner/releases/latest 2>&1 | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/'`

    printf "{\n"
    printf "\"%s\":\"%s\",\n" "error" "false"
    printf "\"%s\":\"%s\",\n" "fw_version"      "$FW_VERSION"
    printf "\"%s\":\"%s\"\n" "latest_fw"       "$LATEST_FW"
    printf "}"

elif [ "$VAL" == "upgrade" ] ; then

    FREE_SD=$(df /tmp/sd/ | grep mmc | awk '{print $4}')
    if [ -z "$FREE_SD" ]; then
        printf "Content-type: text/html\r\n\r\n"
        printf "No SD detected."
        exit
    fi

    if [ $FREE_SD -lt 100000 ]; then
        printf "Content-type: text/html\r\n\r\n"
        printf "No space left on SD."
        exit
    fi

    # Clean old upgrades
    rm -rf /tmp/sd/fw_upgrade
    rm -rf /tmp/sd/Factory
    rm -rf /tmp/sd/newhome

    mkdir -p /tmp/sd/fw_upgrade
    cd /tmp/sd/fw_upgrade

    MODEL_SUFFIX=`cat $YI_HACK_PREFIX/model_suffix`
    FW_VERSION=`cat /home/yi-hack/version`
    LATEST_FW=`/home/yi-hack/usr/bin/wget -O -  https://api.github.com/repos/roleoroleo/yi-hack-Allwinner/releases/latest 2>&1 | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/'`
    if [ "$FW_VERSION" == "$LATEST_FW" ]; then
        printf "Content-type: text/html\r\n\r\n"
        printf "No new firmware available."
        exit
    fi

    /home/yi-hack/usr/bin/wget https://github.com/roleoroleo/yi-hack-Allwinner/releases/download/$LATEST_FW/${MODEL_SUFFIX}_${LATEST_FW}.tgz
    if [ ! -f ${MODEL_SUFFIX}_${LATEST_FW}.tgz ]; then
        printf "Content-type: text/html\r\n\r\n"
        printf "Unable to download firmware file."
        exit
    fi

    tar zxvf ${MODEL_SUFFIX}_${LATEST_FW}.tgz
    rm ${MODEL_SUFFIX}_${LATEST_FW}.tgz
    cp -rf * ..
    rm -rf /tmp/sd/fw_upgrade/*
    cp -f $YI_HACK_PREFIX/etc/*.conf .
    if [ -f $YI_HACK_PREFIX/etc/hostname ]; then
        cp -f $YI_HACK_PREFIX/etc/hostname .
    fi

    # Report the status to the caller
    printf "Content-type: text/html\r\n\r\n"
    printf "Download completed, rebooting and upgrading."

    sync
    sync
    sync
    sleep 1
    reboot
fi
