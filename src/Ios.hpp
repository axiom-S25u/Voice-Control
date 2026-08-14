#pragma once

#include <Geode/utils/function.hpp>
#include <fmod.hpp>

namespace voicecontrol::ios {
    void requestMicrophonePermission(geode::Function<void(bool)> callback);
    FMOD_RESULT configureFmod(FMOD::System* system);
}
