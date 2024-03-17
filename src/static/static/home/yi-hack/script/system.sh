#!/bin/sh

CONF_FILE="etc/system.conf"

YI_PREFIX="/home/app"
YI_HACK_PREFIX="/home/yi-hack"

YI_HACK_VER=$(cat /home/yi-hack/version)
MODEL_SUFFIX=$(cat /home/yi-hack/model_suffix)

HOMEVER=$(cat /home/homever)
HV=${HOMEVER:0:2}
if [ "${HV:1:1}" == "." ]; then
    HV=${HV:0:1}
fi

get_config()
{
    key=$1
    grep -w $1 $YI_HACK_PREFIX/$CONF_FILE | cut -d "=" -f2
}

start_buffer()
{
    # Trick to start circular buffer filling
    ./cloud &
    IDX=`hexdump -n 16 /dev/shm/fshare_frame_buf | awk 'NR==1{print $8}'`
    N=0
    while [ "$IDX" -eq "0000" ] && [ $N -lt 60 ]; do
        IDX=`hexdump -n 16 /dev/shm/fshare_frame_buf | awk 'NR==1{print $8}'`
        N=$(($N+1))
        sleep 0.2
    done
    killall cloud
    ipc_cmd -x
}

log()
{
    if [ "$DEBUG_LOG" == "yes" ]; then
        echo $1 >> /tmp/sd/hack_debug.log

        if [ "$2" == "1" ]; then
            echo "" >> /tmp/sd/hack_debug.log
            ps >> /tmp/sd/hack_debug.log
            echo "" >> /tmp/sd/hack_debug.log
            free >> /tmp/sd/hack_debug.log
            echo "" >> /tmp/sd/hack_debug.log
        fi
    fi
}

DEBUG_LOG=$(get_config DEBUG_LOG)
rm -f /tmp/sd/hack_debug.log

log "Starting system.sh"

export PATH=$PATH:/home/base/tools:/home/yi-hack/bin:/home/yi-hack/sbin:/home/yi-hack/usr/bin:/home/yi-hack/usr/sbin:/tmp/sd/yi-hack/bin:/tmp/sd/yi-hack/sbin
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/lib:/home/yi-hack/lib:/tmp/sd/yi-hack/lib

ulimit -s 1024

echo 1500 > /sys/class/net/eth0/mtu
echo 1500 > /sys/class/net/wlan0/mtu

sysctl -w vm.oom_dump_tasks=0
sysctl -w vm.vfs_cache_pressure=100
sysctl -w kernel.randomize_va_space=0
echo 0 > /sys/block/mmcblk0/queue/iostats
echo 4 > /sys/block/mmcblk0/queue/iosched/quantum
echo 80 > /sys/block/mmcblk0/queue/iosched/fifo_expire_sync
echo 330 > /sys/block/mmcblk0/queue/iosched/fifo_expire_async
echo 12582912 > /sys/block/mmcblk0/queue/iosched/back_seek_max
echo 1 > /sys/block/mmcblk0/queue/iosched/back_seek_penalty
echo 60 > /sys/block/mmcblk0/queue/iosched/slice_sync
echo 50 > /sys/block/mmcblk0/queue/iosched/slice_async
echo 2 > /sys/block/mmcblk0/queue/iosched/slice_async_rq
echo 0 > /sys/block/mmcblk0/queue/iosched/slice_idle
echo 0 > /sys/block/mmcblk0/queue/iosched/group_idle
echo 1 > /sys/block/mmcblk0/queue/iosched/low_latency
echo 300 > /sys/block/mmcblk0/queue/iosched/target_latency
mount -o remount,noatime /tmp/sd
mount --bind /home/yi-hack/script/wifidhcp.sh /home/app/script/wifidhcp.sh
mount --bind /home/yi-hack/script/ethdhcp.sh /home/app/script/ethdhcp.sh

# Remove core files, if any
rm -f $YI_PREFIX/core
rm -f $YI_HACK_PREFIX/bin/core
rm -f $YI_HACK_PREFIX/www/core
rm -f $YI_HACK_PREFIX/www/cgi-bin/core
rm -f $YI_HACK_PREFIX/core

if [ ! -L /home/yi-hack-v4 ]; then
    ln -s $YI_HACK_PREFIX /home/yi-hack-v4
fi

touch /tmp/httpd.conf

# Restore configuration after a firmware upgrade
if [ -f $YI_HACK_PREFIX/fw_upgrade_in_progress ]; then
    log "Upgrade in progress"
    cp -f /tmp/sd/fw_upgrade/*.conf $YI_HACK_PREFIX/etc/
    chmod 0644 $YI_HACK_PREFIX/etc/*.conf
    if [ -f /tmp/sd/fw_upgrade/hostname ]; then
        cp -f /tmp/sd/fw_upgrade/hostname $YI_HACK_PREFIX/etc/
        chmod 0644 $YI_HACK_PREFIX/etc/hostname
    fi
    rm $YI_HACK_PREFIX/fw_upgrade_in_progress
fi

$YI_HACK_PREFIX/script/check_conf.sh

# Make /etc writable
log "Make /etc writable"
mkdir /tmp/etc
cp -R /etc/* /tmp/etc
mount --bind /tmp/etc /etc

hostname -F $YI_HACK_PREFIX/etc/hostname

TZ_TMP=$(get_config TIMEZONE)

if [[ $(get_config SWAP_FILE) == "yes" ]] ; then
    SD_PRESENT=$(mount | grep mmc | grep "/tmp/sd " | grep -c ^)
    if [[ $SD_PRESENT -eq 1 ]]; then
        log "Activating swap file"
        SWAP_SWAPPINESS=$(get_config SWAP_SWAPPINESS)
        log "Set swappiness to $SWAP_SWAPPINESS"
        sysctl -w "vm.swappiness=$SWAP_SWAPPINESS"
        if [[ -f /tmp/sd/swapfile ]]; then
            swapon /tmp/sd/swapfile
        else
            dd if=/dev/zero of=/tmp/sd/swapfile bs=1M count=64
            chmod 0600 /tmp/sd/swapfile
            mkswap /tmp/sd/swapfile
            swapon /tmp/sd/swapfile
        fi
    fi
fi

if [[ x$(get_config USERNAME) != "x" ]] ; then
    log "Setting username and password"
    USERNAME=$(get_config USERNAME)
    PASSWORD=$(get_config PASSWORD)
    RTSP_USERPWD=$USERNAME:$PASSWORD@
    ONVIF_USERPWD="user=$USERNAME\npassword=$PASSWORD"
    echo "/onvif::" > /tmp/httpd.conf
    echo "/:$USERNAME:$PASSWORD" >> /tmp/httpd.conf
    chmod 0600 /tmp/httpd.conf
fi

if [[ x$(get_config SSH_PASSWORD) != "x" ]] ; then
    log "Setting SSH password"
    SSH_PASSWORD=$(get_config SSH_PASSWORD)
    PASSWORD_MD5="$(echo "${SSH_PASSWORD}" | mkpasswd --method=MD5 --stdin)"
    sed -i 's|^root::|root:x:|g' /etc/passwd
    sed -i 's|:/root:|:/home/yi-hack:|g' /etc/passwd
    sed -i 's|^root::|root:'${PASSWORD_MD5}':|g' /etc/shadow
    chmod 0600 /etc/passwd
    chmod 0600 /etc/shadow
else
    sed -i 's|:/root:|:/home/yi-hack:|g' /etc/passwd
    chmod 0600 /etc/passwd
    chmod 0600 /etc/shadow
fi

case $(get_config RTSP_PORT) in
    ''|*[!0-9]*) RTSP_PORT=554 ;;
    *) RTSP_PORT=$(get_config RTSP_PORT) ;;
esac
case $(get_config HTTPD_PORT) in
    ''|*[!0-9]*) HTTPD_PORT=80 ;;
    *) HTTPD_PORT=$(get_config HTTPD_PORT) ;;
esac

log "Configuring cloudAPI"
if [ ! -f $YI_HACK_PREFIX/bin/cloudAPI_real ]; then
    cp $YI_PREFIX/cloudAPI $YI_HACK_PREFIX/bin/cloudAPI_real
fi
mount --bind $YI_HACK_PREFIX/bin/cloudAPI $YI_PREFIX/cloudAPI

log "Starting yi processes" 1
if [[ $(get_config DISABLE_CLOUD) == "no" ]] ; then
    (
        if [ $(get_config RTSP_AUDIO) == "pcm" ] || [ $(get_config RTSP_AUDIO) == "alaw" ] || [ $(get_config RTSP_AUDIO) == "ulaw" ]; then
            touch /tmp/audio_fifo.requested
        fi
        if [ $(get_config SPEAKER_AUDIO) != "no" ]; then
            touch /tmp/audio_in_fifo.requested
        fi
        cd /home/app
        killall dispatch
        LD_PRELOAD=/home/yi-hack/lib/ipc_multiplex.so ./dispatch &
        sleep 2
        set_tz_offset -c osd -o off
        LD_LIBRARY_PATH="/home/yi-hack/lib:/lib:/usr/lib:/home/lib:/home/qigan/lib:/home/app/locallib" ./rmm &
        sleep 10
        dd if=/tmp/audio_fifo of=/dev/null bs=1 count=8192
        if [[ $(get_config TIME_OSD) == "yes" ]] ; then
            (sleep 30; export TZP=`TZ=$TZ_TMP date +%z`; export TZP=${TZP:0:3}:${TZP:3:2}; export TZ=GMT$TZP; ./mp4record) &
        else
            ./mp4record &
        fi
        ./cloud &
        ./p2p_tnp &
        ./oss &
        if [ -f ./oss_fast ]; then
            ./oss_fast &
        fi
        if [ -f ./oss_lapse ]; then
            ./oss_lapse &
        fi
        (sleep 30; ./watch_process) &
    )
else
    (
        while read -r line
        do
            echo "127.0.0.1    $line" >> /etc/hosts
        done < $YI_HACK_PREFIX/script/blacklist/url

        while read -r line
        do
            route add -host $line reject
        done < $YI_HACK_PREFIX/script/blacklist/ip

        if [ $(get_config RTSP_AUDIO) == "pcm" ] || [ $(get_config RTSP_AUDIO) == "alaw" ] || [ $(get_config RTSP_AUDIO) == "ulaw" ]; then
            touch /tmp/audio_fifo.requested
        fi
        if [ $(get_config SPEAKER_AUDIO) != "no" ]; then
            touch /tmp/audio_in_fifo.requested
        fi
        cd /home/app
        killall dispatch
        LD_PRELOAD=/home/yi-hack/lib/ipc_multiplex.so ./dispatch &
        sleep 2
        set_tz_offset -c osd -o off
        LD_LIBRARY_PATH="/home/yi-hack/lib:/lib:/usr/lib:/home/lib:/home/qigan/lib:/home/app/locallib" ./rmm &
        sleep 10
        dd if=/tmp/audio_fifo of=/dev/null bs=1 count=8192
        # Trick to start circular buffer filling
        start_buffer
        if [[ $(get_config REC_WITHOUT_CLOUD) == "yes" ]] ; then
            if [[ $(get_config TIME_OSD) == "yes" ]] ; then
                (sleep 30; export TZP=`TZ=$TZ_TMP date +%z`; export TZP=${TZP:0:3}:${TZP:3:2}; export TZ=GMT$TZP; ./mp4record) &
            else
                ./mp4record &
            fi
        fi
        ./cloud &

        if [ "$HV" == "12" ]; then
            ipc_cmd -1
            sleep 0.5
            if [[ $(get_config MOTION_DETECTION) == "yes" ]] ; then
                ipc_cmd -O on
            else
                if [[ $(get_config AI_HUMAN_DETECTION) == "yes" ]] ; then
                    ipc_cmd -a on
                    sleep 0.1
                fi
                if [[ $(get_config AI_VEHICLE_DETECTION) == "yes" ]] ; then
                    ipc_cmd -E on
                    sleep 0.1
                fi
                if [[ $(get_config AI_ANIMAL_DETECTION) == "yes" ]] ; then
                    ipc_cmd -N on
                    sleep 0.1
                fi
            fi
        fi
    )
fi

log "Yi processes started successfully" 1

# The cam works in GMT time when the hack is disabled
# Yi processes receive the time from the clound
export TZ=$TZ_TMP

if [[ $(get_config HTTPD) == "yes" ]] ; then
    log "Starting http"
    httpd -p $HTTPD_PORT -h $YI_HACK_PREFIX/www/ -c /tmp/httpd.conf
fi

if [[ $(get_config TELNETD) == "no" ]] ; then
    killall telnetd
fi

if [[ $(get_config FTPD) == "yes" ]] ; then
    log "Starting ftp"
    if [[ $(get_config BUSYBOX_FTPD) == "yes" ]] ; then
        tcpsvd -vE 0.0.0.0 21 ftpd -w &
    else
        pure-ftpd -B
    fi
fi

if [[ $(get_config SSHD) == "yes" ]] ; then
    log "Starting sshd"
    mkdir -p $YI_HACK_PREFIX/etc/dropbear
    if [ ! -f /home/base/scp ]; then
        ln -s /home/yi-hack/bin/scp /home/base/scp
    fi
    dropbear -R -B -p 0.0.0.0:22
fi

if [[ $(get_config NTPD) == "yes" ]] ; then
    log "Starting ntp"
    # Wait until all the other processes have been initialized
    sleep 5 && ntpd -p $(get_config NTP_SERVER) &
fi

log "Starting mqtt services"
mqttv4 &
if [[ $(get_config MQTT) == "yes" ]] ; then
    mqtt-config &
    /home/yi-hack/script/conf2mqtt.sh &
fi

sleep 5

if [[ $RTSP_PORT != "554" ]] ; then
    D_RTSP_PORT=:$RTSP_PORT
fi

if [[ $HTTPD_PORT != "80" ]] ; then
    D_HTTPD_PORT=:$HTTPD_PORT
fi

if [[ $(get_config ONVIF_WM_SNAPSHOT) == "yes" ]] ; then
    WATERMARK="&watermark=yes"
fi

if [[ $(get_config SNAPSHOT) == "no" ]] ; then
    touch /tmp/snapshot.disabled
fi

if [[ $(get_config SNAPSHOT_LOW) == "yes" ]] ; then
    touch /tmp/snapshot.low
fi

if [[ $(get_config RTSP) == "yes" ]] ; then
    log "Starting rtsp"
    RTSP_DAEMON="rRTSPServer"
    RTSP_AUDIO_COMPRESSION=$(get_config RTSP_AUDIO)
    RTSP_ALT=$(get_config RTSP_ALT)
    H264GRABBER_AUDIO=""

    if [[ "$RTSP_ALT" == "yes" ]] ; then
        RTSP_DAEMON="rtsp_server_yi"
    fi
    if [[ "$RTSP_AUDIO_COMPRESSION" == "none" ]] ; then
        RTSP_AUDIO_COMPRESSION="no"
    fi

    if [[ "$RTSP_AUDIO_COMPRESSION" == "aac" ]] ; then
        H264GRABBER_AUDIO="-a"
    fi

    if [ ! -z $RTSP_AUDIO_COMPRESSION ]; then
        RTSP_AUDIO_COMPRESSION="-a "$RTSP_AUDIO_COMPRESSION
    fi
    if [ ! -z $RTSP_PORT ]; then
        RTSP_PORT="-p "$RTSP_PORT
    fi
    if [ ! -z $USERNAME ]; then
        RTSP_USER="-u "$USERNAME
    fi
    if [ ! -z $PASSWORD ]; then
        RTSP_PASSWORD="-w "$PASSWORD
    fi
    RTSP_STREAM=$(get_config RTSP_STREAM)
    ONVIF_PROFILE=$(get_config ONVIF_PROFILE)
    ONVIF_AUDIO_BC=$(get_config ONVIF_AUDIO_BC)

    if [[ "$RTSP_STREAM" == "low" ]]; then
        if [[ "$RTSP_ALT" == "yes" ]] ; then
            h264grabber -m $MODEL_SUFFIX -r low $H264GRABBER_AUDIO -f &
            sleep 1
        fi
        $RTSP_DAEMON -m $MODEL_SUFFIX -r low $RTSP_AUDIO_COMPRESSION $RTSP_PORT $RTSP_USER $RTSP_PASSWORD &
        ONVIF_PROFILE_1="name=Profile_1\nwidth=640\nheight=360\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_1.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=low$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
    fi
    if [[ "$RTSP_STREAM" == "high" ]]; then
        if [[ "$RTSP_ALT" == "yes" ]] ; then
            h264grabber -m $MODEL_SUFFIX -r high $H264GRABBER_AUDIO -f &
            sleep 1
        fi
        $RTSP_DAEMON -m $MODEL_SUFFIX -r high $RTSP_AUDIO_COMPRESSION $RTSP_PORT $RTSP_USER $RTSP_PASSWORD &
        ONVIF_PROFILE_0="name=Profile_0\nwidth=1920\nheight=1080\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_0.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=high$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
    fi
    if [[ "$RTSP_STREAM" == "both" ]]; then
        if [[ "$RTSP_ALT" == "yes" ]] ; then
            h264grabber -m $MODEL_SUFFIX -r both $H264GRABBER_AUDIO -f &
            sleep 1
        fi
        $RTSP_DAEMON -m $MODEL_SUFFIX -r both $RTSP_AUDIO_COMPRESSION $RTSP_PORT $RTSP_USER $RTSP_PASSWORD &
        if [[ "$ONVIF_PROFILE" == "low" ]] || [[ "$ONVIF_PROFILE" == "both" ]] ; then
            ONVIF_PROFILE_1="name=Profile_1\nwidth=640\nheight=360\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_1.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=low$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
        fi
        if [[ "$ONVIF_PROFILE" == "high" ]] || [[ "$ONVIF_PROFILE" == "both" ]] ; then
            ONVIF_PROFILE_0="name=Profile_0\nwidth=1920\nheight=1080\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_0.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=high$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
        fi
    fi
    $YI_HACK_PREFIX/script/wd_rtsp.sh &
fi

SERIAL_NUMBER=$(dd bs=1 count=20 skip=592 if=/tmp/mmap.info 2>/dev/null | tr '\0' '0' | cut -c1-20)
HW_ID=$(dd bs=1 count=4 skip=592 if=/tmp/mmap.info 2>/dev/null | tr '\0' '0' | cut -c1-4)

if [[ $(get_config ONVIF) == "yes" ]] ; then
    log "Starting onvif"
    if [[ $(get_config ONVIF_FAULT_IF_UNKNOWN) == "yes" ]] ; then
        ONVIF_FAULT_IF_UNKNOWN=1
    else
        ONVIF_FAULT_IF_UNKNOWN=0
    fi
    if [[ $(get_config ONVIF_SYNOLOGY_NVR) == "yes" ]] ; then
        ONVIF_SYNOLOGY_NVR=1
    else
        ONVIF_SYNOLOGY_NVR=0
    fi

    ONVIF_SRVD_CONF="/tmp/onvif_simple_server.conf"

    echo "model=Yi Hack" > $ONVIF_SRVD_CONF
    echo "manufacturer=Yi" >> $ONVIF_SRVD_CONF
    echo "firmware_ver=$YI_HACK_VER" >> $ONVIF_SRVD_CONF
    echo "hardware_id=$HW_ID" >> $ONVIF_SRVD_CONF
    echo "serial_num=$SERIAL_NUMBER" >> $ONVIF_SRVD_CONF
    echo "ifs=wlan0" >> $ONVIF_SRVD_CONF
    echo "port=$HTTPD_PORT" >> $ONVIF_SRVD_CONF
    echo "scope=onvif://www.onvif.org/Profile/Streaming" >> $ONVIF_SRVD_CONF
    echo "adv_fault_if_unknown=$ONVIF_FAULT_IF_UNKNOWN" >> $ONVIF_SRVD_CONF
    echo "adv_synology_nvr=$ONVIF_SYNOLOGY_NVR" >> $ONVIF_SRVD_CONF
    echo "" >> $ONVIF_SRVD_CONF
    if [ ! -z $ONVIF_USERPWD ]; then
        echo -e $ONVIF_USERPWD >> $ONVIF_SRVD_CONF
        echo "" >> $ONVIF_SRVD_CONF
    fi
    if [ ! -z $ONVIF_PROFILE_0 ]; then
        echo "#Profile 0" >> $ONVIF_SRVD_CONF
        echo -e $ONVIF_PROFILE_0 >> $ONVIF_SRVD_CONF
        echo "" >> $ONVIF_SRVD_CONF
    fi
    if [ ! -z $ONVIF_PROFILE_1 ]; then
        echo "#Profile 1" >> $ONVIF_SRVD_CONF
        echo -e $ONVIF_PROFILE_1 >> $ONVIF_SRVD_CONF
        echo "" >> $ONVIF_SRVD_CONF
    fi

    if [[ $MODEL_SUFFIX == "y30qa" ]] ; then
        echo "#PTZ" >> $ONVIF_SRVD_CONF
        echo "ptz=1" >> $ONVIF_SRVD_CONF
        echo "get_position=/home/yi-hack/bin/ipc_cmd -g" >> $ONVIF_SRVD_CONF
        echo "is_moving=/home/yi-hack/bin/ipc_cmd -u" >> $ONVIF_SRVD_CONF
        echo "move_left=/home/yi-hack/bin/ipc_cmd -m left" >> $ONVIF_SRVD_CONF
        echo "move_right=/home/yi-hack/bin/ipc_cmd -m right" >> $ONVIF_SRVD_CONF
        echo "move_up=/home/yi-hack/bin/ipc_cmd -m up" >> $ONVIF_SRVD_CONF
        echo "move_down=/home/yi-hack/bin/ipc_cmd -m down" >> $ONVIF_SRVD_CONF
        echo "move_stop=/home/yi-hack/bin/ipc_cmd -m stop" >> $ONVIF_SRVD_CONF
        echo "move_preset=/home/yi-hack/bin/ipc_cmd -p %d" >> $ONVIF_SRVD_CONF
        echo "set_preset=/home/yi-hack/script/ptz_presets.sh -a add_preset -m %s" >> $ONVIF_SRVD_CONF
        echo "set_home_position=/home/yi-hack/script/ptz_presets.sh -a set_home_position" >> $ONVIF_SRVD_CONF
        echo "remove_preset=/home/yi-hack/script/ptz_presets.sh -a del_preset -n %d" >> $ONVIF_SRVD_CONF
        echo "jump_to_abs=/home/yi-hack/bin/ipc_cmd -j %f,%f" >> $ONVIF_SRVD_CONF
        echo "jump_to_rel=/home/yi-hack/bin/ipc_cmd -J %f,%f" >> $ONVIF_SRVD_CONF
        echo "get_presets=/home/yi-hack/script/ptz_presets.sh -a get_presets" >> $ONVIF_SRVD_CONF
        echo "" >> $ONVIF_SRVD_CONF
    fi

    echo "#EVENT" >> $ONVIF_SRVD_CONF
    echo "events=3" >> $ONVIF_SRVD_CONF
    echo "#Event 0" >> $ONVIF_SRVD_CONF
    echo "topic=tns1:VideoSource/MotionAlarm" >> $ONVIF_SRVD_CONF
    echo "source_name=VideoSourceConfigurationToken" >> $ONVIF_SRVD_CONF
    echo "source_value=VideoSourceToken" >> $ONVIF_SRVD_CONF
    echo "input_file=/tmp/onvif_notify_server/motion_alarm" >> $ONVIF_SRVD_CONF
    echo "#Event 1" >> $ONVIF_SRVD_CONF
    echo "topic=tns1:RuleEngine/MyRuleDetector/PeopleDetect" >> $ONVIF_SRVD_CONF
    echo "source_name=VideoSourceConfigurationToken" >> $ONVIF_SRVD_CONF
    echo "source_value=VideoSourceToken" >> $ONVIF_SRVD_CONF
    echo "input_file=/tmp/onvif_notify_server/human_detection" >> $ONVIF_SRVD_CONF
    echo "#Event 2" >> $ONVIF_SRVD_CONF
    echo "topic=tns1:RuleEngine/MyRuleDetector/VehicleDetect" >> $ONVIF_SRVD_CONF
    echo "source_name=VideoSourceConfigurationToken" >> $ONVIF_SRVD_CONF
    echo "source_value=VideoSourceToken" >> $ONVIF_SRVD_CONF
    echo "input_file=/tmp/onvif_notify_server/vehicle_detection" >> $ONVIF_SRVD_CONF
    echo "#Event 3" >> $ONVIF_SRVD_CONF
    echo "topic=tns1:RuleEngine/MyRuleDetector/DogCatDetect" >> $ONVIF_SRVD_CONF
    echo "source_name=VideoSourceConfigurationToken" >> $ONVIF_SRVD_CONF
    echo "source_value=VideoSourceToken" >> $ONVIF_SRVD_CONF
    echo "input_file=/tmp/onvif_notify_server/animal_detection" >> $ONVIF_SRVD_CONF
    echo "#Event 4" >> $ONVIF_SRVD_CONF
    echo "topic=tns1:RuleEngine/MyRuleDetector/BabyCryingDetect" >> $ONVIF_SRVD_CONF
    echo "source_name=VideoSourceConfigurationToken" >> $ONVIF_SRVD_CONF
    echo "source_value=VideoSourceToken" >> $ONVIF_SRVD_CONF
    echo "input_file=/tmp/onvif_notify_server/baby_crying" >> $ONVIF_SRVD_CONF
    echo "#Event 5" >> $ONVIF_SRVD_CONF
    echo "topic=tns1:AudioAnalytics/Audio/DetectedSound" >> $ONVIF_SRVD_CONF
    echo "source_name=VideoSourceConfigurationToken" >> $ONVIF_SRVD_CONF
    echo "source_value=VideoSourceToken" >> $ONVIF_SRVD_CONF
    echo "input_file=/tmp/onvif_notify_server/sound_detection" >> $ONVIF_SRVD_CONF

    chmod 0600 $ONVIF_SRVD_CONF
    onvif_simple_server --conf_file $ONVIF_SRVD_CONF
    ipc2file
    mkdir -p /tmp/onvif_notify_server
    onvif_notify_server --conf_file $ONVIF_SRVD_CONF

    if [[ $(get_config ONVIF_WSDD) == "yes" ]] ; then
        wsd_simple_server --pid_file /var/run/wsd_simple_server.pid --if_name wlan0 --xaddr "http://%s$D_HTTPD_PORT/onvif/device_service" -m `hostname` -n Yi
    fi
fi

if [[ $(get_config TIME_OSD) == "yes" ]] ; then
    log "Enable time osd"
    # Enable time osd
    set_tz_offset -c osd -o on
    # Set timezone for time osd
    TZP=$(TZ=$TZ_TMP date +%z)
    TZP_SET=$(echo ${TZP:0:1} ${TZP:1:2} ${TZP:3:2} | awk '{ print ($1$2*3600+$3*60) }')
    set_tz_offset -c tz_offset_osd -m $MODEL_SUFFIX -f $HV -v $TZP_SET
fi

log "Starting crontab"
# Add crontab
CRONTAB=$(get_config CRONTAB)
FREE_SPACE=$(get_config FREE_SPACE)
mkdir -p /var/spool/cron/crontabs/
if [ ! -z "$CRONTAB" ]; then
    echo -e "$CRONTAB" > /var/spool/cron/crontabs/root
fi
if [[ $(get_config SNAPSHOT) == "yes" ]] && [[ $(get_config SNAPSHOT_VIDEO) == "yes" ]] ; then
    echo "* * * * * /home/yi-hack/script/thumb.sh cron" >> /var/spool/cron/crontabs/root
fi
if [ "$FREE_SPACE" != "0" ]; then
    echo "0 * * * * sleep 20; /home/yi-hack/script/clean_records.sh $FREE_SPACE" >> /var/spool/cron/crontabs/root
fi
if [[ $(get_config FTP_UPLOAD) == "yes" ]] ; then
    echo "* * * * * sleep 40; /home/yi-hack/script/ftppush.sh cron" >> /var/spool/cron/crontabs/root
fi
if [[ $(get_config TIMELAPSE) == "yes" ]] ; then
    DT=$(get_config TIMELAPSE_DT)
    OFF=""
    CRDT=""
    if [ "$DT" == "1" ]; then
        CRDT="* * * * *"
    elif [ "$DT" == "2" ] || [ "$DT" == "3" ] || [ "$DT" == "4" ] || [ "$DT" == "5" ] || [ "$DT" == "6" ] || [ "$DT" == "10" ] || [ "$DT" == "15" ] || [ "$DT" == "20" ] || [ "$DT" == "30" ]; then
        CRDT="*/$DT * * * *"
    elif [ "$DT" == "60" ]; then
        CRDT="0 * * * *"
    elif [ "$DT" == "120" ] || [ "$DT" == "180" ] || [ "$DT" == "240" ] || [ "$DT" == "360" ]; then
        DTF=$(($DT/60))
        CRDT="* */$DTF * * *"
    elif [ "$DT" == "1440" ]; then
        CRDT="0 0 * * *"
    elif [ "${DT:0:4}" == "1440" ]; then
        if [ "${DT:4:1}" == "+" ]; then
            OFF="${DT:5:10}"
            if [ ! -z $OFF ]; then
                case $OFF in
                    ''|*[!0-9]* )
                        OFF_OK=0;;
                    * )
                        OFF_OK=1;;
                esac
                if [ "$OFF_OK" == "1" ] && [ $OFF -le 1440 ]; then
                    OFF_H=$(($OFF/60))
                    OFF_M=$(($OFF%60))
                    CRDT="$OFF_M $OFF_H * * *"
                fi
            fi
        fi
    fi
    if [[ $(get_config TIMELAPSE_FTP) == "yes" ]] ; then
        echo "$CRDT sleep 30; /home/yi-hack/script/time_lapse.sh yes" >> /var/spool/cron/crontabs/root
    else
        if [ ! -z "$CRDT" ]; then
            echo "$CRDT sleep 30; /home/yi-hack/script/time_lapse.sh no" >> /var/spool/cron/crontabs/root
        fi
        VDT=$(get_config TIMELAPSE_VDT)
        if [ ! -z "$VDT" ]; then
            echo "$(get_config TIMELAPSE_VDT) /home/yi-hack/script/create_avi.sh /tmp/sd/record/timelapse 1920x1080 5" >> /var/spool/cron/crontabs/root
        fi
    fi
fi
$YI_HACK_PREFIX/usr/sbin/crond -c /var/spool/cron/crontabs/

# Add MQTT Advertise
if [ -f "$YI_HACK_PREFIX/script/mqtt_advertise/startup.sh" ]; then
    $YI_HACK_PREFIX/script/mqtt_advertise/startup.sh
fi

# Add library path for linker
echo "/lib:/usr/lib:/tmp/sd/yi-hack/lib" > /etc/ld-musl-armhf.path

# Add custom binaries to PATH
echo "" >> /etc/profile
echo "# Custom yi-hack binaries" >> /etc/profile
echo "PATH=\$PATH:/home/yi-hack/bin:/home/yi-hack/sbin:/home/yi-hack/usr/bin:/home/yi-hack/usr/sbin:/tmp/sd/yi-hack/bin:/tmp/sd/yi-hack/sbin" >> /etc/profile

# Remove log files written to SD on boot containing the WiFi password
rm -f "/tmp/sd/log/log_first_login.tar.gz"
rm -f "/tmp/sd/log/log_login.tar.gz"
rm -f "/tmp/sd/log/log_oss_exit.tar.gz"
rm -f "/tmp/sd/log/log_oss_fail_x.tar.gz"
rm -f "/tmp/sd/log/log_oss_fragment_x.tar.gz"
rm -f "/tmp/sd/log/log_oss_success_x.tar.gz"
rm -f "/tmp/sd/log/log_p2p_clr.tar.gz"
rm -f "/tmp/sd/log/log_wifi_connected.tar.gz"

unset TZ

log "Starting custom startup.sh"
if [ -f "/tmp/sd/yi-hack/startup.sh" ]; then
    /tmp/sd/yi-hack/startup.sh
fi

log "system.sh completed" 1
