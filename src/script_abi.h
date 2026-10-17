#pragma once
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
#define MIRABILIS_SCRIPT_CALL __cdecl
#else
#define MIRABILIS_SCRIPT_CALL
#endif

namespace mirabilis::scripting {
inline constexpr uint32_t AbiVersion = 1;
enum class Status : uint32_t {
    Ok, InvalidEntity, PendingDestroy, InvalidArgument, WrongThread,
    OutsideCallback, IncompatibleAbi, NotFound, Failure, Faulted, BufferTooSmall
};
struct Entity { uint64_t epoch; uint32_t entity; uint32_t reserved; uint64_t persistentId; };
struct Vec3 { float x,y,z; };
struct Trs { Vec3 position,rotation,scale; };
struct Utf8 { const uint8_t* data; uint32_t size; };
enum class Callback : uint32_t { Create, Update, FixedUpdate };
enum class Key : uint32_t { W,A,S,D,Space,Control,Shift,E,F,Q,R,Escape,Enter,Up,Down,Left,Right,Count };
enum class InputQuery : uint32_t { Down,Pressed,Released };
enum class BodyCommand : uint32_t { Force,Impulse,Velocity };

struct NativeApi {
    uint32_t version,size;
    void* context;
    Status (MIRABILIS_SCRIPT_CALL *is_valid)(void*,Entity,uint8_t*);
    Status (MIRABILIS_SCRIPT_CALL *is_pending)(void*,Entity,uint8_t*);
    Status (MIRABILIS_SCRIPT_CALL *resolve)(void*,uint64_t,Entity*);
    Status (MIRABILIS_SCRIPT_CALL *get_transform)(void*,Entity,uint8_t,Trs*);
    Status (MIRABILIS_SCRIPT_CALL *set_transform)(void*,Entity,uint8_t,const Trs*);
    Status (MIRABILIS_SCRIPT_CALL *get_name)(void*,Entity,uint8_t*,uint32_t,uint32_t*);
    Status (MIRABILIS_SCRIPT_CALL *set_name)(void*,Entity,Utf8);
    Status (MIRABILIS_SCRIPT_CALL *input)(void*,uint32_t,uint32_t,uint8_t*);
    Status (MIRABILIS_SCRIPT_CALL *log)(void*,Utf8);
    Status (MIRABILIS_SCRIPT_CALL *body_command)(void*,Entity,uint32_t,Vec3);
    Status (MIRABILIS_SCRIPT_CALL *destroy_entity)(void*,Entity);
};
struct ManagedApi {
    uint32_t version,size;
    Status (MIRABILIS_SCRIPT_CALL *describe)(Utf8,Utf8,uint8_t*,uint32_t,uint32_t*);
    Status (MIRABILIS_SCRIPT_CALL *create)(Entity,Utf8,Utf8,Utf8,uint64_t*);
    Status (MIRABILIS_SCRIPT_CALL *invoke)(uint64_t,uint32_t,float);
    Status (MIRABILIS_SCRIPT_CALL *destroy)(uint64_t);
    Status (MIRABILIS_SCRIPT_CALL *read_fields)(uint64_t,uint8_t*,uint32_t,uint32_t*);
    Status (MIRABILIS_SCRIPT_CALL *live_count)(uint32_t*);
};
using Initialize = Status (MIRABILIS_SCRIPT_CALL *)(const NativeApi*,ManagedApi*);
static_assert(sizeof(Entity)==24 && offsetof(Entity,epoch)==0 && offsetof(Entity,entity)==8 && offsetof(Entity,reserved)==12 && offsetof(Entity,persistentId)==16);
static_assert(sizeof(Vec3)==12 && sizeof(Trs)==36);
static_assert(offsetof(Vec3,x)==0 && offsetof(Vec3,y)==4 && offsetof(Vec3,z)==8);
static_assert(offsetof(Trs,position)==0 && offsetof(Trs,rotation)==12 && offsetof(Trs,scale)==24);
static_assert(offsetof(Utf8,data)==0 && offsetof(Utf8,size)==sizeof(void*) && sizeof(Utf8)==(sizeof(void*)==8 ? 16 : 8));
static_assert(offsetof(NativeApi,version)==0 && offsetof(NativeApi,size)==4 && offsetof(NativeApi,context)==8);
static_assert(sizeof(NativeApi)==8+12*sizeof(void*) && offsetof(NativeApi,is_valid)==8+sizeof(void*) && offsetof(NativeApi,destroy_entity)==8+11*sizeof(void*));
static_assert(sizeof(ManagedApi)==8+6*sizeof(void*) && offsetof(ManagedApi,describe)==8 && offsetof(ManagedApi,live_count)==8+5*sizeof(void*));
}
