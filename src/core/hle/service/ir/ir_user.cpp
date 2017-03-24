// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <memory>
#include "common/string_util.h"
#include "common/swap.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/shared_memory.h"
#include "core/hle/service/ir/crc8.h"
#include "core/hle/service/ir/extra_hid.h"
#include "core/hle/service/ir/ir.h"
#include "core/hle/service/ir/ir_user.h"

namespace Service {
namespace IR {

/// Structure of data stored in ir:USER shared memory
struct SharedMemory {
    u32_le latest_receive_error_result;
    u32_le latest_send_error_result;
    // TODO(wwylele): for these fields below, make them enum when the meaning of values is known.
    u8 connection_status;
    u8 trying_to_connect_status;
    u8 connection_role;
    u8 machine_id;
    u8 connected;
    u8 network_id;
    u8 initialized;
    u8 unknown;

    // This is not the end of the shared memory. It is followed by a receive buffer and a send
    // buffer. We handle receive buffer by the CircularBuffer class. For the send buffer, because
    // games usually don't access it, we don't emulate it.
};
static_assert(sizeof(SharedMemory) == 16, "SharedMemory has wrong size!");

/// A manager of the circular buffer in the shared memory.
class CircularBuffer {
public:
    CircularBuffer(Kernel::SharedPtr<Kernel::SharedMemory> shared_memory_, u32 info_offset_,
                   u32 buffer_offset_, u32 max_packet_count_, u32 buffer_size)
        : shared_memory(shared_memory_), info_offset(info_offset_), buffer_offset(buffer_offset_),
          max_packet_count(max_packet_count_),
          max_data_size(buffer_size - sizeof(PacketInfo) * max_packet_count_) {

        UpdateBufferInfo();
    }

    /**
     * Puts a packet to the head of the buffer.
     * @params packet The data of the packet to put.
     * @returns whether the operation is successful.
     */
    bool Put(const std::vector<u8>& packet) {
        if (info.packet_count == max_packet_count)
            return false;

        u32 write_offset;

        // finds freespace offset in data buffer
        if (info.packet_count == 0) {
            write_offset = 0;
            if (packet.size() > max_data_size)
                return false;
        } else {
            PacketInfo first, last;
            memcpy(&first, GetPacketInfoPointer(info.begin_index), sizeof(PacketInfo));
            const u32 last_index = (info.end_index + max_packet_count - 1) % max_packet_count;
            memcpy(&last, GetPacketInfoPointer(last_index), sizeof(PacketInfo));
            write_offset = (last.offset + last.size) % max_data_size;
            const u32 free_space = (first.offset + max_data_size - write_offset) % max_data_size;
            if (packet.size() > free_space)
                return false;
        }

        // writes packet info
        PacketInfo packet_info{write_offset, static_cast<u32>(packet.size())};
        memcpy(GetPacketInfoPointer(info.end_index), &packet_info, sizeof(PacketInfo));

        // writes packet data
        for (size_t i = 0; i < packet.size(); ++i) {
            *GetDataBufferPointer((write_offset + i) % max_data_size) = packet[i];
        }

        // updates buffer info
        info.end_index++;
        info.end_index %= max_packet_count;
        info.packet_count++;
        UpdateBufferInfo();
        return true;
    }

    /**
     * Release packets from the tail of the buffer
     * @params count Numbers of packets to release.
     * @returns whether the operation is successful.
     */
    bool Release(u32 count) {
        if (info.packet_count < count)
            return false;

        info.packet_count -= count;
        info.begin_index += count;
        info.begin_index %= max_packet_count;
        UpdateBufferInfo();
        return true;
    }

private:
    struct BufferInfo {
        u32_le begin_index;
        u32_le end_index;
        u32_le packet_count;
        u32_le unknown;
    } info{0, 0, 0, 0};
    static_assert(sizeof(BufferInfo) == 16, "BufferInfo has wrong size!");

    struct PacketInfo {
        u32_le offset;
        u32_le size;
    };
    static_assert(sizeof(PacketInfo) == 8, "PacketInfo has wrong size!");

    u8* GetPacketInfoPointer(u32 index) {
        return shared_memory->GetPointer(buffer_offset + sizeof(PacketInfo) * index);
    }

    u8* GetDataBufferPointer(u32 offset) {
        return shared_memory->GetPointer(buffer_offset + sizeof(PacketInfo) * max_packet_count +
                                         offset);
    }

    void UpdateBufferInfo() {
        if (info_offset) {
            memcpy(shared_memory->GetPointer(info_offset), &info, sizeof(info));
        }
    }

    Kernel::SharedPtr<Kernel::SharedMemory> shared_memory;
    u32 info_offset;
    u32 buffer_offset;
    u32 max_packet_count;
    u32 max_data_size;
};

static Kernel::SharedPtr<Kernel::Event> conn_status_event, send_event, receive_event;
static Kernel::SharedPtr<Kernel::SharedMemory> shared_memory;
static std::unique_ptr<ExtraHID> extra_hid;
static IRDevice* connected_device;
static std::unique_ptr<CircularBuffer> receive_buffer;

/// Wraps the payload into packet and puts it to the receive buffer
static void PutToReceive(const std::vector<u8>& payload) {
    LOG_TRACE(Service_IR, "called, data=%s",
              Common::ArrayToString(payload.data(), payload.size()).c_str());
    size_t size = payload.size();

    std::vector<u8> packet;

    // Builds packet header. For the format info:
    // https://www.3dbrew.org/wiki/IRUSER_Shared_Memory#Packet_structure

    // fixed value
    packet.push_back(0xA5);
    // destination network ID
    packet.push_back(*(shared_memory->GetPointer(offsetof(SharedMemory, network_id))));

    // puts the size info.
    // The highest bit of the first byte is unknown, which is set to zero here. The second highest
    // bit is a flag that determines whether the size info is in extended form. If the packet size
    // can be represent within 6 bits, the short form (1 byte) of size info is chosen, the size is
    // put to the lower bits of this byte, and the flag is clear. If the packet size cannot be
    // represent within 6 bits, the extended form (2 bytes) is chosen, the lower 8 bits of the size
    // is put to the second byte, the higher bits of the size is put to the lower bits of the first
    // byte, and the flag is set. Note that the packet size must be within 14 bits due to this
    // format restriction, or it will overlap with the flag bit.
    DEBUG_ASSERT(size < (1 << 14));
    if (size < (1 << 6)) {
        packet.push_back(static_cast<u8>(size));
    } else {
        packet.push_back(static_cast<u8>((1 << 6) | (size >> 8)));
        packet.push_back(static_cast<u8>(size & 0xFF));
    }

    // puts the payload
    packet.insert(packet.end(), payload.begin(), payload.end());

    // calculates CRC and puts to the end
    packet.push_back(Crc8(packet.data(), packet.size()));

    if (receive_buffer->Put(packet)) {
        receive_event->Signal();
    } else {
        LOG_ERROR(Service_IR, "receive buffer is full!");
    }
}

/**
 * IR::InitializeIrNopShared service function
 *  Inputs:
 *      1 : Size of shared memory
 *      2 : Recv buffer size
 *      3 : Recv buffer packet count
 *      4 : Send buffer size
 *      5 : Send buffer packet count
 *      6 : BaudRate (u8)
 *      7 : 0 (Handle descriptor)
 *      8 : Handle of shared memory
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void InitializeIrNopShared(Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x18, 6, 2);
    const u32 shared_buff_size = rp.Pop<u32>();
    const u32 recv_buff_size = rp.Pop<u32>();
    const u32 recv_buff_packet_count = rp.Pop<u32>();
    const u32 send_buff_size = rp.Pop<u32>();
    const u32 send_buff_packet_count = rp.Pop<u32>();
    const u8 baud_rate = static_cast<u8>(rp.Pop<u32>() & 0xFF);
    const Kernel::Handle handle = rp.PopHandle();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    shared_memory = Kernel::g_handle_table.Get<Kernel::SharedMemory>(handle);
    if (!shared_memory) {
        LOG_CRITICAL(Service_IR, "invalid shared memory handle 0x%08X", handle);
        rb.Push(ResultCode(ErrorDescription::InvalidHandle, ErrorModule::OS,
                           ErrorSummary::WrongArgument, ErrorLevel::Permanent));
        return;
    }
    shared_memory->name = "IR_USER: shared memory";

    receive_buffer = std::make_unique<CircularBuffer>(shared_memory, 0x10, 0x20,
                                                      recv_buff_packet_count, recv_buff_size);
    SharedMemory shared_memory_init;
    shared_memory_init.latest_receive_error_result = 0;
    shared_memory_init.latest_send_error_result = 0;
    shared_memory_init.connection_status = 0;
    shared_memory_init.trying_to_connect_status = 0;
    shared_memory_init.connection_role = 0;
    shared_memory_init.machine_id = 0;
    shared_memory_init.connected = 0;
    shared_memory_init.network_id = 0;
    shared_memory_init.initialized = 1;
    shared_memory_init.unknown = 0;
    std::memcpy(shared_memory->GetPointer(), &shared_memory_init, sizeof(SharedMemory));

    rb.Push(RESULT_SUCCESS);

    LOG_INFO(Service_IR, "called, shared_buff_size=%u, recv_buff_size=%u, "
                         "recv_buff_packet_count=%u, send_buff_size=%u, "
                         "send_buff_packet_count=%u, baud_rate=%u, handle=0x%08X",
             shared_buff_size, recv_buff_size, recv_buff_packet_count, send_buff_size,
             send_buff_packet_count, baud_rate, handle);
}

/**
 * IR::RequireConnection service function
 *  Inputs:
 *      1 : device ID? always 1 for circle pad pro
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void RequireConnection(Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x06, 1, 0);

    const u8 device_id = static_cast<u8>(rp.Pop<u32>() & 0xFF);

    if (device_id == 1) {
        // These values are observed on a New 3DS. The meaning of them is unclear.
        // TODO (wwylele): should assign network_id a (random?) number
        *(shared_memory->GetPointer(offsetof(SharedMemory, connection_status))) = 2;
        *(shared_memory->GetPointer(offsetof(SharedMemory, connection_role))) = 2;
        *(shared_memory->GetPointer(offsetof(SharedMemory, connected))) = 1;

        connected_device = extra_hid.get();
        connected_device->Connect();
        conn_status_event->Signal();
    } else {
        LOG_WARNING(Service_IR, "unknown device id %u. Won't connect.", device_id);
        *(shared_memory->GetPointer(offsetof(SharedMemory, connection_status))) = 1;
        *(shared_memory->GetPointer(offsetof(SharedMemory, trying_to_connect_status))) = 2;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_INFO(Service_IR, "called, device_id = %u", device_id);
}

/**
 * IR::GetReceiveEvent service function
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : 0 (Handle descriptor)
 *      3 : Receive event handle
 */
void GetReceiveEvent(Interface* self) {
    IPC::RequestBuilder rb(Kernel::GetCommandBuffer(), 0x0A, 1, 2);

    rb.Push(RESULT_SUCCESS);
    rb.PushCopyHandles(Kernel::g_handle_table.Create(Service::IR::receive_event).MoveFrom());

    LOG_INFO(Service_IR, "called");
}

/**
 * IR::GetSendEvent service function
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : 0 (Handle descriptor)
 *      3 : Send event handle
 */
void GetSendEvent(Interface* self) {
    IPC::RequestBuilder rb(Kernel::GetCommandBuffer(), 0x0B, 1, 2);

    rb.Push(RESULT_SUCCESS);
    rb.PushCopyHandles(Kernel::g_handle_table.Create(Service::IR::send_event).MoveFrom());

    LOG_INFO(Service_IR, "called");
}

/**
 * IR::Disconnect service function
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void Disconnect(Interface* self) {
    if (connected_device) {
        connected_device->Disconnect();
        connected_device = nullptr;
        conn_status_event->Signal();
    }

    *(shared_memory->GetPointer(offsetof(SharedMemory, connection_status))) = 0;
    *(shared_memory->GetPointer(offsetof(SharedMemory, connected))) = 0;

    IPC::RequestBuilder rb(Kernel::GetCommandBuffer(), 0x09, 1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_INFO(Service_IR, "called");
}

/**
 * IR::GetConnectionStatusEvent service function
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 *      2 : 0 (Handle descriptor)
 *      3 : Connection Status Event handle
 */
static void GetConnectionStatusEvent(Interface* self) {
    IPC::RequestBuilder rb(Kernel::GetCommandBuffer(), 0x0C, 1, 2);

    rb.Push(RESULT_SUCCESS);
    rb.PushCopyHandles(Kernel::g_handle_table.Create(Service::IR::conn_status_event).MoveFrom());

    LOG_INFO(Service_IR, "called");
}

/**
 * IR::FinalizeIrNop service function
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void FinalizeIrNop(Interface* self) {
    if (connected_device) {
        connected_device->Disconnect();
        connected_device = nullptr;
    }

    shared_memory = nullptr;
    receive_buffer = nullptr;

    IPC::RequestBuilder rb(Kernel::GetCommandBuffer(), 0x02, 1, 0);
    rb.Push(RESULT_SUCCESS);

    LOG_INFO(Service_IR, "called");
}

/**
 * IR::SendIrNop service function
 *  Inpus:
 *      1 : Size of data to send
 *      2 : 2 + (size << 14) (Static buffer descriptor)
 *      3 : Data buffer address
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void SendIrNop(Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x0D, 1, 2);
    const u32 size = rp.Pop<u32>();
    const VAddr address = rp.PopStaticBuffer();

    std::vector<u8> buffer(size);
    Memory::ReadBlock(address, buffer.data(), size);

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    if (connected_device) {
        connected_device->Receive(buffer);
        send_event->Signal();
        rb.Push(RESULT_SUCCESS);
    } else {
        LOG_ERROR(Service_IR, "not connected");
        rb.Push(ResultCode(static_cast<ErrorDescription>(13), ErrorModule::IR,
                           ErrorSummary::InvalidState, ErrorLevel::Status));
    }

    LOG_TRACE(Service_IR, "called, data=%s", Common::ArrayToString(buffer.data(), size).c_str());
}

/**
 * IR::ReleaseReceivedData function
 *  Inpus:
 *      1 : Number of packets to release
 *  Outputs:
 *      1 : Result of function, 0 on success, otherwise error code
 */
static void ReleaseReceivedData(Interface* self) {
    IPC::RequestParser rp(Kernel::GetCommandBuffer(), 0x19, 1, 0);
    u32 count = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);

    if (receive_buffer->Release(count)) {
        rb.Push(RESULT_SUCCESS);
    } else {
        LOG_ERROR(Service_IR, "failed to release %u packets", count);
        rb.Push(ResultCode(ErrorDescription::NoData, ErrorModule::IR, ErrorSummary::NotFound,
                           ErrorLevel::Status));
    }

    LOG_TRACE(Service_IR, "called, count=%u", count);
}

const Interface::FunctionInfo FunctionTable[] = {
    {0x00010182, nullptr, "InitializeIrNop"},
    {0x00020000, FinalizeIrNop, "FinalizeIrNop"},
    {0x00030000, nullptr, "ClearReceiveBuffer"},
    {0x00040000, nullptr, "ClearSendBuffer"},
    {0x000500C0, nullptr, "WaitConnection"},
    {0x00060040, RequireConnection, "RequireConnection"},
    {0x000702C0, nullptr, "AutoConnection"},
    {0x00080000, nullptr, "AnyConnection"},
    {0x00090000, Disconnect, "Disconnect"},
    {0x000A0000, GetReceiveEvent, "GetReceiveEvent"},
    {0x000B0000, GetSendEvent, "GetSendEvent"},
    {0x000C0000, GetConnectionStatusEvent, "GetConnectionStatusEvent"},
    {0x000D0042, SendIrNop, "SendIrNop"},
    {0x000E0042, nullptr, "SendIrNopLarge"},
    {0x000F0040, nullptr, "ReceiveIrnop"},
    {0x00100042, nullptr, "ReceiveIrnopLarge"},
    {0x00110040, nullptr, "GetLatestReceiveErrorResult"},
    {0x00120040, nullptr, "GetLatestSendErrorResult"},
    {0x00130000, nullptr, "GetConnectionStatus"},
    {0x00140000, nullptr, "GetTryingToConnectStatus"},
    {0x00150000, nullptr, "GetReceiveSizeFreeAndUsed"},
    {0x00160000, nullptr, "GetSendSizeFreeAndUsed"},
    {0x00170000, nullptr, "GetConnectionRole"},
    {0x00180182, InitializeIrNopShared, "InitializeIrNopShared"},
    {0x00190040, ReleaseReceivedData, "ReleaseReceivedData"},
    {0x001A0040, nullptr, "SetOwnMachineId"},
};

IR_User_Interface::IR_User_Interface() {
    Register(FunctionTable);
}

void InitUser() {
    using namespace Kernel;

    shared_memory = nullptr;

    conn_status_event = Event::Create(ResetType::OneShot, "IR:ConnectionStatusEvent");
    send_event = Event::Create(ResetType::OneShot, "IR:SendEvent");
    receive_event = Event::Create(ResetType::OneShot, "IR:ReceiveEvent");

    receive_buffer = nullptr;

    extra_hid = std::make_unique<ExtraHID>(PutToReceive);

    connected_device = nullptr;
}

void ShutdownUser() {
    if (connected_device) {
        connected_device->Disconnect();
        connected_device = nullptr;
    }

    extra_hid = nullptr;
    shared_memory = nullptr;
    conn_status_event = nullptr;
    send_event = nullptr;
    receive_event = nullptr;
}

void ReloadInputDevices() {
    if (extra_hid)
        extra_hid->ReloadInputDevices();
}

IRDevice::IRDevice(SendFunc send_func_) : send_func(send_func_) {}
IRDevice::~IRDevice() = default;

} // namespace IR
} // namespace Service
