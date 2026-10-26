using System;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

// Build the service's DOM directly from the streaming reader. .NET 10 JsonNode.Parse routes
// through JsonDocument's duplicate-property hash set, whose private Marvin seed uses native
// OpenSSL RNG on linux-bionic. JsonObject.Add keeps duplicate rejection without that provider.
static class JsonText
{
    internal static JsonNode Parse(string text)
    {
        var reader = new Utf8JsonReader(Encoding.UTF8.GetBytes(text));
        if (!reader.Read()) throw new JsonException("Expected a JSON value");
        JsonNode node = Read(ref reader);
        if (reader.Read()) throw new JsonException("Unexpected data after JSON value");
        return node;
    }

    static JsonNode Read(ref Utf8JsonReader reader)
    {
        switch (reader.TokenType)
        {
            case JsonTokenType.StartObject:
                var obj = new JsonObject();
                while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
                {
                    if (reader.TokenType != JsonTokenType.PropertyName) throw new JsonException("Expected property name");
                    string name = reader.GetString();
                    if (!reader.Read()) throw new JsonException("Expected property value");
                    obj.Add(name, Read(ref reader));
                }
                if (reader.TokenType != JsonTokenType.EndObject) throw new JsonException("Unclosed object");
                return obj;
            case JsonTokenType.StartArray:
                var array = new JsonArray();
                while (reader.Read() && reader.TokenType != JsonTokenType.EndArray) array.Add(Read(ref reader));
                if (reader.TokenType != JsonTokenType.EndArray) throw new JsonException("Unclosed array");
                return array;
            case JsonTokenType.String: return JsonValue.Create(reader.GetString());
            case JsonTokenType.Number:
                if (reader.TryGetInt32(out int i)) return JsonValue.Create(i);
                if (reader.TryGetInt64(out long l)) return JsonValue.Create(l);
                if (reader.TryGetUInt64(out ulong u)) return JsonValue.Create(u);
                return JsonValue.Create(reader.GetDouble());
            case JsonTokenType.True: return JsonValue.Create(true);
            case JsonTokenType.False: return JsonValue.Create(false);
            case JsonTokenType.Null: return null;
            default: throw new JsonException("Unexpected JSON token");
        }
    }
}
