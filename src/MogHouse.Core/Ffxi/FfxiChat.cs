using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>What a client asks the server to say - GP_CLI_COMMAND_CHAT_STD_KIND.</summary>
public enum FfxiChatKind : byte
{
    Say = 0x00,
    Shout = 0x01,
    Party = 0x04,
    Linkshell1 = 0x05,
    Emote = 0x08,
    LinkshellPvp = 0x18,
    Yell = 0x1A,
    Linkshell2 = 0x1B,
    Unity = 0x21,
    AssistJ = 0x22,
    AssistE = 0x23,
}

/// <summary>
/// What the server says back - CHAT_MESSAGE_TYPE. Note this is a *different*
/// enumeration from <see cref="FfxiChatKind"/> despite covering similar
/// ground: Say/Shout share values, but the server's list continues into
/// things no client ever sends (tells, system messages, the "no speaker
/// object" variants), and Emote is 8 in both while Party is 4 in both by
/// coincidence rather than by design. Don't cast between them.
/// </summary>
public enum FfxiChatMessageType : byte
{
    Say = 0,
    Shout = 1,
    Unknown = 2,
    Tell = 3,
    Party = 4,
    Linkshell = 5,
    System1 = 6,
    System2 = 7,
    Emotion = 8,
    GmPrompt = 12,
    NoSpeakerSay = 13,
    NoSpeakerShout = 14,
    NoSpeakerParty = 15,
    NoSpeakerLinkshell = 16,
}

/// <summary>Turning what we asked to say into how it should be shown.</summary>
public static class FfxiChatEcho
{
    /// <summary>
    /// How a line we just sent should be coloured in our own log.
    ///
    /// <para>
    /// The two enumerations are not the same list and must not be cast between
    /// - see <see cref="FfxiChatMessageType"/> - so this is written out. Until
    /// it existed every line a player sent was echoed as Say, which put a
    /// linkshell message on screen looking exactly like something shouted at
    /// the street.
    /// </para>
    ///
    /// <para>
    /// Yell and Unity have no message type of their own; the server's list
    /// stops before them. Yell reads as a shout, which is what it is, and
    /// anything else falls back to Say rather than inventing a colour.
    /// </para>
    /// </summary>
    public static FfxiChatMessageType For(FfxiChatKind kind) => kind switch
    {
        FfxiChatKind.Say => FfxiChatMessageType.Say,
        FfxiChatKind.Shout => FfxiChatMessageType.Shout,
        FfxiChatKind.Yell => FfxiChatMessageType.Shout,
        FfxiChatKind.Party => FfxiChatMessageType.Party,
        FfxiChatKind.Linkshell1 => FfxiChatMessageType.Linkshell,
        FfxiChatKind.Linkshell2 => FfxiChatMessageType.Linkshell,
        FfxiChatKind.LinkshellPvp => FfxiChatMessageType.Linkshell,
        FfxiChatKind.Emote => FfxiChatMessageType.Emotion,
        _ => FfxiChatMessageType.Say,
    };
}

/// <summary>
/// GP_CLI_COMMAND_CHAT_STD (C2S 0x0B5) - sending chat.
///
/// This is the project's first variable-length packet, and the length rule is
/// load-bearing rather than cosmetic: the server does not look for a null
/// terminator, it computes the message length from the declared packet size
/// (`min(header.size * 4 - 6, sizeof(Str))`), with its own source warning
/// "Depending on alignment, the message may not be NULL-terminated." Since
/// packet sizes are counted in 4-byte units, the size must be padded up to a
/// multiple of 4 - and the padding lands *inside* the message the server
/// reads. Padding with NULs keeps the trailing bytes harmless.
///
/// Layout read back from the real struct compiled with this environment's
/// MSVC: Kind at 4, unknown00 at 5, Str at 6.
/// </summary>
public static class FfxiChatPacket
{
    public const ushort PacketId = 0x0B5;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetKind = 4;
    private const int OffsetText = 6;

    /// <summary>sizeof(Str) - the server clamps to this regardless of declared size.</summary>
    public const int MaxTextLength = 128;

    public static byte[] Build(FfxiChatKind kind, string message, ushort sync)
    {
        byte[] text = Encoding.ASCII.GetBytes(message);
        if (text.Length >= MaxTextLength)
        {
            text = text[..(MaxTextLength - 1)];
        }

        // One byte for a terminator, then round the whole packet up to the
        // 4-byte unit the size field counts in.
        int unpadded = OffsetText + text.Length + 1;
        int size = (unpadded + 3) & ~3;

        var packet = new byte[size];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, size));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        packet[OffsetKind] = (byte)kind;
        text.CopyTo(packet, OffsetText);

        return packet;
    }
}

/// <summary>
/// GP_CLI_COMMAND_CHAT_NAME (C2S 0x0B6) - sending a tell.
///
/// Same variable-length rule as <see cref="FfxiChatPacket"/>, with a different
/// constant: the recipient name occupies offsets 6..20, so the server derives
/// the message length as `header.size * 4 - 0x15` rather than `- 6`.
///
/// Layout read back from the real compiled struct: unknown04 at 4, unknown05
/// at 5, sName[15] at 6, Mes[128] at 21.
/// </summary>
public static class FfxiTellPacket
{
    public const ushort PacketId = 0x0B6;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetUnknown04 = 4;
    private const int OffsetUnknown05 = 5;
    private const int OffsetRecipient = 6;
    private const int OffsetText = 21;

    private const int RecipientLength = 15;
    public const int MaxTextLength = 128;

    /// <summary>
    /// `unknown04` has to be 3 and `unknown05` has to be 0 - the server
    /// validates both (`mustEqual(this->unknown04, 3, "unknown04 not 3")`) and
    /// drops the packet otherwise. Their meaning is unknown; the names are the
    /// server's own. Leaving them zero, which is the obvious default for a
    /// field called "unknown", silently fails - the server logs a warning the
    /// sender never sees.
    /// </summary>
    private const byte RequiredUnknown04 = 3;
    private const byte RequiredUnknown05 = 0;

    public static byte[] Build(string recipient, string message, ushort sync)
    {
        byte[] text = Encoding.ASCII.GetBytes(message);
        if (text.Length >= MaxTextLength)
        {
            text = text[..(MaxTextLength - 1)];
        }

        int unpadded = OffsetText + text.Length + 1;
        int size = (unpadded + 3) & ~3;

        var packet = new byte[size];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, size));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        packet[OffsetUnknown04] = RequiredUnknown04;
        packet[OffsetUnknown05] = RequiredUnknown05;

        byte[] name = Encoding.ASCII.GetBytes(recipient);
        name.AsSpan(0, Math.Min(name.Length, RecipientLength - 1)).CopyTo(packet.AsSpan(OffsetRecipient, RecipientLength));

        text.CopyTo(packet, OffsetText);
        return packet;
    }
}

/// <summary>
/// GP_SERV_COMMAND_CHAT_STD (S2C 0x017) - a chat message to display.
///
/// Body layout from the real compiled struct: Kind at body+0, Attr at +1,
/// Data (uint16) at +2, sName[15] at +4, Mes[150] at +19 - so within the
/// whole sub-packet, sName is at 8 and the text at 23. The uint16 in the
/// middle is what pushes sName off the offset a by-hand reading would give
/// it.
/// </summary>
public sealed record FfxiChatMessage(
    FfxiChatMessageType Kind,
    byte Attributes,
    ushort Data,
    string Sender,
    string Text)
{
    public const ushort PacketId = 0x017;

    private const int OffsetKind = 4;
    private const int OffsetAttributes = 5;
    private const int OffsetData = 6;
    private const int OffsetSender = 8;
    private const int OffsetText = 23;

    private const int SenderLength = 15;

    /// <summary>Smallest packet that still contains a sender and at least an empty message.</summary>
    public const int MinimumSize = OffsetText;

    public static FfxiChatMessage? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < MinimumSize)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket[..2]));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiChatMessage(
            Kind: (FfxiChatMessageType)subPacket[OffsetKind],
            Attributes: subPacket[OffsetAttributes],
            Data: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetData, 2)),
            Sender: ReadFixedString(subPacket.Slice(OffsetSender, SenderLength)),
            Text: ReadFixedString(subPacket[OffsetText..]));
    }

    private static string ReadFixedString(ReadOnlySpan<byte> field)
    {
        int nul = field.IndexOf((byte)0);
        return Encoding.ASCII.GetString(nul >= 0 ? field[..nul] : field).TrimEnd();
    }
}
