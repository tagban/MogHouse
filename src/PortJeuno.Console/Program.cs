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

            using var zone = new FfxiZoneClient(handoff);

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
            Console.WriteLine($"  Decrypted payload ({reply.Payload.Length} bytes, still Huffman-compressed): {Convert.ToHexString(reply.Payload.AsSpan(0, Math.Min(64, reply.Payload.Length)))}...");

            if (flags.TryGetValue("zone-hold", out string? holdSeconds) && int.TryParse(holdSeconds, out int seconds))
            {
                Console.WriteLine($"Holding the session open for {seconds}s (resending 0x00A every 20s so the server's ~60s cleanup sweep doesn't reap it)...");
                Console.WriteLine("Ctrl+C to stop early.");

                int sent = await zone.HoldSessionAsync(
                    zoneEndpoint,
                    uniqueNo: handoff.ContentId,
                    characterName: handoff.CharacterName,
                    accountName: profile.Username,
                    clientVersion: 99,
                    clientLanguage: 2,
                    duration: TimeSpan.FromSeconds(seconds));

                Console.WriteLine($"Done - sent {sent} keepalives. The session will be reaped ~60s from now.");
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
