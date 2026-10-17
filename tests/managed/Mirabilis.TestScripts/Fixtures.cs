using Mirabilis.ScriptCore;
using System.Threading;
using System.Runtime.Loader;
using Script = Mirabilis.ScriptCore.Script;

namespace Mirabilis.TestScripts;

internal static class Assert
{
    public static void That(bool condition, string message) { if (!condition) throw new Exception("fixture-assert: " + message); }
    public static void Status(Action action, Status expected)
    {
        try { action(); }
        catch (ScriptApiException error) { That(error.Status == expected, $"expected {expected}, got {error.Status}"); return; }
        throw new Exception("fixture-assert: expected " + expected);
    }
}

public class LifecycleProbe : Script
{
    public int Creates, Updates, Fixed;
    public string Label = "initializer", Fail = "";
    protected void Mark(string phase) => Log.Write($"probe:{Entity.PersistentId}:{phase}");
    public override void OnCreate() { Assert.That(AssemblyLoadContext.GetLoadContext(GetType().Assembly)!.IsCollectible, "user assembly must be collectible"); Assert.That(AssemblyLoadContext.GetLoadContext(typeof(Script).Assembly) == AssemblyLoadContext.Default, "ScriptCore identity is shared in default context"); ++Creates; Mark("create"); if (Fail == "create") throw new Exception("fixture-create"); }
    public override void OnUpdate(float dt) { ++Updates; Mark("update"); if (Fail == "update") throw new Exception("fixture-update"); }
    public override void OnFixedUpdate(float dt) { ++Fixed; Mark("fixed"); if (Fail == "fixed") throw new Exception("fixture-fixed"); }
    public override void OnDestroy() { Mark("destroy"); if (Fail == "destroy") throw new Exception("fixture-destroy"); }
}

public sealed class ConstructorThrows : Script
{
    public int VisibleField;
    public ConstructorThrows() => throw new Exception("fixture-constructor");
    public override void OnDestroy() => Log.Write($"probe:{Entity.PersistentId}:destroy");
}

public sealed class CleanupProbe : LifecycleProbe
{
    public Entity Target;
    public bool DestroyTarget;
    public override void OnDestroy()
    {
        var self = Entity;
        Assert.That(Entity.IsValid, "cleanup entity remains readable");
        _ = Entity.Name; _ = Entity.LocalTransform; _ = Entity.WorldTransform;
        if (Entity.IsPendingDestroy)
        {
            Assert.Status(() => self.Name = "forbidden", Status.PendingDestroy);
            Assert.Status(() => self.LocalTransform = Transform.Identity, Status.PendingDestroy);
            Assert.Status(() => Entity.RigidBody.AddImpulse(new Vec3(1, 0, 0)), Status.PendingDestroy);
            Entity.Destroy(); Entity.Destroy();
        }
        if (Target.IsValid && Target.IsPendingDestroy)
        {
            Assert.Status(() => Target.Name = "forbidden-target", Status.PendingDestroy);
        }
        else if (Target.IsValid)
        {
            if (DestroyTarget) { Target.Destroy(); Target.Destroy(); }
            else { Target.Name = "cleanup-target"; var transform = Target.LocalTransform; transform.Position = new Vec3(8, 9, 10); Target.LocalTransform = transform; }
        }
        Mark("cleanup-ok");
    }
}

public sealed class FieldProbe : Script
{
    public float Number = 12.5f;
    public int Count = 23;
    public bool Flag;
    public string Text = "initializer";
    public Vec3 Vector = new(4, 5, 6);
    public Entity MissingEntity;
    public bool Mutate;
    public static int StaticField = 3;
    public readonly int ReadonlyField = 4;
    public double Unsupported = 5;
    public override void OnCreate()
    {
        if (Mutate)
        {
            Assert.That(Number == 2.5f && Count == 7 && Flag && Text == "\0café漢", "saved scalar and UTF8 fields");
            Assert.That(Vector.X == 1 && Vector.Y == 2 && Vector.Z == 3 && !MissingEntity.IsValid, "saved vector/entity fields");
            Log.Write("field-bind-ok");
        }
    }
    public override void OnUpdate(float dt)
    {
        if (!Mutate) return;
        Number = 99; Count = -4; Flag = false; Text = "play"; Vector = new(7, 8, 9);
    }
}

public sealed unsafe class BoundaryProbe : Script
{
    private static Entity previous;
    private static void InvalidReserved(Entity entity)
    {
        var invalid = entity;
        ((uint*)&invalid)[3] = 1;
        var captured = invalid;
        Assert.Status(() => _ = captured.Name, Status.InvalidArgument);
    }
    public override void OnCreate()
    {
        var self = Entity;
        if (previous.Epoch != 0)
        {
            Assert.That(!previous.IsValid, "old epoch must stay stale");
            Assert.Status(() => _ = previous.Name, Status.InvalidEntity);
            Log.Write("stale-epoch-ok");
        }
        previous = Entity;
        foreach (string text in new[] { "", "café漢🙂", "prefix\0suffix" })
        {
            self.Name = text; Assert.That(Entity.Name == text, "UTF8 caller-owned buffer and embedded NUL");
        }
        var zero = new Entity(Entity.Epoch, Entity.Handle, Entity.PersistentId + 1000000);
        Assert.That(!zero.IsValid, "persistent ID identity");
        Assert.Status(() => _ = zero.LocalTransform, Status.InvalidEntity);
        var handle = new Entity(Entity.Epoch, Entity.Handle ^ 0x01000000u, Entity.PersistentId);
        Assert.That(!handle.IsValid, "full versioned handle");
        Assert.That(!Entity.Resolve(ulong.MaxValue).IsValid, "missing resolution");
        InvalidReserved(Entity);
        Assert.Status(() => _ = Input.IsKeyDown((Key)uint.MaxValue), Status.InvalidArgument);
        var nan = Transform.Identity; nan.Position.X = float.NaN;
        Assert.Status(() => self.LocalTransform = nan, Status.InvalidArgument);
        Assert.Status(() => Entity.RigidBody.AddForce(new Vec3(float.PositiveInfinity, 0, 0)), Status.InvalidArgument);
        Exception? threadError = null;
        var thread = new Thread(() =>
        {
            try { Assert.Status(() => _ = Entity.Name, Status.WrongThread); }
            catch (Exception error) { threadError = error; }
        });
        thread.Start(); thread.Join();
        if (threadError != null) throw threadError;
        Log.Write("boundary-ok");
    }
}

public sealed class InputProbe : Script
{
    public int FixedPresses, FixedReleases, FramePresses, FrameReleases, Held;
    public override void OnFixedUpdate(float dt)
    {
        if (Input.IsKeyPressed(Key.Space)) ++FixedPresses;
        if (Input.IsKeyReleased(Key.Space)) ++FixedReleases;
        if (Input.IsKeyDown(Key.Space)) ++Held;
    }
    public override void OnUpdate(float dt)
    {
        if (Input.IsKeyPressed(Key.Space)) ++FramePresses;
        if (Input.IsKeyReleased(Key.Space)) ++FrameReleases;
        if (Input.IsKeyDown(Key.Space)) ++Held;
    }
}

public sealed class PhysicsProbe : Script
{
    public bool DestroyAfter;
    private bool sent;
    public override void OnFixedUpdate(float dt)
    {
        if (sent) return; sent = true;
        var self = Entity;
        var transform = Transform.Identity; transform.Position = new Vec3(10, 4, 0);
        self.LocalTransform = transform;
        Assert.That(Entity.LocalTransform.Position.X == 0, "dynamic setter must wait for command phase");
        Entity.RigidBody.SetTransform(transform);
        Entity.RigidBody.SetVelocity(new Vec3(0, 1, 0));
        Entity.RigidBody.AddForce(new Vec3(0, 1, 0));
        Entity.RigidBody.AddImpulse(new Vec3(0, 1, 0));
        Log.Write("teleport-queued");
        if (DestroyAfter) { Entity.Destroy(); Entity.Destroy(); }
    }
}



public sealed class SelfDestroyProbe : LifecycleProbe
{
    public override void OnCreate()
    {
        base.OnCreate(); Entity.Destroy(); Entity.Destroy();
        Assert.That(Entity.IsValid && Entity.IsPendingDestroy, "OnCreate self destruction must defer");
    }
}

public sealed class ParentMoveProbe : Script
{
    public Entity Target;
    private bool moved;
    public override void OnFixedUpdate(float dt)
    {
        if (moved) return; moved = true;
        var target = Target; var transform = target.LocalTransform; transform.Position = new Vec3(10, 0, 0);
        target.LocalTransform = transform;
        Assert.That(target.LocalTransform.Position.X == 10, "static parent write is immediately visible");
        Log.Write("parent-moved");
    }
}




public sealed class DestroyOtherProbe : LifecycleProbe
{
    public Entity Target;
    public override void OnUpdate(float dt)
    {
        base.OnUpdate(dt);
        if (Target.IsValid && !Target.IsPendingDestroy) Target.Destroy();
    }
}

public sealed class CleanupWorldProbe : Script
{
    public Entity Target;
    public int Mode;
    public override void OnDestroy()
    {
        var target = Target; var transform = Transform.Identity;
        transform.Position = new Vec3(7, 8, 9); transform.Rotation = new Vec3(.2f, .4f, .6f);
        if (Mode == 0) target.WorldTransform = transform;
        else if (Mode == 1) target.RigidBody.SetTransform(transform);
        else target.LocalTransform = transform;
        Log.Write("cleanup-world-queued");
    }
}

public sealed class DiagnosticFloodProbe : Script
{
    public int Mode;
    public override void OnCreate()
    {
        if (Mode == 0)
            for (int i = 0; i < 320; ++i) Log.Write($"history-count-{i:D3}: café漢🙂");
        else if (Mode == 1)
            for (int i = 0; i < 40; ++i) Log.Write($"history-bytes-{i:D2}:" + new string('x', 40000) + $":history-byte-tail-{i:D2}");
        else if (Mode == 2)
        {
            string payload = string.Concat(Enumerable.Repeat("漢🙂", 12000));
            for (int i = 0; i < 4; ++i)
                Log.Write($"history-large-{i}:" + new string('a', i) + payload + $":history-complete-tail-{i}");
        }
        else
            Log.Write("history-giant-latest:" + string.Concat(Enumerable.Repeat("漢🙂", 310000)) + ":history-giant-complete-tail");
    }
}
