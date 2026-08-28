using PortJeuno.Core.Ffxi;

Dictionary<string, string> ParseFlags(string[] args) =>
    args.Skip(1)
        .Select((value, index) => (value, index))
        .Where(t => t.index % 2 == 0 && t.value.StartsWith("--"))
        .ToDictionary(t => t.value[2..], t => args.Skip(1).ElementAtOrDefault(t.index + 1) ?? "");

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

static async Task<int> LoginAsync(Dictionary<string, string> flags)
{
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

    try
    {
        var client = new FfxiLoginClient();
        (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters) = await client.LoginAsync(profile, otp);

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

        Console.WriteLine($"Characters ({characters.Count}):");
        foreach (FfxiCharacter c in characters)
        {
            Console.WriteLine($"  {c.Name,-16} world={c.WorldName,-10} race={c.Race} job={c.MainJob}/{c.MainJobLevel} sub={c.SubJob} zone={c.Zone} contentId={c.ContentId}");
        }

        return 0;
    }
    catch (Exception ex)
    {
        Console.WriteLine($"Login failed: {ex}");
        return 1;
    }
}
