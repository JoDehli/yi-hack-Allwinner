var APP = APP || {};

APP.camera_settings = (function($) {

    function init() {
        registerEventHandler();
        fetchConfigs();
    }

    function registerEventHandler() {
        $(document).on("click", '#button-save', function(e) {
            saveConfigs();
        });
        $(document).on("change", '#MOTION_DETECTION', function(e) {
            motionDetection('#MOTION_DETECTION');
        });
        $(document).on("change", '#AI_HUMAN_DETECTION', function(e) {
            aiMotionDetections('#AI_HUMAN_DETECTION');
        });
        $(document).on("change", '#AI_VEHICLE_DETECTION', function(e) {
            aiMotionDetections('#AI_VEHICLE_DETECTION');
        });
        $(document).on("change", '#AI_ANIMAL_DETECTION', function(e) {
            aiMotionDetections('#AI_ANIMAL_DETECTION');
        });
    }

    function fetchConfigs() {
        loadingStatusElem = $('#loading-status');
        loadingStatusElem.text("Loading...");

        $.ajax({
            type: "GET",
            url: 'cgi-bin/get_configs.sh?conf=camera',
            dataType: "json",
            success: function(response) {
                loadingStatusElem.fadeOut(500);

                $.each(response, function(key, state) {
                    if(key=="MOTION_DETECTION" || key=="SENSITIVITY" || key=="SOUND_SENSITIVITY" || key=="CRUISE")
                        $('select[data-key="' + key + '"]').prop('value', state);
                    else
                        $('input[type="checkbox"][data-key="' + key + '"]').prop('checked', state === 'yes');
                });
                if (response["HOMEVER"].startsWith("12")) {
                    var objects = document.querySelectorAll(".fw12");
                    for (var i = 0; i < objects.length; i++) {
                        objects[i].style.display = "table-row";
                    }
                } else {
                    var objects = document.querySelectorAll(".no_fw12");
                    for (var i = 0; i < objects.length; i++) {
                        objects[i].style.display = "table-row";
                    }
                }
                aiMotionDetections();
            },
            error: function(response) {
                console.log('error', response);
            }
        });
    }

    function motionDetection(el) {
        if ($(el).prop('checked')) {
            $("#AI_HUMAN_DETECTION").prop('checked', false)
            $("#AI_VEHICLE_DETECTION").prop('checked', false)
            $("#AI_ANIMAL_DETECTION").prop('checked', false)
        }
    }

    function aiMotionDetections(el) {
        if ($(el).prop('checked')) {
            $("#MOTION_DETECTION").prop('checked', false)
        }
    }

    function saveConfigs() {
        var saveStatusElem;
        let configs = {};

        saveStatusElem = $('#save-status');

        saveStatusElem.text("Saving...");

        $('.configs-switch input[type="checkbox"]').each(function() {
            configs[$(this).attr('data-key')] = $(this).prop('checked') ? 'yes' : 'no';
        });

        configs["SENSITIVITY"] = $('select[data-key="SENSITIVITY"]').prop('value');
        configs["SOUND_SENSITIVITY"] = $('select[data-key="SOUND_SENSITIVITY"]').prop('value');
        configs["CRUISE"] = $('select[data-key="CRUISE"]').prop('value');

        $.ajax({
            type: "GET",
            url: 'cgi-bin/camera_settings.sh?' +
                'save_video_on_motion=' + configs["SAVE_VIDEO_ON_MOTION"] +
                '&motion_detection=' + configs["MOTION_DETECTION"] +
                '&sensitivity=' + configs["SENSITIVITY"] +
                '&ai_human_detection=' + configs["AI_HUMAN_DETECTION"] +
                '&ai_vehicle_detection=' + configs["AI_VEHICLE_DETECTION"] +
                '&ai_animal_detection=' + configs["AI_ANIMAL_DETECTION"] +
                '&sound_detection=' + configs["SOUND_DETECTION"] +
                '&sound_sensitivity=' + configs["SOUND_SENSITIVITY"] +
                '&led=' + configs["LED"] +
                '&ir=' + configs["IR"] +
                '&rotate=' + configs["ROTATE"] +
                '&switch_on=' + configs["SWITCH_ON"] +
                '&cruise=' + configs["CRUISE"],
            dataType: "json",
            success: function(response) {
                saveStatusElem.text("Saved");
            },
            error: function(response) {
                saveStatusElem.text("Error while saving");
                console.log('error', response);
            }
        });
    }

    return {
        init: init
    };

})(jQuery);
