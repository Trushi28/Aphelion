#pragma once
#include <cosmos/types.hpp>
#include <cosmos/xhci.hpp>
#include <cosmos/diskmap.hpp>

namespace usbstor {

constexpr u32 MAX_LUNS = 8;
constexpr u32 MAX_SECTORS_PER_COMMAND = xhci::BOUNCE_BYTES / 512;

struct LunInfo {
    xhci::Device* dev;
    u8 lun;
    bool ready;
    bool usable;
    u64 blocks;
    u32 block_size;
    char vendor[9];
    char product[17];
    bool scanned;
    diskmap::Verdict verdict;
    bool has_image;
    u64 image_bytes;
    bool has_tail;
    u64 tail_start;
    u64 tail_sectors;
    const char* note;
};

void init(const u8* want_guid, const char* want_volume_id);

u32 lun_count();
const LunInfo& lun(u32 index);

bool window_present();
const LunInfo* window_lun();
const char* window_status();

}
