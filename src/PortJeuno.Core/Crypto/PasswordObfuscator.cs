using System.Text;

namespace PortJeuno.Core.Crypto;

/// <summary>
/// Lightweight, reversible obfuscation for a saved password at rest in a
/// profile JSON file - not real security (XOR with a fixed, published key,
/// Base64-encoded), just enough to keep it from being immediately readable
/// by anyone who opens the file. Same approach as Invigoration's
/// PasswordObfuscator (a sibling project by the same author) - reimplemented
/// here rather than referenced since PortJeuno is a standalone project with
/// no dependency on Invigoration.Core. Wrapped in brackets so it can be told
/// apart from a plaintext password typed directly into the file by hand,
/// which <see cref="Unwrap"/> passes through as-is.
/// </summary>
public static class PasswordObfuscator
{
    private static readonly byte[] Key = Encoding.UTF8.GetBytes("PortJeuno");

    /// <summary>Bracket-wrapped ("[...]") text is treated as obfuscated and decoded; anything else is returned unchanged.</summary>
    public static string Unwrap(string stored)
    {
        if (stored.Length < 2 || stored[0] != '[' || stored[^1] != ']')
        {
            return stored;
        }

        try
        {
            return Encoding.UTF8.GetString(Xor(Convert.FromBase64String(stored[1..^1])));
        }
        catch (FormatException)
        {
            // Not actually obfuscated text, just a password that happens to be bracket-wrapped - use as typed.
            return stored;
        }
    }

    /// <summary>Wraps a plaintext password as "[base64]" for storage.</summary>
    public static string Wrap(string plaintext) =>
        plaintext.Length == 0 ? plaintext : $"[{Convert.ToBase64String(Xor(Encoding.UTF8.GetBytes(plaintext)))}]";

    private static byte[] Xor(byte[] bytes)
    {
        var result = new byte[bytes.Length];
        for (var i = 0; i < bytes.Length; i++)
        {
            result[i] = (byte)(bytes[i] ^ Key[i % Key.Length]);
        }

        return result;
    }
}
