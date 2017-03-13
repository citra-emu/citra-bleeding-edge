// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/hle/result.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/service/nwm/nwm_uds.h"
#include "core/memory.h"

namespace Service {
namespace NWM {

// Event that is signaled every time the connection status changes.
static Kernel::SharedPtr<Kernel::Event> connection_status_event;

// Shared memory provided by the application to store the receive buffer.
// This is not currently used.
static Kernel::SharedPtr<Kernel::SharedMemory> recv_buffer_memory;

// Connection status of this 3DS.
static ConnectionStatus connection_status{};

// Node information about the current 3DS.
// TODO(Subv): Keep an array of all nodes connected to the network,
// that data has to be retransmitted in every beacon frame.
static NodeInfo node_info;

// Mapping of bind node ids to their respective events.
static std::unordered_map<u32, Kernel::SharedPtr<Kernel::Event>> bind_node_events;

// The wifi network channel that the network is currently on.
// Since we're not actually interacting with physical radio waves, this is just a dummy value.
static u8 network_channel = DefaultNetworkChannel;

// The identifier of the network kind, this is used to filter away networks that we're not interested in.
static u32 wlan_comm_id = 0;

// Application data that is sent when broadcasting the beacon frames.
static std::vector<u8> application_data;

/**
 * NWM_UDS::Shutdown service function
 *  Inputs:
 *      1 : None
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void Shutdown(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    // TODO(purpasmart): Verify return header on HW

    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_WARNING(Service_NWM, "(STUBBED) called");
}

/**
 * NWM_UDS::RecvBeaconBroadcastData service function
 *  Inputs:
 *      1 : Output buffer max size
 *      2 : Unknown
 *      3 : Unknown
 *      4 : MAC address?
 *   6-14 : Unknown, usually zero / uninitialized?
 *     15 : WLan Comm ID
 *     16 : This is the ID also located at offset 0xE in the CTR-generation structure.
 *     17 : Value 0
 *     18 : Input handle
 *     19 : (Size<<4) | 12
 *     20 : Output buffer ptr
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void RecvBeaconBroadcastData(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 out_buffer_size = cmd_buff[1];
    u32 unk1 = cmd_buff[2];
    u32 unk2 = cmd_buff[3];
    u32 mac_address = cmd_buff[4];

    u32 unk3 = cmd_buff[6];

    u32 wlan_comm_id = cmd_buff[15];
    u32 ctr_gen_id = cmd_buff[16];
    u32 value = cmd_buff[17];
    u32 input_handle = cmd_buff[18];
    u32 new_buffer_size = cmd_buff[19];
    u32 out_buffer_ptr = cmd_buff[20];

    cmd_buff[1] = RESULT_SUCCESS.raw;

    LOG_WARNING(Service_NWM,
                "(STUBBED) called out_buffer_size=0x%08X, unk1=0x%08X, unk2=0x%08X,"
                "mac_address=0x%08X, unk3=0x%08X, wlan_comm_id=0x%08X, ctr_gen_id=0x%08X,"
                "value=%u, input_handle=0x%08X, new_buffer_size=0x%08X, out_buffer_ptr=0x%08X",
                out_buffer_size, unk1, unk2, mac_address, unk3, wlan_comm_id, ctr_gen_id, value,
                input_handle, new_buffer_size, out_buffer_ptr);
}

/**
 * NWM_UDS::Initialize service function
 *  Inputs:
 *      1 : Shared memory size
 *   2-11 : Input NodeInfo Structure
 *     12 : Version
 *     13 : Value 0
 *     14 : Shared memory handle
 *  Outputs:
 *      0 : Return header
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : Value 0
 *      3 : Output event handle
 */
static void InitializeWithVersion(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();
    u32 sharedmem_size = cmd_buff[1];

    // Update the node information with the data the game gave us.
    memcpy(&node_info, &cmd_buff[2], sizeof(NodeInfo));

    u32 version = cmd_buff[12] & 0xFFFF;
    u32 sharedmem_handle = cmd_buff[14];

    recv_buffer_memory = Kernel::g_handle_table.Get<Kernel::SharedMemory>(sharedmem_handle);

    // Reset the connection status, it contains all zeros after initialization,
    // except for the actual status value.
    connection_status = {};
    connection_status.status = NotConnected;

    cmd_buff[0] = IPC::MakeHeader(0x1B, 1, 2);
    cmd_buff[1] = RESULT_SUCCESS.raw;
    cmd_buff[2] = 0;
    cmd_buff[3] = Kernel::g_handle_table.Create(connection_status_event).MoveFrom();

    LOG_DEBUG(Service_NWM, "called sharedmem_size=0x%08X, version=0x%08X, value=%u, handle=0x%08X",
                sharedmem_size, version, sharedmem_handle);
}

static void GetConnectionStatus(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    cmd_buff[1] = RESULT_SUCCESS.raw;
    memcpy(&cmd_buff[2], &connection_status, sizeof(ConnectionStatus));

    LOG_DEBUG(Service_NWM, "called");
}

static void Bind(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 bind_node_id = cmd_buff[1];
    u32 recv_buffer_size = cmd_buff[2];
    u8 data_channel = cmd_buff[3] & 0xFF;
    u16 network_node_id = cmd_buff[4] & 0xFFFF;

    // TODO(Subv): Store the data channel and verify it when receiving data frames.

    LOG_DEBUG(Service_NWM, "called");

    if (data_channel == 0) {
        cmd_buff[1] = ResultCode(ErrorDescription::NotAuthorized, ErrorModule::UDS,
                                 ErrorSummary::WrongArgument, ErrorLevel::Usage).raw;
        return;
    }

    // Create a new event for this bind node.
    // TODO(Subv): Signal this event when new data is available for this bind node.
    auto event = Kernel::Event::Create(Kernel::ResetType::OneShot,
                                       "NWM::BindNodeEvent" + std::to_string(bind_node_id));
    bind_node_events[bind_node_id] = event;

    cmd_buff[1] = RESULT_SUCCESS.raw;
    cmd_buff[2] = 0;
    cmd_buff[3] = Kernel::g_handle_table.Create(event).MoveFrom();
}

static void BeginHostingNetwork(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 passphrase_size = cmd_buff[1];
    VAddr network_info_address = cmd_buff[3];
    VAddr passphrase_address = cmd_buff[5];

    // TODO(Subv): Store the passphrase and verify it when attempting a connection.

    LOG_DEBUG(Service_NWM, "called");

    NetworkInfo network_info;
    Memory::ReadBlock(network_info_address, &network_info, sizeof(NetworkInfo));

    connection_status.status = ConnectedAsHost;
    connection_status.max_nodes = network_info.max_nodes;
    wlan_comm_id = network_info.wlan_comm_id;

    // There's currently only one node in the network (the host).
    connection_status.total_nodes = 1;
    // The host is always the first node
    connection_status.network_node_id = 1;
    node_info.network_node_id = 1;
    // Set the bit 0 in the nodes bitmask to indicate that node 1 is already taken.
    connection_status.node_bitmask |= 1;

    // If the game has a preferred channel, use that instead.
    if (network_info.channel != 0)
        network_channel = network_info.channel;

    // Clear the pre-existing application data.
    application_data.clear();

    connection_status_event->Signal();

    // TODO(Subv): Start broadcasting the network, send a beacon frame every 102.4ms.

    LOG_WARNING(Service_NWM, "An UDS network has been created, but broadcasting it is unimplemented.");

    cmd_buff[1] = RESULT_SUCCESS.raw;
}

static void GetChannel(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u8 channel = network_channel;

    if (connection_status.status == NotConnected)
        channel = 0;

    cmd_buff[1] = RESULT_SUCCESS.raw;
    cmd_buff[2] = channel;

    LOG_DEBUG(Service_NWM, "called");
}

static void SetApplicationData(Interface* self) {
    u32* cmd_buff = Kernel::GetCommandBuffer();

    u32 size = cmd_buff[1];
    VAddr address = cmd_buff[3];

    LOG_DEBUG(Service_NWM, "called");

    if (size > ApplicationDataSize) {
        cmd_buff[1] = ResultCode(ErrorDescription::TooLarge, ErrorModule::UDS,
                                 ErrorSummary::WrongArgument, ErrorLevel::Usage).raw;
        return;
    }

    application_data.resize(size);
    Memory::ReadBlock(address, application_data.data(), size);

    cmd_buff[1] = RESULT_SUCCESS.raw;
}

const Interface::FunctionInfo FunctionTable[] = {
    {0x00010442, nullptr, "Initialize (deprecated)"},
    {0x00020000, nullptr, "Scrap"},
    {0x00030000, Shutdown, "Shutdown"},
    {0x00040402, nullptr, "CreateNetwork (deprecated)"},
    {0x00050040, nullptr, "EjectClient"},
    {0x00060000, nullptr, "EjectSpectator"},
    {0x00070080, nullptr, "UpdateNetworkAttribute"},
    {0x00080000, nullptr, "DestroyNetwork"},
    {0x00090442, nullptr, "ConnectNetwork (deprecated)"},
    {0x000A0000, nullptr, "DisconnectNetwork"},
    {0x000B0000, GetConnectionStatus, "GetConnectionStatus"},
    {0x000D0040, nullptr, "GetNodeInformation"},
    {0x000E0006, nullptr, "DecryptBeaconData (deprecated)"},
    {0x000F0404, RecvBeaconBroadcastData, "RecvBeaconBroadcastData"},
    {0x00100042, SetApplicationData, "SetApplicationData"},
    {0x00110040, nullptr, "GetApplicationData"},
    {0x00120100, Bind, "Bind"},
    {0x00130040, nullptr, "Unbind"},
    {0x001400C0, nullptr, "PullPacket"},
    {0x00150080, nullptr, "SetMaxSendDelay"},
    {0x00170182, nullptr, "SendTo"},
    {0x001A0000, GetChannel, "GetChannel"},
    {0x001B0302, InitializeWithVersion, "InitializeWithVersion"},
    {0x001D0044, BeginHostingNetwork, "BeginHostingNetwork"},
    {0x001E0084, nullptr, "ConnectToNetwork"},
    {0x001F0006, nullptr, "DecryptBeaconData"},
    {0x00200040, nullptr, "Flush"},
    {0x00210080, nullptr, "SetProbeResponseParam"},
    {0x00220402, nullptr, "ScanOnConnection"},
};

NWM_UDS::NWM_UDS() {
    connection_status_event = Kernel::Event::Create(Kernel::ResetType::OneShot, "NWM::connection_status_event");

    Register(FunctionTable);
}

NWM_UDS::~NWM_UDS() {
    application_data.clear();
    bind_node_events.clear();
    connection_status_event = nullptr;
    recv_buffer_memory = nullptr;

    connection_status = {};
    connection_status.status = NotConnected;
}

} // namespace NWM
} // namespace Service
