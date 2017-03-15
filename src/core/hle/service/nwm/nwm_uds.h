// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include "common/common_types.h"
#include "common/swap.h"
#include "core/hle/service/service.h"

// Local-WLAN service

namespace Service {
namespace NWM {

const size_t ApplicationDataSize = 0xC8;
const u8 DefaultNetworkChannel = 11;

struct NodeInfo {
    u64_le friend_code_seed;
    u16_le username[10];
    INSERT_PADDING_BYTES(4);
    u16_le network_node_id;
    INSERT_PADDING_BYTES(6);
};

static_assert(sizeof(NodeInfo) == 40, "NodeInfo has incorrect size.");

enum NetworkStatus {
    NotConnected = 3,
    ConnectedAsHost = 6,
    ConnectedAsClient = 9,
    ConnectedAsSpectator = 10,
};

struct ConnectionStatus {
    u32_le status;
    INSERT_PADDING_WORDS(1);
    u16_le network_node_id;
    INSERT_PADDING_BYTES(2);
    INSERT_PADDING_BYTES(32);
    u8 total_nodes;
    u8 max_nodes;
    u16_le node_bitmask;
};

static_assert(sizeof(ConnectionStatus) == 0x30, "ConnectionStatus has incorrect size.");

struct NetworkInfo {
    u8 host_mac_address[6];
    u8 channel;
    INSERT_PADDING_BYTES(1);
    u8 initialized;
    u8 oui_value[3];
    u8 oui_type;
    u32_le wlan_comm_id;
    INSERT_PADDING_BYTES(2);
    u16_le attributes;
    u32_le network_id;
    u8 total_nodes;
    u8 max_nodes;
    INSERT_PADDING_BYTES(2);
    INSERT_PADDING_BYTES(0x1F);
    u8 application_data_size;
    u8 application_data[ApplicationDataSize];
};

static_assert(sizeof(NetworkInfo) == 0x108, "NetworkInfo has incorrect size.");

class NWM_UDS final : public Interface {
public:
    NWM_UDS();
    ~NWM_UDS() override;

    std::string GetPortName() const override {
        return "nwm::UDS";
    }
};

} // namespace NWM
} // namespace Service
