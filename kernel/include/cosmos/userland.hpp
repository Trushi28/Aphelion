#pragma once
#include <cosmos/types.hpp>
#include <cosmos/lens_channel.hpp>

namespace userland {

struct Process;

struct Image {
    const u8* code;
    u64 code_bytes;
    u64 entry_offset;
    u32 data_pages;
};

bool supported();
void init_core(u32 core_index);

Process* launch(const char* name, const Image& image);
Process* launch_with(const char* name, const Image& image, lens::Table& from, lens::Lens handle);
bool wait(Process* p, i32* status, u64 timeout_ms);
bool reap(Process* p);
bool kill(Process* p);

u32 pid_of(const Process* p);
u32 task_of(const Process* p);
u32 live_threads(const Process* p);
u32 live_handles(Process* p);
u32 threads_created(const Process* p);
u32 cores_used(const Process* p);
u64 frames_live();
u64 frames_quarantined();
u32 live_processes();

void expect_nmis(u32 count);
u64 nmis_from_user();
u64 nmis_absorbed();

void spawn_selftest();

}
