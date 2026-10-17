using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Mirabilis.ScriptCore;

[StructLayout(LayoutKind.Sequential)]
public unsafe struct Utf8
{
    public byte* Data;
    public uint Size;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct NativeApi
{
    public uint Version, Size;
    public void* Context;
    public delegate* unmanaged[Cdecl]<void*, Entity, byte*, Status> IsValid;
    public delegate* unmanaged[Cdecl]<void*, Entity, byte*, Status> IsPending;
    public delegate* unmanaged[Cdecl]<void*, ulong, Entity*, Status> Resolve;
    public delegate* unmanaged[Cdecl]<void*, Entity, byte, Transform*, Status> GetTransform;
    public delegate* unmanaged[Cdecl]<void*, Entity, byte, Transform*, Status> SetTransform;
    public delegate* unmanaged[Cdecl]<void*, Entity, byte*, uint, uint*, Status> GetName;
    public delegate* unmanaged[Cdecl]<void*, Entity, Utf8, Status> SetName;
    public delegate* unmanaged[Cdecl]<void*, uint, uint, byte*, Status> Input;
    public delegate* unmanaged[Cdecl]<void*, Utf8, Status> Log;
    public delegate* unmanaged[Cdecl]<void*, Entity, uint, Vec3, Status> BodyCommand;
    public delegate* unmanaged[Cdecl]<void*, Entity, Status> DestroyEntity;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ManagedApi
{
    public uint Version, Size;
    public delegate* unmanaged[Cdecl]<Utf8, Utf8, byte*, uint, uint*, Status> Describe;
    public delegate* unmanaged[Cdecl]<Entity, Utf8, Utf8, Utf8, ulong*, Status> Create;
    public delegate* unmanaged[Cdecl]<ulong, uint, float, Status> Invoke;
    public delegate* unmanaged[Cdecl]<ulong, Status> Destroy;
    public delegate* unmanaged[Cdecl]<ulong, byte*, uint, uint*, Status> ReadFields;
    public delegate* unmanaged[Cdecl]<uint*, Status> LiveCount;
    public delegate* unmanaged[Cdecl]<Utf8, Status> Reload;
}
