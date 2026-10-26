// Vulkan seed metadata, ported from android/vk_reflect.py. Keep descriptor classification, complete storage
// extents and text formatting in step with that PC-fill reference; no SPIRV-Cross or dynamic JSON serialization.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static class VulkanReflection
{
    const int Sampler = 0, Combined = 1, SampledImage = 2, StorageImage = 3, UniformTexel = 4, StorageTexel = 5, UniformBuffer = 6, StorageBuffer = 7;
    const int Support = 1, Cbuf = 2, C1 = 3, Drv0 = 4, Drv2 = 5, Sbuf = 6, SbufUbo = 7, UboPtr = 8, Tex = 9, Tbuf = 10, Img = 11, Samp = 12, TexSamp = 13;

    internal sealed class Binding
    {
        internal uint Number;
        internal Int128 Count;
        internal int DescriptorType, Kind, Target = 1, Integer, ReadWrite;
        internal long Register = -1, SamplerRegister = -1, Offset;
        internal ulong Extent;
    }

    internal sealed class Result
    {
        internal readonly List<Binding> Bindings = new();
        internal readonly List<(uint location, int scalarClass)> VertexInputs = new();
        internal readonly SortedSet<uint> Colors = new();
        internal readonly List<string> UsedFloatTextures = new(), Unknown = new();
        internal (uint x, uint y, uint z) Local = (1, 1, 1);
        internal int Depth;
    }

    internal static bool UsesFloat64(byte[] code)
    {
        if (code.Length < 20 || code.Length % 4 != 0) throw new InvalidDataException("not SPIR-V");
        ReadOnlySpan<uint> words = MemoryMarshal.Cast<byte, uint>(code);
        if (words[0] != 0x07230203) throw new InvalidDataException("bad SPIR-V magic");
        for (int i = 5; i < words.Length;)
        {
            int op = (int)(words[i] & 0xffff), n = (int)(words[i] >> 16);
            if (n == 0 || n > words.Length - i) throw new InvalidDataException("bad SPIR-V instruction length");
            if (op == 22 && n == 3 && words[i + 2] == 64) return true;   // OpTypeFloat 64
            i += n;
        }
        return false;
    }

    // Ranges borrow the original module bytes instead of copying every instruction's operands.
    readonly record struct Operands(int Start, int Count);
    sealed class Spirv
    {
        readonly byte[] data;
        readonly Dictionary<uint, (int op, Operands args)> types = new();
        readonly Dictionary<uint, Dictionary<uint, Operands>> decorations = new();
        readonly Dictionary<(uint type, uint member), Dictionary<uint, Operands>> memberDecorations = new();
        readonly Dictionary<uint, Int128> constants = new();
        internal readonly Dictionary<uint, string> Names = new();
        internal readonly List<(uint id, uint pointer, uint storage)> Variables = new();
        internal (uint x, uint y, uint z) Local = (1, 1, 1);
        ReadOnlySpan<uint> Words => MemoryMarshal.Cast<byte, uint>(data);
        ReadOnlySpan<uint> Args(Operands range) => Words.Slice(range.Start, range.Count);

        internal Spirv(byte[] data)
        {
            this.data = data;
            if (data.Length < 20 || data.Length % 4 != 0) throw new InvalidDataException("not SPIR-V");
            ReadOnlySpan<uint> words = Words;
            if (words[0] != 0x07230203) throw new InvalidDataException("bad SPIR-V magic");
            for (int i = 5; i < words.Length;)
            {
                int op = (int)(words[i] & 0xffff), n = (int)(words[i] >> 16);
                if (n == 0 || n > words.Length - i) throw new InvalidDataException("bad SPIR-V instruction length");
                ReadOnlySpan<uint> a = words.Slice(i + 1, n - 1);
                switch (op)
                {
                    case 5:   // OpName
                        ReadOnlySpan<byte> name = MemoryMarshal.AsBytes(a[1..]);
                        int zero = name.IndexOf((byte)0);
                        Names[a[0]] = Encoding.UTF8.GetString(zero >= 0 ? name[..zero] : name);
                        break;
                    case 71:   // OpDecorate
                        if (!decorations.TryGetValue(a[0], out var d)) decorations[a[0]] = d = new();
                        d[a[1]] = new Operands(i + 3, n - 3);
                        break;
                    case 72:   // OpMemberDecorate
                        if (!memberDecorations.TryGetValue((a[0], a[1]), out var md)) memberDecorations[(a[0], a[1])] = md = new();
                        md[a[2]] = new Operands(i + 4, n - 4);
                        break;
                    case 16 when a[1] == 17:   // OpExecutionMode LocalSize
                        Local = (a[2], a[3], a[4]);
                        break;
                    case 43:   // OpConstant: literal integer array lengths only, never specialization constants
                        ReadOnlySpan<uint> ty = Type(a[0], out int tyOp);
                        if (tyOp == 21 && (ty[0] == 8 || ty[0] == 16 || ty[0] == 32 || ty[0] == 64) && a.Length - 2 == (ty[0] + 31) / 32)
                        {
                            int width = (int)ty[0];
                            ulong bits = a[2];
                            if (width == 64) bits |= (ulong)a[3] << 32;
                            else bits &= (1UL << width) - 1;
                            Int128 value = bits;
                            if (ty[1] != 0 && (bits & (1UL << (width - 1))) != 0) value -= (Int128)1 << width;
                            constants[a[1]] = value;
                        }
                        break;
                    case 59:   // OpVariable
                        Variables.Add((a[1], a[0], a[2]));
                        break;
                    case 19 or 20 or 21 or 22 or 23 or 24 or 25 or 26 or 27 or 28 or 29 or 30 or 32:
                        types[a[0]] = (op, new Operands(i + 2, n - 2));
                        break;
                }
                i += n;
            }
        }

        internal ReadOnlySpan<uint> Type(uint id, out int op)
        {
            if (types.TryGetValue(id, out var type)) { op = type.op; return Args(type.args); }
            op = 0;
            return ReadOnlySpan<uint>.Empty;
        }

        ReadOnlySpan<uint> Decoration(Dictionary<uint, Operands> layout, uint decoration) =>
            layout != null && layout.TryGetValue(decoration, out var value) ? Args(value) : ReadOnlySpan<uint>.Empty;
        internal uint? Decoration(uint id, uint decoration)
        {
            decorations.TryGetValue(id, out var layout);
            ReadOnlySpan<uint> value = Decoration(layout, decoration);
            return value.IsEmpty ? null : value[0];
        }
        internal bool HasDecoration(uint id, uint decoration) => decorations.TryGetValue(id, out var layout) && layout.ContainsKey(decoration);
        internal uint Pointee(uint pointer)
        {
            ReadOnlySpan<uint> a = Type(pointer, out int op);
            if (op != 32) throw new InvalidDataException("SPIR-V variable without pointer type");
            return a[1];
        }
        internal (uint type, Int128 count) StripArray(uint type)
        {
            ReadOnlySpan<uint> a = Type(type, out int op);
            return op switch
            {
                28 => (a[0], constants.GetValueOrDefault(a[1], 1)),
                29 => (a[0], 0),
                _ => (type, 1),
            };
        }
        internal int ScalarClass(uint type)
        {
            ReadOnlySpan<uint> a = Type(type, out int op);
            return op == 23 ? ScalarClass(a[0]) : op == 21 ? (a[1] != 0 ? 2 : 1) : 0;
        }

        internal ulong StorageExtent(uint type)
        {
            Type(type, out int op);
            return op == 30 ? Size(type, null, new HashSet<uint>()) : 0;
        }
        ulong Size(uint type, Dictionary<uint, Operands> layout, HashSet<uint> active)
        {
            if (!active.Add(type)) return 0;
            try
            {
                ReadOnlySpan<uint> a = Type(type, out int op);
                ulong extent = 0;
                if (op == 21 || op == 22)
                {
                    if (a[0] == 8 || a[0] == 16 || a[0] == 32 || a[0] == 64) extent = a[0] / 8;
                }
                else if (op == 23)
                {
                    Type(a[0], out int scalarOp);
                    if ((scalarOp == 21 || scalarOp == 22) && a[1] >= 2 && a[1] <= 4) extent = Size(a[0], null, active) * a[1];
                }
                else if (op == 24)
                {
                    ReadOnlySpan<uint> vector = Type(a[0], out int vectorOp), stride = Decoration(layout, 7);
                    bool row = layout != null && layout.ContainsKey(4), column = layout != null && layout.ContainsKey(5);
                    if (vectorOp == 23 && a[1] >= 2 && a[1] <= 4 && vector[1] >= 2 && vector[1] <= 4 && stride.Length == 1 && row != column)
                    {
                        Type(vector[0], out int scalarOp);
                        ulong scalar = scalarOp == 22 ? Size(vector[0], null, active) : 0;
                        uint major = row ? vector[1] : a[1], minor = row ? a[1] : vector[1];
                        if (scalar != 0 && stride[0] >= scalar * minor) extent = (ulong)stride[0] * major;
                    }
                }
                else if (op == 28)
                {
                    Int128 count = constants.GetValueOrDefault(a[1]);
                    decorations.TryGetValue(type, out var arrayLayout);
                    ReadOnlySpan<uint> stride = Decoration(arrayLayout, 6);
                    ulong element = Size(a[0], layout, active);
                    if (count > 0 && stride.Length == 1 && element != 0 && stride[0] >= element && count <= ulong.MaxValue / stride[0])
                        extent = stride[0] * (ulong)count;
                }
                else if (op == 30)
                {
                    for (int member = 0; member < a.Length; member++)
                    {
                        memberDecorations.TryGetValue((type, (uint)member), out var memberLayout);
                        ReadOnlySpan<uint> offset = Decoration(memberLayout, 35);
                        ulong memberSize = Size(a[member], memberLayout, active);
                        if (offset.Length != 1 || memberSize == 0 || memberSize > ulong.MaxValue - offset[0]) return 0;
                        extent = Math.Max(extent, offset[0] + memberSize);
                    }
                }
                return extent;
            }
            finally { active.Remove(type); }
        }
    }

    static int ImageTarget(uint dim, uint arrayed, uint ms) => dim switch
    {
        5 => 10,
        0 => arrayed != 0 ? 3 : 0,
        2 => 2,
        3 => arrayed != 0 ? 9 : 8,
        _ => ms != 0 ? (arrayed != 0 ? 6 : 5) : (arrayed != 0 ? 4 : 1),
    };
    static long Long(JsonNode n)
    {
        JsonValue v = n.AsValue();
        return v.TryGetValue(out long l) ? l : v.TryGetValue(out int i) ? i : v.TryGetValue(out uint u) ? u :
            v.TryGetValue(out byte b) ? b : v.TryGetValue(out sbyte sb) ? sb :
            v.TryGetValue(out short s) ? s : v.TryGetValue(out ushort us) ? us :
            v.TryGetValue(out ulong ul) ? checked((long)ul) : v.TryGetValue(out float f) ? (long)f :
            v.TryGetValue(out decimal d) ? (long)d : (long)v.GetValue<double>();
    }

    internal static Result Reflect(byte[] code, JsonObject resources, int stage)
    {
        var sp = new Spirv(code);
        var result = new Result { Local = sp.Local };
        var sbufs = resources["sbufs"] is JsonArray buffers ? buffers.Select(b => b.AsObject()).OrderBy(b => Long(b["binding"])).ToArray() : Array.Empty<JsonObject>();
        foreach (var (id, pointer, storage) in sp.Variables)
        {
            string name = sp.Names.GetValueOrDefault(id, "");
            uint type = sp.Pointee(pointer);
            if (storage == 1)
            {
                if (stage == 0 && sp.Decoration(id, 30) is uint location && !sp.HasDecoration(id, 11))
                    result.VertexInputs.Add((location, sp.ScalarClass(sp.StripArray(type).type)));
                continue;
            }
            if (storage == 3)
            {
                if (stage == 1 && sp.Decoration(id, 30) is uint location && !sp.HasDecoration(id, 11)) result.Colors.Add(location);
                if (stage == 1 && sp.Decoration(id, 11) == 22) result.Depth = 1;
                continue;
            }
            if (storage != 0 && storage != 2 && storage != 12) continue;
            if (sp.Decoration(id, 33) is not uint binding) continue;
            var (inner, count) = sp.StripArray(type);
            ReadOnlySpan<uint> a = sp.Type(inner, out int op);
            int descriptor, target = 1, integer = 0;
            if (storage == 2 || storage == 12) descriptor = storage == 2 && sp.HasDecoration(inner, 2) ? UniformBuffer : StorageBuffer;
            else if (op == 26) descriptor = Sampler;
            else if (op == 27)
            {
                ReadOnlySpan<uint> image = sp.Type(a[0], out _);
                target = ImageTarget(image[1], image[3], image[4]); integer = sp.ScalarClass(image[0]) != 0 ? 1 : 0;
                descriptor = image[1] == 5 ? UniformTexel : Combined;
            }
            else if (op == 25)
            {
                target = ImageTarget(a[1], a[3], a[4]); integer = sp.ScalarClass(a[0]) != 0 ? 1 : 0;
                descriptor = a[1] == 5 ? (a[5] == 1 ? UniformTexel : StorageTexel) : (a[5] == 1 ? SampledImage : StorageImage);
            }
            else continue;
            var record = new Binding { Number = binding, Count = count != 0 ? count : 32, DescriptorType = descriptor, Target = target, Integer = integer,
                                       Extent = descriptor == StorageBuffer ? sp.StorageExtent(inner) : 0 };
            Match cb = Regex.Match(name, @"^[a-z]+_c(\d+)$"), sb = Regex.Match(name, @"^[a-z]+_s(\d+)$");
            if (name == "support_buffer") record.Kind = Support;
            else if (cb.Success && descriptor == UniformBuffer)
            {
                long bank = long.Parse(cb.Groups[1].Value, CultureInfo.InvariantCulture);
                if (bank >= 3 && bank <= 16) { record.Kind = Cbuf; record.Register = bank - 3; }
                else record.Kind = bank switch { 0 => Drv0, 1 => C1, 2 => Drv2, _ => 0 };
            }
            else if (sb.Success && descriptor == StorageBuffer)
            {
                long index = long.Parse(sb.Groups[1].Value, CultureInfo.InvariantCulture);
                JsonObject b = index < sbufs.Length ? sbufs[(int)index] : null;
                long slot = b != null ? Long(b["cb_slot"]) : 0, offset = b != null ? Long(b["cb_offset"]) : 0;
                long? off = b != null && slot == 0 ? offset * 4 : null;
                record.ReadWrite = b != null && b["flags"] is JsonNode flags && (Long(flags) & 1) != 0 ? 1 : 0;
                if (stage == 5 && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) { record.Kind = UboPtr; record.Register = (off.Value - 0x210) / 16; }
                else if (b != null && slot >= 3 && slot <= 16) { record.Kind = SbufUbo; record.Register = slot - 3; record.Offset = offset * 4; }
                else
                {
                    long baseOffset = stage switch { 0 => 0x110, 5 => 0x310, 1 => 0x510, _ => -1 };
                    if (off != null && baseOffset >= 0 && off >= baseOffset && off - baseOffset < 0x100 && (off - baseOffset) % 16 == 0)
                    { record.Kind = Sbuf; record.Register = (off.Value - baseOffset) / 16; }
                }
            }
            else if (descriptor == Sampler)
            {
                var d = ShaderService.DecodeTex(name, "s");
                if (d != null)
                {
                    var (texture, sampler, handleType) = d.Value;
                    if (handleType != 0 && sampler != null) { record.Kind = Samp; record.Register = sampler.Value; }
                    else if (texture != null) { record.Kind = Samp; record.Register = texture.Value; record.SamplerRegister = -2; }
                }
            }
            else if (descriptor == Combined || descriptor == SampledImage || descriptor == UniformTexel)
            {
                var d = ShaderService.DecodeTex(name);
                if (d != null && d.Value.t is long texture)
                {
                    var (_, sampler, handleType) = d.Value;
                    record.Register = texture;
                    record.Kind = descriptor == Combined ? TexSamp : descriptor == UniformTexel ? Tbuf : Tex;
                    if (descriptor == Combined) record.SamplerRegister = handleType != 0 && sampler != null ? sampler.Value : -1;
                }
                // These become texture*/depth*<float> declarations in the Metal service, whose bound names are deps too.
                if (integer == 0) result.UsedFloatTextures.Add(name);
            }
            else if (descriptor == StorageImage || descriptor == StorageTexel)
            {
                var d = ShaderService.DecodeTex(name, "i");
                if (d != null && d.Value.t is long texture) { record.Kind = Img; record.Register = texture; record.ReadWrite = 1; }
            }
            if (record.Kind == 0) result.Unknown.Add($"{name} (binding {binding}, type {descriptor})");
            result.Bindings.Add(record);
        }
        result.VertexInputs.Sort();
        return result;
    }

    internal static string Text(Result result, int stage, string c1)
    {
        var text = new StringBuilder("vk 1\n");
        var (x, y, z) = result.Local;
        text.Append(CultureInfo.InvariantCulture, $"stage {stage}\nlocal {x} {y} {z}\nc1 {c1}\nvin ");
        for (int i = 0; i < result.VertexInputs.Count; i++)
        {
            if (i != 0) text.Append(' ');
            var (location, scalarClass) = result.VertexInputs[i];
            text.Append(CultureInfo.InvariantCulture, $"{location}:{scalarClass}");
        }
        text.Append("\nfout ");
        bool first = true;
        foreach (uint color in result.Colors)
        {
            if (!first) text.Append(' ');
            text.Append(CultureInfo.InvariantCulture, $"{color}"); first = false;
        }
        text.Append(CultureInfo.InvariantCulture, $"\ndepth {result.Depth}\n");
        foreach (Binding b in result.Bindings.OrderBy(b => b.Number))
            text.Append(CultureInfo.InvariantCulture, $"b {b.Number} {b.Count} {b.DescriptorType} {b.Kind} {b.Register} {b.SamplerRegister} {b.Offset} {b.ReadWrite} {b.Target} {b.Integer} {b.Extent}\n");
        return text.ToString();
    }
}
