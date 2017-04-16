// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include "core/frontend/input.h"
#include "core/hle/service/ir/ir_user.h"

namespace Service {
namespace IR {

/// An IRDevice emulating Circle Pad Pro or New 3DS additional HID hardware.
class ExtraHID final : public IRDevice {
public:
    explicit ExtraHID(SendFunc send_func);
    ~ExtraHID();

    void Connect() override;
    void Disconnect() override;
    void Receive(const std::vector<u8>& data) override;
    void ReloadInputDevices();

private:
    void SendHIDStatus(int cycles_late);
    void HandleReadHIDStatusRequest(const std::vector<u8>& data);
    void HandleReadCalibrationDataRequest(const std::vector<u8>& data);
    void LoadInputDevices();

    u8 hid_period;
    int send_callback;
    std::array<u8, 0x40> calibration_data;
    std::unique_ptr<Input::ButtonDevice> zl;
    std::unique_ptr<Input::ButtonDevice> zr;
    std::unique_ptr<Input::AnalogDevice> c_stick;
    std::atomic<bool> is_device_reload_pending;
};

} // namespace IR
} // namespace Service
