#!/bin/sh

CONF_FILE="etc/system.conf"

YI_HACK_PREFIX="/home/yi-hack"

YI_HACK_VER=$(cat /home/yi-hack/version)
MODEL_SUFFIX=$(cat /home/yi-hack/model_suffix)
SERIAL_NUMBER=$(dd bs=1 count=20 skip=592 if=/tmp/mmap.info 2>/dev/null | tr '\0' '0' | cut -c1-20)
HW_ID=$(dd bs=1 count=4 skip=592 if=/tmp/mmap.info 2>/dev/null | tr '\0' '0' | cut -c1-4)

get_config()
{
    key=$1
    grep -w $1 $YI_HACK_PREFIX/$CONF_FILE | cut -d "=" -f2
}

init_config()
{
    TZ_TMP=$(get_config TIMEZONE)

    if [[ x$(get_config USERNAME) != "x" ]] ; then
        USERNAME=$(get_config USERNAME)
        PASSWORD=$(get_config PASSWORD)
        ONVIF_USERPWD="user=$USERNAME\npassword=$PASSWORD"
        RTSP_USERPWD=$USERNAME:$PASSWORD@
    fi

    case $(get_config RTSP_PORT) in
        ''|*[!0-9]*) RTSP_PORT=554 ;;
        *) RTSP_PORT=$(get_config RTSP_PORT) ;;
    esac
    case $(get_config HTTPD_PORT) in
        ''|*[!0-9]*) HTTPD_PORT=80 ;;
        *) HTTPD_PORT=$(get_config HTTPD_PORT) ;;
    esac

    if [[ $RTSP_PORT != "554" ]] ; then
        D_RTSP_PORT=:$RTSP_PORT
    fi

    if [[ $HTTPD_PORT != "80" ]] ; then
        D_HTTPD_PORT=:$HTTPD_PORT
    fi

    if [[ $(get_config RTSP) == "yes" ]] ; then
        if [[ $(get_config RTSP_ALT) == "alternative" ]] ; then
            RTSP_DAEMON="rtsp_server_yi"
        elif [[ $(get_config RTSP_ALT) == "go2rtc" ]] ; then
            RTSP_DAEMON="go2rtc"
        else
            RTSP_DAEMON="rRTSPServer"
        fi

        RTSP_AUDIO_COMPRESSION=$(get_config RTSP_AUDIO)
        if [ "$RTSP_AUDIO_COMPRESSION" == "aac" ]; then
            H264GRABBER_AUDIO="-a"
        fi

        if [[ "$RTSP_AUDIO_COMPRESSION" == "none" ]] ; then
            RTSP_AUDIO_COMPRESSION="no"
        fi
        if [ ! -z $RTSP_AUDIO_COMPRESSION ]; then
            RTSP_AUDIO="-a "$RTSP_AUDIO_COMPRESSION
        fi
        if [ ! -z $RTSP_PORT ]; then
            P_RTSP_PORT="-p "$RTSP_PORT
        fi
        if [ ! -z $USERNAME ]; then
            RTSP_USER="-u "$USERNAME
        fi
        if [ ! -z $PASSWORD ]; then
            RTSP_PASSWORD="-w "$PASSWORD
        fi

        RTSP_RES=$(get_config RTSP_STREAM)
        RTSP_ALT=$(get_config RTSP_ALT)
    fi

    ONVIF_AUDIO_BC=$(get_config ONVIF_AUDIO_BC)
    if [ ! -z $ONVIF_AUDIO_BC ]; then
        if [ "$ONVIF_AUDIO_BC" != "NONE" ] && [ "$ONVIF_AUDIO_BC" != "none" ]; then
            if [ "$ONVIF_AUDIO_BC" == "G711" ]; then
                B_ONVIF_AUDIO_BC="-b ulaw"
            else
                B_ONVIF_AUDIO_BC="-b $ONVIF_AUDIO_BC"
            fi
        fi
    fi
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
}

start_rtsp()
{
    # If "null" use default

    if [ "$1" == "low" ] || [ "$1" == "high" ] || [ "$1" == "both" ]; then
        RTSP_RES=$1
    fi
    if [ "$2" == "aac" ]; then
        H264GRABBER_AUDIO="-a"
    fi
    if [ "$2" == "no" ] || [ "$2" == "yes" ] || [ "$2" == "alaw" ] || [ "$2" == "ulaw" ] || [ "$2" == "pcm" ] || [ "$2" == "aac" ] ; then
        RTSP_AUDIO_COMPRESSION=$2
        RTSP_AUDIO="-a "$2
    fi

    if [ "$RTSP_ALT" == "go2rtc" ]; then
        echo "streams:" > /tmp/go2rtc.yaml
        if [ "$RTSP_RES" == "high" ] || [ "$RTSP_RES" == "both" ]; then
            echo "  ch0_0.h264:" >> /tmp/go2rtc.yaml
            echo "    - exec:h264grabber -m h52ga -r high#backchannel=0" >> /tmp/go2rtc.yaml
        fi
        if [ "$RTSP_RES" != "low" ] && [ "$RTSP_AUDIO_COMPRESSION" == "aac" ] ; then
            echo "    - exec:h264grabber -m $MODEL_SUFFIX -r none -a#backchannel=0" >> /tmp/go2rtc.yaml
        fi
        if [ "$RTSP_RES" == "low" ] || [ "$RTSP_RES" == "both" ]; then
            echo "  ch0_1.h264:" >> /tmp/go2rtc.yaml
            echo "    - exec:h264grabber -m $MODEL_SUFFIX -r low#backchannel=0" >> /tmp/go2rtc.yaml
        fi
        if [ "$RTSP_RES" == "low" ] && [ "$RTSP_AUDIO_COMPRESSION" == "aac" ] ; then
            echo "    - exec:h264grabber -m $MODEL_SUFFIX -r none -a#backchannel=0" >> /tmp/go2rtc.yaml
        fi

        echo "" >> /tmp/go2rtc.yaml
        echo "api:" >> /tmp/go2rtc.yaml
        echo "  listen: \"\"" >> /tmp/go2rtc.yaml
        echo "" >> /tmp/go2rtc.yaml
        echo "webrtc:" >> /tmp/go2rtc.yaml
        echo "  listen: \"\"" >> /tmp/go2rtc.yaml
        echo "" >> /tmp/go2rtc.yaml
        echo "rtsp:" >> /tmp/go2rtc.yaml
        echo "  listen: \":$RTSP_PORT\"" >> /tmp/go2rtc.yaml
        if [ ! -z $USERNAME ]; then
            echo "  username: \"$USERNAME\"" >> /tmp/go2rtc.yaml
            echo "  password: \"$PASSWORD\"" >> /tmp/go2rtc.yaml
        fi

        $RTSP_DAEMON -c /tmp/go2rtc.yaml -d
    else

        if [[ $RTSP_RES == "low" ]]; then
            if [ "$RTSP_ALT" == "yes" ]; then
                h264grabber -m $MODEL_SUFFIX -r low $H264GRABBER_AUDIO -f &
                sleep 1
            fi
            $RTSP_DAEMON -m $MODEL_SUFFIX -r low $RTSP_AUDIO $P_RTSP_PORT $RTSP_USER $RTSP_PASSWORD $B_ONVIF_AUDIO_BC &
        elif [[ $RTSP_RES == "high" ]]; then
            if [ "$RTSP_ALT" == "yes" ]; then
                h264grabber -m $MODEL_SUFFIX -r high $H264GRABBER_AUDIO -f &
                sleep 1
            fi
            $RTSP_DAEMON -m $MODEL_SUFFIX -r high $RTSP_AUDIO $P_RTSP_PORT $RTSP_USER $RTSP_PASSWORD $B_ONVIF_AUDIO_BC &
        elif [[ $RTSP_RES == "both" ]]; then
            if [ "$RTSP_ALT" == "yes" ]; then
                h264grabber -m $MODEL_SUFFIX -r both $H264GRABBER_AUDIO -f &
                sleep 1
            fi
            $RTSP_DAEMON -m $MODEL_SUFFIX -r both $RTSP_AUDIO $P_RTSP_PORT $RTSP_USER $RTSP_PASSWORD $B_ONVIF_AUDIO_BC &
        fi

        WD_COUNT=$(ps | grep wd_rtsp.sh | grep -v grep | grep -c ^)
        if [ $WD_COUNT -eq 0 ]; then
            (sleep 30; $YI_HACK_PREFIX/script/wd_rtsp.sh >/dev/null) &
        fi
    fi
}

stop_rtsp()
{
    killall wd_rtsp.sh
    killall $RTSP_DAEMON
}

start_onvif()
{
    # If "null" use default

    if [[ "$2" == "null" ]]; then
        ONVIF_WM_SNAPSHOT=$(get_config ONVIF_WM_SNAPSHOT)
        WATERMARK="&watermark="$ONVIF_WM_SNAPSHOT
    elif [[ "$2" == "yes" ]]; then
        WATERMARK="&watermark=yes"
    fi
    if [[ "$1" == "null" ]]; then
        ONVIF_PROFILE=$(get_config ONVIF_PROFILE)
    elif [[ "$1" == "low" ]] || [[ "$1" == "high" ]] || [[ "$1" == "both" ]]; then
        ONVIF_PROFILE=$1
    fi
    if [[ $ONVIF_PROFILE == "high" ]]; then
        ONVIF_PROFILE_0="name=Profile_0\nwidth=1920\nheight=1080\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_0.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=high$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
    fi
    if [[ $ONVIF_PROFILE == "low" ]]; then
        ONVIF_PROFILE_1="name=Profile_1\nwidth=640\nheight=360\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_1.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=low$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
    fi
    if [[ $ONVIF_PROFILE == "both" ]]; then
        ONVIF_PROFILE_0="name=Profile_0\nwidth=1920\nheight=1080\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_0.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=high$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
        ONVIF_PROFILE_1="name=Profile_1\nwidth=640\nheight=360\nurl=rtsp://$RTSP_USERPWD%s$D_RTSP_PORT/ch0_1.h264\nsnapurl=http://$RTSP_USERPWD%s$D_HTTPD_PORT/cgi-bin/snapshot.sh?res=low$WATERMARK\ntype=H264\ndecoder=$ONVIF_AUDIO_BC"
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
        echo "goto_home_position=/home/yi-hack/bin/ipc_cmd -p 0" >> $ONVIF_SRVD_CONF
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
}

stop_onvif()
{
    killall onvif_notify_server
    killall ipc2file
    killall onvif_simple_server
}

start_wsdd()
{
    wsd_simple_server --pid_file /var/run/wsd_simple_server.pid --if_name wlan0 --xaddr "http://%s$D_HTTPD_PORT/onvif/device_service" -m `hostname` -n Yi
}

stop_wsdd()
{
    killall wsd_simple_server
}

start_ftpd()
{
    if [[ "$1" == "null" ]] ; then
        if [[ $(get_config BUSYBOX_FTPD) == "yes" ]] ; then
            FTPD_DAEMON="busybox"
        else
            FTPD_DAEMON="pure-ftpd"
        fi
    else
        FTPD_DAEMON=$1
    fi

    if [[ $FTPD_DAEMON == "busybox" ]] ; then
        tcpsvd -vE 0.0.0.0 21 ftpd -w >/dev/null &
    elif [[ $FTPD_DAEMON == "pure-ftpd" ]] ; then
        pure-ftpd -B
    fi
}

stop_ftpd()
{
    if [[ "$1" == "null" ]] ; then
        if [[ $(get_config BUSYBOX_FTPD) == "yes" ]] ; then
            FTPD_DAEMON="busybox"
        else
            FTPD_DAEMON="pure-ftpd"
        fi
    else
        FTPD_DAEMON=$1
    fi

    if [[ $FTPD_DAEMON == "busybox" ]] ; then
        killall tcpsvd
    elif [[ $FTPD_DAEMON == "pure-ftpd" ]] ; then
        killall pure-ftpd
    fi
}

ps_program()
{
    PS_PROGRAM=$(ps | grep $1 | grep -v grep | grep -c ^)
    if [ $PS_PROGRAM -gt 0 ]; then
        echo "started"
    else
        echo "stopped"
    fi
}

NAME="null"
ACTION="null"
PARAM1="null"
PARAM2="null"
RES=""

if [ $# -lt 2 ]; then
    exit
fi

NAME=$1
ACTION=$2
if [ $# -eq 3 ]; then
    PARAM1=$3
fi
if [ $# -eq 4 ]; then
    PARAM2=$4
fi

init_config

if [ "$ACTION" == "start" ] ; then
    if [ "$NAME" == "rtsp" ]; then
        start_rtsp $PARAM1 $PARAM2
    elif [ "$NAME" == "onvif" ]; then
        start_onvif $PARAM1 $PARAM2
    elif [ "$NAME" == "wsdd" ]; then
        start_wsdd
    elif [ "$NAME" == "ftpd" ]; then
        start_ftpd $PARAM1
    elif [ "$NAME" == "mqtt" ]; then
        mqttv4 > /dev/null &
    elif [ "$NAME" == "mqtt-config" ]; then
        mqtt-config > /dev/null &
    elif [ "$NAME" == "mp4record" ]; then
        cd /home/app
        if [[ $(get_config TIME_OSD) == "yes" ]] ; then
            TZP=`TZ=$TZ_TMP date +%z`
            TZP=${TZP:0:3}:${TZP:3:2}
            TZ=GMT$TZP ./mp4record > /dev/null &
        else
            ./mp4record > /dev/null &
        fi
    elif [ "$NAME" == "all" ]; then
        start_rtsp
        start_onvif
        start_wsdd
        start_ftpd
        mqttv4 > /dev/null &
        mqtt-config > /dev/null &
        cd /home/app
        if [[ $(get_config TIME_OSD) == "yes" ]] ; then
            TZP=`TZ=$TZ_TMP date +%z`
            TZP=${TZP:0:3}:${TZP:3:2}
            TZ=GMT$TZP ./mp4record > /dev/null &
        else
            ./mp4record > /dev/null &
        fi
    fi
elif [ "$ACTION" == "stop" ] ; then
    if [ "$NAME" == "rtsp" ]; then
        stop_rtsp
    elif [ "$NAME" == "onvif" ]; then
        stop_onvif
    elif [ "$NAME" == "wsdd" ]; then
        stop_wsdd
    elif [ "$NAME" == "ftpd" ]; then
        stop_ftpd $PARAM1
    elif [ "$NAME" == "mqtt-config" ]; then
        killall mqtt-config
    elif [ "$NAME" == "mqtt" ]; then
        killall mqttv4
    elif [ "$NAME" == "mp4record" ]; then
        killall mp4record
    elif [ "$NAME" == "all" ]; then
        stop_rtsp
        stop_onvif
        stop_wsdd
        stop_ftpd
        killall mqtt-config
        killall mqttv4
        killall mp4record
    fi
elif [ "$ACTION" == "status" ] ; then
    if [ "$NAME" == "rtsp" ]; then
        RES=$(ps_program rRTSPServer)
    elif [ "$NAME" == "onvif" ]; then
        RES=$(ps_program onvif_notify_server)
    elif [ "$NAME" == "wsdd" ]; then
        RES=$(ps_program wsd_simple_server)
    elif [ "$NAME" == "ftpd" ]; then
        RES=$(ps_program ftpd)
    elif [ "$NAME" == "mqtt" ]; then
        RES=$(ps_program mqttv4)
    elif [ "$NAME" == "mqtt-config" ]; then
        RES=$(ps_program mqtt-config)
    elif [ "$NAME" == "mp4record" ]; then
        RES=$(ps_program mp4record)
    elif [ "$NAME" == "all" ]; then
        RES=$(ps_program rRTSPServer)
    fi
fi

if [ ! -z $RES ]; then
    echo $RES
fi