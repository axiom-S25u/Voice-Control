#include "Android.hpp"

#include <Geode/Utils.hpp>
#include <utility>

namespace voicecontrol::android {
    void requestMicrophonePermission(geode::Function<void(bool)> callback) {
#if defined(VOICE_CONTROL_ANDROID)
        if (geode::utils::permission::getPermissionStatus(
            geode::utils::permission::Permission::RecordAudio
        )) {
            callback(true);
            return;
        }

        geode::utils::permission::requestPermission(
            geode::utils::permission::Permission::RecordAudio,
            std::move(callback)
        );
#else
        callback(true);
#endif
    }

    FMOD_RESULT configureFmod(FMOD::System* system) {
        (void)system;
        return FMOD_OK;
    }
}
