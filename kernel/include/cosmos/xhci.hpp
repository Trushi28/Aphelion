#pragma once
#include <cosmos/types.hpp>
#include <cosmos/dma.hpp>
#include <cosmos/usbproto.hpp>

namespace xhci {

constexpr u32 MAX_CONTROLLERS = 2;
constexpr u32 MAX_LISTED = 16;
constexpr u32 MAX_STORAGE = 4;

enum class Speed : u8 { Unknown = 0, Full = 1, Low = 2, High = 3, Super = 4, SuperPlus = 5 };

enum class Result : u8 { Ok, Stall, Timeout, Error, Disconnected };

struct Ring {
    dma::Buffer mem;
    u32 enqueue = 0;
    u8 cycle = 1;
};

struct Device {
    bool in_use = false;
    bool dead = false;
    u8 controller = 0;
    u8 slot = 0;
    u8 port = 0;
    Speed speed = Speed::Unknown;
    u16 vendor = 0;
    u16 product = 0;
    u8 device_class = 0;
    usbproto::MassStorage storage{};
    u8 in_dci = 0;
    u8 out_dci = 0;
    dma::Buffer out_context;
    dma::Buffer in_context;
    dma::Buffer small;
    dma::Buffer bounce;
    Ring ep0;
    Ring bulk_in;
    Ring bulk_out;
};

struct Listed {
    u8 controller;
    u8 port;
    Speed speed;
    u16 vendor;
    u16 product;
    u8 device_class;
    bool mass_storage;
    const char* note;
};

struct ControllerInfo {
    bool started;
    u16 vendor;
    u16 device;
    u8 bus, slot, function;
    u32 ports;
    u32 slots;
    bool context64;
    bool took_over_from_firmware;
    const char* status;
};

constexpr u32 BOUNCE_BYTES = 65536;
constexpr u32 SMALL_BYTES = 4096;

bool init(u64 hhdm_offset);

u32 controller_count();
const ControllerInfo& controller(u32 index);
u32 listed_count();
const Listed& listed(u32 index);
u32 storage_count();
Device* storage_device(u32 index);

Result control(Device& dev, const usbproto::Setup& setup, void* data, u32 length, u32* actual, u32 timeout_ms);
Result bulk_out(Device& dev, const void* data, u32 length, u32 timeout_ms);
Result bulk_in(Device& dev, void* data, u32 length, u32* actual, u32 timeout_ms);
bool clear_halt(Device& dev, bool in);

const char* speed_name(Speed s);

}
