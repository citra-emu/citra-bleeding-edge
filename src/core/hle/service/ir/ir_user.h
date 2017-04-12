// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include "core/hle/service/service.h"

namespace Service {
namespace IR {

/// An interface representing a device that can communicate with 3DS via ir:USER service
class IRDevice {
public:
    using SendFunc = std::function<void(const std::vector<u8>& data)>;
    explicit IRDevice(SendFunc send_func);
    virtual ~IRDevice();

    /// Called when connected with 3DS
    virtual void Connect() = 0;

    /// Called when disconnected from 3DS
    virtual void Disconnect() = 0;

    /// Called by ir:USER send function and receive data from 3DS
    virtual void Receive(const std::vector<u8>& data) = 0;

protected:
    /// This functon is connected with ir:USER receive function and can be used to send data to 3DS
    const SendFunc send_func;
};

class IR_User_Interface : public Service::Interface {
public:
    IR_User_Interface();

    std::string GetPortName() const override {
        return "ir:USER";
    }
};

void InitUser();
void ShutdownUser();

/// Reload input devices. Used when input configuration changed
void ReloadInputDevices();

} // namespace IR
} // namespace Service
