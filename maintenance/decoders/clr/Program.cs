// takaro-icons-decoder-clr: facts out of .NET assemblies for the icon pipeline.
//
// Runs inside the decoder container with the assembly mounted read-only at the request's root and a writable /out.
// Nothing here knows a game. The assembly is untrusted game data: it is only read, never loaded or executed.
//
// Request: {"schemaVersion": 1, "decoder": "clr", "settings": {}, "root": "/game", "exports": [{"id", "kind", "file", ...}]}
// Kinds:
//   assembly-info {file}                  -> {"name", "version", "mvid"}
//   callers {file, member}                -> [{"type", "method", "count"}] for every method body that calls
//                                            "Ns.Type::Method" (call, callvirt, newobj, ldftn), sorted
//   decompile-type {file, type}           -> {"type", "source"}: the C# of "Ns.Type" or "Ns.Outer+Inner"
// A failure of one export is recorded in the manifest and the tool still exits 0; a request failure exits non-zero.
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Text.Json;
using System.Text.RegularExpressions;
using ICSharpCode.Decompiler;
using ICSharpCode.Decompiler.CSharp;
using ICSharpCode.Decompiler.Disassembler;
using ICSharpCode.Decompiler.Metadata;
using ICSharpCode.Decompiler.TypeSystem;

namespace TakaroClrExport;

internal static class Program
{
    private const string ToolName = "takaro-clr-export";
    private const string ToolVersion = "1";
    private static readonly Regex IdPattern = new("^[a-z0-9][a-z0-9-]{0,63}$", RegexOptions.Compiled);
    private static readonly JsonSerializerOptions Pretty = new() { WriteIndented = true };
    private const string OutDir = "/out";

    private sealed class ExportFailure(string kind, string message) : Exception(message)
    {
        public string Kind { get; } = kind;
    }

    private static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("usage: TakaroClrExport <request.json>");
            return 2;
        }

        var request = JsonDocument.Parse(File.ReadAllText(args[0])).RootElement;
        if (request.GetProperty("schemaVersion").GetInt32() != 1 || request.GetProperty("decoder").GetString() != "clr")
        {
            Console.Error.WriteLine("unsupported request: schemaVersion 1 and decoder 'clr' are required");
            return 2;
        }

        var root = request.GetProperty("root").GetString()!;
        var exports = request.GetProperty("exports").EnumerateArray().ToList();
        var seen = new HashSet<string>();
        foreach (var export in exports)
        {
            var id = export.GetProperty("id").GetString()!;
            if (!IdPattern.IsMatch(id) || !seen.Add(id))
            {
                Console.Error.WriteLine($"invalid or duplicate export id '{id}'");
                return 2;
            }
        }

        Directory.CreateDirectory(Path.Combine(OutDir, "json"));
        var results = new List<Dictionary<string, object?>>();
        foreach (var export in exports)
        {
            var id = export.GetProperty("id").GetString()!;
            try
            {
                results.Add(Run(root, export, id));
            }
            catch (ExportFailure failure)
            {
                results.Add(new() { ["id"] = id, ["ok"] = false, ["error"] = failure.Message, ["errorKind"] = failure.Kind });
            }
            catch (Exception exception)
            {
                results.Add(new()
                {
                    ["id"] = id, ["ok"] = false, ["errorKind"] = "failed",
                    ["error"] = $"{exception.GetType().Name}: {exception.Message}",
                });
            }
        }

        var manifest = new Dictionary<string, object?>
        {
            ["schemaVersion"] = 1,
            ["tool"] = new Dictionary<string, object?>
            {
                ["name"] = ToolName,
                ["version"] = ToolVersion,
                ["decompiler"] = typeof(CSharpDecompiler).Assembly.GetName().Version?.ToString(),
                ["dotnet"] = Environment.Version.ToString(),
            },
            ["results"] = results,
        };
        File.WriteAllText(Path.Combine(OutDir, "manifest.json"), JsonSerializer.Serialize(manifest, Pretty) + "\n");
        Console.Error.WriteLine($"answered {results.Count} exports, {results.Count(r => (bool)r["ok"]!)} ok");
        return 0;
    }

    private static string ResolveFile(string root, JsonElement export)
    {
        var relative = export.GetProperty("file").GetString() ?? "";
        if (relative.Length == 0 || Path.IsPathRooted(relative) || relative.Split('/').Contains(".."))
        {
            throw new ExportFailure("failed", $"refusing file path '{relative}'");
        }

        var rootFull = Path.GetFullPath(root);
        var full = Path.GetFullPath(Path.Combine(rootFull, relative));
        if (!full.StartsWith(rootFull + Path.DirectorySeparatorChar, StringComparison.Ordinal))
        {
            throw new ExportFailure("failed", $"'{relative}' resolves outside {root}");
        }

        var info = new FileInfo(full);
        if (!info.Exists)
        {
            throw new ExportFailure("not-found", relative);
        }

        if (info.LinkTarget is not null || info.Attributes.HasFlag(FileAttributes.ReparsePoint))
        {
            throw new ExportFailure("failed", $"{relative} is a link");
        }

        return full;
    }

    private static Dictionary<string, object?> Run(string root, JsonElement export, string id)
    {
        var path = ResolveFile(root, export);
        return export.GetProperty("kind").GetString() switch
        {
            "assembly-info" => Write(id, AssemblyInfo(path)),
            "callers" => Write(id, Callers(path, export.GetProperty("member").GetString()!)),
            "decompile-type" => Write(id, DecompileType(path, export.GetProperty("type").GetString()!)),
            var other => throw new ExportFailure("failed", $"unknown export kind '{other}'"),
        };
    }

    private static Dictionary<string, object?> Write(string id, object value)
    {
        var relative = $"json/{id}.json";
        File.WriteAllText(Path.Combine(OutDir, relative), JsonSerializer.Serialize(value, Pretty) + "\n");
        return new() { ["id"] = id, ["ok"] = true, ["file"] = relative, ["meta"] = new Dictionary<string, object?>() };
    }

    private static object AssemblyInfo(string path)
    {
        using var file = new PEFile(path);
        var metadata = file.Metadata;
        var definition = metadata.GetAssemblyDefinition();
        return new
        {
            name = metadata.GetString(definition.Name),
            version = definition.Version.ToString(),
            mvid = metadata.GetGuid(metadata.GetModuleDefinition().Mvid).ToString(),
        };
    }

    private static string TypeName(MetadataReader metadata, EntityHandle handle) =>
        handle.GetFullTypeName(metadata).ReflectionName;

    /// <summary>The reflection full name and method name a call token points at; null for a token this tool does not follow.</summary>
    private static (string Type, string Method)? Target(MetadataReader metadata, EntityHandle handle)
    {
        if (handle.Kind == HandleKind.MethodSpecification)
        {
            handle = metadata.GetMethodSpecification((MethodSpecificationHandle)handle).Method;
        }

        switch (handle.Kind)
        {
            case HandleKind.MethodDefinition:
                var definition = metadata.GetMethodDefinition((MethodDefinitionHandle)handle);
                return (TypeName(metadata, definition.GetDeclaringType()), metadata.GetString(definition.Name));
            case HandleKind.MemberReference:
                var reference = metadata.GetMemberReference((MemberReferenceHandle)handle);
                return reference.Parent.Kind is HandleKind.TypeDefinition or HandleKind.TypeReference
                    ? (TypeName(metadata, reference.Parent), metadata.GetString(reference.Name))
                    : null;
            default:
                return null;
        }
    }

    private static object Callers(string path, string member)
    {
        var parts = member.Split("::");
        if (parts.Length != 2 || parts[0].Length == 0 || parts[1].Length == 0)
        {
            throw new ExportFailure("failed", $"member '{member}' is not 'Ns.Type::Method'");
        }

        using var file = new PEFile(path);
        var metadata = file.Metadata;
        var counts = new SortedDictionary<(string Type, string Method), int>(Comparer<(string Type, string Method)>.Create(
            (left, right) =>
            {
                var byType = string.CompareOrdinal(left.Type, right.Type);
                return byType != 0 ? byType : string.CompareOrdinal(left.Method, right.Method);
            }));
        var defined = false;
        foreach (var typeHandle in metadata.TypeDefinitions)
        {
            var type = metadata.GetTypeDefinition(typeHandle);
            var typeName = TypeName(metadata, typeHandle);
            foreach (var methodHandle in type.GetMethods())
            {
                var method = metadata.GetMethodDefinition(methodHandle);
                if (typeName == parts[0] && metadata.GetString(method.Name) == parts[1])
                {
                    defined = true;
                }

                if (method.RelativeVirtualAddress == 0)
                {
                    continue;
                }

                var calls = CountCalls(file, method, metadata, parts[0], parts[1]);
                if (calls > 0)
                {
                    var key = (typeName, metadata.GetString(method.Name));
                    counts[key] = counts.GetValueOrDefault(key) + calls;
                }
            }
        }

        if (!defined)
        {
            throw new ExportFailure("not-found", member);
        }

        return counts.Select(entry => new { type = entry.Key.Type, method = entry.Key.Method, count = entry.Value }).ToList();
    }

    private static int CountCalls(PEFile file, MethodDefinition method, MetadataReader metadata, string type, string name)
    {
        var body = file.Reader.GetMethodBody(method.RelativeVirtualAddress);
        var reader = body.GetILReader();
        var count = 0;
        while (reader.RemainingBytes > 0)
        {
            var opCode = ILParser.DecodeOpCode(ref reader);
            if (opCode is ILOpCode.Call or ILOpCode.Callvirt or ILOpCode.Newobj or ILOpCode.Ldftn)
            {
                var token = reader.ReadInt32();
                var target = Target(metadata, MetadataTokens.EntityHandle(token));
                if (target is { } found && found.Type == type && found.Method == name)
                {
                    count++;
                }
            }
            else
            {
                ILParser.SkipOperand(ref reader, opCode);
            }
        }

        return count;
    }

    private static object DecompileType(string path, string typeName)
    {
        var settings = new DecompilerSettings { ThrowOnAssemblyResolveErrors = false };
        var decompiler = new CSharpDecompiler(path, settings);
        var name = new FullTypeName(typeName);
        if (decompiler.TypeSystem.MainModule.GetTypeDefinition(name) is null)
        {
            throw new ExportFailure("not-found", typeName);
        }

        return new { type = typeName, source = decompiler.DecompileTypeAsString(name) };
    }
}
