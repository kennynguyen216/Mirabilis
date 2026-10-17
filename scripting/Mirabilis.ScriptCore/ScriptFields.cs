using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;

namespace Mirabilis.ScriptCore;

public static unsafe partial class NativeBridge
{
    private sealed class UserContext(string directory) : AssemblyLoadContext("Mirabilis.Scripts", isCollectible: true)
    {
        private readonly List<AssemblyDependencyResolver> resolvers = [];
        public Assembly LoadUser(string identity)
        {
            var requested = new AssemblyName(identity);
            var name = requested.Name ?? throw new TypeLoadException("Empty script assembly identity.");
            if (name.IndexOfAny([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar, ':']) >= 0)
                throw new TypeLoadException("Script assembly must be an assembly identity, not a path.");
            var assembly = string.Equals(name, typeof(Script).Assembly.GetName().Name, StringComparison.OrdinalIgnoreCase)
                ? typeof(Script).Assembly
                : Assemblies.FirstOrDefault(value => string.Equals(value.GetName().Name, name, StringComparison.OrdinalIgnoreCase));
            if (assembly == null)
            {
                var path = Path.Combine(directory, name + ".dll");
                if (!File.Exists(path)) throw new FileNotFoundException($"Script assembly '{identity}' is missing.", path);
                resolvers.Add(new AssemblyDependencyResolver(path));
                assembly = LoadManaged(path);
            }
            if (identity.Contains(',') && !string.Equals(requested.FullName, assembly.GetName().FullName, StringComparison.OrdinalIgnoreCase))
                throw new TypeLoadException($"Assembly identity mismatch: requested '{identity}', found '{assembly.FullName}'.");
            return assembly;
        }
        private Assembly LoadManaged(string path)
        {
            using var bytes = new MemoryStream(File.ReadAllBytes(path));
            var pdb = Path.ChangeExtension(path, ".pdb");
            using var symbols = File.Exists(pdb) ? new MemoryStream(File.ReadAllBytes(pdb)) : null;
            return LoadFromStream(bytes, symbols);
        }
        public void Retire() { resolvers.Clear(); Unload(); }
        protected override Assembly? Load(AssemblyName name)
        {
            if (string.Equals(name.Name, typeof(Script).Assembly.GetName().Name, StringComparison.OrdinalIgnoreCase))
                return typeof(Script).Assembly;
            foreach (var resolver in resolvers)
            {
                var path = resolver.ResolveAssemblyToPath(name);
                if (path != null) return LoadManaged(path);
            }
            return null;
        }
        protected override nint LoadUnmanagedDll(string name)
        {
            foreach (var resolver in resolvers)
            {
                var path = resolver.ResolveUnmanagedDllToPath(name);
                if (path != null) return LoadUnmanagedDllFromPath(path);
            }
            return 0;
        }
    }

    private static UserContext userContext = new(Path.GetDirectoryName(typeof(Script).Assembly.Location)!);
    private static Type FindType(string assembly, string typeName) => FindType(userContext, assembly, typeName);
    private static Type FindType(UserContext context, string assembly, string typeName)
    {
        if (string.IsNullOrWhiteSpace(assembly) || string.IsNullOrWhiteSpace(typeName))
            throw new TypeLoadException("Script assembly and full type name are required.");
        var type = context.LoadUser(assembly).GetType(typeName, throwOnError: true)!;
        if (!type.IsVisible || type.IsAbstract || type.ContainsGenericParameters || !typeof(Script).IsAssignableFrom(type)
            || type.GetConstructor(Type.EmptyTypes) == null)
            throw new TypeLoadException($"'{typeName}' must be a public concrete Script with a public parameterless constructor.");
        return type;
    }
    private static IEnumerable<FieldInfo> EditableFields(Type type) => type.GetFields(BindingFlags.Public | BindingFlags.Instance)
        .Where(field => !field.IsInitOnly && !field.IsLiteral && !field.IsDefined(typeof(NonSerializedAttribute)) && FieldType(field.FieldType) != null)
        .OrderBy(field => field.Name, StringComparer.Ordinal);
    private static string? FieldType(Type type) => type == typeof(float) ? "float" : type == typeof(int) ? "int"
        : type == typeof(bool) ? "bool" : type == typeof(string) ? "string" : type == typeof(Vec3) ? "vec3"
        : type == typeof(Entity) ? "entity" : null;
    private static string DescribeFields(Type type)
    {
        var result = new Dictionary<string, object?>();
        foreach (var field in EditableFields(type))
        {
            var kind = FieldType(field.FieldType)!;
            object? value = kind switch { "float" => 0f, "int" => 0, "bool" => false, "string" => "", "vec3" => new float[3], _ => null };
            result[field.Name] = new { type = kind, value };
        }
        return JsonSerializer.Serialize(result);
    }
    private static Dictionary<string, JsonElement> ParseFields(string fields)
    {
        using var document = JsonDocument.Parse(fields);
        if (document.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException("Script fields must be an object.");
        return document.RootElement.EnumerateObject().ToDictionary(field => field.Name, field => field.Value.Clone(), StringComparer.Ordinal);
    }
    private static float Finite(JsonElement value)
    {
        var number = value.GetSingle();
        if (!float.IsFinite(number)) throw new JsonException("Field float must be finite.");
        return number;
    }
    private static void BindFields(Instance instance, bool restoreValues = true)
    {
        var fields = EditableFields(instance.Script.GetType()).ToDictionary(field => field.Name, StringComparer.Ordinal);
        foreach (var (name, saved) in instance.Saved)
        {
            try
            {
                if (!fields.TryGetValue(name, out var field) || !saved.TryGetProperty("type", out var type)
                    || type.GetString() != FieldType(field.FieldType) || !saved.TryGetProperty("value", out var value))
                    throw new JsonException("Field was removed, renamed, unsupported, or changed type; saved value retained.");
                object? bound = type.GetString() switch
                {
                    "float" => Finite(value), "int" => value.GetInt32(), "bool" => value.GetBoolean(),
                    "string" => value.ValueKind == JsonValueKind.String ? value.GetString() : throw new JsonException("Expected string."),
                    "vec3" => value.ValueKind == JsonValueKind.Array && value.GetArrayLength() == 3
                        ? new Vec3(Finite(value[0]), Finite(value[1]), Finite(value[2])) : throw new JsonException("Expected three finite floats."),
                    "entity" => BindEntity(value), _ => throw new JsonException("Unsupported field type.")
                };
                if (restoreValues) field.SetValue(instance.Script, bound);
            }
            catch (Exception error)
            {
                instance.Unbound.Add(name);
                Report($"field '{name}' (saved value retained)", instance, error);
            }
        }
    }
    private static Entity BindEntity(JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Null) return default;
        var entity = Resolve(value.GetUInt64());
        if (entity.Epoch == 0) throw new JsonException("Entity reference target is missing; saved persistent ID retained.");
        return entity;
    }
    private static string CurrentFields(Instance instance)
    {
        var result = new Dictionary<string, object?>(instance.Saved.ToDictionary(pair => pair.Key, pair => (object?)pair.Value), StringComparer.Ordinal);
        foreach (var field in EditableFields(instance.Script.GetType()))
        {
            if (instance.Unbound.Contains(field.Name)) continue;
            var value = field.GetValue(instance.Script);
            switch (value)
            {
                case float number when !float.IsFinite(number):
                    Report($"field '{field.Name}' retained", instance, new InvalidOperationException("Runtime field is not finite.")); continue;
                case Vec3 vector:
                    if (!float.IsFinite(vector.X) || !float.IsFinite(vector.Y) || !float.IsFinite(vector.Z))
                    { Report($"field '{field.Name}' retained", instance, new InvalidOperationException("Runtime vec3 is not finite.")); continue; }
                    value = new[] { vector.X, vector.Y, vector.Z }; break;
                case Entity entity: value = entity.Epoch == 0 ? null : (object)entity.PersistentId; break;
                case null when field.FieldType == typeof(string): value = ""; break;
            }
            result[field.Name] = new { type = FieldType(field.FieldType), value };
        }
        return JsonSerializer.Serialize(result);
    }
}
