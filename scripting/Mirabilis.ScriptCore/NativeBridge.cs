using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Mirabilis.ScriptCore;

public static unsafe partial class NativeBridge
{
    private const uint AbiVersion = 2;
    private static NativeApi native;
    private static int mainThread;
    private static bool initialized;
    private static ulong nextHandle;
    private static Dictionary<ulong, Instance> instances = [];
    private static readonly UTF8Encoding utf8 = new(false, true);

    private sealed class Instance(Script script, Dictionary<string, JsonElement> saved)
    {
        public readonly Script Script = script;
        public readonly Dictionary<string, JsonElement> Saved = saved;
        public readonly HashSet<string> Unbound = [];
        public bool Faulted, Created, Destroying;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status Initialize(NativeApi* api, ManagedApi* result)
    {
        try
        {
            if (api == null || result == null) return Status.InvalidArgument;
            *result = default;
            if (!ValidLayout() || System.Runtime.Loader.AssemblyLoadContext.GetLoadContext(typeof(Script).Assembly) != System.Runtime.Loader.AssemblyLoadContext.Default
                || api->Version != AbiVersion || api->Size != sizeof(NativeApi)
                || api->IsValid == null || api->IsPending == null || api->Resolve == null
                || api->GetTransform == null || api->SetTransform == null || api->GetName == null
                || api->SetName == null || api->Input == null || api->Log == null
                || api->BodyCommand == null || api->DestroyEntity == null) return Status.IncompatibleAbi;
            if (initialized && Environment.CurrentManagedThreadId != mainThread) return Status.WrongThread;
            if (instances.Count != 0) return Status.Failure;
            native = *api;
            mainThread = Environment.CurrentManagedThreadId;
            initialized = true;
            *result = new ManagedApi
            {
                Version = AbiVersion, Size = (uint)sizeof(ManagedApi), Describe = &Describe,
                Create = &Create, Invoke = &Invoke, Destroy = &Destroy, ReadFields = &ReadFields, LiveCount = &LiveCount, Reload = &Reload
            };
            Diagnostic("ScriptCore initialized in default AssemblyLoadContext; user assemblies use replaceable collectible Mirabilis.Scripts contexts.");
            return Status.Ok;
        }
        catch (Exception error) { Report("Initialize", null, error); return Status.Failure; }
    }

    private static bool ValidLayout()
    {
        static int Offset<T>(string name) => checked((int)Marshal.OffsetOf<T>(name));
        var pointer = IntPtr.Size;
        string[] nativePointers = [nameof(NativeApi.IsValid), nameof(NativeApi.IsPending), nameof(NativeApi.Resolve),
            nameof(NativeApi.GetTransform), nameof(NativeApi.SetTransform), nameof(NativeApi.GetName), nameof(NativeApi.SetName),
            nameof(NativeApi.Input), nameof(NativeApi.Log), nameof(NativeApi.BodyCommand), nameof(NativeApi.DestroyEntity)];
        string[] managedPointers = [nameof(ManagedApi.Describe), nameof(ManagedApi.Create), nameof(ManagedApi.Invoke),
            nameof(ManagedApi.Destroy), nameof(ManagedApi.ReadFields), nameof(ManagedApi.LiveCount), nameof(ManagedApi.Reload)];
        return nativePointers.Select((name, index) => Offset<NativeApi>(name) == 8 + (index + 1) * pointer).All(valid => valid)
            && managedPointers.Select((name, index) => Offset<ManagedApi>(name) == 8 + index * pointer).All(valid => valid)
            && sizeof(Entity) == 24 && Offset<Entity>(nameof(Entity.Epoch)) == 0
            && Offset<Entity>(nameof(Entity.Handle)) == 8 && Offset<Entity>(nameof(Entity.Reserved)) == 12
            && Offset<Entity>(nameof(Entity.PersistentId)) == 16 && sizeof(Vec3) == 12
            && Offset<Vec3>(nameof(Vec3.Y)) == 4 && Offset<Vec3>(nameof(Vec3.Z)) == 8
            && sizeof(Transform) == 36 && Offset<Transform>(nameof(Transform.Rotation)) == 12
            && Offset<Transform>(nameof(Transform.Scale)) == 24
            && sizeof(Utf8) == (pointer == 8 ? 16 : 8) && Offset<Utf8>(nameof(Utf8.Size)) == pointer
            && sizeof(NativeApi) == 8 + 12 * pointer && sizeof(ManagedApi) == 8 + 7 * pointer
            && Offset<NativeApi>(nameof(NativeApi.Version)) == 0 && Offset<NativeApi>(nameof(NativeApi.Size)) == 4
            && Offset<NativeApi>(nameof(NativeApi.Context)) == 8
            && Offset<ManagedApi>(nameof(ManagedApi.Version)) == 0 && Offset<ManagedApi>(nameof(ManagedApi.Size)) == 4;
    }

    private static Status ThreadStatus() => !initialized ? Status.IncompatibleAbi
        : Environment.CurrentManagedThreadId != mainThread ? Status.WrongThread : Status.Ok;
    private static void Check(Status status, string operation)
    {
        if (status != Status.Ok) throw new ScriptApiException(status, operation);
    }
    private static void CheckThread() => Check(ThreadStatus(), "Native scene access");
    private static string Decode(Utf8 value)
    {
        if (value.Data == null && value.Size != 0) throw new ArgumentException("Null UTF-8 buffer with nonzero size.");
        return utf8.GetString(new ReadOnlySpan<byte>(value.Data, checked((int)value.Size)));
    }
    private static Status WriteBuffer(string text, byte* output, uint capacity, uint* required)
    {
        if (required == null || (output == null && capacity != 0)) return Status.InvalidArgument;
        var bytes = utf8.GetBytes(text);
        *required = checked((uint)bytes.Length);
        if (capacity < bytes.Length) return Status.BufferTooSmall;
        bytes.CopyTo(new Span<byte>(output, bytes.Length));
        return Status.Ok;
    }
    private static void Diagnostic(string message)
    {
        try
        {
            if (!initialized) return;
            var bytes = utf8.GetBytes(message);
            fixed (byte* data = bytes) _ = native.Log(native.Context, new Utf8 { Data = data, Size = (uint)bytes.Length });
        }
        catch (Exception) { /* The diagnostic path must not escape an unmanaged entry point. */ }
    }
    private static void Report(string operation, Instance? instance, Exception error)
        => Diagnostic($"Script {instance?.Script.GetType().FullName ?? "<metadata>"} [{instance?.Script.Entity.ToString() ?? "no entity"}] {operation}: {error}");

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status Describe(Utf8 assembly, Utf8 typeName, byte* output, uint capacity, uint* required)
    {
        try
        {
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            var type = FindType(Decode(assembly), Decode(typeName));
            return WriteBuffer(DescribeFields(type), output, capacity, required);
        }
        catch (FileNotFoundException error) { Report("Describe", null, error); return Status.NotFound; }
        catch (TypeLoadException error) { Report("Describe", null, error); return Status.NotFound; }
        catch (Exception error) { Report("Describe", null, error); return Status.Failure; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status Create(Entity entity, Utf8 assembly, Utf8 typeName, Utf8 fields, ulong* handle)
    {
        string identity = "<invalid type>";
        try
        {
            if (handle == null) return Status.InvalidArgument;
            *handle = 0;
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            identity = $"{Decode(assembly)}::{Decode(typeName)}";
            if (entity.Reserved != 0) return Status.InvalidArgument;
            if (!IsValid(entity)) return Status.InvalidEntity;
            if (IsPending(entity)) return Status.PendingDestroy;
            var type = FindType(Decode(assembly), Decode(typeName));
            var saved = ParseFields(Decode(fields));
            if (nextHandle == ulong.MaxValue) return Status.Failure;
            var script = (Script)Activator.CreateInstance(type)!;
            script.Entity = entity;
            var instance = new Instance(script, saved);
            BindFields(instance);
            var id = ++nextHandle;
            instances.Add(id, instance);
            *handle = id;
            return Status.Ok;
        }
        catch (FileNotFoundException error) { Diagnostic($"Script {identity} [{entity}] Create: {error}"); return Status.NotFound; }
        catch (TypeLoadException error) { Diagnostic($"Script {identity} [{entity}] Create: {error}"); return Status.NotFound; }
        catch (ScriptApiException error) { Diagnostic($"Script {identity} [{entity}] Create: {error}"); return error.Status; }
        catch (Exception error) { Diagnostic($"Script {identity} [{entity}] Create: {error}"); return Status.Failure; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status Invoke(ulong handle, uint callback, float dt)
    {
        Instance? instance = null;
        try
        {
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            if (!instances.TryGetValue(handle, out instance)) return Status.NotFound;
            if (callback > 2 || !float.IsFinite(dt) || dt < 0) return Status.InvalidArgument;
            if (instance.Destroying || instance.Faulted) return Status.Faulted;
            if (!IsValid(instance.Script.Entity)) return Status.InvalidEntity;
            if (IsPending(instance.Script.Entity)) return Status.PendingDestroy;
            if (callback == 0)
            {
                if (instance.Created) return Status.InvalidArgument;
                instance.Created = true;
                instance.Script.OnCreate();
            }
            else
            {
                if (!instance.Created) return Status.InvalidArgument;
                if (callback == 1) instance.Script.OnUpdate(dt); else instance.Script.OnFixedUpdate(dt);
            }
            return Status.Ok;
        }
        catch (Exception error)
        {
            if (instance != null) instance.Faulted = true;
            Report($"callback {callback}", instance, error);
            return Status.Faulted;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status Destroy(ulong handle)
    {
        Instance? instance = null;
        try
        {
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            if (!instances.TryGetValue(handle, out instance)) return Status.NotFound;
            if (instance.Destroying) return Status.Ok;
            instance.Destroying = true;
            try { instance.Script.OnDestroy(); }
            finally { instances.Remove(handle); }
            return Status.Ok;
        }
        catch (Exception error) { Report("OnDestroy", instance, error); return Status.Faulted; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status ReadFields(ulong handle, byte* output, uint capacity, uint* required)
    {
        Instance? instance = null;
        try
        {
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            if (!instances.TryGetValue(handle, out instance)) return Status.NotFound;
            return WriteBuffer(CurrentFields(instance), output, capacity, required);
        }
        catch (Exception error)
        {
            if (instance != null) instance.Faulted = true;
            Report("ReadFields", instance, error); return Status.Faulted;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status LiveCount(uint* count)
    {
        try
        {
            if (count == null) return Status.InvalidArgument;
            *count = 0;
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            *count = checked((uint)instances.Count); return Status.Ok;
        }
        catch (Exception error) { Report("LiveCount", null, error); return Status.Failure; }
    }
}
