using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Mirabilis.ScriptCore;

public static unsafe partial class NativeBridge
{
    private static WeakReference? retiredContext;
    private static IEnumerable<FieldInfo> ReloadFields(Type type)
    {
        for (Type? current = type; current != null && current != typeof(Script); current = current.BaseType)
            foreach (var field in current.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
                if (!field.IsInitOnly && !field.IsLiteral && !field.IsDefined(typeof(NonSerializedAttribute)) && FieldType(field.FieldType) != null)
                    yield return field;
    }
    private static string FieldKey(FieldInfo field) => field.DeclaringType!.FullName + "::" + field.Name;
    private static bool CollectRetired()
    {
        for (int i = 0; retiredContext?.IsAlive == true && i < 3; ++i)
        {
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        }
        return retiredContext?.IsAlive != true;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static Status Reload(Utf8 directory)
    {
        UserContext? candidate = null;
        try
        {
            var status = ThreadStatus(); if (status != Status.Ok) return status;
            if (!CollectRetired()) throw new InvalidOperationException("Previous scripts are still referenced. Stop script threads/event subscriptions before reloading again.");
            var path = Path.GetFullPath(Decode(directory));
            if (!Directory.Exists(path)) throw new DirectoryNotFoundException(path);
            candidate = new UserContext(path);
            foreach (var assembly in userContext.Assemblies) candidate.LoadUser(assembly.GetName().FullName!);
            var replacements = new Dictionary<ulong, Instance>();
            foreach (var (handle, old) in instances)
            {
                var oldType = old.Script.GetType();
                var type = FindType(candidate, oldType.Assembly.GetName().FullName!, oldType.FullName!);
                var fields = ReloadFields(oldType).ToDictionary(FieldKey,
                    field => (Kind: FieldType(field.FieldType), Value: field.GetValue(old.Script)), StringComparer.Ordinal);
                var script = (Script)Activator.CreateInstance(type)!;
                script.Entity = old.Script.Entity;
                var replacement = new Instance(script, ParseFields(CurrentFields(old))) { Created = old.Created };
                BindFields(replacement);
                foreach (var field in ReloadFields(type))
                    if (fields.TryGetValue(FieldKey(field), out var saved) && saved.Kind == FieldType(field.FieldType))
                        field.SetValue(script, saved.Value);
                replacements.Add(handle, replacement);
            }
            var previous = userContext;
            instances = replacements;
            userContext = candidate; candidate = null;
            retiredContext = new WeakReference(previous);
            previous.Retire();
            Diagnostic($"Script reload committed: {instances.Count} instances; live world and runtime fields retained.");
            return Status.Ok;
        }
        catch (Exception error)
        {
            if (candidate != null) { retiredContext = new WeakReference(candidate); candidate.Retire(); }
            Report("Reload rejected; previous scripts retained", null, error);
            return Status.Failure;
        }
    }
}
