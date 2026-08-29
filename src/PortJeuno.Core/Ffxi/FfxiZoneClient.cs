using System.Net;
using System.Net.Sockets;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// What the server sent back to an 0x00A, after decryption and integrity
/// checking. <see cref="Payload"/> is still compressed - see
/// <see cref="FfxiZoneClient"/>'s remarks.
/// </summary>
/// <param name="Payload">
/// The decrypted body, still compressed, with its trailing MD5 stripped. Its
/// last four bytes are the declared bit count - see <see cref="DeclaredBits"/>.
/// </param>
/// <param name="DeclaredBits">
/// The compressed size the server stamped into the packet, in bits and
/// including the compression marker's 8 bits. Null when the payload is too
/// short to hold one.
/// </param>
/// <param name="Plaintext">
/// The decompressed body, when a Huffman codec was supplied and decoding
/// succeeded - otherwise null. This is the sequence of sub-packets the game
/// actually runs on.
/// </param>
public sealed record FfxiZoneReply(
    byte[] Datagram,
    byte[] Payload,
    bool ChecksumValid,
    ushort ServerCounter,
    ushort AcknowledgedClientCounter,
    uint? DeclaredBits,
    byte[]? Plaintext);

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
    private readonly FfxiHuffman? _codec;

    /// <summary>Our own outgoing packet counter (header offset 0).</summary>
    private ushort _ownCounter;

    /// <param name="codec">
    /// Optional. Without it, replies still decrypt and verify but their bodies
    /// stay compressed - useful because the compression tables aren't bundled
    /// (see <see cref="FfxiHuffmanTables"/>), so the transport is testable
    /// without them.
    /// </param>
    public FfxiZoneClient(FfxiZoneHandoff handoff, FfxiHuffman? codec = null)
    {
        _blowfish = FfxiBlowfish.FromSessionKey(handoff.SessionKey);
        _codec = codec;
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

        return Decode(result.Buffer, _blowfish, _codec);
    }

    /// <summary>
    /// Holds an established session open by resending the login packet on an
    /// interval, until <paramref name="duration"/> elapses or the caller
    /// cancels. Returns how many keepalives were sent.
    ///
    /// This works because a repeat 0x00A on a session the server already knows
    /// takes a different path than the first one: `recv_parse` finds the
    /// session by address, so it skips the pending-session adoption and the
    /// character reload, and `handle_incoming_packet` then runs `parse`, whose
    /// first act is `tapLastUpdate()` for any session no longer in the
    /// WAITING or PENDING_ZONE state - which is exactly what an accepted
    /// session is. That tap is what the cleanup sweep checks, so the session
    /// stops being reaped ~60s after login.
    ///
    /// It also survives `GP_CLI_COMMAND_LOGIN::validate`'s "Player already
    /// logged in" rejection, which only fires once `hasDecryptedPacket` is
    /// set - and that flag is only set by a successfully *decrypted* packet,
    /// which this client cannot send yet. So this is a keepalive that happens
    /// to work rather than the mechanism the real client uses; the real one
    /// holds its session open with ordinary encrypted traffic. Replace this
    /// once the compressor lands and real packets can be sent.
    /// </summary>
    public async Task<int> HoldSessionAsync(
        IPEndPoint zoneServer,
        uint uniqueNo,
        string characterName,
        string accountName,
        uint clientVersion,
        ushort clientLanguage,
        TimeSpan duration,
        TimeSpan? interval = null,
        CancellationToken ct = default)
    {
        TimeSpan gap = interval ?? TimeSpan.FromSeconds(20);
        DateTimeOffset until = DateTimeOffset.UtcNow + duration;
        int sent = 0;

        while (DateTimeOffset.UtcNow < until && !ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(gap, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            await SendLoginAsync(zoneServer, uniqueNo, characterName, accountName, clientVersion, clientLanguage, ct);
            sent++;
        }

        return sent;
    }

    /// <summary>
    /// Decrypts and verifies a received datagram. Split out from
    /// <see cref="ReceiveAsync"/> so it can be tested against captured bytes
    /// without a socket.
    /// </summary>
    internal static FfxiZoneReply Decode(byte[] datagram, FfxiBlowfish blowfish, FfxiHuffman? codec = null)
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

        // The server stamps the compressed size, in bits, into the four bytes
        // immediately before the MD5 - so within `payload` (which excludes the
        // hash) it's the last word. See `compressPacket`, which writes it at
        // `zlib_compressed_size(packetSize)`, i.e. right past the compressed
        // bytes.
        uint? declaredBits = null;
        byte[]? plaintext = null;

        if (payload.Length >= 4)
        {
            declaredBits = BitConverter.ToUInt32(payload, payload.Length - 4);

            if (codec is not null && checksumValid)
            {
                plaintext = TryDecompress(payload, declaredBits.Value, codec);
            }
        }

        return new FfxiZoneReply(
            Datagram: working,
            Payload: payload,
            ChecksumValid: checksumValid,
            ServerCounter: BitConverter.ToUInt16(working, FfxiZonePacket.OffsetOwnCounter),
            AcknowledgedClientCounter: BitConverter.ToUInt16(working, FfxiZonePacket.OffsetPeerCounter),
            DeclaredBits: declaredBits,
            Plaintext: plaintext);
    }

    /// <summary>
    /// The declared size counts the compression marker's 8 bits, so the actual
    /// bitstream is eight bits shorter.
    ///
    /// The server passes the declared value to its own decompressor *unadjusted*,
    /// even though that function measures bits from after the marker - so it
    /// walks eight bits past the real end and decodes a stray trailing symbol
    /// or two. Harmless there, because sub-packet iteration is bounded by each
    /// sub-packet's own size field and stops before reaching the garbage. This
    /// subtracts the 8 instead, to produce exactly the bytes the server
    /// compressed rather than replicating an off-by-one that only adds noise.
    /// </summary>
    private static byte[]? TryDecompress(byte[] payload, uint declaredBits, FfxiHuffman codec)
    {
        if (declaredBits < 8)
        {
            return null;
        }

        int bitstreamBits = (int)declaredBits - 8;
        int compressedBytes = 1 + FfxiHuffman.CompressedByteLength(bitstreamBits);
        if (compressedBytes > payload.Length - 4)
        {
            return null;
        }

        // A Huffman-coded byte can't take fewer than one bit, so the bit count
        // is its own upper bound on the symbol count.
        var output = new byte[bitstreamBits];
        int written = codec.Decompress(payload.AsSpan(0, compressedBytes), bitstreamBits, output);

        return written < 0 ? null : output.AsSpan(0, written).ToArray();
    }

    public void Dispose() => _udp.Dispose();
}
