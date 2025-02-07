/*
 * Copyright (C) 2018-2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LightsService"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <fstream>

#include "Light.h"

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

/*
 * Write value to path and close file.
 */
template <typename T>
static void set(const std::string& path, const T& value) {
    std::ofstream file(path);
    file << value;
}

template <typename T>
static T get(const std::string& path, const T& def) {
    std::ifstream file(path);
    T result;

    file >> result;
    return file.fail() ? def : result;
}

static int rgbToBrightness(const LightState& state) {
    int color = state.color & 0x00ffffff;

    return ((77 * ((color >> 16) & 0x00ff))
            + (150 * ((color >> 8) & 0x00ff))
            + (29 * (color & 0x00ff))) >> 8;
}

Light::Light() {
    mLights.emplace(Type::ATTENTION, std::bind(&Light::setRgbLight, this, std::placeholders::_1, 0));
    mLights.emplace(Type::BACKLIGHT, std::bind(&Light::setBacklight, this, std::placeholders::_1));
    mLights.emplace(Type::BATTERY, std::bind(&Light::setRgbLight, this, std::placeholders::_1, 2));
    mLights.emplace(Type::BUTTONS, std::bind(&Light::setButtonBacklight, this, std::placeholders::_1));
    mLights.emplace(Type::NOTIFICATIONS, std::bind(&Light::setRgbLight, this, std::placeholders::_1, 1));
}

void Light::setBacklight(const LightState& state) {
    int maxBrightness = get("/sys/class/leds/lcd-backlight/max_brightness", -1);
    if (maxBrightness < 0) {
        maxBrightness = 255;
    }
    int newBrightness = rgbToBrightness(state);

    // If max panel brightness exceeds 255, apply proper linear scaling
    if (newBrightness > 0 && maxBrightness != 255) {
        int originalBrightness = newBrightness;
        newBrightness = (((maxBrightness - 1) * (newBrightness - 1)) / (254)) + 1;
        LOG(DEBUG) << "Scaling backlight brightness from " << originalBrightness << " => " << newBrightness;
    }

    set("/sys/class/leds/lcd-backlight/brightness", newBrightness);
}

void Light::setButtonBacklight(const LightState& state) {
    set("/sys/class/leds/button-backlight/brightness", rgbToBrightness(state));
}

void Light::setRgbLight(const LightState& state, size_t index) {
    mLightStates.at(index) = state;

    LightState stateToUse = mLightStates.front();
    for (const auto& lightState : mLightStates) {
        if (lightState.color & 0xffffff) {
            stateToUse = lightState;
            break;
        }
    }

    std::map<std::string, int> colorValues;
    colorValues["red"] = (stateToUse.color >> 16) & 0xff;
    colorValues["green"] = ((stateToUse.color >> 8) & 0xff);
    colorValues["blue"] = (stateToUse.color & 0xff);

    int onMs = stateToUse.flashMode == Flash::TIMED ? stateToUse.flashOnMs : 0;
    int offMs = stateToUse.flashMode == Flash::TIMED ? stateToUse.flashOffMs : 0;

    // LUT has 63 entries, we could theoretically use them as 3 (colors) * 21 (steps).
    // However, the last LUT entries don't seem to behave correctly for unknown
    // reasons, so we use 17 steps for a total of 51 LUT entries only.
    static constexpr int kRampSteps = 16;
    static constexpr int kRampMaxStepDurationMs = 15;

    auto makeLedPath = [](const std::string& led, const std::string& op) -> std::string {
        return "/sys/class/leds/" + led + "/" + op;
    };
    auto getScaledDutyPercent = [](int brightness) -> std::string {
        std::string output;
        for (int i = 0; i <= kRampSteps; i++) {
            if (i != 0) {
                output += ",";
            }
            output += std::to_string(i * 512 * brightness / (255 * kRampSteps));
        }
        return output;
    };

    // Disable all blinking before starting
    for (const auto& entry : colorValues) {
        set(makeLedPath(entry.first, "blink"), 0);
    }

    if (onMs > 0 && offMs > 0) {
        int pauseLo, pauseHi, stepDuration, index = 0;
        if (kRampMaxStepDurationMs * kRampSteps > onMs) {
            stepDuration = onMs / kRampSteps;
            pauseHi = 0;
            pauseLo = offMs;
        } else {
            stepDuration = kRampMaxStepDurationMs;
            pauseHi = onMs - kRampSteps * stepDuration;
            pauseLo = offMs - kRampSteps * stepDuration;
        }

        for (const auto& entry : colorValues) {
            set(makeLedPath(entry.first, "lut_flags"), 95);
            set(makeLedPath(entry.first, "start_idx"), index);
            set(makeLedPath(entry.first, "duty_pcts"), getScaledDutyPercent(entry.second));
            set(makeLedPath(entry.first, "pause_lo"), pauseLo);
            set(makeLedPath(entry.first, "pause_hi"), pauseHi);
            set(makeLedPath(entry.first, "ramp_step_ms"), stepDuration);
            index += kRampSteps + 1;
        }

        // Start blinking
        for (const auto& entry : colorValues) {
            set(makeLedPath(entry.first, "blink"), entry.second);
        }
    } else {
        for (const auto& entry : colorValues) {
            set(makeLedPath(entry.first, "brightness"), entry.second);
        }
    }

    LOG(DEBUG) << base::StringPrintf(
        "setRgbLight: mode=%d, color=%08X, onMs=%d, offMs=%d",
        static_cast<std::underlying_type<Flash>::type>(stateToUse.flashMode), stateToUse.color,
        onMs, offMs);
}

// Methods from ::android::hardware::light::V2_0::ILight follow.
Return<Status> Light::setLight(Type type, const LightState& state) {
    auto it = mLights.find(type);

    if (it == mLights.end()) {
        return Status::LIGHT_NOT_SUPPORTED;
    }

    // Lock global mutex until light state is updated.
    std::lock_guard<std::mutex> lock(mLock);

    it->second(state);

    return Status::SUCCESS;
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;

    for (auto const& light : mLights) {
        types.push_back(light.first);
    }

    _hidl_cb(types);

    return Void();
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
