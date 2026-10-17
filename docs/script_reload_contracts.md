# Script reload extension

The user explicitly authorizes hot reload after completed Milestone 3, overriding
that milestone's no-reload scope. Work stays local on feature/script-hot-reload.
The lead owns this extension serially; preserve the user's modified demo and
.last_scene byte-for-byte. Builds and scratch evidence stay outside OneDrive.

Reload runs on the main thread between frames, never inside a callback. Compile
user projects under scripting/ asynchronously into a separate generation directory when their source
changes, with a manual Build & Reload action too. Compilation failures leave the
current instances running. Never deploy over loaded ScriptCore or native binaries.

Keep ScriptCore/default context and the .NET runtime alive. Reload only user
assemblies in a fresh collectible context, sharing the same Core identity.
Prepare every replacement before swapping any live handle. Missing types, invalid
assemblies and constructor failures reject the generation and retain old scripts.
Constructors cannot mutate native scene state during preparation. Native opaque
handles remain stable, faulted scripts can recover, and no OnCreate/OnDestroy is
called merely because code was reloaded. These remain entity/Play lifecycle hooks.

Preserve writable, nonstatic, non-[NonSerialized] instance fields of the existing
six supported types, including private/inherited fields and auto-property backing
fields. Match declaring type, field name and field type. Added fields keep their
new initializer; removed/type-changed authored fields remain in saved Script data.
Statics reset with the user context. Unsupported runtime object graphs/threads
are outside the current script API. Do not serialize runtime private fields into
scene files. Existing scene snapshots and authored public fields stay authoritative
for later Reset/leave Play. ScriptCore/native API changes still need restart.
Reload validates saved data without rebinding it by public name; only the strict
runtime field match assigns values. Carry prior unbound-field bookkeeping through
reload so unresolved saved entity IDs survive until normal instance creation.

No scene restore, epoch change, body recreation, velocity reset, pending-command
discard or interpolation/history reset occurs during reload. Preserve all engine
state and normal queued commands. Suppress native mutations during preparation,
while reads and logging remain available. Clear metadata/error cache after success.

ABI v2 appends a managed reload entry taking a UTF-8 generation directory. Update
native/managed mirrors, layout assertions and rejection fixtures together. Every
boundary catches exceptions. Track old contexts weakly; bounded collection checks
must prevent repeated reloads retaining an unbounded sequence of contexts.

Validate two compiled versions with the same identity: new behavior, retained
public/private/reference fields, unchanged live physics/epoch/queues/snapshot,
compile/load/constructor failures retaining old behavior, missing/changed fields,
fault recovery, repeated unloads and actual engine source-edit/build/reload while
Play continues. Run existing native/managed/Python regressions and both builds.
