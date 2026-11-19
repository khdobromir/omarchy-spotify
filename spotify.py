import base64
from contextlib import contextmanager
import fcntl
import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import shutil
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
API_URL = "https://api.spotify.com/v1"
PLAYER_URL = API_URL + "/me/player"
SCOPES = frozenset(("user-read-playback-state", "user-modify-playback-state",
                    "user-library-read", "user-library-modify", "playlist-read-private"))
PLAYER_METHODS = {"next": "POST", "previous": "POST", "pause": "PUT", "play": "PUT", "seek": "PUT"}
LIBRARY_TYPES = {"tracks": "track", "albums": "album", "playlists": "playlist"}
PAGE_SIZE = 20
MAX_OFFSET = 100_000
LIBRARY_RESPONSE_LIMIT = 2 * 1024 * 1024
CLIENT_ID_FILE = (Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
                  / "omarchy/spotify-client-id")
TOKEN_FILE = (Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state")
              / "omarchy/spotify/tokens.json")
MAX_RESPONSE = 256 * 1024


class NoRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def read_json(response, limit=MAX_RESPONSE):
    data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Spotify response is too large")
    return json.loads(data) if data else {}


def request_json(url, *, form=None, token=None, method=None, limit=MAX_RESPONSE):
    data = urlencode(form).encode() if form is not None else None
    request = Request(url, data=data, headers={"Accept": "application/json"}, method=method)
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with build_opener(NoRedirects).open(request, timeout=7) as response:
        return read_json(response, limit)


def record(value):
    return value if isinstance(value, dict) else {}


def label(value):
    return " ".join(value.split())[:180] if isinstance(value, str) else ""


def milliseconds(value):
    return value if type(value) is int and 0 <= value <= 86_400_000 else None


def item_id(value):
    return value if isinstance(value, str) and re.fullmatch(r"[a-zA-Z0-9]{22}", value) else ""


def artist_label(item):
    artists = item.get("artists")
    names = [label(record(artist).get("name"))
             for artist in artists[:8]] if isinstance(artists, list) else []
    return ", ".join(filter(None, names))[:220]


def device_id(value):
    return value if isinstance(value, str) and re.fullmatch(r"[!-~]{1,256}", value) else ""


def artwork(images):
    variants = []
    for image in images if isinstance(images, list) else []:
        image = record(image)
        url = image.get("url")
        if not isinstance(url, str) or not re.fullmatch(
                r"https://(?:i\.scdn\.co/image|image-cdn-(?:ak|fa)\.spotifycdn\.com/image|"
                r"mosaic\.scdn\.co/(?:60|300|640))/[a-fA-F0-9]{1,200}", url):
            continue
        size = image.get("width") or image.get("height")
        if size is None and url.startswith("https://mosaic.scdn.co/"):
            size = int(url.split("/")[3])
        variants.append((size if type(size) is int and size > 0 else 0, url))
    if not variants:
        return {}
    high = max(variants, key=lambda variant: variant[0])
    medium = min((variant for variant in variants if variant[0] >= 256),
                 key=lambda variant: variant[0], default=high)
    return {"art": medium[1], "artHigh": high[1]}


def playback_sample(data):
    data = record(data)
    device = record(data.get("device"))
    track = record(data.get("item"))
    title = label(track.get("name"))
    track_id = item_id(track.get("id"))
    if (device.get("is_private_session") is True or track.get("type") != "track"
            or not title or not track_id):
        return {"status": "idle"}

    album = record(track.get("album"))

    duration = milliseconds(track.get("duration_ms"))
    progress = milliseconds(data.get("progress_ms"))
    if progress is not None and duration is not None:
        progress = min(progress, duration)
    actions = record(data.get("actions"))
    disallows = record(actions.get("disallows")) or actions
    return {
        "status": "track",
        "trackId": track_id,
        "title": title,
        "artist": artist_label(track),
        "album": label(album.get("name")),
        "device": label(device.get("name")),
        "deviceId": (device_id(device.get("id"))
                     if device.get("is_restricted") is not True and device.get("is_active") is not False else ""),
        "disallows": [action for action in ("skipping_next", "skipping_prev", "pausing", "resuming", "seeking")
                      if disallows.get(action) is True],
        "art": artwork(album.get("images")).get("art", ""),
        "url": f"https://open.spotify.com/track/{track_id}",
        "playing": data.get("is_playing") is True,
        "duration": duration,
        "progress": progress,
    }


def library_item(item, kind, inherited_art=None):
    identity, title = item_id(item.get("id")), label(item.get("name"))
    if not identity or not title or item.get("is_local") is True:
        return None
    subtitle = artist_label(item)
    if kind == "playlist":
        subtitle = label(record(item.get("owner")).get("display_name")) or label(item.get("description"))
    images = record(item.get("album")).get("images") if kind == "track" else item.get("images")
    return {"title": title, "subtitle": subtitle, "uri": f"spotify:{kind}:{identity}",
            "url": f"https://open.spotify.com/{kind}/{identity}",
            "duration": milliseconds(item.get("duration_ms")),
            **(artwork(images) or inherited_art or {})}


def page_items(data):
    entries = data.get("items")
    if not isinstance(entries, list):
        raise ValueError("Invalid library response")
    return [record(entry) for entry in entries[:PAGE_SIZE]]


def page_result(data, items, offset):
    next_offset = offset + PAGE_SIZE
    return {"status": "ok", "items": [item for item in items if item],
            "next": next_offset if data.get("next") and next_offset <= MAX_OFFSET else None}


def library_page(kind, offset, token):
    singular = LIBRARY_TYPES[kind]
    data = record(request_json(f"{API_URL}/me/{kind}?limit={PAGE_SIZE}&offset={offset}",
                               token=token, limit=LIBRARY_RESPONSE_LIMIT))
    return page_result(data, (library_item(entry if singular == "playlist" else record(entry.get(singular)),
                                           singular) for entry in page_items(data)), offset)


def collection_page(kind, identity, offset, token):
    base = f"{API_URL}/{kind}s/{identity}"
    playlist = kind == "playlist"
    fields = "?fields=id,name,description,owner(display_name),images" if playlist else ""
    item = record(request_json(base + fields, token=token, limit=LIBRARY_RESPONSE_LIMIT))
    collection = library_item(item, kind)
    if not collection:
        raise ValueError("Invalid collection metadata")
    embedded = record(item.get("tracks")) if not playlist else {}
    entries = embedded.get("items")
    end = offset + PAGE_SIZE
    if isinstance(entries, list) and (end <= len(entries) or (offset <= len(entries) and not embedded.get("next"))):
        data = {"items": entries[offset:end], "next": end < len(entries) or bool(embedded.get("next"))}
    else:
        query = f"?limit={PAGE_SIZE}&offset={offset}"
        try:
            data = record(request_json(base + ("/items" if playlist else "/tracks") + query,
                                       token=token, limit=LIBRARY_RESPONSE_LIMIT))
        except HTTPError as error:
            if playlist and error.code == 403:
                error.close()
                return {"status": "collection_forbidden", "collection": collection}
            if not playlist or error.code != 404:
                raise
            error.close()
            data = record(request_json(base + "/tracks" + query, token=token, limit=LIBRARY_RESPONSE_LIMIT))
    art = artwork(item.get("images")) if not playlist else None
    tracks = (record(entry.get("item", entry.get("track"))) if playlist else entry
              for entry in page_items(data))
    items = (library_item(track, track.get("type") or "track", art)
             for track in tracks if track.get("type") in (None, "track", "episode"))
    return {**page_result(data, items, offset), "collection": collection}


def perform(command, args, token):
    if command == "status":
        return playback_sample(request_json(PLAYER_URL, token=token))
    if command == "library" and len(args) == 2 and args[0] in LIBRARY_TYPES and re.fullmatch(r"[0-9]{1,6}", args[1]):
        offset = int(args[1])
        if offset <= MAX_OFFSET:
            return library_page(args[0], offset, token)
    if command == "collection" and len(args) == 2 and re.fullmatch(r"[0-9]{1,6}", args[1]):
        match = re.fullmatch(r"spotify:(album|playlist):([a-zA-Z0-9]{22})", args[0])
        if match and int(args[1]) <= MAX_OFFSET:
            return collection_page(*match.groups(), int(args[1]), token)
    if command == "track" and len(args) == 1 and item_id(args[0]):
        track = library_item(record(request_json(f"{API_URL}/tracks/{args[0]}", token=token)), "track")
        return {"status": "ok", "track": track} if track else {"status": "unavailable"}
    if command in ("favorite", "save", "remove") and len(args) == 1 and item_id(args[0]):
        query = "?" + urlencode({"uris": "spotify:track:" + args[0]})
        url = API_URL + "/me/library"
        if command == "favorite":
            saved = request_json(url + "/contains" + query, token=token)
            if not isinstance(saved, list) or len(saved) != 1 or type(saved[0]) is not bool:
                raise ValueError("Invalid saved-track response")
            return {"status": "ok", "saved": saved[0]}
        request_json(url + query, method="PUT" if command == "save" else "DELETE", token=token)
        return {"status": "ok", "saved": command == "save"}
    if command in PLAYER_METHODS and args and device_id(args[0]):
        params = {"device_id": args[0]}
        if command == "seek":
            if len(args) != 3 or not item_id(args[1]) or not re.fullmatch(r"[0-9]{1,8}", args[2]):
                return {"status": "action_failed"}
            sample = playback_sample(request_json(PLAYER_URL, token=token))
            if (sample.get("trackId") != args[1] or sample.get("deviceId") != args[0]
                    or not sample["duration"] or "seeking" in sample["disallows"]):
                return {"status": "target_changed"}
            params["position_ms"] = max(1, min(int(args[2]), sample["duration"] - 1))
        elif len(args) != 1:
            return {"status": "action_failed"}
        request_json(f"{PLAYER_URL}/{command}?{urlencode(params)}", method=PLAYER_METHODS[command], token=token)
        return {"status": "ok"}
    return {"status": "action_failed"}


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


@contextmanager
def token_lock():
    TOKEN_FILE.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(TOKEN_FILE.with_suffix(".lock"), os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def store_tokens(response, client_id, previous_refresh=None, previous_scope=""):
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
        "scope": response.get("scope", previous_scope),
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
        }), client_id, refresh, tokens.get("scope", ""))
    except HTTPError as error:
        if error.code == 400:
            with error:
                if record(read_json(error)).get("error") == "invalid_grant":
                    return None
        raise


def has_extended_scope(tokens):
    scope = tokens.get("scope")
    return isinstance(scope, str) and SCOPES.issubset(scope.split())


def session_tokens(client_id, rejected=None):
    tokens = load_tokens()
    if tokens is None or tokens.get("client_id", client_id) != client_id:
        return None
    if rejected is None and "client_id" in tokens and tokens["expires_at"] > time.time() + 30:
        return tokens
    # The desktop player and bar widget share this session and may refresh it together.
    with token_lock():
        tokens = load_tokens()
        if tokens is None or tokens.get("client_id", client_id) != client_id:
            return None
        if ("client_id" not in tokens or tokens["expires_at"] <= time.time() + 30
                or tokens["access_token"] == rejected):
            return refresh_tokens(tokens, client_id)
        return tokens


def account_operation(client_id, command, args):
    tokens = session_tokens(client_id)
    if tokens is None:
        return {"status": "disconnected"}
    if command != "status" and not has_extended_scope(tokens):
        return {"status": "reauthorize"}
    try:
        result = perform(command, args, tokens["access_token"])
    except HTTPError as error:
        if error.code != 401:
            raise
        error.close()
        tokens = session_tokens(client_id, rejected=tokens["access_token"])
        if tokens is None:
            return {"status": "disconnected"}
        if command != "status" and not has_extended_scope(tokens):
            return {"status": "reauthorize"}
        result = perform(command, args, tokens["access_token"])
    if command == "status":
        result["extended"] = has_extended_scope(tokens)
    return result


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
        "scope": " ".join(sorted(SCOPES)),
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
                    with token_lock():
                        store_tokens(request_json(TOKEN_URL, form={
                            "grant_type": "authorization_code",
                            "code": code,
                            "redirect_uri": REDIRECT_URI,
                            "client_id": client_id,
                            "code_verifier": verifier,
                        }), client_id, previous_scope=" ".join(sorted(SCOPES)))
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


def launch_desktop(args):
    if len(args) > 1 or (args and not re.fullmatch(r"spotify:(track|album|playlist):[a-zA-Z0-9]{22}", args[0])):
        return {"status": "desktop_failed"}
    executable = shutil.which("omarchy-spotify") or shutil.which(str(Path.home() / ".local/bin/omarchy-spotify"))
    if not executable:
        return {"status": "desktop_missing"}
    try:
        player = subprocess.Popen([executable, *args], stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  start_new_session=True)
        if player.wait(timeout=0.5) != 0:
            return {"status": "desktop_failed"}
    except subprocess.TimeoutExpired:
        pass
    return {"status": "ok"}


def main(command, *args):
    try:
        if command == "desktop":
            return launch_desktop(args)
        if command == "disconnect":
            with token_lock():
                TOKEN_FILE.unlink(missing_ok=True)
            return {"status": "disconnected"}
        elif command not in ("connect", "status", "library", "collection", "track", "favorite", "save", "remove") and command not in PLAYER_METHODS:
            raise ValueError("Unknown command")
        client_id = load_client_id()
        if client_id is None:
            return {"status": "setup_required"}
        return connect(client_id) if command == "connect" else account_operation(client_id, command, args)
    except HTTPError as error:
        if command in PLAYER_METHODS and error.code in (403, 404):
            error.close()
            return {"status": "no_device" if error.code == 404 else "action_forbidden"}
        return http_failure(error)
    except (OSError, ValueError, KeyError, TypeError):
        if command == "desktop":
            return {"status": "desktop_failed"}
        if command == "status":
            return {"status": "unavailable", "retry": 30}
        if command == "connect":
            return {"status": "auth_error"}
        return {"status": "disconnect_error" if command == "disconnect" else "action_failed"}


if __name__ == "__main__":
    print(json.dumps(main(*sys.argv[1:])))
