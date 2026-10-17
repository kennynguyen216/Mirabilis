using Mirabilis.ScriptCore;
namespace Mirabilis.ReloadProbe;

public class ProbeBase : Script
{
    private int inherited = 20;
    protected int AdvanceBase() => ++inherited;
}
#if RELOAD_5
public sealed class RemovedProbe : ProbeBase { }
#else
public sealed class LiveProbe : ProbeBase
{
    private int counter = 10;
    private int Hidden { get; set; } = 30;
    private static int global;
#if RELOAD_6
    private static int constructors;
#endif
    [NonSerialized] private int ephemeral = 5;
    [NonSerialized] public int PublicEphemeral = 7;
    public int PublicEphemeralObserved;
    public int Creates, Updates, PrivateObserved, InheritedObserved, PropertyObserved, EphemeralObserved, StaticObserved;
    public Entity Target;
    public bool ThrowNext;
#if RELOAD_1
    public int OldField = 73;
#else
    public string OldField = "new-type";
    public int Added = 77;
#endif
    public LiveProbe()
    {
#if RELOAD_6
        if (++constructors==2) throw new InvalidOperationException("reload-second-constructor-failure");
#endif
#if RELOAD_3
        throw new InvalidOperationException("reload-constructor-failure");
#endif
#if RELOAD_4
        Entity.Resolve(0).Destroy();
#endif
    }
    public override void OnCreate() { ++Creates; Log.Write("reload-create"); }
    public override void OnUpdate(float dt)
    {
#if RELOAD_1
        if (ThrowNext) throw new InvalidOperationException("reload-recoverable-fault");
        ++Updates;
#else
        Updates += 10;
#endif
        PrivateObserved = ++counter; InheritedObserved = AdvanceBase(); PropertyObserved = ++Hidden;
        EphemeralObserved = ++ephemeral; StaticObserved = ++global;
        PublicEphemeralObserved = ++PublicEphemeral;
        if (!Target.IsValid || Target.PersistentId != 0) throw new InvalidOperationException("reload lost entity reference");
    }
    public override void OnDestroy() => Log.Write("reload-destroy");
}
#endif
