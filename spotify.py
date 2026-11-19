import base64
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
from tempfile import NamedTemporaryFile
import time
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

CALLBACK_HOST = "127.0.0.1"
CALLBACK_PORT = 8765
REDIRECT_URI = f"http://{CALLBACK_HOST}:{CALLBACK_PORT}/callback"
TOKEN_URL = "https://accounts.spotify.com/api/token"
PLAYER_URL = "https://api.spotify.com/v1/me/player"
CLIENT_ID_FILE = (Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
                  / "omarchy/spotify-client-id")
TOKEN_FILE = (Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
              / "omarchy/spotify/tokens.json")
MAX_RESPONSE = 256 * 1024


class NoRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def read_json(response):
    data = response.read(MAX_RESPONSE + 1)
    if len(data) > MAX_RESPONSE:
        raise ValueError("Spotify response is too large")
    return json.loads(data) if data else {}


def request_json(url, *, form=None, token=None):
    data = urlencode(form).encode() if form is not None else None
    request = Request(url, data=data, headers={"Accept": "application/json"})
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with build_opener(NoRedirects).open(request, timeout=7) as response:
        return read_json(response)


def record(value):
    return value if isinstance(value, dict) else {}


def label(value):
    return " ".join(value.split())[:180] if isinstance(value, str) else ""


def milliseconds(value):
    return value if type(value) is int and 0 <= value <= 86_400_000 else None


def playback_sample(data):
    data = record(data)
    device = record(data.get("device"))
    track = record(data.get("item"))
    title = label(track.get("name"))
    track_id = track.get("id")
    if (device.get("is_private_session") is True or track.get("type") != "track"
            or not title or not isinstance(track_id, str)
            or not re.fullmatch(r"[a-zA-Z0-9]{22}", track_id)):
        return {"status": "idle"}

    album = record(track.get("album"))
    artists = track.get("artists")
    names = [label(record(artist).get("name"))
             for artist in artists[:8]] if isinstance(artists, list) else []
    images = album.get("images")
    art = ""
    if isinstance(images, list):
        for image in images[:8]:
            url = record(image).get("url")
            if isinstance(url, str) and re.fullmatch(r"https://i\.scdn\.co/image/[a-fA-F0-9]{1,128}", url):
                art = url
                break

    duration = milliseconds(track.get("duration_ms"))
    progress = milliseconds(data.get("progress_ms"))
    if progress is not None and duration is not None:
        progress = min(progress, duration)
    return {
        "status": "track",
        "title": title,
        "artist": ", ".join(filter(None, names))[:220],
        "album": label(album.get("name")),
        "device": label(device.get("name")),
        "art": art,
        "url": f"https://open.spotify.com/track/{track_id}",
        "playing": data.get("is_playing") is True,
        "duration": duration,
        "progress": progress,
    }


def load_client_id():
    try:
        with CLIENT_ID_FILE.open("rb") as file:
            value = file.read(129)
    except FileNotFoundError:
        return None
    client_id = value.strip()
    if len(value) <= 128 and re.fullmatch(rb"[a-fA-F0-9]{32}", client_id):
        return client_id.decode("ascii")
    return None


def load_tokens():
    try:
        with TOKEN_FILE.open("rb") as file:
            return read_json(file)
    except FileNotFoundError:
        return None


def store_tokens(response, client_id, previous_refresh=None):
    access = response.get("access_token")
    refresh = response.get("refresh_token", previous_refresh)
    lifetime = response.get("expires_in")
    if not access or not refresh or type(lifetime) is not int or lifetime <= 0:
        raise ValueError("Invalid token response")
    tokens = {
        "client_id": client_id,
        "access_token": access,
        "refresh_token": refresh,
        "expires_at": time.time() + lifetime,
    }
    TOKEN_FILE.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with NamedTemporaryFile(mode="w", dir=TOKEN_FILE.parent, delete=False) as temporary:
        temp_path = Path(temporary.name)
        try:
            json.dump(tokens, temporary)
            temporary.close()
            temp_path.replace(TOKEN_FILE)
        finally:
            temp_path.unlink(missing_ok=True)
    return tokens


def refresh_tokens(tokens, client_id):
    refresh = tokens["refresh_token"]
    try:
        return store_tokens(request_json(TOKEN_URL, form={
            "grant_type": "refresh_token",
            "client_id": client_id,
            "refresh_token": refresh,
        }), client_id, refresh)
    except HTTPError as error:
        if error.code == 400:
            with error:
                if record(read_json(error)).get("error") == "invalid_grant":
                    return None
        raise


def get_playback(client_id):
    tokens = load_tokens()
    if tokens is None or tokens.get("client_id", client_id) != client_id:
        return {"status": "disconnected"}
    # Validate older sessions against this client before reusing them.
    if "client_id" not in tokens or tokens["expires_at"] <= time.time() + 30:
        tokens = refresh_tokens(tokens, client_id)
        if tokens is None:
            return {"status": "disconnected"}
    try:
        sample = request_json(PLAYER_URL, token=tokens["access_token"])
    except HTTPError as error:
        if error.code != 401:
            raise
        error.close()
        tokens = refresh_tokens(tokens, client_id)
        if tokens is None:
            return {"status": "disconnected"}
        sample = request_json(PLAYER_URL, token=tokens["access_token"])
    return playback_sample(sample)


def callback_result(target, state):
    url = urlsplit(target)
    if url.path != "/callback" or url.scheme or url.netloc or url.fragment:
        return None
    query = parse_qs(url.query, keep_blank_values=True, max_num_fields=12)
    states = query.get("state", [])
    if len(states) != 1 or not secrets.compare_digest(states[0].encode(), state.encode()):
        return None
    errors = query.get("error", [])
    codes = query.get("code", [])
    if len(errors) == 1 and errors[0] and not codes:
        return "", "denied"
    if not errors and len(codes) == 1 and 0 < len(codes[0]) <= 2048:
        return codes[0], ""
    return None


def connect(client_id):
    state = secrets.token_urlsafe(32)
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    authorize_url = "https://accounts.spotify.com/authorize?" + urlencode({
        "response_type": "code",
        "client_id": client_id,
        "scope": "user-read-playback-state",
        "redirect_uri": REDIRECT_URI,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
        "state": state,
    })
    result = None

    class Callback(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def setup(self):
            self.request.settimeout(2)
            super().setup()

        def do_GET(self):
            nonlocal result
            try:
                callback = (callback_result(self.path, state)
                            if self.headers.get("Host") == f"{CALLBACK_HOST}:{CALLBACK_PORT}" else None)
            except ValueError:
                callback = None
            if callback is None:
                self.send_error(400, "Unexpected Spotify callback")
                return
            code, reason = callback
            message = b"Spotify authorization was declined. You can close this tab."
            if code:
                try:
                    store_tokens(request_json(TOKEN_URL, form={
                        "grant_type": "authorization_code",
                        "code": code,
                        "redirect_uri": REDIRECT_URI,
                        "client_id": client_id,
                        "code_verifier": verifier,
                    }), client_id)
                    message = b"Spotify connected. You can close this tab."
                    reason = "authorized"
                except (OSError, ValueError, KeyError, TypeError):
                    message = b"Spotify authorization could not be completed. Try again in Omarchy."
                    reason = "auth_error"
            result = {"status": reason}
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Content-Length", str(len(message)))
            self.end_headers()
            self.wfile.write(message)

    with HTTPServer((CALLBACK_HOST, CALLBACK_PORT), Callback) as server:
        server.timeout = 1
        browser = subprocess.Popen(["xdg-open", authorize_url], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, start_new_session=True)
        deadline = time.monotonic() + 180
        while result is None and time.monotonic() < deadline:
            if browser.poll() not in (None, 0):
                return {"status": "browser_error"}
            server.handle_request()
    return result or {"status": "auth_timeout"}


def http_failure(error):
    with error:
        if error.code == 429:
            retry = error.headers.get("Retry-After", "")
            retry = int(retry) if re.fullmatch(r"[0-9]{1,10}", retry) else 0
            try:
                quota = record(record(read_json(error)).get("error")).get("reason") == "QUOTA_EXCEEDED"
            except ValueError:
                quota = False
            return {"status": "quota" if quota else "limited",
                    "retry": max(900 if quota else 60, retry)}
        if error.code == 401:
            return {"status": "disconnected"}
        if error.code == 403:
            return {"status": "forbidden"}
        return {"status": "unavailable", "retry": 30}


def main(command):
    try:
        if command == "disconnect":
            TOKEN_FILE.unlink(missing_ok=True)
            return {"status": "disconnected"}
        if command not in ("connect", "status"):
            raise ValueError("Unknown command")
        client_id = load_client_id()
        if client_id is None:
            return {"status": "setup_required"}
        return connect(client_id) if command == "connect" else get_playback(client_id)
    except HTTPError as error:
        return http_failure(error)
    except (OSError, ValueError, KeyError, TypeError):
        if command == "status":
            return {"status": "unavailable", "retry": 30}
        return {"status": "auth_error" if command == "connect" else "disconnect_error"}


if __name__ == "__main__":
    print(json.dumps(main(sys.argv[1])))
