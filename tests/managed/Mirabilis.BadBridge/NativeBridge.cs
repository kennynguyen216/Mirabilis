using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Mirabilis.ScriptCore;
public static unsafe class NativeBridge
{
    [StructLayout(LayoutKind.Sequential)] private struct ManagedTable
    {
        public uint Version, Size;
        public void* Describe;
        public void* Create;
        public void* Invoke;
        public void* Destroy;
        public void* Read;
        public void* Count;
        public void* Reload;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint Unused() => 8;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static uint Initialize(void* native, void* output)
    {
        if (output == null) return 3;
        var table = (ManagedTable*)output;
        void* unused = (void*)(delegate* unmanaged[Cdecl]<uint>)&Unused;
        *table = new ManagedTable { Version = 2, Size = (uint)sizeof(ManagedTable), Describe = unused, Create = unused,
            Invoke = unused, Destroy = unused, Read = unused, Count = unused, Reload = unused };
        switch (Environment.GetEnvironmentVariable("MIRABILIS_BAD_MANAGED_ABI"))
        {
            case "version": table->Version = 1; break;
            case "size": table->Size = 8; break;
            case "null": table->Describe = null; break;
            default: return 3;
        }
        return 0;
    }
}

