using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// An NPC saying something, as a line id rather than as words.
///
/// Two packets carry NPC dialogue and differ only in how much they carry with
/// it. TALKNUM (0x036) is the id and who said it; TALKNUMWORK (0x02A) adds four
/// numbers and a string the line's placeholders are filled from - a price, a
/// count, somebody's name. Both index the zone's dialogue DAT.
///
/// The text itself never crosses the network. That was a dial-up era decision:
/// the client already has every line on disk, so the server sends the number.
/// It is also why an NPC speaks the language the client was installed in.
/// </summary>
public sealed record FfxiNpcMessage(uint UniqueNo, ushort ActIndex, ushort MessageId, byte Type)
{
    /// <summary>
    /// Whether this line should appear without a name in front of it - which
    /// the server also sets for anything a player says. See
    /// <see cref="NamelessFlag"/>.
    /// </summary>
    public bool Nameless { get; init; }

    public const ushort TalkNumPacketId = 0x036;
    public const ushort TalkNumWorkPacketId = 0x02A;

    /// <summary>
    /// The top bit of MesNum is a flag, not part of the id.
    ///
    /// From the server's own builder:
    ///
    /// <code>
    /// packet.MesNum = (PEntity->objtype == TYPE_PC || !showName)
    ///                 ? (messageID + 0x8000) : messageID;
    /// </code>
    ///
    /// So it is set when a player is speaking, or when the line is meant to
    /// appear without a name in front of it. Taken as part of the id it puts
    /// every such line 32,768 past the end of the zone's table, which is
    /// exactly what "(line 48555)" was: 48555 - 32768 is 15787, an ordinary
    /// line in Southern San d'Oria, and 39207 - 32768 is 6439, an ordinary
    /// line in West Ronfaure.
    /// </summary>
    private const ushort NamelessFlag = 0x8000;

    private const int Body = 4; // id/size/sync sub-packet header

    // TALKNUM: UniqueNo, ActIndex, MesNum, Type.
    private const int TalkNumActIndex = Body + 4;
    private const int TalkNumMesNum = Body + 6;
    private const int TalkNumType = Body + 8;

    // TALKNUMWORK: UniqueNo, num[4], ActIndex, MesNum, Type, Flag, String[32].
    private const int TalkNumWorkActIndex = Body + 0x14;
    private const int TalkNumWorkMesNum = Body + 0x16;
    private const int TalkNumWorkType = Body + 0x18;

    /// <summary>Reads either dialogue packet, or null if this is neither.</summary>
    public static FfxiNpcMessage? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < Body + 2)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));

        int actIndex, mesNum, type;
        switch (id)
        {
            case TalkNumPacketId:
                (actIndex, mesNum, type) = (TalkNumActIndex, TalkNumMesNum, TalkNumType);
                break;
            case TalkNumWorkPacketId:
                (actIndex, mesNum, type) = (TalkNumWorkActIndex, TalkNumWorkMesNum, TalkNumWorkType);
                break;
            default:
                return null;
        }

        if (subPacket.Length < type + 1)
        {
            return null;
        }

        ushort raw = BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(mesNum, 2));

        return new FfxiNpcMessage(
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(Body, 4)),
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(actIndex, 2)),
            (ushort)(raw & ~NamelessFlag),
            subPacket[type])
        {
            Nameless = (raw & NamelessFlag) != 0,
        };
    }
}
