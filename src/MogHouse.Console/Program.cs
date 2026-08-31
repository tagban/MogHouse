using System.Text.Json;
using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

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

    case "play":
        return await PlayAsync(ParseFlags(args));

    case "view":
        return await ViewAsync(ParseFlags(args));

    case "text":
        return Text(ParseFlags(args));

    default:
        PrintUsage();
        return 1;
}

static void PrintUsage()
{
    Console.WriteLine("""
        MogHouse.Console - manual live-test harness for the FFXI login/roster client.
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
                Console.WriteLine($"   Set MOGHOUSE_FFXI_RES to a directory with {FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.)");
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
                    Console.WriteLine($"    EVENT     no={zoneState.EventNo} num={zoneState.EventNum} para={zoneState.EventPara} mode={zoneState.EventMode} loginState={zoneState.LoginState}");
                }
            }

            // Complete the zone-in handshake before anything else: the server
            // only starts sending the initialization batch once it sees this.
            await zone.SendGameOkAsync(zoneEndpoint);
            Console.WriteLine("Sent GP_CLI_COMMAND_GAMEOK (0x00C) - zone-in handshake completed.");

            // The login message has to be asked for. It is not pushed with
            // everything else at zone-in, so a client that never sends this
            // never sees it - which is indistinguishable from a server with
            // nothing to say, and is why the chat panel opened empty.
            await zone.SendServerMessageRequestAsync(zoneEndpoint);
            Console.WriteLine("Requested the server message (GP_CLI_COMMAND_FRAGMENTS 0x04B).");

            // Events are answered by the hold loop as the server starts them,
            // including the one it starts at zone-in. Until an event is ended the
            // character is InEvent, and a character in an event is never spawned
            // for anyone else: present, addressable by name, reachable with !goto,
            // receiving everyone else's positions, and invisible to all of them.
            //
            // Their ids are script decisions rather than constants - the opening
            // Bastok cutscene is 0 and then 7, a later zone-in was 22 - so nothing
            // here can name them in advance.

            if (flags.TryGetValue("zone-hold", out string? holdSeconds) && int.TryParse(holdSeconds, out int seconds))
            {
                // Ctrl+C used to just kill the process, which skips the logout
                // below and leaves the server holding the session row for about
                // a minute - the character lingers, then vanishes, and the next
                // login is refused with error 201. Cancelling a token instead
                // walks out through the same exit as a hold that ran its course.
                using var stopping = new CancellationTokenSource();
                Console.CancelKeyPress += (_, e) =>
                {
                    e.Cancel = true;
                    Console.WriteLine("stopping - logging out cleanly...");
                    stopping.Cancel();
                };

                Console.WriteLine("Ctrl+C to stop early.");

                if (zoneState is not null)
                {
                    Console.WriteLine($"Sending 0x015 position updates for {seconds}s - the real client's heartbeat, so the character reads as live rather than timed out...");

                    bool trace = flags.ContainsKey("zone-trace");
                    var seen = new Dictionary<ushort, int>();
                    var entitiesSeen = new Dictionary<uint, (ushort PacketId, bool IsSelf, int Count)>();

                    // The same updates, through the tracker that will feed the
                    // radar - so what the radar would show is what gets
                    // reported here, rather than a second count that could
                    // agree by accident.
                    // Our own id comes from the zone login reply. The handoff has
                    // a serverId field that reads zero on this server, and using
                    // it meant every packet the server validates against the
                    // sender - a jump among them - was built with a zero id and
                    // silently refused before it left.
                    uint selfId = zoneState?.UniqueNo is > 0 and uint fromZone ? fromZone : handoff.ServerId;
                    var tracker = new FfxiEntityTracker { SelfUniqueNo = selfId };

                    // --view opens the renderer in this same process and lets
                    // the tracker drive its radar. The two halves have agreed
                    // on shape and coordinates for a while; this is the wire.
                    LiveRadar? liveRadar = null;
                    if (flags.ContainsKey("view"))
                    {
                        liveRadar = zoneState is null
                            ? null
                            : LiveRadar.Open(selected.Zone, zoneState.X, zoneState.Vertical, zoneState.Depth,
                                             zoneState.GameTime, selected.Name,
                                             // The roster knows our race and face; the
                                             // server never sends us the entity update
                                             // it sends for everyone else, so equipment
                                             // has to wait. Race alone is the difference
                                             // between a Tarutaru and a Hume, which is
                                             // the part you notice.
                                             FfxiAppearance.LookString(selected.Race, selected.Face));
                    }

                    // Closing the window is how a person ends the session, so it
                    // has to reach the same exit Ctrl+C does - through the
                    // cancellation, so the logout below still runs.
                    if (liveRadar is not null)
                    {
                        _ = Task.Run(async () =>
                        {
                            while (!stopping.IsCancellationRequested)
                            {
                                if (liveRadar.Closed)
                                {
                                    Console.WriteLine("renderer window closed - logging out cleanly...");
                                    await stopping.CancelAsync();
                                    return;
                                }

                                await Task.Delay(200);
                            }
                        });
                    }

                    // The world's name, which the roster gives us and which the
                    // real client shows above the login message.
                    liveRadar?.Say("", $"Welcome to {selected.WorldName}.");

                    // A jump is refused without our own targid, and the guard
                    // that skips it is silent. Say up front whether we have one.
                    Console.WriteLine($"Our targid for jumps: 0x{zoneState.ActIndex:X3} " +
                                      $"(uniqueNo {selfId}) - a zero here means jumps cannot be sent.");

                    // The first sighting of each entity, raw. Which byte says
                    // "this is a shopkeeper, not a crab" is not derivable from
                    // the struct - bitfield blocks and a misaligned uint8 sit
                    // between the fields - so it gets found by diffing packets
                    // whose answer is already known from the server data.
                    var rawFirstSeen = new Dictionary<uint, byte[]>();
                    var looksSeen = new Dictionary<FfxiLookKind, int>();
                    var equipmentLooks = new Dictionary<uint, string>();
                    var fixedModels = new HashSet<ushort>();
                    var entityTraits = new Dictionary<uint, string>();
                    var lastFull = new Dictionary<uint, byte[]>();

                    // Built up across fragments, then split into lines once the
                    // last one lands.
                    var serverMessage = new System.Text.StringBuilder();
                    var despawned = new List<(uint Id, ushort ActIndex, ushort PacketId)>();
                    var lastFlags = new Dictionary<uint, (uint Flags0, uint Flags1)>();

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

                    int sent = 0;
                    try
                    {
                    sent = await zone.HoldWithPositionAsync(
                        zoneEndpoint,
                        x: posX,
                        vertical: posY,
                        depth: posZ,
                        direction: zoneState.Direction,
                        duration: TimeSpan.FromSeconds(seconds),
                        interval: TimeSpan.FromMilliseconds(400),
                        // Default to standing still. We report a FIXED vertical -
                        // we echo back whatever height the server first gave us and
                        // never adjust it - but FFXI terrain is not flat, so any
                        // horizontal movement walks the character into the ground.
                        // Moving safely needs terrain height, which this client does
                        // not have, so movement is opt-in rather than the default.
                        walkRadius: flags.TryGetValue("zone-walk", out string? radius) && float.TryParse(radius, out float r) ? r : 0f,
                        sayEvery: flags.TryGetValue("zone-say", out string? sayText) && sayText.Length > 0 ? sayText : null,
                        sayKind: flags.TryGetValue("zone-say-kind", out string? kindText) && Enum.TryParse(kindText, true, out FfxiChatKind parsedKind) ? parsedKind : FfxiChatKind.Say,
                        followCharId: flags.TryGetValue("zone-follow", out string? followSpec) && uint.TryParse(followSpec, out uint followId) ? followId : null,
                        gmCommand: flags.TryGetValue("zone-command", out string? gmCmd) && gmCmd.Length > 0 ? gmCmd : null,
                        stopFile: flags.TryGetValue("zone-stopfile", out string? stopPath) && stopPath.Length > 0 ? stopPath : null,
                        // With --view the renderer is where the character
                        // actually is, so that is what gets reported. Without
                        // it nothing is driving movement and the heartbeat
                        // keeps repeating the spawn, which reads on the server
                        // as a character standing perfectly still.
                        positionProvider: liveRadar is null
                            ? null
                            : () => liveRadar.Position() ?? (posX, posY, posZ, zoneState.Direction),
                        tellTo: flags.TryGetValue("zone-tell", out string? tellTarget) && tellTarget.Length > 0 ? tellTarget : null,
                        tellText: flags.GetValueOrDefault("zone-tell-text", "hello from MogHouse"),
                        // The renderer knows a jump happened; only this side can
                        // tell the server, which is what makes anyone else see it.
                        jumpRequested: liveRadar is null ? null : liveRadar.TakeJump,
                        chatToSend: liveRadar is null ? null : liveRadar.TakeChat,
                        selfUniqueNo: selfId,
                        // From the zone login reply, which is where the server
                        // tells us our own targid. It never sends us the entity
                        // update it sends for everyone else, so the tracker only
                        // learns this if something else fills it in - and jumps
                        // were being dropped for want of it.
                        selfActIndex: () => zoneState.ActIndex != 0 ? zoneState.ActIndex : tracker.SelfActIndex,
                        ct: stopping.Token,
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
                                    int before = tracker.Count;
                                    // What actually separates a door from a
                                    // shopkeeper. Printed rather than guessed: the
                                    // namevis bit alone did not account for the
                                    // entities that should stay unlabelled.
                                    if (entity.NameVis is not null && !entityTraits.ContainsKey(entity.UniqueNo))
                                    {
                                        entityTraits[entity.UniqueNo] =
                                            $"namevis=0x{entity.NameVis:X2} flags0=0x{entity.RawFlags0 ?? 0:X8} " +
                                            $"flags1=0x{entity.RawFlags1 ?? 0:X8} look={entity.Look?.Kind} " +
                                            $"model={entity.Look?.ModelId}";
                                    }

                                    if (entity.Look is FfxiEntityLook seen)
                                    {
                                        looksSeen[seen.Kind] = looksSeen.GetValueOrDefault(seen.Kind) + 1;
                                        if (seen.IsEquipment)
                                        {
                                            equipmentLooks[entity.UniqueNo] = seen.ToLookString();
                                        }
                                        else if (seen.IsFixedModel && seen.ModelId != 0)
                                        {
                                            fixedModels.Add(seen.ModelId);
                                        }
                                    }

                                    tracker.Observe(entity, DateTimeOffset.UtcNow);
                                    if (entity.IsDespawn)
                                    {
                                        despawned.Add((entity.UniqueNo, entity.ActIndex, entity.PacketId));
                                        // Before and after, so the log says the
                                        // tracker acted rather than just that the
                                        // flag parsed.
                                        Console.WriteLine($"    DESPAWN {entity.PacketId:X3} id {entity.UniqueNo} " +
                                            $"targid 0x{entity.ActIndex:X3} - tracked {before} -> {tracker.Count}");
                                    }
                                    liveRadar?.Publish(tracker);
                                    if (!rawFirstSeen.ContainsKey(entity.UniqueNo))
                                    {
                                        rawFirstSeen[entity.UniqueNo] = reply.Plaintext.AsSpan(offset, size).ToArray();
                                    }

                                    // Sample another player's reported position
                                    // occasionally. If our axis mapping were
                                    // wrong, this is where it would show:
                                    // the vertical would drift into the
                                    // terrain rather than staying put.
                                    // Report flag transitions the moment they happen -
                                    // e.g. a GM toggling hide - rather than
                                    // sampling and hoping to catch the change.
                                    if (entity.RawFlags1 is uint f1 && entity.RawFlags0 is uint f0)
                                    {
                                        var now = (f0, f1);
                                        if (!lastFlags.TryGetValue(entity.UniqueNo, out var prev) || prev != now)
                                        {
                                            lastFlags[entity.UniqueNo] = now;
                                            Console.WriteLine($"    FLAGS charid {entity.UniqueNo} ({entity.Name}) flags0=0x{f0:X8} flags1=0x{f1:X8}" +
                                                $"  [hide={(f1 >> 1) & 1} monster={f1 & 1} anon={(f1 >> 12) & 1} invis={(f1 >> 29) & 1} gmlvl={(f1 >> 24) & 7} size={(f1 >> 9) & 3}]");
                                        }
                                    }

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

                                    // And then every byte that changes afterwards.
                                    //
                                    // A field nobody has identified is best found
                                    // by changing one thing about a character and
                                    // watching what moves - a GM flag toggled off
                                    // and on, say. A first sighting cannot show
                                    // that, because there is nothing to compare.
                                    //
                                    // 0x0A to 0x17 is skipped: update flags,
                                    // rotation and the three position floats move
                                    // constantly and would bury everything else.
                                    if (entity.PacketId == FfxiEntityUpdate.PlayerPacketId && !isSelf)
                                    {
                                        byte[] now = reply.Plaintext.AsSpan(offset, size).ToArray();
                                        if (lastFull.TryGetValue(entity.UniqueNo, out byte[]? previous) &&
                                            previous.Length == now.Length)
                                        {
                                            var moved = new List<string>();
                                            for (int at = 0; at < now.Length; at++)
                                            {
                                                if (at is >= 0x0A and <= 0x17 || previous[at] == now[at])
                                                {
                                                    continue;
                                                }

                                                moved.Add($"0x{at:X2}: {previous[at]:X2}->{now[at]:X2}");
                                            }

                                            if (moved.Count > 0)
                                            {
                                                Console.WriteLine(
                                                    $"    CHANGED charid {entity.UniqueNo}: {string.Join("  ", moved)}");
                                            }
                                        }

                                        lastFull[entity.UniqueNo] = now;
                                    }
                                }

                                FfxiJobInfo? jobInfo = FfxiJobInfo.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (jobInfo is not null)
                                {
                                    Console.WriteLine($"    JOB main={jobInfo.MainJob} lvl={jobInfo.MainJobLevel} sub={jobInfo.SubJob} hp={jobInfo.MaxHp} mp={jobInfo.MaxMp} stats=[{string.Join(",", jobInfo.BaseStats)}]");
                                }

                                // The login message, a fragment at a time. Each
                                // one says where it sits in the whole, and the
                                // next has to be asked for - the server answers
                                // exactly what it was asked for and nothing more.
                                FfxiServerMessageFragment? fragment =
                                    FfxiServerMessageFragment.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (fragment is not null)
                                {
                                    serverMessage.Append(fragment.Text);
                                    if (fragment.NextOffset is int next)
                                    {
                                        // Sent inline rather than awaited: onReply is a
                                        // synchronous callback on the hold loop's own
                                        // thread, and the packet counter it shares is not
                                        // safe to advance from another one.
                                        zone.SendServerMessageRequestAsync(zoneEndpoint, next)
                                            .GetAwaiter().GetResult();
                                    }
                                    else
                                    {
                                        // Server messages are written with line
                                        // breaks in them, and the panel is a list
                                        // of lines rather than a block of text.
                                        foreach (string messageLine in serverMessage.ToString()
                                                     .Replace("\r\n", "\n").Split('\n'))
                                        {
                                            if (messageLine.Trim().Length > 0)
                                            {
                                                Console.WriteLine($"    SERVER MESSAGE: {messageLine}");
                                                liveRadar?.Say("Server", messageLine.Trim());
                                            }
                                        }

                                        serverMessage.Clear();
                                    }
                                }

                                // What the server wants playing. One track per
                                // slot: zoning, nightfall, combat and mounting
                                // all arrive this way rather than being the
                                // client's decision.
                                FfxiMusicChange? music =
                                    FfxiMusicChange.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (music is not null)
                                {
                                    Console.WriteLine($"    MUSIC {music.Slot} -> track {music.Track} ({music.FileName})");
                                }

                                // The server moving us: a GM command, a zone
                                // line, a failed check putting us back. Without
                                // this the renderer keeps reporting where it
                                // thinks it is and undoes the move next frame.
                                FfxiServerPosition? placed =
                                    FfxiServerPosition.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (placed is not null && placed.UniqueNo == selfId)
                                {
                                    Console.WriteLine(
                                        $"    PLACED by server at {placed.X:F1} {placed.Vertical:F1} {placed.Depth:F1} " +
                                        $"dir={placed.Direction} mode={placed.Mode}");
                                    liveRadar?.PlaceCharacter(placed.X, placed.Vertical, placed.Depth, placed.Direction);
                                }

                                FfxiChatMessage? chat = FfxiChatMessage.TryParse(reply.Plaintext.AsSpan(offset, size));
                                if (chat is not null)
                                {
                                    Console.WriteLine($"    CHAT [{chat.Kind}] <{chat.Sender}> {chat.Text}");
                                    // And onto the panel in the window, which
                                    // is the point: seeing that something
                                    // arrived without watching a console.
                                    liveRadar?.Say(chat.Sender, chat.Text);
                                }

                                if (trace)
                                {
                                    Console.WriteLine($"    <- 0x{id:X3} ({size} bytes) {Convert.ToHexString(reply.Plaintext.AsSpan(offset, Math.Min(size, 48)))}");
                                }
                            }
                        });

                    }
                    finally
                    {
                        // In a finally because the ways out of that loop are not
                        // all the tidy one: Ctrl+C, the renderer window closing,
                        // or a throw mid-session all used to skip the logout and
                        // leave the session to be reaped on a timeout.
                        Console.WriteLine($"Done - sent {sent} position updates. Blowfish key rotations detected: {zone.KeyRotations}.");

                        // Leave cleanly so the session row is released now rather
                        // than on the server's timeout, which would block the next
                        // login for this character for about a minute.
                        await zone.SendLogoutAsync(zoneEndpoint);
                        Console.WriteLine("Sent GP_CLI_COMMAND_REQLOGOUT (0x0E7) - clean logout requested.");

                        // The server answers by applying Leavegame for five
                        // seconds and logging the character out when it expires,
                        // so leaving after two meant walking away before it had
                        // acted.
                        await Task.Delay(TimeSpan.FromSeconds(6));
                    }
                    Console.WriteLine("Sub-packets received during the session (id x count):");
                    foreach ((ushort id, int count) in seen.OrderByDescending(kv => kv.Value))
                    {
                        Console.WriteLine($"    0x{id:X3}  x{count}");
                    }

                    // The point of this harness: whether we were told about
                    // any player other than ourselves.
                    if (flags.ContainsKey("dump-entities"))
                    {
                        Console.WriteLine();
                        Console.WriteLine("RAW ENTITY PACKETS (id, size, hex):");
                        foreach ((uint id, byte[] raw) in rawFirstSeen.OrderBy(kv => kv.Key))
                        {
                            Console.WriteLine($"  {id} {raw.Length} {Convert.ToHexString(raw)}");
                        }
                        Console.WriteLine();
                    }

                    liveRadar?.Dispose();

                    // What the zone is actually made of, by how the server
                    // describes each thing's appearance. Equipment looks can be
                    // built from the files a player character already uses;
                    // fixed model ids need a mapping that does not exist yet;
                    // doors and transport are not character models at all.
                    if (looksSeen.Count > 0)
                    {
                        Console.WriteLine();
                        Console.WriteLine("Entity looks seen:");
                        foreach ((FfxiLookKind kind, int count) in looksSeen.OrderByDescending(kv => kv.Value))
                        {
                            Console.WriteLine($"  {kind,-9} x{count}");
                        }

                        if (equipmentLooks.Count > 0)
                        {
                            Console.WriteLine("Equipment looks (race,face,head,body,hands,legs,feet):");
                            foreach ((uint id, string spec) in equipmentLooks.Take(10))
                            {
                                Console.WriteLine($"  {id,-10} {spec}");
                            }
                        }

                        if (fixedModels.Count > 0)
                        {
                            Console.WriteLine($"Fixed model ids: {string.Join(", ", fixedModels.Order().Take(20))}");
                        }
                    }

                    if (entityTraits.Count > 0)
                    {
                        Console.WriteLine();
                        Console.WriteLine("Entity traits (first sighting):");
                        foreach ((uint id, string traits) in entityTraits.OrderBy(kv => kv.Key).Take(30))
                        {
                            Console.WriteLine($"  {id,-10} {traits}");
                        }
                    }

                    Console.WriteLine($"Despawns seen: {despawned.Count}");

                    IReadOnlyList<FfxiTrackedEntity> visible = tracker.Visible(DateTimeOffset.UtcNow);
                    Console.WriteLine();
                    Console.WriteLine($"Radar would show {visible.Count} entities:");
                    foreach (FfxiEntityKind kind in new[] { FfxiEntityKind.Player, FfxiEntityKind.Npc, FfxiEntityKind.Enemy })
                    {
                        var ofKind = visible.Where(e => e.Kind == kind).OrderBy(e => e.ActIndex).ToList();
                        Console.WriteLine($"  {kind,-6} {ofKind.Count}");
                        foreach (FfxiTrackedEntity e in ofKind.Take(12))
                        {
                            Console.WriteLine($"      targid 0x{e.ActIndex:X3}  id {e.UniqueNo,-10} " +
                                $"hp {(e.HealthPercent?.ToString() ?? "-"),-4} " +
                                $"({e.X,8:F1},{e.Depth,8:F1})  {e.Name}");
                        }
                        if (ofKind.Count > 12)
                        {
                            Console.WriteLine($"      ... and {ofKind.Count - 12} more");
                        }
                    }
                    Console.WriteLine();

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

/// <summary>
/// Opens the renderer inside this process and feeds its radar while it runs.
///
/// This is the whole point of the interop: one application, not a client
/// talking to a viewer. Without --live it walks a ring of synthetic entities
/// around the character, which is enough to prove the cross-thread feed
/// reaches the radar - the dots have to move.
/// </summary>
/// <summary>
/// Opens the renderer on a live session, rather than on the older hold loop.
///
/// The difference that matters is zoning. FfxiGameSession knows the zone lines,
/// notices when the character walks onto one, asks the server to move them, and
/// follows the handover to the next zone server. The hold loop `login --view`
/// uses does none of that: it posts position to whichever zone server it first
/// met until something stops it.
///
/// Movement still belongs to the renderer, which walks against the zone's own
/// collision mesh - the session's navmesh is a coarser thing. The renderer says
/// where it ended up and PlaceAt takes it from there.
/// </summary>
static async Task<int> PlayAsync(Dictionary<string, string> flags)
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
            Console.WriteLine($"No saved profile matches '{idOrName}'.");
            return 1;
        }
    }
    else if (flags.TryGetValue("host", out string? host) &&
             flags.TryGetValue("username", out string? username) &&
             flags.TryGetValue("password", out string? password))
    {
        profile = new FfxiServerProfile { Host = host, Username = username, Password = password };
    }
    else
    {
        Console.WriteLine("play needs --profile, or --host/--username/--password, or --credentials-file.");
        return 1;
    }

    if (!flags.TryGetValue("character", out string? wanted))
    {
        Console.WriteLine("play needs --character <name>.");
        return 1;
    }

    FfxiHuffmanTables? tables = FfxiHuffmanTables.TryLoadDefault();
    if (tables is null)
    {
        Console.WriteLine($"Compression tables not found - set MOGHOUSE_FFXI_RES to a directory with " +
                          $"{FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.");
        return 1;
    }

    using var session = new FfxiGameSession(new FfxiHuffman(tables),
                                            Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_NAVMESHES"),
                                            Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_ZONEDATA"),
                                            OpenFileTable());
    session.Status += message => Console.WriteLine($"  {message}");

    (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters) = await session.LoginAsync(profile);
    if (login.Result != FfxiLoginResult.Success || login.SessionHash is null)
    {
        Console.WriteLine($"Login failed: {login.Result}");
        return 1;
    }

    FfxiCharacter? selected = characters.FirstOrDefault(
        c => c.Name.Equals(wanted, StringComparison.OrdinalIgnoreCase));
    if (selected is null)
    {
        Console.WriteLine($"No character named '{wanted}' in the roster.");
        return 1;
    }

    // A session row outlives the character that left it.
    //
    // Logging out cleanly takes the character out of the zone straight
    // away, but the row the login server checks is only swept by
    // cleanupSessions, which ran thirty seconds later in testing. Anyone
    // who quits and comes straight back hits 201 through no fault of their
    // own, so wait it out rather than failing in their face.
    for (int attempt = 0; ; attempt++)
    {
        try
        {
            await session.ConnectToZoneAsync(selected, login.SessionHash, profile.Host);
            break;
        }
        catch (FfxiLoginErrorException e) when (e.Code == 201 && attempt < 6)
        {
            Console.WriteLine($"  {selected.Name} is still logged in on the server; waiting for it to clear...");
            await Task.Delay(TimeSpan.FromSeconds(10));
        }
    }

    if (session.ZoneState is null)
    {
        Console.WriteLine("The zone server did not answer.");
        return 1;
    }

    await session.StartHeartbeatAsync();

    // The tracker is ours rather than the session's: it holds everything worked
    // out about what an entity looks like and whether it should be drawn, and
    // the session only reports the updates.
    bool countedNames = false;

    var tracker = new FfxiEntityTracker { SelfUniqueNo = session.ZoneState.UniqueNo };
    session.EntitiesChanged += updates =>
    {
        DateTimeOffset now = DateTimeOffset.UtcNow;
        foreach (FfxiEntityUpdate update in updates)
        {
            tracker.Observe(update, now);
        }

        // Why a nameplate is missing: no name at all, or a name we were
        // told to hide. Printed once, when the zone has finished arriving.
        if (!countedNames && tracker.Visible(now).Count >= 8)
        {
            countedNames = true;
            var seen = tracker.Visible(now);
            int named = seen.Count(e => !string.IsNullOrEmpty(e.Name));
            int hidden = seen.Count(e => e.NameHidden);
            Console.WriteLine($"  entities {seen.Count}: {named} named by the server, " +
                              $"{hidden} name-hidden, {seen.Count - named} needing the zone table");
        }
    };

    string look = FfxiAppearance.LookString(selected.Race, selected.Face);
    LiveRadar? radar = LiveRadar.Open((int)session.ZoneState.ZoneNo, session.PosX, session.PosVertical, session.PosDepth,
                                      session.ZoneState.GameTime, selected.Name, look);
    if (radar is null)
    {
        // We are logged in at this point, so walking away here is what
        // leaves a session for the server to reap on a timeout - the next
        // attempt then fails on 201 for a minute, blaming the login rather
        // than the renderer that actually failed.
        Console.WriteLine("Could not open the renderer.");
        await session.LogoutAsync();
        return 1;
    }

    session.ChatReceived += line => radar?.Say(line.Sender, line.Text);

    // Zoning. The renderer holds one zone's geometry, collision and name table,
    // and none of it survives a move - so it is opened again on the other side.
    // The real client shows a loading screen here for the same reason.
    uint currentZone = session.ZoneState.ZoneNo;
    session.ZoneChanged += zone =>
    {
        if (zone == currentZone)
        {
            return;
        }

        Console.WriteLine($"Zoned to {zone} - reopening the window.");
        currentZone = zone;

        LiveRadar? old = radar;
        radar = null;
        old?.Dispose();

        // Nothing from the old zone belongs in the new one.
        tracker.Clear();
        radar = LiveRadar.Open((int)zone, session.PosX, session.PosVertical, session.PosDepth,
                               session.ZoneState?.GameTime ?? 0, selected.Name, look);
        if (radar is not null)
        {
            radar.Say("", $"Now in zone {zone}.");
        }
    };

    Console.WriteLine($"Playing as {selected.Name} in zone {currentZone}. Close the window to stop.");

    // /logout and /shutdown both end this loop; the server needs a few seconds
    // to act on either, and closing the socket first is what leaves a session
    // to be reaped on a timeout.
    bool leaving = false;

    // A file another process can create to ask us to log out. Closing the
    // window works when there is someone at it; this is how a script stops
    // the client without killing it and stranding the session.
    string? stopFile = flags.TryGetValue("zone-stopfile", out string? stopPath) && stopPath.Length > 0
        ? stopPath
        : null;

    // The character stays logged in on the server until we say otherwise,
    // and a session left behind locks the next login out for a minute. An
    // exception in the loop strands one exactly as surely as a clean exit
    // does, so the goodbye goes in a finally.
    try
    {
        while (radar is not null && session.IsConnected && !leaving && !(stopFile is not null && File.Exists(stopFile)))
        {
            // Where the renderer walked to, which is what the zone-line check runs
            // against.
            if (radar.Position() is (float x, float vertical, float depth, sbyte direction))
            {
                session.PlaceAt(x, vertical, depth, direction);
            }

            while (radar?.TakeChat() is { Length: > 0 } typed)
            {
                // The client's own commands never reach the server as chat.
                FfxiClientCommand command = FfxiClientCommands.Parse(typed);
                switch (command.Kind)
                {
                    case FfxiClientCommandKind.Logout:
                        radar?.Say("", "Logging out...");
                        await session.LogoutAsync(FfxiLogoutKind.Logout);
                        leaving = true;
                        break;

                    case FfxiClientCommandKind.Shutdown:
                        radar?.Say("", "Shutting down...");
                        await session.LogoutAsync(FfxiLogoutKind.Shutdown);
                        leaving = true;
                        break;

                    case FfxiClientCommandKind.Unsupported:
                        radar?.Say("", $"/{command.Name} is not something this client does yet.");
                        break;

                    default:
                        await session.SayAsync(typed);
                        break;
                }
            }

            radar?.Publish(tracker);

            if (radar is not null && radar.Closed)
            {
                break;
            }

            await Task.Delay(50);
        }
    }
    finally
    {
        Console.WriteLine("Leaving.");
        if (leaving)
        {
            // Already asked; give the server its Leavegame window rather than
            // asking twice and closing underneath it.
            await Task.Delay(TimeSpan.FromSeconds(6));   // the server's Leavegame effect runs five
        }
        else
        {
            await session.LogoutAsync();
        }
        radar?.Dispose();
    }

    return 0;
}

/// <summary>
/// The installed game's files, or null if we cannot find them.
///
/// Only NPC dialogue needs these: the server sends line ids and the client
/// looks the words up. Without them the session still runs, and says so.
/// </summary>
static FfxiFileTable? OpenFileTable()
{
    try
    {
        return new FfxiFileTable(FfxiFileTable.DefaultInstallRoot());
    }
    catch (Exception e)
    {
        Console.WriteLine($"  No FFXI install found, so NPC dialogue will show as line numbers: {e.Message}");
        return null;
    }
}

/// <summary>
/// Print lines from a zone's dialogue file.
///
/// The server sends NPC dialogue as line ids, so being able to look one up
/// is how you tell a wrong id from a wrong table.
/// </summary>
/// <summary>One line on one console line: 0x07 is a break within an entry.</summary>
static string Flat(string? line) => (line ?? "").Replace(((char)10).ToString(), " / ");
static int Text(Dictionary<string, string> flags)
{
    if (!flags.TryGetValue("zone", out string? zoneText) || !int.TryParse(zoneText, out int zone))
    {
        Console.WriteLine("text --zone <id> [--line <id>] [--count <n>] [--find <substring>]");
        return 2;
    }

    FfxiFileTable? files = OpenFileTable();
    if (files is null)
    {
        return 1;
    }

    FfxiDialogueTable lines = FfxiDialogueTable.Load(files, zone);
    Console.WriteLine($"zone {zone}: {lines.Count} lines");

    if (flags.TryGetValue("find", out string? needle) && needle.Length > 0)
    {
        int shown = 0;
        for (int i = 0; i < lines.Count && shown < 20; i++)
        {
            string? line = lines.Line(i);
            if (line is not null && line.Contains(needle, StringComparison.OrdinalIgnoreCase))
            {
                Console.WriteLine($"  [{i}] {Flat(line)}");
                shown++;
            }
        }

        return 0;
    }

    int first = flags.TryGetValue("line", out string? at) && int.TryParse(at, out int one) ? one : 0;
    int count = flags.TryGetValue("count", out string? many) && int.TryParse(many, out int n) ? n : 5;
    for (int i = first; i < first + count && i < lines.Count; i++)
    {
        Console.WriteLine($"  [{i}] {Flat(lines.Line(i))}");
    }

    return 0;
}

static async Task<int> ViewAsync(Dictionary<string, string> flags)
{
    if (!flags.TryGetValue("zone", out string? zonePath))
    {
        Console.WriteLine("view --zone <path to a zone DAT> [--look 1,0,0,1,1,1,1] [--at x,y,z] [--time hhmm]");
        return 2;
    }

    string keys = flags.GetValueOrDefault("keys")
        ?? Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_KEYTABLE") ?? "";
    string keys2 = flags.GetValueOrDefault("keys2")
        ?? Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_KEYTABLE2") ?? "";

    var options = new NativeViewerOptions
    {
        ZonePath = zonePath,
        KeyTablePath = keys,
        KeyTable2Path = keys2,
        Look = flags.GetValueOrDefault("look"),
        CharacterAt = flags.GetValueOrDefault("at"),
        CharacterFacing = flags.GetValueOrDefault("facing"),
        TimeOfDay = flags.TryGetValue("time", out string? time) && int.TryParse(time, out int hhmm) ? hhmm : null,
    };

    using var viewer = new NativeViewer(options);
    using var feeding = new CancellationTokenSource();

    // Parse the character position so the ring has something to orbit.
    float centreX = 0, centreZ = 0;
    if (options.CharacterAt is { } at)
    {
        string[] parts = at.Split(',');
        if (parts.Length == 3)
        {
            float.TryParse(parts[0], out centreX);
            float.TryParse(parts[2], out centreZ);
        }
    }

    Task feeder = Task.Run(async () =>
    {
        double phase = 0;
        while (!feeding.Token.IsCancellationRequested)
        {
            // Three dots of each kind, orbiting at different radii. Movement is
            // the assertion: a static list could be a stale first frame.
            var entities = new NativeRadarEntity[9];
            for (int i = 0; i < entities.Length; i++)
            {
                double angle = phase + i * (Math.Tau / entities.Length);
                float radius = 12f + (i % 3) * 14f;
                entities[i] = new NativeRadarEntity
                {
                    X = centreX + (float)(Math.Cos(angle) * radius),
                    Z = centreZ + (float)(Math.Sin(angle) * radius),
                    Kind = i % 3,
                };
            }
            viewer.SetEntities(entities);

            phase += 0.05;
            try
            {
                await Task.Delay(50, feeding.Token);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }, feeding.Token);

    Console.WriteLine("renderer running in-process; close the window to return");

    // Blocking, and it owns the window and the event loop - so it gets this
    // thread and the feed gets another.
    int code = viewer.Run();

    await feeding.CancelAsync();
    try
    {
        await feeder;
    }
    catch (OperationCanceledException)
    {
    }

    Console.WriteLine($"renderer closed with {code}");
    return code;
}
