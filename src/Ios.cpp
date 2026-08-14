#include "Ios.hpp"

#include <Geode/Utils.hpp>
#include <utility>

namespace voicecontrol::ios {
    void requestMicrophonePermission(geode::Function<void(bool)> callback) {
#if defined(VOICE_CONTROL_IOS)
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

    FMOD_RESULT configureFmod(FMOD::System*) {
        return FMOD_OK;
    }
}
