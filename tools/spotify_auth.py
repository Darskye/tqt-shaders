#!/usr/bin/env python3
"""One-time Spotify authorisation -- run this on your own machine.

Prints a refresh token to paste into src/secrets.h. Refresh tokens do not
expire, so this is a once-ever step; the board exchanges it for short-lived
access tokens by itself.

Nothing is transmitted anywhere except to Spotify. The token is printed to
your terminal and not written to disk.

Setup, first:
  1. https://developer.spotify.com/dashboard -> Create app
  2. Add redirect URI exactly:  http://127.0.0.1:8888/callback
  3. Copy the Client ID and Client Secret

Then:  python tools/spotify_auth.py
"""
import base64, http.server, json, secrets, socketserver, sys, threading
import urllib.parse, urllib.request, webbrowser
from getpass import getpass

PORT = 8888
REDIRECT = f"http://127.0.0.1:{PORT}/callback"
SCOPES = "user-read-currently-playing user-read-playback-state"

result = {}
state = secrets.token_urlsafe(16)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if "code" in q and q.get("state", [None])[0] == state:
            result["code"] = q["code"][0]
            body = b"<h2>Authorised.</h2><p>You can close this tab.</p>"
        else:
            result["error"] = q.get("error", ["state mismatch"])[0]
            body = b"<h2>Failed.</h2><p>Check the terminal.</p>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    print(__doc__)
    client_id = input("Client ID: ").strip()
    client_secret = getpass("Client Secret (hidden): ").strip()
    if not client_id or not client_secret:
        sys.exit("both are required")

    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT,
        "scope": SCOPES,
        "state": state,
    })

    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.TCPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.handle_request, daemon=True).start()

    print(f"\nOpening your browser. If it does not open, visit:\n{auth_url}\n")
    webbrowser.open(auth_url)

    for _ in range(120):
        if result:
            break
        threading.Event().wait(1)
    srv.server_close()

    if "code" not in result:
        sys.exit(f"authorisation failed: {result.get('error', 'timed out')}")

    basic = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT,
        }).encode(),
        headers={"Authorization": f"Basic {basic}",
                 "Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        tok = json.load(r)

    if "refresh_token" not in tok:
        sys.exit(f"no refresh token in response: {tok}")

    print("\n" + "=" * 66)
    print("Paste these into src/secrets.h (which is gitignored):\n")
    print(f'#define SPOTIFY_CLIENT_ID     "{client_id}"')
    print(f'#define SPOTIFY_CLIENT_SECRET "{client_secret}"')
    print(f'#define SPOTIFY_REFRESH_TOKEN "{tok["refresh_token"]}"')
    print("=" * 66)


if __name__ == "__main__":
    main()
