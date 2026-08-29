using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_GAMEOK (C2S 0x00C, 12 bytes) - "I have finished loading the
/// zone and I am ready for the rest".
///
/// The server's own handler describes it as "one of the first packets sent
/// when zoning in", and it "causes the server to start rapidly sending a lot
/// of information to initialize the client": ENTERZONE, the config and job
/// blocks, inventory, key items, quest and mission logs, merits, magic, mounts,
/// a char sync, and more. A client that never sends it completes the 0x00A
/// handshake and then simply never receives any of that.
///
/// PortJeuno skipped this entirely until now, which made its zone-in
/// incomplete even though login appeared to succeed - the character reaches
/// the world, moves and talks, but the server never runs the initialization
/// half of zoning for it.
///
/// Both fields are validated as zero (`ClientState not 0`, `DebugClientFlg not
/// 0`), so the packet is a header and eight zero bytes. That makes it cheap to
/// send and impossible to get subtly wrong, unlike the tell packet's
/// deceptively-named "unknown" fields.
/// </summary>
public static class FfxiGameOkPacket
{
    public const ushort PacketId = 0x00C;
    public const int PacketSize = 12;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;

    public static byte[] Build(ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);

        // ClientState (offset 4) and the flag word (offset 8) both stay zero -
        // the server rejects anything else.
        return packet;
    }
}
