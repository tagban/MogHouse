using System.Net;
using System.Net.Sockets;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// What the server sent back to an 0x00A, after decryption and integrity
/// checking. <see cref="Payload"/> is still compressed - see
/// <see cref="FfxiZoneClient"/>'s remarks.
/// </summary>
public sealed record FfxiZoneReply(
    byte[] Datagram,
    byte[] Payload,
    bool ChecksumValid,
    ushort ServerCounter,
    ushort AcknowledgedClientCounter);

/// <summary>
/// The UDP zone/map connection (the address and port come from
/// <see cref="FfxiZoneHandoff"/>). This is the third and last transport in
/// the login chain and shares nothing with the other two: the auth session is
/// TLS+JSON, the data/view sessions are plain TCP, and this is UDP with its
/// own framing, its own integrity check, and Blowfish encryption.
///
/// Grounded in LandSandBoat/server's src/map/map_networking.cpp (branch
/// `base`, 2026-08-28).
///
/// Send path: the opening 0x00A is plaintext, and it's the only packet that
/// ever is - the server tries a plaintext MD5 first and requires any packet
/// that validates that way to be an 0x00A.
///
/// Receive path: strip the header, Blowfish-decrypt the body, then MD5-check
/// it. That order matters and is genuinely useful here - the checksum is
/// computed over the *decrypted* bytes, so a passing checksum proves the
/// cipher, the key schedule and the key derivation are all correct. It's the
/// server's own test for "did this decrypt?", which is how it decides whether
/// to retry with the previous key mid-zone-transition.
///
/// What this class deliberately does NOT do yet is decompress the payload.
/// The server's compression is not zlib despite the file being named zlib.cpp
/// - it's a static-Huffman codec driven by two lookup tables loaded at
/// startup from `res/compress.dat` and `res/decompress.dat`, with a bit
/// length (not byte length) as its size field. Implementing it needs those
/// tables, so <see cref="FfxiZoneReply.Payload"/> hands back the decrypted
/// but still-compressed body rather than pretending to parse it.
/// </summary>
public sealed class FfxiZoneClient : IDisposable
{
    private readonly UdpClient _udp = new();
    private readonly FfxiBlowfish _blowfish;

    /// <summary>Our own outgoing packet counter (header offset 0).</summary>
    private ushort _ownCounter;

    public FfxiZoneClient(FfxiZoneHandoff handoff)
    {
        _blowfish = FfxiBlowfish.FromSessionKey(handoff.SessionKey);
    }

    /// <summary>
    /// Sends the opening GP_CLI_COMMAND_LOGIN. `uniqueNo` is the character id;
    /// the server will only accept it if a *pending session* exists for that
    /// id, which the login server creates by IPC as part of character select -
    /// so this must follow a real SelectCharacterAsync, not stand alone.
    /// </summary>
    public async Task SendLoginAsync(IPEndPoint zoneServer, uint uniqueNo, string characterName, string accountName, uint clientVersion, ushort clientLanguage, CancellationToken ct = default)
    {
        if (_ownCounter == 0)
        {
            _ownCounter = 1;
        }

        byte[] datagram = FfxiZoneLoginPacket.BuildDatagram(
            uniqueNo: uniqueNo,
            characterName: characterName,
            accountName: accountName,
            ticket: default,
            clientVersion: clientVersion,
            clientLanguage: clientLanguage,
            ownCounter: _ownCounter,
            sync: _ownCounter,
            timestamp: (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds());

        LastSentDatagram = datagram;
        await _udp.SendAsync(datagram, zoneServer, ct);
    }

    /// <summary>The last datagram handed to the socket, kept for diagnostics.</summary>
    public byte[]? LastSentDatagram { get; private set; }

    /// <summary>
    /// Sends the login packet and waits for the server's answer, retransmitting
    /// until one arrives. Retransmission is not a convenience here, it's
    /// required by how the server is built - a first 0x00A from an unknown
    /// address is answered by *nothing*, twice over:
    ///
    ///  1. `handle_incoming_packet` looks its session up by address, gets null,
    ///     and passes that null pointer to `recv_parse` **by value**.
    ///     `recv_parse` does create the session (adopting the pending session
    ///     the login server set up during character select), but it assigns it
    ///     to its own local parameter, so the caller's pointer is still null on
    ///     return - and the very next line is `if (PSession == nullptr) return;`.
    ///     The session now exists; nothing was sent back.
    ///  2. Independently, the pending session that step 1 consumes is created
    ///     by an IPC message that travels login server -> world server -> map
    ///     server after the handoff packet is already on its way to us. A
    ///     client fast enough to answer the handoff immediately can beat it.
    ///
    /// Either way the fix is the same and matches what the protocol expects of
    /// a UDP client: keep sending until answered. The second attempt finds the
    /// session registered against our address and gets a real reply.
    ///
    /// Each retransmit reuses the same counter and sync deliberately - it's the
    /// same logical packet, and the server's `parse` loop drops any sub-packet
    /// whose sync exceeds the outer header counter.
    /// </summary>
    public async Task<FfxiZoneReply?> LoginAsync(
        IPEndPoint zoneServer,
        uint uniqueNo,
        string characterName,
        string accountName,
        uint clientVersion,
        ushort clientLanguage,
        int attempts = 5,
        TimeSpan? retryInterval = null,
        CancellationToken ct = default)
    {
        TimeSpan interval = retryInterval ?? TimeSpan.FromSeconds(2);

        for (int attempt = 1; attempt <= attempts; attempt++)
        {
            await SendLoginAsync(zoneServer, uniqueNo, characterName, accountName, clientVersion, clientLanguage, ct);

            FfxiZoneReply? reply = await ReceiveAsync(interval, ct);
            if (reply is not null)
            {
                return reply;
            }
        }

        return null;
    }

    /// <summary>
    /// Waits for a reply, then decrypts and integrity-checks it. Returns null
    /// if nothing arrives before <paramref name="timeout"/> - a silent drop is
    /// the normal way this protocol rejects a packet it doesn't like, so a
    /// timeout is an expected outcome worth reporting rather than an
    /// exception.
    /// </summary>
    public async Task<FfxiZoneReply?> ReceiveAsync(TimeSpan timeout, CancellationToken ct = default)
    {
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(timeout);

        UdpReceiveResult result;
        try
        {
            result = await _udp.ReceiveAsync(timeoutCts.Token);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            return null;
        }

        return Decode(result.Buffer, _blowfish);
    }

    /// <summary>
    /// Decrypts and verifies a received datagram. Split out from
    /// <see cref="ReceiveAsync"/> so it can be tested against captured bytes
    /// without a socket.
    /// </summary>
    internal static FfxiZoneReply Decode(byte[] datagram, FfxiBlowfish blowfish)
    {
        var working = (byte[])datagram.Clone();

        // map_decipher_packet: the cipher covers whole 64-bit blocks starting
        // at byte 28 (`(uint32*)buff + 7`), and an odd trailing 4-byte word is
        // left in the clear (`tmp -= tmp % 2`).
        int words = (working.Length - FfxiZonePacket.HeaderSize) / 4;
        words -= words % 2;

        if (words > 0)
        {
            Span<uint> body = new uint[words];
            for (int i = 0; i < words; i++)
            {
                body[i] = BitConverter.ToUInt32(working, FfxiZonePacket.HeaderSize + i * 4);
            }

            blowfish.DecipherBlocks(body);

            for (int i = 0; i < words; i++)
            {
                BitConverter.TryWriteBytes(working.AsSpan(FfxiZonePacket.HeaderSize + i * 4, 4), body[i]);
            }
        }

        int payloadLength = working.Length - FfxiZonePacket.HeaderSize - FfxiZonePacket.ChecksumSize;
        bool checksumValid = false;
        byte[] payload = [];

        if (payloadLength > 0)
        {
            payload = working.AsSpan(FfxiZonePacket.HeaderSize, payloadLength).ToArray();
            byte[] expected = System.Security.Cryptography.MD5.HashData(payload);
            checksumValid = working.AsSpan(working.Length - FfxiZonePacket.ChecksumSize).SequenceEqual(expected);
        }

        return new FfxiZoneReply(
            Datagram: working,
            Payload: payload,
            ChecksumValid: checksumValid,
            ServerCounter: BitConverter.ToUInt16(working, FfxiZonePacket.OffsetOwnCounter),
            AcknowledgedClientCounter: BitConverter.ToUInt16(working, FfxiZonePacket.OffsetPeerCounter));
    }

    public void Dispose() => _udp.Dispose();
}
