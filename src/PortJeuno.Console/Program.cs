using System.Text.Json;
using PortJeuno.Core.Ffxi;

// A flag's value is the next token, unless that token is itself another
// "--flag" - this lets valueless flags like --save sit anywhere in the
// list without desyncing the ones after it (a strict --flag/value pairing
// scheme breaks the instant one flag in the middle has no value).
Dictionary<string, string> ParseFlags(string[] args)
{
    var flags = new Dictionary<string, string>();
    string[] rest = args.Skip(1).ToArray();

    for (int i = 0; i < rest.Length; i++)
    {
        if (!rest[i].StartsWith("--"))
        {
            continue;
        }

        string name = rest[i][2..];
        bool hasValue = i + 1 < rest.Length && !rest[i + 1].StartsWith("--");
        flags[name] = hasValue ? rest[++i] : "";
    }

    return flags;
}

if (args.Length == 0)
{
    PrintUsage();
    return 1;
}

switch (args[0])
{
    case "list":
        ListProfiles();
        return 0;

    case "login":
        return await LoginAsync(ParseFlags(args));

    case "create-account":
        return await CreateAccountAsync(ParseFlags(args));

    default:
        PrintUsage();
        return 1;
}

static void PrintUsage()
{
    Console.WriteLine("""
        PortJeuno.Console - manual live-test harness for the FFXI login/roster client.
        NOT the real client - just enough to confirm FfxiAuthClient/FfxiRosterClient
        actually work against a real LandSandBoat server.

        Usage:
          list
          login --host <host> --username <user> --password <pass> [--name <profileName>] [--otp <code>] [--save]
          login --profile <idOrName> [--otp <code>] [--password <pass>]
          login --credentials-file <path.json> [--save]
            (file shape: {"host":"...","username":"...","password":"...","name":"...","otp":"..."} -
             avoids the password ever appearing in the command line/shell history)
          create-account --host <host> --username <user> --password <pass> [--auth-port <port>]
          create-account --credentials-file <path.json>

        Add --select-character <name> to any login to also select a
        character after listing the roster and print the zone-server
        handoff. Add --zone on top of that to also send the zone server's
        GP_CLI_COMMAND_LOGIN (0x00A) over UDP and decrypt its reply, and
        --zone-hold <seconds> to keep that session alive afterwards
        (otherwise the server reaps it about 60s after login).

        Ports default to the standard 54231/54230/54001 - override with
        --auth-port/--data-port/--view-port if your server differs.
        """);
}

static void ListProfiles()
{
    if (FfxiServerProfileStore.Profiles.Count == 0)
    {
        Console.WriteLine($"No saved profiles yet ({FfxiServerProfileStore.FilePath}).");
        return;
    }

    foreach (FfxiServerProfile p in FfxiServerProfileStore.Profiles)
    {
        Console.WriteLine($"{p.Id}  {p.Name,-20} {p.Host}:{p.AuthPort}/{p.DataPort}/{p.ViewPort}  user={p.Username}  hasPassword={p.Password.Length > 0}  trustToken={p.TrustToken.Length > 0}");
    }
}

/// <summary>Merges --credentials-file's JSON into flags (only filling keys not already set). Returns false (with a message printed) if the file was requested but missing.</summary>
static bool TryMergeCredentialsFile(Dictionary<string, string> flags)
{
    if (!flags.TryGetValue("credentials-file", out string? credentialsPath))
    {
        return true;
    }

    if (!File.Exists(credentialsPath))
    {
        Console.WriteLine($"Credentials file not found: {credentialsPath}");
        return false;
    }

    using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(credentialsPath));
    JsonElement root = doc.RootElement;

    void CopyIfPresent(string jsonName, string flagName)
    {
        if (root.TryGetProperty(jsonName, out JsonElement el) && el.ValueKind == JsonValueKind.String && !flags.ContainsKey(flagName))
        {
            flags[flagName] = el.GetString() ?? "";
        }
    }

    CopyIfPresent("host", "host");
    CopyIfPresent("username", "username");
    CopyIfPresent("password", "password");
    CopyIfPresent("name", "name");
    CopyIfPresent("otp", "otp");
    return true;
}

static async Task<int> CreateAccountAsync(Dictionary<string, string> flags)
{
    if (!TryMergeCredentialsFile(flags))
    {
        return 1;
    }

    if (!flags.TryGetValue("host", out string? host) || !flags.TryGetValue("username", out string? username) || !flags.TryGetValue("password", out string? password))
    {
        Console.WriteLine("create-account requires --host, --username, and --password (or --credentials-file).");
        return 1;
    }

    int authPort = flags.TryGetValue("auth-port", out string? authPortStr) ? int.Parse(authPortStr) : FfxiConstants.AuthPort;

    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
    try
    {
        using var auth = new FfxiAuthClient();
        await auth.ConnectAsync(host, authPort, timeout.Token);
        FfxiLoginResponse response = await auth.CreateAccountAsync(username, password, timeout.Token);

        Console.WriteLine($"Result: {response.Result}");
        if (response.ErrorMessage is not null)
        {
            Console.WriteLine($"Server message: {response.ErrorMessage}");
        }

        return response.Result == FfxiLoginResult.SuccessCreate ? 0 : 1;
    }
    catch (Exception ex)
    {
        Console.WriteLine($"Account creation failed: {ex}");
        return 1;
    }
}

static async Task<int> LoginAsync(Dictionary<string, string> flags)
{
    if (!TryMergeCredentialsFile(flags))
    {
        return 1;
    }

    FfxiServerProfile? profile = null;

    if (flags.TryGetValue("profile", out string? idOrName))
    {
        profile = FfxiServerProfileStore.Find(idOrName)
            ?? FfxiServerProfileStore.Profiles.FirstOrDefault(p => p.Name.Equals(idOrName, StringComparison.OrdinalIgnoreCase));

        if (profile is null)
        {
            Console.WriteLine($"No saved profile matches '{idOrName}'. Run 'list' to see what's saved.");
            return 1;
        }

        if (flags.TryGetValue("password", out string? overridePassword) && overridePassword.Length > 0)
        {
            profile.Password = overridePassword;
        }
    }
    else
    {
        if (!flags.TryGetValue("host", out string? host) || !flags.TryGetValue("username", out string? username) || !flags.TryGetValue("password", out string? password))
        {
            Console.WriteLine("login without --profile requires --host, --username, and --password.");
            return 1;
        }

        bool save = flags.ContainsKey("save");
        profile = save
            ? FfxiServerProfileStore.CreateAndSave(flags.GetValueOrDefault("name", host), host)
            : new FfxiServerProfile { Host = host };

        profile.Username = username;
        profile.Password = password;

        if (flags.TryGetValue("auth-port", out string? authPort))
        {
            profile.AuthPort = int.Parse(authPort);
        }
        if (flags.TryGetValue("data-port", out string? dataPort))
        {
            profile.DataPort = int.Parse(dataPort);
        }
        if (flags.TryGetValue("view-port", out string? viewPort))
        {
            profile.ViewPort = int.Parse(viewPort);
        }

        if (save)
        {
            FfxiServerProfileStore.Save();
        }
    }

    string? otp = flags.GetValueOrDefault("otp");

    Console.WriteLine($"Connecting to {profile.Host}:{profile.AuthPort} as '{profile.Username}'...");

    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));

    try
    {
        var client = new FfxiLoginClient();
        (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters, FfxiRosterClient? roster) = await client.LoginAsync(profile, otp, ct: timeout.Token);
        using (roster)
        {
            Console.WriteLine($"Result: {login.Result}");
            if (login.ErrorMessage is not null)
            {
                Console.WriteLine($"Server message: {login.ErrorMessage}");
            }

            if (login.Result != FfxiLoginResult.Success)
            {
                return 1;
            }

            Console.WriteLine($"account_id={login.AccountId}  session_hash={Convert.ToHexString(login.SessionHash!)}");

            if (login.TrustToken is not null)
            {
                profile.TrustToken = login.TrustToken;
                if (FfxiServerProfileStore.Profiles.Contains(profile))
                {
                    FfxiServerProfileStore.Save();
                }
                Console.WriteLine("Received and saved a new trust_token.");
            }

            // The server always sends a full slot array, padded up to the
            // account's content-id limit if character creation is enabled - an
            // empty slot's name is a single space (data_session.cpp sets only
            // byte[0] = 0x20 so the real client shows a blank slot instead of
            // a "hume" placeholder for an all-zero name), not literally empty.
            List<FfxiCharacter> occupied = characters.Where(c => !string.IsNullOrWhiteSpace(c.Name)).ToList();
            Console.WriteLine($"Characters ({occupied.Count} of {characters.Count} slots occupied):");
            foreach (FfxiCharacter c in occupied)
            {
                Console.WriteLine($"  {c.Name,-16} world={c.WorldName,-10} race={c.Race} job={c.MainJob}/{c.MainJobLevel} sub={c.SubJob} zone={c.Zone} contentId={c.ContentId}");
            }

            if (!flags.TryGetValue("select-character", out string? selectName))
            {
                return 0;
            }

            FfxiCharacter? selected = occupied.FirstOrDefault(c => c.Name.Equals(selectName, StringComparison.OrdinalIgnoreCase));
            if (selected is null)
            {
                Console.WriteLine($"No character named '{selectName}' in the roster.");
                return 1;
            }

            if (roster is null)
            {
                Console.WriteLine("Roster session unavailable - cannot select a character.");
                return 1;
            }

            Console.WriteLine($"Selecting '{selected.Name}'...");
            FfxiZoneHandoff handoff = await roster.SelectCharacterAsync(selected, login.SessionHash!, timeout.Token);

            Console.WriteLine($"Zone handoff: character={handoff.CharacterName} serverId={handoff.ServerId}");
            Console.WriteLine($"  Zone server:   {FfxiRosterClient.FormatIpAddress(handoff.ZoneServerIp)}:{handoff.ZoneServerPort}");
            Console.WriteLine($"  Search server: {FfxiRosterClient.FormatIpAddress(handoff.SearchServerIp)}:{handoff.SearchServerPort}");
            Console.WriteLine($"  Session key (from our own 0xA2 key3): {string.Join(" ", handoff.SessionKey.Select(k => k.ToString("X8")))}");

            if (!flags.ContainsKey("zone"))
            {
                Console.WriteLine("(Add --zone to also send the zone-server login packet.)");
                return 0;
            }

            // The handoff reports the zone server's own configured address,
            // which for a server bound to 0.0.0.0 isn't a routable target -
            // fall back to the host the login server was reached on.
            string zoneHost = FfxiRosterClient.FormatIpAddress(handoff.ZoneServerIp);
            if (zoneHost is "0.0.0.0")
            {
                zoneHost = profile.Host;
                Console.WriteLine($"  (zone server reported 0.0.0.0; using {zoneHost} instead)");
            }

            var zoneEndpoint = new System.Net.IPEndPoint(System.Net.IPAddress.Parse(zoneHost), (int)handoff.ZoneServerPort);

            FfxiHuffmanTables? tables = FfxiHuffmanTables.TryLoadDefault();
            if (tables is null)
            {
                Console.WriteLine("  (compression tables not found - replies will decrypt but stay compressed.");
                Console.WriteLine($"   Set PORTJEUNO_FFXI_RES to a directory with {FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.)");
            }

            using var zone = new FfxiZoneClient(handoff, tables is null ? null : new FfxiHuffman(tables));

            Console.WriteLine($"Sending GP_CLI_COMMAND_LOGIN (0x00A) to {zoneEndpoint} for charid {handoff.ContentId} (retransmitting until answered)...");

            FfxiZoneReply? reply = await zone.LoginAsync(
                zoneEndpoint,
                uniqueNo: handoff.ContentId,
                characterName: handoff.CharacterName,
                accountName: profile.Username,
                clientVersion: 99,
                clientLanguage: 2);

            Console.WriteLine($"  sent: {Convert.ToHexString(zone.LastSentDatagram!)}");

            if (reply is null)
            {
                Console.WriteLine("No reply after retrying - the zone server drops packets it rejects rather than answering, so check its log for why.");
                return 1;
            }

            Console.WriteLine($"Zone reply: {reply.Datagram.Length} bytes, serverCounter={reply.ServerCounter}, acks our counter {reply.AcknowledgedClientCounter}");
            Console.WriteLine($"  MD5 after Blowfish decrypt: {(reply.ChecksumValid ? "VALID - cipher, key schedule and key derivation all confirmed correct" : "INVALID - decryption is wrong somewhere")}");
            FfxiZoneLoginReply? zoneState = null;
            Console.WriteLine($"  Decrypted payload ({reply.Payload.Length} bytes compressed, declared {reply.DeclaredBits} bits)");

            if (reply.Plaintext is null)
            {
                Console.WriteLine($"  Still compressed: {Convert.ToHexString(reply.Payload.AsSpan(0, Math.Min(64, reply.Payload.Length)))}...");
            }
            else
            {
                Console.WriteLine($"  DECOMPRESSED to {reply.Plaintext.Length} bytes:");
                Console.WriteLine($"    {Convert.ToHexString(reply.Plaintext.AsSpan(0, Math.Min(96, reply.Plaintext.Length)))}...");

                // Walk the sub-packet chain the way the server's own parse loop
                // does: each entry's first uint16 packs id:9 and size:7, with
                // the size counted in 4-byte units.
                Console.WriteLine("  Sub-packets:");
                int offset = 0;
                while (offset + 4 <= reply.Plaintext.Length)
                {
                    ushort word = BitConverter.ToUInt16(reply.Plaintext, offset);
                    (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(word);
                    if (size == 0 || offset + size > reply.Plaintext.Length)
                    {
                        break;
                    }

                    ushort sync = BitConverter.ToUInt16(reply.Plaintext, offset + 2);
                    Console.WriteLine($"    0x{id:X3}  {size,4} bytes  sync={sync}");

                    if (id == FfxiZoneLoginReply.PacketId && size == FfxiZoneLoginReply.PacketSize)
                    {
                        zoneState = FfxiZoneLoginReply.Parse(reply.Plaintext.AsSpan(offset, size));
                    }

                    offset += size;
                }

                if (zoneState is not null)
                {
                    Console.WriteLine();
                    Console.WriteLine($"  Zone state for {zoneState.Name} (charid {zoneState.UniqueNo}, entity index {zoneState.ActIndex}):");
                    Console.WriteLine($"    position  x={zoneState.X:F3} y={zoneState.Vertical:F3} z={zoneState.Depth:F3} facing={zoneState.Direction}");
                    Console.WriteLine($"    zone      {zoneState.ZoneNo} (map {zoneState.MapNumber}, sub {zoneState.ZoneSubNo})");
                    Console.WriteLine($"    playtime  {zoneState.PlayTime}s, game clock {zoneState.GameTime}");
                }
            }

            // Complete the zone-in handshake before anything else: the server
            // only starts sending the initialization batch once it sees this.
            await zone.SendGameOkAsync(zoneEndpoint);
            Console.WriteLine("Sent GP_CLI_COMMAND_GAMEOK (0x00C) - zone-in handshake completed.");

            if (flags.TryGetValue("zone-hold", out string? holdSeconds) && int.TryParse(holdSeconds, out int seconds))
            {
                Console.WriteLine("Ctrl+C to stop early.");

                if (zoneState is not null)
                {
                    Console.WriteLine($"Sending 0x015 position updates for {seconds}s - the real client's heartbeat, so the character reads as live rather than timed out...");

                    bool trace = flags.ContainsKey("zone-trace");
                    var seen = new Dictionary<ushort, int>();
                    var entitiesSeen = new Dictionary<uint, (ushort PacketId, bool IsSelf, int Count)>();

                    // Lets two test clients be parked next to each other
                    // regardless of where they logged out, which is what the
                    // "can A see B" experiment needs.
                    float posX = zoneState.X, posY = zoneState.Vertical, posZ = zoneState.Depth;
                    if (flags.TryGetValue("zone-pos", out string? posSpec))
                    {
                        string[] parts = posSpec.Split(',');
                        if (parts.Length == 3 &&
                            float.TryParse(parts[0], out float px) &&
                            float.TryParse(parts[1], out float py) &&
                            float.TryParse(parts[2], out float pz))
                        {
                            (posX, posY, posZ) = (px, py, pz);
                            Console.WriteLine($"  position overridden to x={posX} y={posY} z={posZ}");
                        }
                        else
                        {
                            Console.WriteLine($"  --zone-pos '{posSpec}' is not 'x,y,z' - using the server's position.");
                        }
                    }

                    int sent = await zone.HoldWithPositionAsync(
                        zoneEndpoint,
                        x: posX,
                        vertical: posY,
                        depth: posZ,
                        direction: zoneState.Direction,
                        duration: TimeSpan.FromSeconds(seconds),
                        interval: TimeSpan.FromMilliseconds(400),
                        walkRadius: flags.TryGetValue("zone-walk", out string? radius) && float.TryParse(radius, out float r) ? r : 2.0f,
                        sayEvery: flags.TryGetValue("zone-say", out string? sayText) && sayText.Length > 0 ? sayText : null,
                        sayKind: flags.TryGetValue("zone-say-kind", out string? kindText) && Enum.TryParse(kindText, true, out FfxiChatKind parsedKind) ? parsedKind : FfxiChatKind.Say,
                        followCharId: flags.TryGetValue("zone-follow", out string? followSpec) && uint.TryParse(followSpec, out uint followId) ? followId : null,
                        tellTo: flags.TryGetValue("zone-tell", out string? tellTarget) && tellTarget.Length > 0 ? tellTarget : null,
                        tellText: flags.GetValueOrDefault("zone-tell-text", "hello from PortJeuno"),
                        onReply: reply =>
                        {
                            if (reply.Plaintext is null)
                            {
                                return;
                            }

                            foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
                            {
                                seen[id] = seen.GetValueOrDefault(id) + 1;

                                FfxiEntityUpdate? entity = FfxiEntityUpdate.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (entity is not null)
                                {
                                    // The interesting question is never "did an
                                    // 0x00D arrive" - the server describes you
                                    // to yourself. It's whether one arrived
                                    // about somebody else.
                                    bool isSelf = entity.UniqueNo == handoff.ContentId;
                                    int count = entitiesSeen.GetValueOrDefault(entity.UniqueNo).Count + 1;
                                    entitiesSeen[entity.UniqueNo] = (entity.PacketId, isSelf, count);

                                    // Sample another player's reported position
                                    // occasionally. If our axis mapping were
                                    // wrong, this is where it would show:
                                    // the vertical would drift into the
                                    // terrain rather than staying put.
                                    if (!isSelf && entity.PacketId == FfxiEntityUpdate.PlayerPacketId && count % 40 == 1)
                                    {
                                        Console.WriteLine($"    POS charid {entity.UniqueNo} ({entity.Name}): x={entity.X:F2} y={entity.Vertical:F2} z={entity.Depth:F2} dir={entity.Direction} modelSize={entity.ModelSize}");
                                    }

                                    // Dump the whole spawn packet once per
                                    // character. Comparing a character the
                                    // retail client renders against one it
                                    // doesn't is the only way to see which
                                    // field it actually cares about.
                                    if (entity.PacketId == FfxiEntityUpdate.PlayerPacketId && count == 1)
                                    {
                                        Console.WriteLine($"    FULL 0x00D charid {entity.UniqueNo} (self={isSelf}, {size} bytes):");
                                        Console.WriteLine($"      {Convert.ToHexString(reply.Plaintext.AsSpan(offset, size))}");
                                    }
                                }

                                FfxiChatMessage? chat = FfxiChatMessage.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (chat is not null)
                                {
                                    Console.WriteLine($"    CHAT [{chat.Kind}] <{chat.Sender}> {chat.Text}");
                                }

                                if (trace)
                                {
                                    Console.WriteLine($"    <- 0x{id:X3} ({size} bytes) {Convert.ToHexString(reply.Plaintext.AsSpan(offset, Math.Min(size, 48)))}");
                                }
                            }
                        });

                    Console.WriteLine($"Done - sent {sent} position updates. Blowfish key rotations detected: {zone.KeyRotations}.");
                    Console.WriteLine("Sub-packets received during the session (id x count):");
                    foreach ((ushort id, int count) in seen.OrderByDescending(kv => kv.Value))
                    {
                        Console.WriteLine($"    0x{id:X3}  x{count}");
                    }

                    // The point of this harness: whether we were told about
                    // any player other than ourselves.
                    var otherPlayers = entitiesSeen
                        .Where(kv => kv.Value.PacketId == FfxiEntityUpdate.PlayerPacketId && !kv.Value.IsSelf)
                        .ToList();

                    Console.WriteLine($"Entities we were told about: {entitiesSeen.Count} " +
                        $"({entitiesSeen.Count(kv => kv.Value.PacketId == FfxiEntityUpdate.PlayerPacketId)} players, " +
                        $"{entitiesSeen.Count(kv => kv.Value.PacketId == FfxiEntityUpdate.NpcPacketId)} NPCs)");

                    if (otherPlayers.Count == 0)
                    {
                        Console.WriteLine("    OTHER PLAYERS: none - every 0x00D we got described ourselves.");
                    }
                    else
                    {
                        Console.WriteLine("    OTHER PLAYERS SEEN:");
                        foreach ((uint uniqueNo, var info) in otherPlayers)
                        {
                            Console.WriteLine($"      charid {uniqueNo}  x{info.Count} updates");
                        }
                    }
                }
                else
                {
                    Console.WriteLine($"Holding for {seconds}s by resending 0x00A (no zone state parsed, so no position to report)...");

                    int sent = await zone.HoldSessionAsync(
                        zoneEndpoint,
                        uniqueNo: handoff.ContentId,
                        characterName: handoff.CharacterName,
                        accountName: profile.Username,
                        clientVersion: 99,
                        clientLanguage: 2,
                        duration: TimeSpan.FromSeconds(seconds));

                    Console.WriteLine($"Done - sent {sent} keepalives.");
                }
            }

            return reply.ChecksumValid ? 0 : 1;
        }
    }
    catch (Exception ex)
    {
        Console.WriteLine($"Login failed: {ex}");
        return 1;
    }
}
