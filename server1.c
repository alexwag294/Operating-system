"""
server.py
---------
Multi-threaded TCP server exposing a small authenticated key-value store.

Satisfies:
  1. Sockets-based server                          -> socket.socket(AF_INET, SOCK_STREAM)
  2. Simple protocol for data exchange              -> see protocol.py
  3. Multiple concurrent client connections         -> one thread per client
  4. Basic security (authentication, validation)    -> password hashing, auth
                                                        gate, input validation,
                                                        message size caps,
                                                        connection/rate limits
  5. Proper error handling / connection management  -> try/except around all
                                                        I/O, clean shutdown,
                                                        per-client cleanup,
                                                        idle timeouts
"""

import argparse
import hashlib
import logging
import secrets
import socket
import threading
import time

from protocol import (
    ProtocolError,
    ConnectionClosed,
    send_msg,
    recv_msg,
    error,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(threadName)s: %(message)s",
)
log = logging.getLogger("server")

# ---------------------------------------------------------------------------
# "User database" - in a real system this would be a proper DB with salted
# hashes (e.g. bcrypt/argon2). Here we use salted SHA-256 to keep the demo
# dependency-free while still not storing plaintext passwords.
# ---------------------------------------------------------------------------
def _hash_password(password: str, salt: str) -> str:
    return hashlib.sha256((salt + password).encode("utf-8")).hexdigest()


USER_SALT = "static-demo-salt"  # in production: unique random salt per user
USER_DB = {
    "alice": _hash_password("wonderland", USER_SALT),
    "bob": _hash_password("builder123", USER_SALT),
}

MAX_CLIENTS = 50           # cap concurrent connections (basic DoS protection)
MAX_AUTH_ATTEMPTS = 3      # per connection
IDLE_TIMEOUT_SECONDS = 120 # drop silent/hung connections
MAX_KEY_LEN = 128
MAX_VALUE_LEN = 4096


class KVStore:
    """Thread-safe in-memory key-value store shared across client threads."""

    def __init__(self):
        self._data = {}
        self._lock = threading.Lock()

    def set(self, key, value):
        with self._lock:
            self._data[key] = value

    def get(self, key):
        with self._lock:
            return self._data.get(key)

    def delete(self, key):
        with self._lock:
            return self._data.pop(key, None) is not None

    def list_keys(self):
        with self._lock:
            return list(self._data.keys())


class ClientHandler(threading.Thread):
    """Handles one client connection on its own thread."""

    def __init__(self, conn: socket.socket, addr, store: KVStore, conn_semaphore: threading.Semaphore):
        super().__init__(daemon=True)
        self.conn = conn
        self.addr = addr
        self.store = store
        self.conn_semaphore = conn_semaphore
        self.authenticated = False
        self.username = None
        self.session_token = None

    def run(self):
        self.conn.settimeout(IDLE_TIMEOUT_SECONDS)
        log.info("Connection opened from %s", self.addr)
        try:
            self._handle_authentication()
            if self.authenticated:
                self._serve_commands()
        except ConnectionClosed:
            log.info("Client %s disconnected", self.addr)
        except ProtocolError as e:
            log.warning("Protocol error from %s: %s", self.addr, e)
            self._safe_send(error("PROTOCOL_ERROR", str(e)))
        except socket.timeout:
            log.info("Client %s timed out (idle)", self.addr)
            self._safe_send(error("TIMEOUT", "Connection idle too long"))
        except Exception as e:  # last-resort catch-all so one bad client can't crash the server
            log.exception("Unexpected error handling %s: %s", self.addr, e)
        finally:
            self._cleanup()

    # -- authentication -----------------------------------------------------
    def _handle_authentication(self):
        attempts = 0
        while attempts < MAX_AUTH_ATTEMPTS:
            msg = recv_msg(self.conn)
            if msg.get("type") != "AUTH":
                self._safe_send(error("AUTH_REQUIRED", "You must authenticate first"))
                attempts += 1
                continue

            username = msg.get("username")
            password = msg.get("password")

            # --- input validation (security requirement) ---
            if not isinstance(username, str) or not isinstance(password, str):
                self._safe_send(error("BAD_REQUEST", "username/password must be strings"))
                attempts += 1
                continue
            if not (1 <= len(username) <= 64) or not (1 <= len(password) <= 256):
                self._safe_send(error("BAD_REQUEST", "username/password length invalid"))
                attempts += 1
                continue

            expected = USER_DB.get(username)
            supplied_hash = _hash_password(password, USER_SALT)

            # constant-time compare to reduce timing side-channel leakage
            if expected and secrets.compare_digest(expected, supplied_hash):
                self.authenticated = True
                self.username = username
                self.session_token = secrets.token_hex(16)
                send_msg(self.conn, {"type": "AUTH_OK", "session": self.session_token})
                log.info("User '%s' authenticated from %s", username, self.addr)
                return
            else:
                attempts += 1
                log.warning(
                    "Failed auth attempt %d/%d for user '%s' from %s",
                    attempts, MAX_AUTH_ATTEMPTS, username, self.addr,
                )
                send_msg(self.conn, {"type": "AUTH_FAIL", "message": "Invalid credentials"})

        # too many failed attempts -> disconnect
        self._safe_send(error("AUTH_LOCKED", "Too many failed authentication attempts"))
        raise ConnectionClosed("Max auth attempts exceeded")

    # -- command loop ---------------------------------------------------
    def _serve_commands(self):
        while True:
            msg = recv_msg(self.conn)
            mtype = msg.get("type")

            if mtype == "QUIT":
                send_msg(self.conn, {"type": "OK", "message": "Goodbye"})
                break

            elif mtype == "PING":
                send_msg(self.conn, {"type": "PONG"})

            elif mtype == "SET":
                key, value = msg.get("key"), msg.get("value")
                if not self._validate_kv(key, value):
                    continue
                self.store.set(key, value)
                send_msg(self.conn, {"type": "OK", "message": f"Set '{key}'"})
                log.info("%s SET %s", self.username, key)

            elif mtype == "GET":
                key = msg.get("key")
                if not self._validate_key(key):
                    continue
                value = self.store.get(key)
                if value is None:
                    send_msg(self.conn, {"type": "NOT_FOUND", "key": key})
                else:
                    send_msg(self.conn, {"type": "VALUE", "key": key, "value": value})

            elif mtype == "DELETE":
                key = msg.get("key")
                if not self._validate_key(key):
                    continue
                existed = self.store.delete(key)
                if existed:
                    send_msg(self.conn, {"type": "OK", "message": f"Deleted '{key}'"})
                else:
                    send_msg(self.conn, {"type": "NOT_FOUND", "key": key})

            elif mtype == "LIST":
                send_msg(self.conn, {"type": "LIST_RESULT", "keys": self.store.list_keys()})

            else:
                send_msg(self.conn, error("UNKNOWN_TYPE", f"Unrecognized message type '{mtype}'"))

    # -- validation helpers (security / robustness requirement) ----------
    def _validate_key(self, key) -> bool:
        if not isinstance(key, str) or not (1 <= len(key) <= MAX_KEY_LEN):
            self._safe_send(error("BAD_REQUEST", f"key must be a string of length 1-{MAX_KEY_LEN}"))
            return False
        return True

    def _validate_kv(self, key, value) -> bool:
        if not self._validate_key(key):
            return False
        if not isinstance(value, (str, int, float, bool)) and value is not None:
            self._safe_send(error("BAD_REQUEST", "value must be a primitive JSON type"))
            return False
        if isinstance(value, str) and len(value) > MAX_VALUE_LEN:
            self._safe_send(error("BAD_REQUEST", f"value exceeds max length {MAX_VALUE_LEN}"))
            return False
        return True

    def _safe_send(self, obj):
        try:
            send_msg(self.conn, obj)
        except Exception:
            pass  # connection likely already broken; nothing more to do

    def _cleanup(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.conn_semaphore.release()
        log.info("Connection closed for %s", self.addr)


def main():
    parser = argparse.ArgumentParser(description="Authenticated multi-threaded KV server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5050)
    args = parser.parse_args()

    store = KVStore()
    conn_semaphore = threading.Semaphore(MAX_CLIENTS)

    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        server_sock.bind((args.host, args.port))
        server_sock.listen(128)
    except OSError as e:
        log.error("Failed to bind/listen on %s:%s -> %s", args.host, args.port, e)
        return

    log.info("Server listening on %s:%s", args.host, args.port)

    try:
        while True:
            try:
                conn, addr = server_sock.accept()
            except OSError as e:
                log.error("accept() failed: %s", e)
                continue

            if not conn_semaphore.acquire(blocking=False):
                log.warning("Connection limit reached, rejecting %s", addr)
                try:
                    send_msg(conn, error("SERVER_BUSY", "Too many connections, try again later"))
                except Exception:
                    pass
                conn.close()
                continue

            handler = ClientHandler(conn, addr, store, conn_semaphore)
            handler.start()
    except KeyboardInterrupt:
        log.info("Shutdown requested (Ctrl+C)")
    finally:
        server_sock.close()
        log.info("Server socket closed")


if __name__ == "__main__":
    main()
