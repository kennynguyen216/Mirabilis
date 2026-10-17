using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Mirabilis.ScriptCore;

internal static unsafe class Program
{
    [StructLayout(LayoutKind.Sequential)] private struct Ref { public ulong Epoch; public uint Handle, Reserved; public ulong Id; }
    [StructLayout(LayoutKind.Sequential)] private struct V3 { public float X, Y, Z; }
    [StructLayout(LayoutKind.Sequential)] private struct Trs { public V3 Position, Rotation, Scale; }
    [StructLayout(LayoutKind.Sequential)] private struct Text { public byte* Data; public uint Size; }
    [StructLayout(LayoutKind.Sequential)] private struct Native
    {
        public uint Version, Size;
        public void* Context;
        public delegate* unmanaged[Cdecl]<void*, Ref, byte*, uint> Valid, Pending;
        public delegate* unmanaged[Cdecl]<void*, ulong, Ref*, uint> Resolve;
        public delegate* unmanaged[Cdecl]<void*, Ref, byte, Trs*, uint> GetTransform;
        public delegate* unmanaged[Cdecl]<void*, Ref, byte, Trs*, uint> SetTransform;
        public delegate* unmanaged[Cdecl]<void*, Ref, byte*, uint, uint*, uint> GetName;
        public delegate* unmanaged[Cdecl]<void*, Ref, Text, uint> SetName;
        public delegate* unmanaged[Cdecl]<void*, uint, uint, byte*, uint> Input;
        public delegate* unmanaged[Cdecl]<void*, Text, uint> Log;
        public delegate* unmanaged[Cdecl]<void*, Ref, uint, V3, uint> Body;
        public delegate* unmanaged[Cdecl]<void*, Ref, uint> Destroy;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Managed
    {
        public uint Version, Size;
        public delegate* unmanaged[Cdecl]<Text, Text, byte*, uint, uint*, uint> Describe;
        public delegate* unmanaged[Cdecl]<Ref, Text, Text, Text, ulong*, uint> Create;
        public delegate* unmanaged[Cdecl]<ulong, uint, float, uint> Invoke;
        public delegate* unmanaged[Cdecl]<ulong, uint> Destroy;
        public delegate* unmanaged[Cdecl]<ulong, byte*, uint, uint*, uint> Read;
        public delegate* unmanaged[Cdecl]<uint*, uint> Count;
    }
    private static int assertions;
    private static string storedName = "";
    private static string logged = "";
    private static uint response;
    private static void Check(bool condition, string message) { ++assertions; if (!condition) throw new Exception(message); }
    private static uint Code(Status status) => (uint)status;
    private static void Throws(Action action, Status status)
    {
        try { action(); }
        catch (ScriptApiException error) { Check(error.Status == status, "Safe API failure status"); return; }
        throw new Exception("Expected safe API failure " + status);
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Valid(void* c, Ref e, byte* result) { if (result != null) *result = 1; return response; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Pending(void* c, Ref e, byte* result) { if (result != null) *result = 0; return response; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Resolve(void* c, ulong id, Ref* result) { if (result != null) *result = new Ref { Epoch = 7, Handle = 9, Id = id }; return response; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint GetTransform(void* c, Ref e, byte world, Trs* result) { if (result != null) *result = new Trs { Scale = new V3 { X = 1, Y = 1, Z = 1 } }; return response; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint SetTransform(void* c, Ref e, byte world, Trs* value) => response;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint GetName(void* c, Ref e, byte* buffer, uint capacity, uint* required)
    {
        if (response != 0) return response;
        byte[] bytes = Encoding.UTF8.GetBytes(storedName); *required = (uint)bytes.Length;
        if (capacity < bytes.Length) return Code(Status.BufferTooSmall);
        if (bytes.Length > 0) bytes.CopyTo(new Span<byte>(buffer, (int)capacity));
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint SetName(void* c, Ref e, Text text)
    {
        if (response != 0) return response;
        storedName = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(text.Data, checked((int)text.Size))); return 0;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Input(void* c, uint key, uint query, byte* result) { if (result != null) *result = 0; return response; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Log(void* c, Text text) { logged = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(text.Data, checked((int)text.Size))); return response; }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Body(void* c, Ref e, uint command, V3 value) => response;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])] private static uint Destroy(void* c, Ref e) => response;
    private static uint Create(ref Managed api, string type, string fields, out ulong result)
    {
        byte[] assembly = Encoding.UTF8.GetBytes("Mirabilis.TestScripts"), name = Encoding.UTF8.GetBytes("Mirabilis.TestScripts." + type), json = Encoding.UTF8.GetBytes(fields);
        ulong handle = 0; uint status;
        fixed (byte* a = assembly, n = name, j = json)
            status = api.Create(new Ref { Epoch = 7, Handle = 9, Id = 3 }, new Text { Data = a, Size = (uint)assembly.Length }, new Text { Data = n, Size = (uint)name.Length }, new Text { Data = j, Size = (uint)json.Length }, &handle);
        result = handle; return status;
    }
    private static string Read(ref Managed api, ulong handle)
    {
        uint required = 0;
        Check(api.Read(handle, null, 0, &required) == Code(Status.BufferTooSmall) && required > 0, "Field getter reports required length");
        byte[] bytes = new byte[required];
        fixed (byte* buffer = bytes) Check(api.Read(handle, buffer, required, &required) == 0, "Field getter fills caller-owned buffer");
        return Encoding.UTF8.GetString(bytes);
    }
    private static void RealInstances(ref Managed managed)
    {
        foreach (string phase in new[] { "", "create", "update", "fixed", "destroy" })
        {
            uint count = 99;
            string fields = "{\"Fail\":{\"type\":\"string\",\"value\":\"" + phase + "\"}}";
            uint created = Create(ref managed, "LifecycleProbe", fields, out ulong handle);
            Check(handle != 0, "Successful construction roots instance even if OnCreate will fail: status=" + created + " log=" + logged);
            Check(managed.Count(&count) == 0 && count == 1, "Managed handle count after construction");
            uint createResult = managed.Invoke(handle, 0, 0);
            Check((createResult == 0) == (phase != "create"), "OnCreate exception boundary");
            string before = Read(ref managed, handle);
            if (phase == "create")
            {
                Check(managed.Invoke(handle, 1, .01f) != 0 && managed.Invoke(handle, 2, .01f) != 0, "Faulted OnCreate suppresses later updates");
                Check(Read(ref managed, handle) == before, "Faulted instance fields stay unchanged");
            }
            else
            {
                uint update = managed.Invoke(handle, 1, .01f);
                Check((update == 0) == (phase != "update"), "OnUpdate exception boundary");
                if (phase != "update") Check((managed.Invoke(handle, 2, .01f) == 0) == (phase != "fixed"), "OnFixedUpdate exception boundary");
            }
            uint destroyed = managed.Destroy(handle);
            Check((destroyed == 0) == (phase != "destroy"), "OnDestroy exception boundary");
            Check(managed.Count(&count) == 0 && count == 0, "Every final cleanup releases handle including faults");
            Check(managed.Destroy(handle) != 0, "Final cleanup runs once");
        }
        Check(Create(ref managed, "ConstructorThrows", "{}", out ulong failed) != 0 && failed == 0, "Constructor failure never roots callable instance");
        Check(Create(ref managed, "AbsentType", "{}", out failed) != 0 && failed == 0, "Missing type never roots instance");
        uint remaining = 1; Check(managed.Count(&remaining) == 0 && remaining == 0, "Failure paths leak no handles");
    }

    private static int Main()
    {
        try
        {
            Check(Marshal.SizeOf<Entity>() == 24, "Entity size");
            Check(Marshal.OffsetOf<Entity>(nameof(Entity.Epoch)).ToInt32() == 0, "Epoch offset");
            Check(Marshal.OffsetOf<Entity>(nameof(Entity.Handle)).ToInt32() == 8, "Handle offset");
            Check(Marshal.OffsetOf<Entity>(nameof(Entity.Reserved)).ToInt32() == 12, "Reserved offset");
            Check(Marshal.OffsetOf<Entity>(nameof(Entity.PersistentId)).ToInt32() == 16, "PersistentId offset");
            Check(Marshal.SizeOf<Vec3>() == 12 && Marshal.SizeOf<Transform>() == 36, "Vector/TRS size");
            Check(Marshal.OffsetOf<Transform>(nameof(Transform.Rotation)).ToInt32() == 12 && Marshal.OffsetOf<Transform>(nameof(Transform.Scale)).ToInt32() == 24, "TRS offsets");
            Check(Marshal.SizeOf<NativeApi>() == sizeof(Native) && sizeof(Native) == 104, "Native table layout");
            Check(Marshal.SizeOf<ManagedApi>() == sizeof(Managed) && sizeof(Managed) == 56, "Managed table layout");
            Check(Marshal.SizeOf<Utf8>() == sizeof(Text) && sizeof(Text) == 16, "UTF8 pointer alignment");
            Check(Enum.GetUnderlyingType(typeof(Status)) == typeof(uint), "Status fixed width");
            var method = typeof(NativeBridge).GetMethod("Initialize", BindingFlags.Static | BindingFlags.Public)!;
            Check(method.GetCustomAttribute<UnmanagedCallersOnlyAttribute>() != null, "UCO entry");
            var initialize = (delegate* unmanaged[Cdecl]<Native*, Managed*, uint>)method.MethodHandle.GetFunctionPointer();
            Native native = new() { Version = 1, Size = (uint)sizeof(Native), Valid = &Valid, Pending = &Pending, Resolve = &Resolve,
                GetTransform = &GetTransform, SetTransform = &SetTransform, GetName = &GetName, SetName = &SetName,
                Input = &Input, Log = &Log, Body = &Body, Destroy = &Destroy };
            Managed managed = default;
            Check(initialize(null, &managed) != 0, "Null native table");
            native.Version = 2; Check(initialize(&native, &managed) == Code(Status.IncompatibleAbi), "Bad ABI version"); native.Version = 1;
            native.Size--; Check(initialize(&native, &managed) == Code(Status.IncompatibleAbi), "Truncated table"); native.Size++;
            var saved = native.Destroy; native.Destroy = null;
            Check(initialize(&native, &managed) == Code(Status.IncompatibleAbi), "Missing native function"); native.Destroy = saved;
            Check(initialize(&native, null) != 0, "Null managed output");
            Check(initialize(&native, &managed) == 0, "Valid ABI initialization");
            Check(managed.Version == 1 && managed.Size == sizeof(Managed), "Returned ABI header");
            Check(managed.Describe != null && managed.Create != null && managed.Invoke != null && managed.Destroy != null && managed.Read != null && managed.Count != null, "Complete managed table");
            uint count = 99; Check(managed.Count(&count) == 0 && count == 0, "No handles at startup");
            Check(managed.Count(null) != 0, "Null count output");
            uint required = 0; Check(managed.Describe(new Text { Size = 1 }, default, null, 0, &required) != 0, "Null UTF8 input with nonzero length");
            ulong handle = 123; Check(managed.Create(default, default, default, default, &handle) != 0 && handle == 0, "Failed creation releases partial handle");
            Check(managed.Invoke(ulong.MaxValue, 0, 0) != 0, "Stale instance invoke");
            Check(managed.Destroy(ulong.MaxValue) != 0, "Stale instance destroy");
            Check(managed.Read(ulong.MaxValue, null, 0, &required) != 0, "Stale instance field read");
            Check(managed.Count(&count) == 0 && count == 0, "No leaked handles after failures");
            var entity = new Entity(7, 9, 3);
            foreach (string text in new[] { "", "café漢🙂", "before\0after" })
            {
                entity.Name = text; Check(entity.Name == text && storedName == text, "Borrowed UTF8 roundtrip");
                Mirabilis.ScriptCore.Log.Write(text); Check(logged == text, "UTF8 log roundtrip");
            }
            response = Code(Status.InvalidEntity); Check(!entity.IsValid && !entity.IsPendingDestroy, "Safe stale booleans");
            foreach (Status status in new[] { Status.OutsideCallback, Status.WrongThread, Status.PendingDestroy, Status.InvalidArgument })
            {
                response = Code(status); Throws(() => _ = entity.Name, status); Throws(() => entity.Name = "reject", status);
                Throws(() => entity.RigidBody.AddImpulse(new Vec3(1, 2, 3)), status); Throws(() => entity.Destroy(), status);
            }
            response = 0;
            RealInstances(ref managed);
            Console.WriteLine($"Managed ABI checks passed: {assertions} assertions"); return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}





