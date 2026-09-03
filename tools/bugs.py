#!/usr/bin/env python3
"""Read the bug reports testers post, and mark the ones that are done.

`/bug` in the client posts a report - the words, where the reporter was
standing, and a screenshot - to a Discord webhook. A webhook can only write,
so this is the other half: it reads the same channel back with a bot token,
which is what makes "check the bug channel" one command at the start of a
session rather than somebody pasting things.

The workflow the reactions describe, which needs no tooling beyond this:

    a report is posted        somebody has found something
    you react   OK            accepted - worth fixing
    it is fixed               `bugs.py done <id> <commit>`
    the bot reacts  TOOL      and replies with the commit

So `bugs.py fetch --accepted` is exactly the queue of things you have agreed
are worth doing, and nothing else. An unreacted report sits there costing
nothing until you look at it.

    tools/bugs.py fetch                 everything new since last time
    tools/bugs.py fetch --accepted      only what you have accepted and is not done
    tools/bugs.py fetch --all           the whole channel, ignoring the marker
    tools/bugs.py done <id> [commit]    mark one done, and say so in the channel
    tools/bugs.py where                 which files it is reading, for when it will not

Screenshots are downloaded beside the report so they can actually be looked
at; the summary prints the local path.

Nothing here needs a package. What it needs is a bot token with View Channel
and Read Message History on that channel, Add Reactions if it is to mark
anything, and the Message Content Intent switched on - without that last one
Discord hands back messages with empty text, which looks like an empty
channel rather than a missing permission.
"""

import argparse
import json
import os
import pathlib
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

API = "https://discord.com/api/v10"

# Discord's edge refuses the default "Python-urllib/3.x" with a 403 that says
# nothing about why, so every request out of here names itself.
AGENT = "MogHouse-bugs/1.0 (+https://github.com/tagban/MogHouse)"


def tls():
    """
    An SSL context that actually has certificates in it.

    A Python installed by Homebrew does not use the system keychain and ships
    no bundle of its own, so every HTTPS call out of it fails with
    CERTIFICATE_VERIFY_FAILED - which reads like a network fault and is a
    missing file. certifi if it happens to be installed, otherwise whichever
    of the usual bundles exists.
    """
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        pass

    for bundle in ("/etc/ssl/cert.pem",                       # macOS, BSD
                   "/etc/ssl/certs/ca-certificates.crt",       # Debian, Ubuntu
                   "/etc/pki/tls/certs/ca-bundle.crt"):        # Fedora, RHEL
        if os.path.exists(bundle):
            return ssl.create_default_context(cafile=bundle)

    return ssl.create_default_context()

# Reacted by a person to accept a report; reacted by the bot when it is done.
ACCEPTED = "✅"   # white heavy check mark
DONE = "\U0001f527"   # wrench


def config_dir() -> pathlib.Path:
    """Where the client keeps its settings, which is where the secrets live."""
    if (override := os.environ.get("MOGHOUSE_CONFIG_DIR")):
        return pathlib.Path(override)

    home = pathlib.Path.home()
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "MogHouse"
    if sys.platform.startswith("win"):
        return pathlib.Path(os.environ.get("APPDATA", home)) / "MogHouse"
    return pathlib.Path(os.environ.get("XDG_CONFIG_HOME", home / ".config")) / "MogHouse"


def read_secret(name: str, variable: str) -> str | None:
    """From the environment, or a file beside the settings. Never from the repo."""
    if (from_environment := os.environ.get(variable)):
        return from_environment.strip()
    path = config_dir() / name
    if path.exists():
        text = path.read_text().strip()
        return text or None
    return None


def channel_id() -> str | None:
    """
    Which channel to read.

    Derived from the webhook when possible, because that file already exists
    and a webhook will tell you its own channel without any authentication at
    all. One less thing to configure and one less thing to get wrong.
    """
    if (explicit := read_secret("bug-channel.txt", "MOGHOUSE_BUG_CHANNEL")):
        return explicit

    hook = read_secret("bug-webhook.txt", "MOGHOUSE_BUG_WEBHOOK")
    if not hook:
        return None
    try:
        ask = urllib.request.Request(hook, headers={"User-Agent": AGENT})
        with urllib.request.urlopen(ask, timeout=20, context=tls()) as answer:
            return json.load(answer).get("channel_id")
    except Exception:
        return None


def call(method: str, path: str, token: str, body=None):
    """One Discord API request. Raises with the body, which says what was wrong."""
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(f"{API}{path}", data=data, method=method)
    request.add_header("Authorization", f"Bot {token}")
    request.add_header("User-Agent", AGENT)
    if data is not None:
        request.add_header("Content-Type", "application/json")

    try:
        with urllib.request.urlopen(request, timeout=30, context=tls()) as answer:
            raw = answer.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as failed:
        detail = failed.read().decode(errors="replace")[:400]
        raise SystemExit(f"discord said {failed.code} to {method} {path}\n  {detail}")


def reacted(message, emoji: str) -> bool:
    for reaction in message.get("reactions") or []:
        if reaction.get("emoji", {}).get("name") == emoji:
            return True
    return False


def reacted_by_me(message, emoji: str) -> bool:
    for reaction in message.get("reactions") or []:
        if reaction.get("emoji", {}).get("name") == emoji:
            return bool(reaction.get("me"))
    return False


def fetch(args) -> int:
    token = read_secret("bug-bot-token.txt", "MOGHOUSE_BUG_BOT_TOKEN")
    if not token:
        print("No bot token. Put one in "
              f"{config_dir() / 'bug-bot-token.txt'} (chmod 600), or set "
              "MOGHOUSE_BUG_BOT_TOKEN.", file=sys.stderr)
        return 1

    channel = channel_id()
    if not channel:
        print("No channel. Set MOGHOUSE_BUG_CHANNEL, or leave the webhook file "
              "in place and it will be worked out from that.", file=sys.stderr)
        return 1

    cursor_file = config_dir() / "bug-cursor.txt"
    after = None if args.all else (cursor_file.read_text().strip()
                                   if cursor_file.exists() else None)

    query = {"limit": str(min(args.limit, 100))}
    if after:
        query["after"] = after
    messages = call("GET", f"/channels/{channel}/messages?{urllib.parse.urlencode(query)}", token)

    # Discord answers newest first; reading oldest first is how a person reads.
    messages = list(reversed(messages or []))

    if args.accepted:
        messages = [m for m in messages
                    if reacted(m, ACCEPTED) and not reacted_by_me(m, DONE)]

    if not messages:
        print("nothing new" if not args.accepted else "nothing accepted and unfinished")
        return 0

    out = pathlib.Path(args.out) if args.out else config_dir() / "bug-attachments"
    out.mkdir(parents=True, exist_ok=True)

    for message in messages:
        who = message.get("author", {}).get("username", "?")
        when = message.get("timestamp", "")[:19].replace("T", " ")
        marks = "".join(r.get("emoji", {}).get("name", "")
                        for r in (message.get("reactions") or []))
        print(f"\n--- {message['id']}  {when}  {who} {marks}")
        content = (message.get("content") or "").strip()
        print(content if content else
              "(no text - if every report looks like this, the Message Content "
              "Intent is off)")

        for attachment in message.get("attachments") or []:
            target = out / f"{message['id']}-{attachment['filename']}"
            if not target.exists():
                try:
                    ask = urllib.request.Request(attachment["url"],
                                                 headers={"User-Agent": AGENT})
                    with urllib.request.urlopen(ask, timeout=60,
                                                context=tls()) as picture:
                        target.write_bytes(picture.read())
                except Exception as failed:
                    print(f"    could not download {attachment['filename']}: {failed}")
                    continue
            print(f"    {target}")

    # Only move the marker on a plain fetch. --accepted and --all are ways of
    # looking rather than of catching up, and moving it would skip reports
    # nobody has read.
    if not args.all and not args.accepted:
        cursor_file.write_text(messages[-1]["id"])

    print(f"\n{len(messages)} report(s)")
    return 0


def done(args) -> int:
    token = read_secret("bug-bot-token.txt", "MOGHOUSE_BUG_BOT_TOKEN")
    channel = channel_id()
    if not token or not channel:
        print("Need a bot token and a channel - see `bugs.py where`.", file=sys.stderr)
        return 1

    emoji = urllib.parse.quote(DONE)
    call("PUT", f"/channels/{channel}/messages/{args.id}/reactions/{emoji}/@me", token)

    if args.commit:
        call("POST", f"/channels/{channel}/messages", token,
             {"content": f"Fixed in `{args.commit}`.",
              "message_reference": {"message_id": args.id}})

    print(f"marked {args.id} done" + (f", citing {args.commit}" if args.commit else ""))
    return 0


def where(_args) -> int:
    directory = config_dir()
    print(f"config directory   {directory}")
    for name, variable in (("bug-bot-token.txt", "MOGHOUSE_BUG_BOT_TOKEN"),
                           ("bug-webhook.txt", "MOGHOUSE_BUG_WEBHOOK"),
                           ("bug-channel.txt", "MOGHOUSE_BUG_CHANNEL")):
        have = "set" if read_secret(name, variable) else "MISSING"
        print(f"  {name:<22} {have}")
    print(f"  bug-cursor.txt         "
          f"{(directory / 'bug-cursor.txt').read_text().strip() if (directory / 'bug-cursor.txt').exists() else '(none yet)'}")
    print(f"channel            {channel_id() or '(could not work it out)'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    get = commands.add_parser("fetch", help="read reports")
    get.add_argument("--all", action="store_true", help="the whole channel, ignoring the marker")
    get.add_argument("--accepted", action="store_true",
                     help=f"only those reacted {ACCEPTED} and not yet {DONE}")
    get.add_argument("--limit", type=int, default=100)
    get.add_argument("--out", help="where to put screenshots")
    get.set_defaults(run=fetch)

    finished = commands.add_parser("done", help="mark one done")
    finished.add_argument("id")
    finished.add_argument("commit", nargs="?")
    finished.set_defaults(run=done)

    commands.add_parser("where", help="which files it reads").set_defaults(run=where)

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
