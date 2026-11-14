#pragma once
#include "win32.hpp"

namespace pehost {

constexpr u32 WAIT_OBJECT_0 = 0;
constexpr u32 WAIT_ABANDONED_0 = 0x80;
constexpr u32 WAIT_TIMEOUT = 0x102;
constexpr u32 WAIT_FAILED = 0xFFFFFFFFu;
constexpr u32 MAXIMUM_WAIT_OBJECTS = 64;
constexpr u32 INFINITE_WAIT = 0xFFFFFFFFu;
constexpr u32 ERROR_ALREADY_EXISTS = 183;
constexpr u32 ERROR_NOT_OWNER = 288;
constexpr u32 ERROR_TOO_MANY_POSTS = 298;
constexpr u32 ERROR_OBJECT_NOT_FOUND = 2;
constexpr u64 OBJECT_HANDLE_BASE = 0x400;
constexpr u32 CREATE_EVENT_MANUAL_RESET = 1;
constexpr u32 CREATE_EVENT_INITIAL_SET = 2;
constexpr u32 CREATE_MUTEX_INITIAL_OWNER = 1;
constexpr u32 DUPLICATE_CLOSE_SOURCE = 1;

static_assert(OBJECT_HANDLE_BASE + 4ull * sync::MAX_HANDLES < 0x1000);

namespace objects {

inline sync::ObjectTable& table() { return sync::g_sync.objects; }

inline void changed() {
    sync::add(&table().epoch, 1);
    sync::wake(&table().epoch);
}

inline bool name_of_wide(const u16* wide, char* out) {
    u32 n = 0;
    while (wide[n] && n < sync::OBJECT_NAME) ++n;
    if (wide[n]) return false;
    i64 w = codec::utf16_to_utf8(wide, n, reinterpret_cast<u8*>(out), sync::OBJECT_NAME * 3, true);
    if (w < 0 || w >= static_cast<i64>(sync::OBJECT_NAME)) return false;
    out[w] = 0;
    return true;
}

inline bool same_name(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

inline i32 find_named(u8 kind, const char* name, bool* wrong_kind) {
    *wrong_kind = false;
    for (u32 i = 0; i < sync::MAX_OBJECTS; ++i) {
        sync::Object& o = table().items[i];
        if (o.kind == sync::OBJ_NONE || o.name[0] == 0 || !same_name(o.name, name)) continue;
        if (o.kind != kind) *wrong_kind = true;
        return static_cast<i32>(i);
    }
    return -1;
}

inline i32 allocate(u8 kind, const char* name) {
    for (u32 i = 0; i < sync::MAX_OBJECTS; ++i) {
        sync::Object& o = table().items[i];
        if (o.kind != sync::OBJ_NONE) continue;
        u8* raw = reinterpret_cast<u8*>(&o);
        for (usize k = 0; k < sizeof(o); ++k) raw[k] = 0;
        o.kind = kind;
        o.slot = -1;
        if (name) {
            u32 n = 0;
            while (name[n] && n + 1 < sync::OBJECT_NAME) {
                o.name[n] = name[n];
                ++n;
            }
            o.name[n] = 0;
        }
        return static_cast<i32>(i);
    }
    return -1;
}

inline void unref(i32 index) {
    sync::Object& o = table().items[index];
    if (o.refs) --o.refs;
    if (o.refs != 0) return;
    u8* raw = reinterpret_cast<u8*>(&o);
    for (usize k = 0; k < sizeof(o); ++k) raw[k] = 0;
}

inline u64 handle_for(i32 index) {
    for (u32 h = 0; h < sync::MAX_HANDLES; ++h) {
        if (table().handles[h] != 0) continue;
        table().handles[h] = static_cast<u32>(index) + 1;
        ++table().items[index].refs;
        return OBJECT_HANDLE_BASE + 4ull * h;
    }
    return 0;
}

inline u32 handle_slot(u64 handle) {
    if (handle < OBJECT_HANDLE_BASE || (handle - OBJECT_HANDLE_BASE) % 4 != 0) return sync::MAX_HANDLES;
    u64 h = (handle - OBJECT_HANDLE_BASE) / 4;
    if (h >= sync::MAX_HANDLES || table().handles[h] == 0) return sync::MAX_HANDLES;
    return static_cast<u32>(h);
}

inline sync::Object* resolve(u64 handle, i32* index = nullptr) {
    u32 h = handle_slot(handle);
    if (h == sync::MAX_HANDLES) return nullptr;
    i32 i = static_cast<i32>(table().handles[h]) - 1;
    if (index) *index = i;
    return &table().items[i];
}

inline bool is_object_handle(u64 handle) { return handle >= OBJECT_HANDLE_BASE && handle < OBJECT_HANDLE_BASE + 4ull * sync::MAX_HANDLES; }

inline bool close(u64 handle) {
    sync::Host guard;
    u32 h = handle_slot(handle);
    if (h == sync::MAX_HANDLES) return false;
    i32 index = static_cast<i32>(table().handles[h]) - 1;
    table().handles[h] = 0;
    unref(index);
    return true;
}

inline u64 thread_key() { return detail::current_thread_id(); }

inline bool ready(const sync::Object& o, u64 me) {
    switch (o.kind) {
        case sync::OBJ_EVENT: return o.signaled;
        case sync::OBJ_MUTEX: return o.owner == 0 || o.owner == me;
        case sync::OBJ_SEMAPHORE: return o.count > 0;
        case sync::OBJ_THREAD: return o.finished;
        default: return false;
    }
}

inline bool take(sync::Object& o, u64 me) {
    bool abandoned = false;
    switch (o.kind) {
        case sync::OBJ_EVENT:
            if (!o.manual) o.signaled = false;
            break;
        case sync::OBJ_MUTEX:
            abandoned = o.abandoned;
            o.abandoned = false;
            o.owner = me;
            ++o.recursion;
            break;
        case sync::OBJ_SEMAPHORE: --o.count; break;
        default: break;
    }
    return abandoned;
}

inline void abandon_mutexes(u64 owner) {
    bool any = false;
    for (u32 i = 0; i < sync::MAX_OBJECTS; ++i) {
        sync::Object& o = table().items[i];
        if (o.kind == sync::OBJ_MUTEX && o.owner == owner) {
            o.owner = 0;
            o.recursion = 0;
            o.abandoned = true;
            any = true;
        }
    }
    if (any) changed();
}

inline u32 wait_many(u32 count, const u64* handles, bool all, u32 milliseconds) {
    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || handles == nullptr) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    u64 me = thread_key();
    sync::Deadline deadline(milliseconds == INFINITE_WAIT ? sync::FOREVER : milliseconds);
    for (;;) {
        u32 epoch = sync::load(&table().epoch);
        {
            sync::Host guard;
            sync::Object* found[MAXIMUM_WAIT_OBJECTS];
            for (u32 i = 0; i < count; ++i) {
                found[i] = resolve(handles[i]);
                if (!found[i] || found[i]->kind == sync::OBJ_NONE) {
                    set_last_error(ERROR_INVALID_HANDLE);
                    return WAIT_FAILED;
                }
            }
            if (all) {
                bool every = true;
                for (u32 i = 0; i < count && every; ++i) every = ready(*found[i], me);
                if (every) {
                    bool abandoned = false;
                    for (u32 i = 0; i < count; ++i) abandoned = take(*found[i], me) || abandoned;
                    changed();
                    return abandoned ? WAIT_ABANDONED_0 : WAIT_OBJECT_0;
                }
            } else {
                for (u32 i = 0; i < count; ++i) {
                    if (!ready(*found[i], me)) continue;
                    bool abandoned = take(*found[i], me);
                    changed();
                    return (abandoned ? WAIT_ABANDONED_0 : WAIT_OBJECT_0) + i;
                }
            }
        }
        if (milliseconds == 0 || deadline.expired()) return WAIT_TIMEOUT;
        if (sync::block(&table().epoch, epoch, deadline.remaining()) == 1) return WAIT_TIMEOUT;
    }
}

inline u64 create(u8 kind, const char* name, i32* index_out, bool* existed) {
    sync::Host guard;
    *existed = false;
    i32 index = -1;
    if (name && name[0]) {
        bool wrong = false;
        index = find_named(kind, name, &wrong);
        if (wrong) {
            set_last_error(ERROR_INVALID_HANDLE);
            return 0;
        }
        if (index >= 0) *existed = true;
    }
    if (index < 0) {
        index = allocate(kind, name);
        if (index < 0) {
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
    }
    u64 handle = handle_for(index);
    if (handle == 0) {
        if (!*existed) table().items[index] = sync::Object{};
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    *index_out = index;
    set_last_error(*existed ? ERROR_ALREADY_EXISTS : 0);
    return handle;
}

inline u64 open(u8 kind, const char* name) {
    sync::Host guard;
    if (name == nullptr || name[0] == 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    bool wrong = false;
    i32 index = find_named(kind, name, &wrong);
    if (index < 0) {
        set_last_error(ERROR_OBJECT_NOT_FOUND);
        return 0;
    }
    if (wrong) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    u64 handle = handle_for(index);
    if (handle == 0) set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return handle;
}

inline i32 current_thread_object() {
    sync::Slot& me = sync::current();
    if (me.object != 0) return me.object - 1;
    i32 index = allocate(sync::OBJ_THREAD, nullptr);
    if (index < 0) return -1;
    sync::Object& o = table().items[index];
    o.refs = 1;
    o.id = static_cast<u32>(detail::current_thread_id());
    o.slot = static_cast<i32>(&me - sync::g_sync.slots);
    me.object = index + 1;
    return index;
}

}

namespace win32 {

PEHOST_WINAPI inline u64 CreateEventA(void*, i32 manual, i32 initial, const char* name) {
    i32 index = 0;
    bool existed = false;
    u64 handle = objects::create(sync::OBJ_EVENT, name, &index, &existed);
    if (handle && !existed) {
        sync::Host guard;
        sync::Object& o = objects::table().items[index];
        o.manual = manual != 0;
        o.signaled = initial != 0;
    }
    return handle;
}

PEHOST_WINAPI inline u64 CreateEventW(void* security, i32 manual, i32 initial, const u16* name) {
    char narrow[sync::OBJECT_NAME];
    if (name && !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateEventA(security, manual, initial, name ? narrow : nullptr);
}

PEHOST_WINAPI inline u64 CreateEventExA(void* security, const char* name, u32 flags, u32) {
    return CreateEventA(security, (flags & CREATE_EVENT_MANUAL_RESET) != 0, (flags & CREATE_EVENT_INITIAL_SET) != 0, name);
}

PEHOST_WINAPI inline u64 CreateEventExW(void* security, const u16* name, u32 flags, u32 access) {
    char narrow[sync::OBJECT_NAME];
    if (name && !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateEventExA(security, name ? narrow : nullptr, flags, access);
}

PEHOST_WINAPI inline u64 OpenEventA(u32, i32, const char* name) { return objects::open(sync::OBJ_EVENT, name); }

PEHOST_WINAPI inline u64 OpenEventW(u32 access, i32 inherit, const u16* name) {
    char narrow[sync::OBJECT_NAME];
    if (!name || !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return OpenEventA(access, inherit, narrow);
}

PEHOST_WINAPI inline i32 SetEvent(u64 handle) {
    sync::Host guard;
    sync::Object* o = objects::resolve(handle);
    if (!o || o->kind != sync::OBJ_EVENT) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    o->signaled = true;
    objects::changed();
    return 1;
}

PEHOST_WINAPI inline i32 ResetEvent(u64 handle) {
    sync::Host guard;
    sync::Object* o = objects::resolve(handle);
    if (!o || o->kind != sync::OBJ_EVENT) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    o->signaled = false;
    return 1;
}

PEHOST_WINAPI inline u64 CreateMutexA(void*, i32 initial_owner, const char* name) {
    i32 index = 0;
    bool existed = false;
    u64 handle = objects::create(sync::OBJ_MUTEX, name, &index, &existed);
    if (handle && !existed && initial_owner) {
        sync::Host guard;
        sync::Object& o = objects::table().items[index];
        o.owner = objects::thread_key();
        o.recursion = 1;
    }
    return handle;
}

PEHOST_WINAPI inline u64 CreateMutexW(void* security, i32 initial_owner, const u16* name) {
    char narrow[sync::OBJECT_NAME];
    if (name && !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateMutexA(security, initial_owner, name ? narrow : nullptr);
}

PEHOST_WINAPI inline u64 CreateMutexExA(void* security, const char* name, u32 flags, u32) {
    return CreateMutexA(security, (flags & CREATE_MUTEX_INITIAL_OWNER) != 0, name);
}

PEHOST_WINAPI inline u64 CreateMutexExW(void* security, const u16* name, u32 flags, u32 access) {
    char narrow[sync::OBJECT_NAME];
    if (name && !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateMutexExA(security, name ? narrow : nullptr, flags, access);
}

PEHOST_WINAPI inline u64 OpenMutexA(u32, i32, const char* name) { return objects::open(sync::OBJ_MUTEX, name); }

PEHOST_WINAPI inline u64 OpenMutexW(u32 access, i32 inherit, const u16* name) {
    char narrow[sync::OBJECT_NAME];
    if (!name || !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return OpenMutexA(access, inherit, narrow);
}

PEHOST_WINAPI inline i32 ReleaseMutex(u64 handle) {
    sync::Host guard;
    sync::Object* o = objects::resolve(handle);
    if (!o || o->kind != sync::OBJ_MUTEX) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (o->owner != objects::thread_key() || o->recursion == 0) {
        set_last_error(ERROR_NOT_OWNER);
        return 0;
    }
    if (--o->recursion == 0) {
        o->owner = 0;
        objects::changed();
    }
    return 1;
}

PEHOST_WINAPI inline u64 CreateSemaphoreA(void*, i32 initial, i32 maximum, const char* name) {
    if (maximum <= 0 || initial < 0 || initial > maximum) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    i32 index = 0;
    bool existed = false;
    u64 handle = objects::create(sync::OBJ_SEMAPHORE, name, &index, &existed);
    if (handle && !existed) {
        sync::Host guard;
        sync::Object& o = objects::table().items[index];
        o.count = initial;
        o.maximum = maximum;
    }
    return handle;
}

PEHOST_WINAPI inline u64 CreateSemaphoreW(void* security, i32 initial, i32 maximum, const u16* name) {
    char narrow[sync::OBJECT_NAME];
    if (name && !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateSemaphoreA(security, initial, maximum, name ? narrow : nullptr);
}

PEHOST_WINAPI inline u64 CreateSemaphoreExA(void* security, i32 initial, i32 maximum, const char* name, u32, u32) {
    return CreateSemaphoreA(security, initial, maximum, name);
}

PEHOST_WINAPI inline u64 CreateSemaphoreExW(void* security, i32 initial, i32 maximum, const u16* name, u32 flags, u32 access) {
    char narrow[sync::OBJECT_NAME];
    if (name && !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return CreateSemaphoreExA(security, initial, maximum, name ? narrow : nullptr, flags, access);
}

PEHOST_WINAPI inline u64 OpenSemaphoreA(u32, i32, const char* name) { return objects::open(sync::OBJ_SEMAPHORE, name); }

PEHOST_WINAPI inline u64 OpenSemaphoreW(u32 access, i32 inherit, const u16* name) {
    char narrow[sync::OBJECT_NAME];
    if (!name || !objects::name_of_wide(name, narrow)) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return OpenSemaphoreA(access, inherit, narrow);
}

PEHOST_WINAPI inline i32 ReleaseSemaphore(u64 handle, i32 release, i32* previous) {
    sync::Host guard;
    sync::Object* o = objects::resolve(handle);
    if (!o || o->kind != sync::OBJ_SEMAPHORE) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (release <= 0) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (release > o->maximum - o->count) {
        set_last_error(ERROR_TOO_MANY_POSTS);
        return 0;
    }
    if (previous) *previous = o->count;
    o->count += release;
    objects::changed();
    return 1;
}

PEHOST_WINAPI inline u32 WaitForSingleObject(u64 handle, u32 milliseconds) { return objects::wait_many(1, &handle, false, milliseconds); }

PEHOST_WINAPI inline u32 WaitForSingleObjectEx(u64 handle, u32 milliseconds, i32) { return objects::wait_many(1, &handle, false, milliseconds); }

PEHOST_WINAPI inline u32 WaitForMultipleObjects(u32 count, const u64* handles, i32 all, u32 milliseconds) {
    return objects::wait_many(count, handles, all != 0, milliseconds);
}

PEHOST_WINAPI inline u32 WaitForMultipleObjectsEx(u32 count, const u64* handles, i32 all, u32 milliseconds, i32) {
    return objects::wait_many(count, handles, all != 0, milliseconds);
}

PEHOST_WINAPI inline i32 DuplicateHandle(u64 source_process, u64 source, u64 target_process, u64* target, u32, i32, u32 options) {
    if (source_process != CURRENT_PROCESS || target_process != CURRENT_PROCESS) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    sync::Host guard;
    i32 index = -1;
    if (source == CURRENT_THREAD) {
        index = objects::current_thread_object();
        if (index < 0) {
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
    } else if (objects::resolve(source, &index) == nullptr) {
        set_last_error(source == HANDLE_STDIN || source == HANDLE_STDOUT || source == HANDLE_STDERR || source == CURRENT_PROCESS ? ERROR_NOT_SUPPORTED : ERROR_INVALID_HANDLE);
        return 0;
    }
    if (target) {
        u64 handle = objects::handle_for(index);
        if (handle == 0) {
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        *target = handle;
    }
    if ((options & DUPLICATE_CLOSE_SOURCE) && source != CURRENT_THREAD) objects::close(source);
    return 1;
}

}

}
