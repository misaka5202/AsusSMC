//
//  KeyImplementations.cpp
//  AsusSMC
//
//  Copyright © 2018-2020 Le Bao Hiep. All rights reserved.
//

#include "KeyImplementations.hpp"
#include "AsusSMC.hpp"

SMC_RESULT SMCALSValue::readAccess() {
    auto value = reinterpret_cast<Value *>(data);
    uint32_t lux = atomic_load_explicit(currentLux, memory_order_acquire);
    uint8_t bits = forceBits->bits();

    if (lux == 0xFFFFFFFF) {
        value->valid = false;
    } else {
        value->valid = true;
        if (!(bits & ALSForceBits::kALSForceHighGain))
            value->highGain = true;
        if (!(bits & ALSForceBits::kALSForceChan))
            value->chan0 = OSSwapHostToBigInt16(lux);
        if (!(bits & ALSForceBits::kALSForceLux))
            value->roomLux = OSSwapHostToBigInt32(lux << 14);
    }

    return SmcSuccess;
}

SMC_RESULT SMCKBrdBLightValue::update(const SMC_DATA *src)  {
    lkb *value = new lkb;
    lilu_os_memcpy(value, src, size);

    // tval is in range [0x0, 0xffb] on Macs, which ship 16 backlight levels.
    // This firmware has only ASUS_KBD_BACKLIGHT_MAX + 1 = 4 levels, so map
    // the full macOS range onto 0..3 instead of passing the raw value.
    uint16_t tval = (value->val1 << 4) | (value->val2 >> 4);
    uint16_t level = (uint16_t)((uint32_t)tval * ASUS_KBD_BACKLIGHT_MAX / 0xFFB);
    if (level > ASUS_KBD_BACKLIGHT_MAX) {
        level = ASUS_KBD_BACKLIGHT_MAX;
    }

    DBGLOG("lksb", "LKSB update %d -> level %d", tval, level);

    delete value;

    if (asusSMCInstance) {
        asusSMCInstance->message(kSetKeyboardBacklightMessage, nullptr, &level);
    }

    lilu_os_memcpy(data, src, size);
    return SmcSuccess;
}

SMC_RESULT F0Ac::readAccess() {
    uint16_t speed = atomic_load_explicit(currentSpeed, memory_order_acquire);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, speed);
    return SmcSuccess;
}

SMC_RESULT F1Ac::readAccess() {
    uint16_t speed = atomic_load_explicit(currentSpeed, memory_order_acquire);
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, speed);
    return SmcSuccess;
}

SMC_RESULT FConstAc::readAccess() {
    *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, speed);
    return SmcSuccess;
}

SMC_RESULT FMode::readAccess() {
    uint32_t mode = atomic_load_explicit(currentMode, memory_order_acquire);
    uint8_t *ptr = reinterpret_cast<uint8_t *>(data);
    ptr[0] = (uint8_t)(mode & 0xff);
    return SmcSuccess;
}

SMC_RESULT FThermalProfile::readAccess() {
    AsusSMC *drv = OSDynamicCast(AsusSMC, asusSMCInstance);
    if (drv == nullptr) {
        return SmcError;
    }

    reinterpret_cast<uint8_t *>(data)[0] = (uint8_t)(drv->getThermalProfile() & 0xff);
    return SmcSuccess;
}

SMC_RESULT FThermalProfile::update(const SMC_DATA *src) {
    uint8_t profile = reinterpret_cast<const uint8_t *>(src)[0];

    AsusSMC *drv = OSDynamicCast(AsusSMC, asusSMCInstance);
    if (drv == nullptr) {
        return SmcError;
    }

    // setThermalProfile rejects anything outside 0..2 before it reaches ACPI.
    // Returning SmcError makes the failed write visible to the caller rather
    // than silently pretending it worked.
    if (drv->setThermalProfile(profile) != 0) {
        return SmcError;
    }

    reinterpret_cast<uint8_t *>(data)[0] = profile;
    return SmcSuccess;
}

SMC_RESULT FMode::update(const SMC_DATA *src) {
    uint8_t mode = reinterpret_cast<const uint8_t *>(src)[0];

    AsusSMC *drv = OSDynamicCast(AsusSMC, asusSMCInstance);
    if (drv != nullptr) {
        // Returns non-zero for the modes we cannot honour (manual PWM is
        // not exposed by boards without the AGFN 0x00110012 device), in
        // which case we leave the reported mode untouched so the caller
        // can see the write did not take.
        if (drv->setFanMode(index, mode) == 0) {
            reinterpret_cast<uint8_t *>(data)[0] = mode;
            return SmcSuccess;
        }
        return SmcError;
    }

    return SmcError;
}

SMC_RESULT BDVT::update(const SMC_DATA *src)  {
    bool state = false;
    lilu_os_memcpy(&state, src, size);

    // BDVT is 00 when battery health is enabled and 01 when disabled
    state = !state;

    AsusSMC *drv = OSDynamicCast(AsusSMC, dst);
    drv->toggleBatteryConservativeMode(state);

    lilu_os_memcpy(data, src, size);
    return SmcSuccess;
}
