#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include "Android.hpp"
#include "Ios.hpp"
#include <fmod.hpp>
#include <fmod_errors.h>
#if defined(VOICE_CONTROL_ANDROID)
#include <jni.h>
#include <Geode/cocos/platform/android/jni/JniHelper.h>
#endif
#include <thread>
#include <atomic>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>
#include <utility>

using namespace geode::prelude;

// hi
static std::atomic<bool>  g_running(false);
static std::atomic<bool>  g_threadReady(false);
static std::atomic<bool>  g_wasAbove(false); // shared so PlayLayer hooks can reset it
static std::atomic<int>   g_selectedDevice(-1);
static std::atomic<int>   g_activeDevice(0);
static std::atomic<bool>  g_restartRequested(false);
static std::atomic<float> g_currentPeak(0.0f);
static std::thread        g_micThread;

static FMOD::System* g_fmodSystem = nullptr;
static FMOD::Sound*  g_recordSound = nullptr;
static int           g_recordDevice = 0;
static bool          g_usingGameFmod = false;

static const int RECORD_LEN = 1024 * 2;

// ok uhm i saw a lot of tiktoks of someone making an external program for this so i made it for geode .-. lol
// axiom was here
// mic detection is literally just comparing db levels, if peak is above threshold = jump. thats it
// https://www.youtube.com/shorts/ZjJJFN9lXuY :O

static bool canClick() {
    PlayLayer* pl = PlayLayer::get();
    return pl && !pl->m_isPaused && Mod::get()->getSettingValue<bool>("enabled");
}

static float get_threshold() {
    return (float)Mod::get()->getSettingValue<double>("threshold-db");
}

struct MicInfo {
    int index;
    std::string name;
};

static bool is_speaker(const std::string& name) { // for the ppl who for some reason have 12 speakrers plugged in 
    std::string lower = name;
    for (char& c : lower) c = tolower(c);
    if (lower.find("speaker") != std::string::npos) return true;
    if (lower.find("headphone") != std::string::npos) return true;
    if (lower.find("headset") != std::string::npos) return true;
    if (lower.find("output") != std::string::npos) return true;
    if (lower.find("speakers") != std::string::npos) return true;
    if (lower.find("audio output") != std::string::npos) return true;
    if (lower.find("realtek audio") != std::string::npos) return true;
    return false;
}

static std::vector<MicInfo> list_mics() {
    std::vector<MicInfo> out;
    if (!g_fmodSystem) return out;
    int num = 0, conn = 0;
    g_fmodSystem->getRecordNumDrivers(&num, &conn);
    for (int i = 0; i < num; i++) {
        char name[256] = {0};
        FMOD_RESULT r = g_fmodSystem->getRecordDriverInfo(
            i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr, nullptr
        );
        if (r != FMOD_OK) continue;
        std::string sname(name);
        if (is_speaker(sname)) continue;
        out.push_back({ i, sname });
    }
    return out;
}

static int find_mic_with_audio() {
    // fmod is stupid so we gotta do this manually
    int numDevices = 0, numConnected = 0;
    g_fmodSystem->getRecordNumDrivers(&numDevices, &numConnected);

    for (int i = 0; i < numDevices; i++) {
        char name[256] = {0};
        g_fmodSystem->getRecordDriverInfo(i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (is_speaker(std::string(name))) continue;

        FMOD_CREATESOUNDEXINFO ex;
        memset(&ex, 0, sizeof(ex));
        ex.cbsize = sizeof(ex);
        ex.numchannels = 1;
        ex.format = FMOD_SOUND_FORMAT_PCM16;
        ex.defaultfrequency = 44100;
        ex.length = RECORD_LEN * sizeof(short);

        FMOD::Sound* test = nullptr;
        if (g_fmodSystem->createSound(nullptr, FMOD_CREATESAMPLE | FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &ex, &test) != FMOD_OK)
            continue;
        if (g_fmodSystem->recordStart(i, test, true) != FMOD_OK) {
            test->release();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        unsigned int curr = 0;
        g_fmodSystem->getRecordPosition(i, &curr);
        g_fmodSystem->recordStop(i);
        test->release();

        if (curr > 0) return i;
    }
    return 0;// fallbackkkkkkkkkkkkkkkkkkk
}

static void mic_thread_func() {
#if defined(VOICE_CONTROL_ANDROID)
    JNIEnv* env = nullptr;
    JavaVM* vm = cocos2d::JniHelper::getJavaVM();
    bool attached = false;
    if (vm && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
    }

    FMODAudioEngine* audioEngine = FMODAudioEngine::get();
    g_fmodSystem = audioEngine ? audioEngine->m_system : nullptr;
    g_usingGameFmod = g_fmodSystem != nullptr;
    FMOD_RESULT result = FMOD_OK;
#else
    FMOD_RESULT result = FMOD::System_Create(&g_fmodSystem);
#endif
    if (!g_fmodSystem) {
        log::error("FMOD failed to create: {}", FMOD_ErrorString(result));
#if defined(VOICE_CONTROL_ANDROID)
        if (attached && vm) vm->DetachCurrentThread();
#endif
        g_running.store(false);
        return;
    }

#if !defined(VOICE_CONTROL_ANDROID) && defined(VOICE_CONTROL_IOS)
    result = voicecontrol::ios::configureFmod(g_fmodSystem);
#endif
    if (result != FMOD_OK) {
        log::error("FMOD failed to configure mobile audio: {}", FMOD_ErrorString(result));
        if (!g_usingGameFmod) g_fmodSystem->release();
        g_fmodSystem = nullptr;
#if defined(VOICE_CONTROL_ANDROID)
        if (attached && vm) vm->DetachCurrentThread();
#endif
        g_running.store(false);
        return;
    }

#if !defined(VOICE_CONTROL_ANDROID)
    result = g_fmodSystem->init(1, FMOD_INIT_NORMAL, nullptr);
    if (result != FMOD_OK) {
        log::error("FMOD failed to initialize: {}", FMOD_ErrorString(result));
        if (!g_usingGameFmod) g_fmodSystem->release();
        g_fmodSystem = nullptr;
        g_running.store(false);
        return;
    }
#endif

    int numDevices = 0;
    int numConnected = 0;
    result = g_fmodSystem->getRecordNumDrivers(&numDevices, &numConnected);
    if (result != FMOD_OK || numDevices == 0) {
        log::error("FMOD found no recording devices: {}", FMOD_ErrorString(result));
        if (!g_usingGameFmod) g_fmodSystem->release();
        g_fmodSystem = nullptr;
#if defined(VOICE_CONTROL_ANDROID)
        if (attached && vm) vm->DetachCurrentThread();
#endif
        g_running.store(false);
        return;
    }

    int saved = Mod::get()->getSavedValue<int>("selected-device", -1);
    g_selectedDevice.store(saved);

    if (saved >= 0) {
        g_recordDevice = saved;
    } else {
        g_recordDevice = find_mic_with_audio();
    }

    FMOD_CREATESOUNDEXINFO exparams;
    memset(&exparams, 0, sizeof(FMOD_CREATESOUNDEXINFO));
    exparams.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
    exparams.numchannels = 1; // mono only, stereo was overkill
    exparams.format = FMOD_SOUND_FORMAT_PCM16;
    exparams.defaultfrequency = 44100; // tried 48k, didnt matter
    exparams.length = RECORD_LEN * sizeof(short);

    result = g_fmodSystem->createSound(nullptr, FMOD_CREATESAMPLE | FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exparams, &g_recordSound);
    if (result != FMOD_OK || !g_recordSound) {
        log::error("FMOD failed to create the microphone buffer: {}", FMOD_ErrorString(result));
        if (!g_usingGameFmod) g_fmodSystem->release();
        g_fmodSystem = nullptr;
#if defined(VOICE_CONTROL_ANDROID)
        if (attached && vm) vm->DetachCurrentThread();
#endif
        g_running.store(false);
        return;
    }

    result = g_fmodSystem->recordStart(g_recordDevice, g_recordSound, true);
    if (result != FMOD_OK) {
        log::error("FMOD failed to start recording: {}", FMOD_ErrorString(result));
        g_recordSound->release();
        g_recordSound = nullptr;
        if (!g_usingGameFmod) g_fmodSystem->release();
        g_fmodSystem = nullptr;
#if defined(VOICE_CONTROL_ANDROID)
        if (attached && vm) vm->DetachCurrentThread();
#endif
        g_running.store(false);
        return;
    }
    g_activeDevice.store(g_recordDevice);

    g_threadReady.store(true);

    unsigned int pos = 0;
    int release_counter = 0;
    const int RELEASE_FRAMES = 1;

    while (g_running.load()) {
#if !defined(VOICE_CONTROL_ANDROID)
        g_fmodSystem->update();
#endif

        if (g_restartRequested.exchange(false)) {
            g_fmodSystem->recordStop(g_recordDevice);
            if (g_recordSound) {
                g_recordSound->release();
                g_recordSound = nullptr;
            }

            int newdev = g_selectedDevice.load();
            if (newdev < 0) newdev = find_mic_with_audio();
            g_recordDevice = newdev;

            memset(&exparams, 0, sizeof(FMOD_CREATESOUNDEXINFO));
            exparams.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
            exparams.numchannels = 1;
            exparams.format = FMOD_SOUND_FORMAT_PCM16;
            exparams.defaultfrequency = 44100;
            exparams.length = RECORD_LEN * sizeof(short);

            result = g_fmodSystem->createSound(nullptr, FMOD_CREATESAMPLE | FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exparams, &g_recordSound);
            if (result != FMOD_OK || !g_recordSound) {
                log::error("FMOD failed to recreate the microphone buffer: {}", FMOD_ErrorString(result));
                continue;
            }

            result = g_fmodSystem->recordStart(g_recordDevice, g_recordSound, true);
            if (result != FMOD_OK) {
                log::error("FMOD failed to restart recording: {}", FMOD_ErrorString(result));
                g_recordSound->release();
                g_recordSound = nullptr;
                continue;
            }
            g_activeDevice.store(g_recordDevice);
            pos = 0;
            g_currentPeak.store(0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        unsigned int curr;
        g_fmodSystem->getRecordPosition(g_recordDevice, &curr);

        if (curr == pos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int samples = (static_cast<int>(curr) - static_cast<int>(pos) + RECORD_LEN) % RECORD_LEN;
        bool was_above = g_wasAbove.load();

        if (samples > 0) {
            void* p1 = nullptr; void* p2 = nullptr;
            unsigned int l1 = 0; unsigned int l2 = 0;

            g_recordSound->lock(pos * sizeof(short), samples * sizeof(short), &p1, &p2, &l1, &l2);

            float peak = 0.0f;
            float sumsq = 0.0f;
            int cnt = 0;

            auto process = [&](void* p, unsigned int l) {
                if (!p) return;
                short* buf = (short*)p;
                int n = l / sizeof(short);
                for (int i = 0; i < n; i++) {
                    float s = buf[i] / 32768.0f;
                    float abss = s < 0.0f ? -s : s;
                    if (abss > peak) peak = abss;
                    sumsq += s * s;
                    cnt++;
                }
            };
            process(p1, l1);
            process(p2, l2);

            g_recordSound->unlock(p1, p2, l1, l2);

            float peak_db = (peak <= 0.0f) ? -100.0f : 20.0f * log10f(peak);
            float rms     = (cnt > 0) ? sqrtf(sumsq / (float)cnt) : 0.0f;
            float rms_db  = (rms <= 0.0f) ? -100.0f : 20.0f * log10f(rms);

            float prev = g_currentPeak.load();
            float disp = (peak > prev) ? peak : (prev * 0.85f + peak * 0.15f);
            g_currentPeak.store(disp);

            float thresh         = get_threshold();
            float release_thresh = thresh - 12.0f;

            bool peak_above = peak_db >= thresh;
            bool rms_above  = rms_db  >= release_thresh;

            if (peak_above && !was_above) {
                g_wasAbove.store(true);
                release_counter = 0;
                Loader::get()->queueInMainThread([]() {
                    if (canClick()) {
                        PlayLayer::get()->handleButton(true, (int)PlayerButton::Jump, true);
                    } else {
                        g_wasAbove.store(false);
                    }
                });
            // still loud, keep holding
            } else if (was_above && !peak_above && !rms_above) {
                release_counter++;
                if (release_counter >= RELEASE_FRAMES) {
                    g_wasAbove.store(false);
                    release_counter = 0;
                    Loader::get()->queueInMainThread([]() {
                        PlayLayer* pl = PlayLayer::get();
                        if (pl && !pl->m_isPaused && pl->m_player1)
                            pl->handleButton(false, (int)PlayerButton::Jump, true);
                    });
                }

            } else if (peak_above && was_above) {
                release_counter = 0;
            }
        }

        pos = curr;
        // -sleep so were not burning cpu. 2ms seems like a good balance, talking from experience-, nvm going to 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    g_fmodSystem->recordStop(g_recordDevice);
    if (g_recordSound) {
        g_recordSound->release();
        g_recordSound = nullptr;
    }
    if (!g_usingGameFmod) g_fmodSystem->release();
    g_fmodSystem = nullptr;
#if defined(VOICE_CONTROL_ANDROID)
    if (attached && vm) vm->DetachCurrentThread();
#endif
}

static void start_mic() {
    if (g_running.load()) return;
    g_running.store(true);
    g_threadReady.store(false);
    g_micThread = std::thread(mic_thread_func);
}

static void request_mic_permission() {
    geode::Function<void(bool)> callback = [](bool granted) {
        if (granted) {
            start_mic();
        } else {
            log::warn("microphone permission got denied, so voice control is taking a nap");
        }
    };

#if defined(VOICE_CONTROL_ANDROID)
    voicecontrol::android::requestMicrophonePermission(std::move(callback));
#elif defined(VOICE_CONTROL_IOS)
    voicecontrol::ios::requestMicrophonePermission(std::move(callback));
#else
    callback(true);
#endif
}
// reason is people cant fucking set their default mic to wtw they are using and etc
class MicSelectPopup : public Popup {
public:
    std::vector<MicInfo> m_mics;
    int m_selectedIdx = -1;
    float m_blinkPhase = 0.f;
    std::vector<std::pair<int, CCLayerColor*>> m_dots;
    std::vector<std::pair<int, CCLayerColor*>> m_bars;
    std::vector<std::pair<int, CCMenuItemSpriteExtra*>> m_buttons;

    bool init(float w, float h) {
        if (!Popup::init(w, h)) return false;

        auto winSize = m_mainLayer->getContentSize();

        auto title = CCLabelBMFont::create("Select Microphone", "bigFont.fnt");
        title->setScale(0.6f);
        title->setPosition(winSize.width / 2.f, winSize.height - 18.f);
        m_mainLayer->addChild(title);

        m_mics = list_mics();
        m_selectedIdx = g_selectedDevice.load();

        float cw = winSize.width - 30.f;
        float ch = winSize.height - 60.f;

        std::vector<std::pair<int, std::string>> rows;
        rows.push_back({ -1, "Auto detect" });
        for (auto& m : m_mics) rows.push_back({ m.index, m.name });

        const float rowHeight = 28.f;
        const float topPad = 4.f;
        float fullHeight = topPad + rowHeight * (float)rows.size();
        if (fullHeight < ch) fullHeight = ch;

        auto scroll = ScrollLayer::create({ cw, ch });
        scroll->setPosition(winSize.width / 2 - cw / 2, winSize.height / 2 - ch / 2 - 8.f);
        scroll->setStealingTouches(false);
        scroll->m_contentLayer->setContentSize({ cw, fullHeight });
        m_mainLayer->addChild(scroll);

        auto container = CCNode::create();
        container->setContentSize({ cw, fullHeight });
        container->setAnchorPoint({ 0.f, 0.f });
        container->setPosition(0.f, 0.f);
        scroll->m_contentLayer->addChild(container);
        scroll->scrollToTop();

        auto buttonsMenu = CCMenu::create();
        buttonsMenu->setPosition(0, 0);
        buttonsMenu->ignoreAnchorPointForPosition(true);
        buttonsMenu->setContentSize(container->getContentSize());
        container->addChild(buttonsMenu);

        for (size_t i = 0; i < rows.size(); i++) {
            int idx = rows[i].first;
            const std::string& name = rows[i].second;
            float yTop = fullHeight - topPad - rowHeight * (float)i;
            float yMid = yTop - rowHeight / 2.f;

            auto rowBg = CCLayerColor::create({ 0, 0, 0, 70 }, cw, rowHeight - 2.f);
            rowBg->setPosition(0.f, yTop - rowHeight + 1.f);
            container->addChild(rowBg);

            auto dot = CCLayerColor::create({ 0, 255, 0, 60 }, 12.f, 12.f);
            dot->setPosition(6.f, yMid - 6.f);
            container->addChild(dot);
            m_dots.push_back({ idx, dot });

            auto barBg = CCLayerColor::create({ 30, 30, 30, 120 }, 80.f, 6.f);
            barBg->setPosition(cw - 140.f, yMid - 3.f);
            container->addChild(barBg);

            auto bar = CCLayerColor::create({ 0, 220, 60, 220 }, 1.f, 6.f);
            bar->setPosition(cw - 140.f, yMid - 3.f);
            container->addChild(bar);
            m_bars.push_back({ idx, bar });

            auto lbl = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
            lbl->setAnchorPoint({ 0.f, 0.5f });
            lbl->setScale(0.45f);
            lbl->setPosition(24.f, yMid);
            lbl->limitLabelWidth(cw - 240.f, 0.45f, 0.2f);
            container->addChild(lbl);

            auto bs = ButtonSprite::create(
                (m_selectedIdx == idx) ? "Selected" : "Select",
                "bigFont.fnt", "GJ_button_05.png", 0.6f
            );
            bs->setScale(0.5f);
            auto btn = CCMenuItemSpriteExtra::create(bs, this, menu_selector(MicSelectPopup::onSelect));
            btn->setPosition(cw - 36.f, yMid);
            btn->setUserData((void*)(intptr_t)idx);
            buttonsMenu->addChild(btn);
            m_buttons.push_back({ idx, btn });
        }

        this->schedule(schedule_selector(MicSelectPopup::tick), 0.04f);
        return true;
    }

    void onSelect(CCObject* sender) {
        auto node = static_cast<CCNode*>(sender);
        int idx = (int)(intptr_t)node->getUserData();
        g_selectedDevice.store(idx);
        Mod::get()->setSavedValue<int>("selected-device", idx);
        g_restartRequested.store(true);
        m_selectedIdx = idx;

        for (auto& kv : m_buttons) {
            auto bs = static_cast<ButtonSprite*>(kv.second->getNormalImage());
            if (bs) bs->setString((kv.first == idx) ? "Selected" : "Select");
        }
    }
// here undefined0 or whoever is reviewing
    void tick(float dt) {
        int active = g_activeDevice.load();
        int sel    = g_selectedDevice.load();
        float peak = g_currentPeak.load();

        m_blinkPhase += dt;
        bool blinkOn  = (fmodf(m_blinkPhase, 0.6f) < 0.3f);
        bool hasAudio = peak > 0.02f;

        for (auto& kv : m_dots) {
            int idx = kv.first;
            auto dot = kv.second;
            bool isActive = (idx == active) || (idx == -1 && sel == -1);

            GLubyte op = 50;
            if (isActive) {
                if (hasAudio) op = blinkOn ? 255 : 110;
                else          op = 90;
            }
            dot->setOpacity(op);
        }

        for (auto& kv : m_bars) {
            int idx = kv.first;
            auto bar = kv.second;
            bool isActive = (idx == active) || (idx == -1 && sel == -1);
            float w = isActive ? std::min(1.0f, peak * 2.0f) * 80.f : 0.f;
            if (w < 1.f) w = 1.f;
            bar->setContentSize({ w, 6.f });
        }
    }

public:
    static MicSelectPopup* create() {
        auto ret = new MicSelectPopup();
        if (ret->init(380.f, 260.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};
// simplest ui but we ball
class $modify(PlayLayer) {
    void resetLevel() {
        g_wasAbove.store(false);
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        g_wasAbove.store(false);
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(VCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = getChildByID("right-button-menu");
        if (!menu) menu = getChildByID("left-button-menu");
        if (!menu) return;

        CCSprite* spr = CCSprite::create("logo.png"_spr);
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        spr->setScale(0.45f);

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(VCPauseLayer::onMicSelect));
        btn->setID("voicecontrol-mic-btn"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }

    void onMicSelect(CCObject*) {
        if (auto popup = MicSelectPopup::create()) popup->show();
    }
};

$execute {
    request_mic_permission();
}
