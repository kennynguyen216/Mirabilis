namespace Mirabilis.ScriptCore;

public static unsafe partial class NativeBridge
{
    internal static bool IsValid(Entity entity)
    {
        CheckThread(); byte value = 0;
        var status = native.IsValid(native.Context, entity, &value);
        if (status is Status.InvalidEntity or Status.NotFound) return false;
        Check(status, "Entity.IsValid"); return value != 0;
    }
    internal static bool IsPending(Entity entity)
    {
        CheckThread(); byte value = 0;
        var status = native.IsPending(native.Context, entity, &value);
        if (status is Status.InvalidEntity or Status.NotFound) return false;
        Check(status, "Entity.IsPendingDestroy"); return value != 0;
    }
    internal static Entity Resolve(ulong persistentId)
    {
        CheckThread(); Entity entity = default;
        var status = native.Resolve(native.Context, persistentId, &entity);
        if (status is Status.InvalidEntity or Status.NotFound) return default;
        Check(status, "Entity.Resolve"); return entity;
    }
    internal static Transform GetTransform(Entity entity, bool world)
    {
        CheckThread(); Transform value = default;
        Check(native.GetTransform(native.Context, entity, (byte)(world ? 1 : 0), &value), "Transform.Get");
        return value;
    }
    internal static void SetTransform(Entity entity, bool world, Transform value)
    {
        CheckThread();
        Check(native.SetTransform(native.Context, entity, (byte)(world ? 1 : 0), &value), "Transform.Set");
    }
    internal static string GetName(Entity entity)
    {
        CheckThread(); uint required = 0;
        var status = native.GetName(native.Context, entity, null, 0, &required);
        if (status != Status.BufferTooSmall) Check(status, "Name.Get");
        var bytes = new byte[checked((int)required)];
        fixed (byte* data = bytes)
        {
            Check(native.GetName(native.Context, entity, data, (uint)bytes.Length, &required), "Name.Get");
            if (required > bytes.Length) throw new ScriptApiException(Status.Failure, "Name.Get returned an invalid length");
            return utf8.GetString(bytes.AsSpan(0, (int)required));
        }
    }
    internal static void SetName(Entity entity, string value)
    {
        CheckThread(); ArgumentNullException.ThrowIfNull(value);
        var bytes = utf8.GetBytes(value);
        fixed (byte* data = bytes)
            Check(native.SetName(native.Context, entity, new Utf8 { Data = data, Size = (uint)bytes.Length }), "Name.Set");
    }
    internal static bool QueryInput(Key key, uint query)
    {
        CheckThread(); byte value = 0;
        Check(native.Input(native.Context, (uint)key, query, &value), "Input"); return value != 0;
    }
    internal static void WriteLog(string message)
    {
        CheckThread(); ArgumentNullException.ThrowIfNull(message);
        var bytes = utf8.GetBytes(message);
        fixed (byte* data = bytes)
            Check(native.Log(native.Context, new Utf8 { Data = data, Size = (uint)bytes.Length }), "Log");
    }
    internal static void BodyCommand(Entity entity, uint command, Vec3 value)
    {
        CheckThread(); Check(native.BodyCommand(native.Context, entity, command, value), "RigidBody");
    }
    internal static void DestroyEntity(Entity entity)
    {
        CheckThread(); Check(native.DestroyEntity(native.Context, entity), "DestroyEntity");
    }
}
