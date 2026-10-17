# Milestone 3 frozen contracts

CURRENT MILESTONE 3, overriding the downloaded prompt's stale 1. Base is reviewed
physics tip e46908d3e10cd17276a2410c6b597a8ae05123cb. Preserve every M1/M2 rule,
body/cache behavior and authored file. No push/main merge, hot reload, player/body
interaction, body portal traversal, raycasts, joints or new collider shapes.

## Saved data and ownership

Optional `Script` in scene_components.h is saved/snapshotted/duplicated, with
assembly identity, full typeName, enabled and ordered map fields. Each ScriptField
has type string and valueJson containing an owned JSON value. Known types are
`float`, `int`, `bool`, `string`, `vec3`, `entity`; values are finite float,
int32, bool, UTF-8 string, three finite floats, or null/uint64 persistent ID.
Unknown field types and fields which fail managed binding retain their JSON and
get an inspector diagnostic; no implicit deletion. Missing assemblies/types
retain the entire component. `script` v2 block uses assembly/type/enabled/fields;
fields object maps name to `{type,value}`. Strictly validate known types in staging
and before save. Duplicate remaps only entity-valued fields inside the copied
subtree; null, ID0 and external references remain distinct. Runtime handles,
faults and diagnostics are never saved. Core serialization provides
serialize_script_fields/parse_script_fields; caller buffers own all JSON.

Lead owns components, Scene, script_abi.h, script_host.h/.cpp, engine main/header,
vk_engine_input.cpp (reassigned from default lead ownership), all CMake, the new
scripted demo and report. Managed specialist owns everything under scripting/,
including Mirabilis.ScriptCore, Mirabilis.SampleScripts and author guide
docs/scripting.md (explicit reassignment); no CMake edits. Editor specialist owns
engine editor/scene IO, core scene serialization header/cpp and scene_format.md.
Validation owns tests/fixtures/runners and writes from contracts/spec/public
headers without reading implementation. Shared declaration changes remain lead
owned and are communicated before use. Workers branch from this commit.

## ABI

src/script_abi.h is the exact, ordered Cdecl ABI v1; managed mirror must match all
sizes/offsets and table sizes. Status is uint32, flags uint8. Entity is exactly
24 bytes (epoch0/entity8/reserved12/persistentId16), reserved=0. Vec3 is12 and
TRS is36, position/rotation/scale with Euler radians Y*X*Z. Utf8 is pointer+uint32
length, explicitly Sequential with native pointer alignment. Null pointer is
allowed only at length0. Strings may contain NUL/non-ASCII, no implicit terminator.
Getters return BufferTooSmall with required byte length, then fill caller-owned
buffers; no cross-runtime allocation/free. NativeApi starts version,size,context
followed by function pointers; ManagedApi starts version,size. Reject version,
size, null functions or layouts before script callbacks. All entrypoints catch
exceptions; defined failure statuses never unwind across the boundary.

Bridge type is Mirabilis.ScriptCore.NativeBridge in Mirabilis.ScriptCore.dll,
static Initialize marked UnmanagedCallersOnly(Cdecl), signature Initialize in
script_abi.h. Native loads it with official nethost/hostfxr APIs and the generated
Mirabilis.ScriptCore.runtimeconfig.json, framework-dependent net10.0. Bridge and
API stay in default ALC; user/sample assemblies load once in a collectible ALC,
sharing default ScriptCore identity. No reload/unload implementation. SDK10 builds;
runtime10 runs; record actual SDK/host pack versions. Deploy assemblies, deps,
runtimeconfig and nethost.dll beside engine and headless test executables. All
outputs/intermediates outside OneDrive; no configure-time downloads.

NativeApi functions check main thread and live callback scope before Scene access,
then full Entity identity/reserved word and pending/read/mutation permissions.
is_valid/is_pending return defined safe false/error on stale. resolve binds saved
IDs to the current epoch. local/world get_transform uses existing best-fit world
decomposition; set_transform builds Y*X*Z matrix and routes through Scene ownership,
so Dynamic Play writes enqueue teleports. Body commands enqueue through PhysicsWorld.
DestroyEntity delegates to deferred public Scene destroy, preserving native-owned
and portal-host protections. Wrong thread/outside callback fails before touching
the registry. Native log may report host/metadata diagnostics without Scene access;
other functions require a script callback. Log text includes script type/entity/
stack trace on managed failures.

ManagedApi describe returns fields in the saved fields JSON shape with zero/null
defaults for editable public instance fields only. It does not construct user
objects. Actual initializers apply at instance creation when no saved value exists.
Create constructs and binds Entity and matching saved fields, rooting only a
successfully constructed instance under an opaque uint64 handle. A failed ctor
returns failure and no callable instance/OnDestroy. Invoke Create/Update/FixedUpdate
catches user exceptions, logs context/stack, faults that instance, and suppresses
future updates. Destroy attempts OnDestroy once even when faulted and releases
the handle in finally. read_fields returns current bindable fields while retaining
orphan fields in native saved data. All table exports are static UCO Cdecl.

## Lifecycle, fields and input

ScriptHost binds Scene/PhysicsWorld, initializes lazily when a Script is used,
keeps .NET alive until process exit and has start/reconcile/stop/shutdown methods.
No Script entities means no runtime initialization and minimal no-script overhead.
Initialization failure disables scripting with clear diagnostics while native
scene/editor/body behavior and Script data remain available.

Scene cleanup_script invokes the registered handler under its existing
_inDestroyCallback guard. It runs before Script detach/replacement and before
physics removal/entity destruction. ScriptHost owns runtime handles/faults only;
scene Script fields own saved/current field values. Host updates runtime field
values through owner edit after callbacks without triggering detach/recreate.
stop destroys every instance before snapshot restore/replacement, while registry
and physics remain readable. Teardown-generated commands are discarded before
epoch change/new callbacks; normal deferred cleanup queues writes to other live
entities for next tick. Callback iteration revalidates refs/pending state every
time; pending gets no update. Disabled/replaced/detached components retire instances.
OnCreate self-destruction still retains instance until normal deferred teardown.
Snapshot captures authored Script data before instances start. Reset/leave Play
restores it and rebinds entity fields to the new epoch; runtime-only protections
and allocator high-water stay as M1/M2.

Order: script fixed callbacks; apply initial command batch; unchanged player;
sync dirty bodies; Jolt; ECS writeback; drain destruction. Per frame: script
OnUpdate; drain; final physics flush; interpolation/render. No new clock. Script
metadata/saved configuration edits only in Edit; read-only Play inspector.

ScriptInput's fixed key enum is in script_abi.h. event accumulates separate frame
and fixed press/release bits, ignores repeats and preserves same-frame press+release.
Held levels are shared. finish_frame clears only frame edges; finish_fixed_tick
clears only fixed edges AFTER all scripts observe them. Fixed edges survive zero
tick frames but never enter later frame edges. clear resets all on enter/leave/
reset/replacement. focus_lost drops pending presses, releases held keys into both
latches, and clears held levels. SDL capture gates new presses; releases are still
processed to avoid stuck keys; losing mouse/UI capture releases held game input.

EngineStats script_instance_count and MIRABILIS_TEST_SCRIPT_* hooks report actual
live handles. Headless tests validate host/boundary/lifecycle without Vulkan;
existing GPU harness verifies renderer/portal/cache preservation. Validation must
exercise real managed callbacks rather than native mocks alone. Fresh independent
GPT-6 Astra High reviewer starts after workers retire at the gate.
