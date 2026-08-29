using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>GP_GAME_LOGOUT_STATE - why the server is ending our time in this zone.</summary>
public enum FfxiLogoutState : byte
{
    None = 0,
    Logout = 1,
    ZoneChange = 2,
    MyRoom = 3,
    Cancelled = 4,
    PolExit = 5,
    JobExit = 6,
    PolExitMyRoom = 7,
    Timeout = 8,
    GmLogout = 9,
    End = 10,
}

/// <summary>
/// GP_SERV_COMMAND_LOGOUT (S2C 0x00B) - the server telling us we are leaving
/// this zone, and where to go next.
///
/// Despite the name this is mostly *not* a logout: state 2 is an ordinary zone
/// change, which is what a client sees when it walks through a zone line or a
/// GM teleports it somewhere else. The packet carries the next zone server's
/// address, so it is both a notification and a handoff.
///
/// Handling it is not optional. The server rotates its Blowfish key
/// immediately after sending this (`incrementBlowfish`, key[4] += 2), so a
/// client that ignores the packet keeps talking to the old address with the
/// old key and the session simply dies - which looks, from the outside, like
/// the character being logged off for no reason.
/// </summary>
public sealed record FfxiZoneTransition(
    FfxiLogoutState State,
    uint ZoneServerIp,
    uint ZoneServerPort,
    uint ErrorCode)
{
    public const ushort PacketId = 0x00B;

    private const int OffsetState = 4;
    private const int OffsetIp = 8;
    private const int OffsetPort = 12;
    private const int OffsetErrorCode = 24;

    /// <summary>Header, state (padded), the 16-byte address block, then the error code.</summary>
    public const int MinimumSize = 28;

    /// <summary>True when we should reconnect rather than stop.</summary>
    public bool IsZoneChange => State is FfxiLogoutState.ZoneChange or FfxiLogoutState.MyRoom;

    public static FfxiZoneTransition? TryParse(ReadOnlySpan<byte> subPacket)
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

        return new FfxiZoneTransition(
            State: (FfxiLogoutState)subPacket[OffsetState],
            // The address is written from an IPP built out of str2ip, so the
            // octets are in wire order and read big-endian - the same
            // convention as the login server's zone handoff.
            ZoneServerIp: BinaryPrimitives.ReadUInt32BigEndian(subPacket.Slice(OffsetIp, 4)),
            ZoneServerPort: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetPort, 4)),
            ErrorCode: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetErrorCode, 4)));
    }
}
