using Mirabilis.ScriptCore;

namespace Mirabilis.SampleScripts;

public sealed class PushBox : Script
{
    public Vec3 Force = new(5, 0, 0);
    public Vec3 Impulse = new(0, 4, 0);
    public override void OnFixedUpdate(float dt)
    {
        Entity.RigidBody.AddForce(Force);
        if (Input.IsKeyPressed(Key.Space)) Entity.RigidBody.AddImpulse(Impulse);
    }
}

public sealed class MoveAndSpin : Script
{
    public Vec3 Velocity = new(1, 0, 0);
    public Vec3 AngularVelocity = new(0, 0.5f, 0);
    public override void OnFixedUpdate(float dt)
    {
        var transform = Entity.LocalTransform;
        transform.Position += Velocity * dt;
        transform.Rotation += AngularVelocity * dt;
        var self = Entity;
        self.LocalTransform = transform;
    }
}

public sealed class BodyInteraction : Script
{
    public Entity Target;
    public Vec3 Impulse = new(0, 4, 0);
    public override void OnFixedUpdate(float dt)
    {
        if (Input.IsKeyPressed(Key.E) && Target.IsValid) Target.RigidBody.AddImpulse(Impulse);
    }
}
