# C# scripts

Mirabilis hosts framework-dependent .NET 10 through the official `nethost` and
`hostfxr` APIs. Building requires a .NET 10 SDK; running requires the x64
`Microsoft.NETCore.App` 10 runtime. The verified environment uses SDK **10.0.203**,
runtime **10.0.7**, and host pack **10.0.7**. .NET is MIT licensed. The SDK and
runtime are installed prerequisites, with no NuGet package dependencies or
configure-time downloads.

From a checkout **outside OneDrive**, build both configurations:

```powershell
dotnet build scripting/Mirabilis.SampleScripts/Mirabilis.SampleScripts.csproj -c Debug --ignore-failed-sources -p:NuGetAudit=false
dotnet build scripting/Mirabilis.SampleScripts/Mirabilis.SampleScripts.csproj -c Release --ignore-failed-sources -p:NuGetAudit=false
```

Keep both build output and intermediate directories outside OneDrive. If using
the OneDrive checkout, pass `--artifacts-path C:/dev/mirabilis-managed` instead.
After one restore, `--no-restore` builds entirely offline. Restore needs no
external feeds because these projects only reference the installed framework.

Deploy these generated files beside `engine.exe` (and beside a headless test
executable that hosts scripts), choosing one matching configuration:

| Build directory | Files |
| --- | --- |
| `scripting/Mirabilis.ScriptCore/bin/<configuration>/net10.0` | `Mirabilis.ScriptCore.dll`, `Mirabilis.ScriptCore.deps.json`, `Mirabilis.ScriptCore.runtimeconfig.json` |
| `scripting/Mirabilis.SampleScripts/bin/<configuration>/net10.0` | `Mirabilis.SampleScripts.dll`, `Mirabilis.SampleScripts.deps.json` |

Copy PDBs for useful source lines in exception stack traces. Copy every dependency
of a user assembly beside it as well. The core class library explicitly generates
its runtimeconfig; the native host uses that file to select .NET 10. No runtime
is embedded into the engine. An unavailable/incompatible runtime disables scripting
with a diagnostic, retaining scene components and saved fields.

The native hosting headers, `nethost.lib` and `nethost.dll` come from the installed
host pack, for example
`C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/10.0.7/runtimes/win-x64/native`.
Ship `nethost.dll` beside the executable. `hostfxr.dll` is located by `nethost` in
the installed .NET distribution and is not copied from an arbitrary SDK version.

## Write a script

Create a `net10.0` class library referencing `Mirabilis.ScriptCore.csproj`, or
the deployed `Mirabilis.ScriptCore.dll`. Use a public concrete class with a
public parameterless constructor:

```csharp
using Mirabilis.ScriptCore;

namespace MyGame;

public sealed class PushBox : Script
{
    public Vec3 Force = new(5, 0, 0);
    public Vec3 Impulse = new(0, 4, 0);
    public Entity Target;

    public override void OnCreate() => Log.Write($"Started {Entity.Name}");

    public override void OnFixedUpdate(float dt)
    {
        Entity.RigidBody.AddForce(Force);
        if (Input.IsKeyPressed(Key.Space)) Entity.RigidBody.AddImpulse(Impulse);
        if (Input.IsKeyPressed(Key.E) && Target.IsValid)
            Target.RigidBody.AddImpulse(Impulse);
    }
}
```

Build and deploy the assembly and its dependencies. Attach it in the Edit-mode
inspector using its assembly identity (`MyGame`, or its full assembly display
name) and full case-sensitive type name (`MyGame.PushBox`). Assembly identities
are not file paths. Assemblies must sit beside ScriptCore. The bundled assembly
is `Mirabilis.SampleScripts`, with `Mirabilis.SampleScripts.PushBox`,
`Mirabilis.SampleScripts.MoveAndSpin` and `Mirabilis.SampleScripts.BodyInteraction`.

The inspector exposes writable public instance fields of exactly `float`, `int`,
`bool`, `string`, `Vec3`, and `Entity`. Other types, static fields, and readonly
fields are excluded. Metadata discovery never constructs a user type and shows
zero/empty/null metadata defaults. Actual field initializers run during instance
construction; existing saved values override matching initializers.

Entities store the scene epoch, full versioned EnTT value, and persistent ID.
`Entity.Resolve(id)` resolves an ID in the current scene, returning `default` if
absent. `default(Entity).IsValid` is false. Do not use ID zero as a null sentinel;
it is a valid saved ID. Entity fields save a persistent ID or JSON null and bind
to fresh runtime identities when instances are created. Old references become
stale after scene replacement/reset. Missing targets retain their original saved
ID with a binding diagnostic. Removed, renamed, changed-type, and unknown fields
retain their raw saved JSON until explicitly edited; no implicit deletion occurs.

## API and callbacks

`Entity.Name`, `LocalTransform` and `WorldTransform` get/set scene state. Since
Entity is a value type, copy `Script.Entity` before using a property setter:

```csharp
var self = Entity;
var transform = self.LocalTransform;
transform.Rotation += new Vec3(0, dt, 0);
self.LocalTransform = transform;
self.Name = "Moving box";
```

`Transform` contains `Position`, `Rotation`, and `Scale`; `Transform.Identity`
sets unit scale. Rotations use Euler radians with the engine's Y*X*Z convention.
`Vec3` contains X/Y/Z floats. Body `AddForce`, `AddImpulse`, `SetVelocity` and
`SetTransform` enqueue commands. Body `SetTransform` uses world coordinates.
Transform setters on dynamic bodies during play also enqueue teleports; reads
see the prior authoritative pose until the fixed step applies them. Other writes
mark the existing transform-dirty path. The native engine enforces protections
for owned entities and validates finite transforms and physics values.

Scripts run only in Play. `OnCreate()` runs once after construction and field
binding. Every fixed tick runs `OnFixedUpdate(dt)` before queued commands and
physics. `OnUpdate(dt)` runs once per rendered frame. `OnDestroy()` runs once
when an instance retires, including detach, replacement, deletion, leave/reset
Play and shutdown. Constructor failure creates no rooted instance and gets no
`OnDestroy`. Callback exceptions log type, full entity identity and stack trace,
disable future updates, and retain the instance until cleanup attempts
`OnDestroy`. Exceptions in cleanup still release its root.

`Entity.Destroy()` requests deferred destruction. Pending entities remain valid
and readable, including during `OnDestroy`, while mutations fail. Commands to
pending entities are discarded. Cleanup may queue mutations to other live
entities for the next fixed tick. Teardown discards those commands before changing
epochs. Never retain a background thread that accesses scene state: native calls
require the engine thread and an active script callback. `Log.Write` also permits
host/metadata diagnostics outside a callback on that thread.

`IsValid`/`IsPendingDestroy` return false for stale/missing entities; `Resolve`
returns default for a missing target. Other native failures throw
`ScriptApiException`, whose `Status` names invalid arguments, pending destruction,
wrong thread, outside callback, or other boundary failures. A script may catch
this exception; otherwise the callback fault rule applies. Strings are UTF-8
with explicit byte lengths, including empty, non-ASCII and embedded-NUL values;
neither runtime frees the other's memory.

Input supports W/A/S/D, Space, Control, Shift, E/F/Q/R, Escape, Enter, and arrows.
`Input.IsKeyDown`, `IsKeyPressed` and `IsKeyReleased` expose frame edges during
`OnUpdate`, and separate fixed edges during `OnFixedUpdate`. All scripts see an
edge on the first eligible fixed tick; later fixed ticks do not repeat it. Edges
survive zero-tick frames, and a same-frame press/release preserves both flags.
The API coalesces repeated edges, rather than supplying an event queue.

ScriptCore and its Cdecl `UnmanagedCallersOnly` bridge stay in the default
`AssemblyLoadContext`. User assemblies load once into the collectible
`Mirabilis.Scripts` context, sharing the default ScriptCore identity. Instance
roots are released at cleanup; the context and .NET runtime remain alive. There
is no hot reload/unload workflow: restart the process to use rebuilt assemblies.
