using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_FRAGMENTS (C2S 0x04B, 24 bytes) - asks for something too
/// big for one packet, a fragment at a time.
///
/// The server's login message is the reason this exists here. It is not pushed
/// on zone-in the way chat is: the client has to ask for it, and the server
/// only answers what it was asked for. A client that never sends this simply
/// never sees the message, which is not a parsing failure and looks exactly
/// like a server that has nothing to say.
///
/// The reply is GP_SERV_COMMAND_FRAGMENTS::SERVMES (S2C 0x04D), up to 236
/// bytes at a time - see <see cref="FfxiServerMessage"/>, which reassembles it.
/// </summary>
public static class FfxiFragmentsPacket
{
    public const ushort PacketId = 0x04B;
    public const int PacketSize = 24;

    /// <summary>value1 - which of the fragmented things is wanted.</summary>
    public const byte KindServerMessage = 1;
    public const byte KindFishingRank = 2;

    /// <summary>value2 - the language to answer in, as the server reads it.</summary>
    public const byte LanguageEnglish = 2;
    public const byte LanguageFrench = 4;

    /// <summary>Command - 1 opens the transfer, 2 continues it.</summary>
    public const byte CommandStart = 1;
    public const byte CommandContinue = 2;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetCommand = 4;
    private const int OffsetResult = 5;
    private const int OffsetValue1 = 6;
    private const int OffsetValue2 = 7;
    private const int OffsetTimestamp = 8;
    private const int OffsetSizeTotal = 12;
    private const int OffsetOffset = 16;
    private const int OffsetDataSize = 20;

    /// <summary>
    /// Requests the server message from <paramref name="offset"/> onward. The
    /// server refuses to answer the same offset twice in a row - it
    /// deduplicates fragment retries - so asking again for what has already
    /// arrived is silently ignored rather than answered.
    /// </summary>
    public static byte[] BuildServerMessageRequest(ushort sync, int offset = 0, int timestamp = 0,
                                                   byte language = LanguageEnglish)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);

        packet[OffsetCommand] = offset == 0 ? CommandStart : CommandContinue;
        packet[OffsetResult] = 0;
        packet[OffsetValue1] = KindServerMessage;
        packet[OffsetValue2] = language;

        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(OffsetTimestamp, 4), timestamp);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(OffsetSizeTotal, 4), 0);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(OffsetOffset, 4), offset);
        BinaryPrimitives.WriteInt32LittleEndian(packet.AsSpan(OffsetDataSize, 4), 0);
        return packet;
    }
}
