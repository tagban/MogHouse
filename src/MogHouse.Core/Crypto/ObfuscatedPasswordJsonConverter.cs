using System.Text.Json;
using System.Text.Json.Serialization;

namespace MogHouse.Core.Crypto;

/// <summary>
/// Applied to a saved password property so it's obfuscated at rest wherever
/// that model gets JSON-serialized, without the in-memory value ever
/// needing to be anything but plaintext. See FfxiServerProfile.Password.
/// </summary>
public sealed class ObfuscatedPasswordJsonConverter : JsonConverter<string>
{
    public override string Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        PasswordObfuscator.Unwrap(reader.GetString() ?? "");

    public override void Write(Utf8JsonWriter writer, string value, JsonSerializerOptions options) =>
        writer.WriteStringValue(PasswordObfuscator.Wrap(value));
}
