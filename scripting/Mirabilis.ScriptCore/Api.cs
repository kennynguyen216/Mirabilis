using System.Runtime.InteropServices;

namespace Mirabilis.ScriptCore;

[StructLayout(LayoutKind.Sequential)]
public struct Vec3(float x, float y, float z)
{
    public float X = x, Y = y, Z = z;
    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator *(Vec3 a, float scale) => new(a.X * scale, a.Y * scale, a.Z * scale);
}

[StructLayout(LayoutKind.Sequential)]
public struct Transform
{
    public Vec3 Position, Rotation, Scale;
    public static Transform Identity => new() { Scale = new(1, 1, 1) };
}

[StructLayout(LayoutKind.Sequential)]
public readonly struct Entity : IEquatable<Entity>
{
    public readonly ulong Epoch;
    public readonly uint Handle;
    public readonly uint Reserved;
    public readonly ulong PersistentId;
    public Entity(ulong epoch, uint handle, ulong persistentId)
        => (Epoch, Handle, Reserved, PersistentId) = (epoch, handle, 0, persistentId);

    public bool IsValid => NativeBridge.IsValid(this);
    public bool IsPendingDestroy => NativeBridge.IsPending(this);
    public string Name { get => NativeBridge.GetName(this); set => NativeBridge.SetName(this, value); }
    public Transform LocalTransform { get => NativeBridge.GetTransform(this, false); set => NativeBridge.SetTransform(this, false, value); }
    public Transform WorldTransform { get => NativeBridge.GetTransform(this, true); set => NativeBridge.SetTransform(this, true, value); }
    public RigidBody RigidBody => new(this);
    public void Destroy() => NativeBridge.DestroyEntity(this);
    public static Entity Resolve(ulong persistentId) => NativeBridge.Resolve(persistentId);
    public bool Equals(Entity other) => Epoch == other.Epoch && Handle == other.Handle && Reserved == other.Reserved && PersistentId == other.PersistentId;
    public override bool Equals(object? obj) => obj is Entity entity && Equals(entity);
    public override int GetHashCode() => HashCode.Combine(Epoch, Handle, Reserved, PersistentId);
    public static bool operator ==(Entity a, Entity b) => a.Equals(b);
    public static bool operator !=(Entity a, Entity b) => !a.Equals(b);
    public override string ToString() => $"epoch={Epoch}, entity={Handle}, persistentId={PersistentId}";
}

public readonly struct RigidBody(Entity entity)
{
    public void AddForce(Vec3 force) => NativeBridge.BodyCommand(entity, 0, force);
    public void AddImpulse(Vec3 impulse) => NativeBridge.BodyCommand(entity, 1, impulse);
    public void SetVelocity(Vec3 velocity) => NativeBridge.BodyCommand(entity, 2, velocity);
    public void SetTransform(Transform transform) => NativeBridge.SetTransform(entity, true, transform);
}

public enum Key : uint { W, A, S, D, Space, Control, Shift, E, F, Q, R, Escape, Enter, Up, Down, Left, Right }
public static class Input
{
    public static bool IsKeyDown(Key key) => NativeBridge.QueryInput(key, 0);
    public static bool IsKeyPressed(Key key) => NativeBridge.QueryInput(key, 1);
    public static bool IsKeyReleased(Key key) => NativeBridge.QueryInput(key, 2);
}
public static class Log
{
    public static void Write(string message) => NativeBridge.WriteLog(message);
}

public abstract class Script
{
    public Entity Entity { get; internal set; }
    public virtual void OnCreate() { }
    public virtual void OnUpdate(float dt) { }
    public virtual void OnFixedUpdate(float dt) { }
    public virtual void OnDestroy() { }
}

public enum Status : uint
{
    Ok, InvalidEntity, PendingDestroy, InvalidArgument, WrongThread,
    OutsideCallback, IncompatibleAbi, NotFound, Failure, Faulted, BufferTooSmall
}

public sealed class ScriptApiException(Status status, string operation) : Exception($"{operation}: {status}")
{
    public Status Status { get; } = status;
}
