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
    public const ushort TalkNumPacketId = 0x036;
    public const ushort TalkNumWorkPacketId = 0x02A;

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

        return new FfxiNpcMessage(
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(Body, 4)),
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(actIndex, 2)),
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(mesNum, 2)),
            subPacket[type]);
    }
}
