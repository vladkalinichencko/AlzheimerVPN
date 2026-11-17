#!/usr/bin/env python3
"""Verify the baked PROD_AGW_PUBLIC_KEY_B64 decrypts the live proxy list.

libagw derives the AES-256-CBC key/iv for endpoints.json from
sha256... sha512 of the exact PEM bytes embedded via PROD_AGW_PUBLIC_KEY_B64.
The upstream binary stores the PEM without a trailing newline; if the build
script ever bakes a different byte sequence the whole gateway path fails with
error 1103. This check fetches the real endpoints.json and decrypts it.
"""
import base64
import hashlib
import json
import os
import re
import sys
import urllib.request
from pathlib import Path

ENDPOINTS = [
    "https://s3.eu-north-1.amazonaws.com/amnezia/endpoints.json",
    "https://storage.googleapis.com/lambda-list/endpoints.json",
    "https://amnzstrg01.blob.core.windows.net/lambda-list/endpoints.json",
    "https://objectstorage.eu-zurich-1.oraclecloud.com/n/zrhfyaq6qxvh/b/lambda-list/o/endpoints.json",
    "https://storage.mwsapis.ru/lambda-list/endpoints.json",
    "https://46.8.209.252/lambda-list/endpoints.json",
    "https://s3.cloudruserapi.net/lambda-list/endpoints.json",
]

CACHE_PATH = Path(os.environ.get("AGW_CMAKE_CACHE", "/private/tmp/alzheimervpn-build/CMakeCache.txt"))


def prod_key_b64(cache_path: Path) -> str:
    if len(sys.argv) > 1:
        return sys.argv[1]
    if cache_path.is_file():
        match = re.search(r"PROD_AGW_PUBLIC_KEY_B64:STRING=(\S+)", cache_path.read_text())
        if match:
            return match.group(1)
    print(f"PROD_AGW_PUBLIC_KEY_B64 not found in {cache_path}; pass it as argv[1]", file=sys.stderr)
    return ""


def fetch_payload() -> bytes | None:
    for url in ENDPOINTS:
        try:
            with urllib.request.urlopen(url, timeout=10) as resp:
                return resp.read().strip()
        except Exception:
            continue
    return None


def decrypt(payload_b64: str, pem: bytes) -> bytes:
    from Crypto.Cipher import AES

    h = hashlib.sha512(pem).hexdigest()
    key = bytes.fromhex(h[:64])
    iv = bytes.fromhex(h[64:96])
    pt = AES.new(key, AES.MODE_CBC, iv).decrypt(base64.b64decode(payload_b64))
    pad = pt[-1]
    if not 0 < pad <= 16:
        raise ValueError("bad padding")
    return pt[:-pad]


def main() -> int:
    b64 = prod_key_b64(CACHE_PATH)
    if not b64:
        return 1
    pem = base64.b64decode(b64)
    if b"-----BEGIN PUBLIC KEY-----" not in pem:
        print("baked value is not a PEM block", file=sys.stderr)
        return 1

    body = fetch_payload()
    if body is None:
        print("could not reach any proxy-list endpoint; skipping (network?)")
        return 0

    try:
        plain = decrypt(body.decode(), pem)
        urls = json.loads(plain)
    except Exception as exc:
        print(f"PROD_AGW_PUBLIC_KEY_B64 does not decrypt endpoints.json: {exc}")
        return 1

    if not isinstance(urls, list) or not all(isinstance(u, str) for u in urls):
        print("decrypted payload is not a list of URLs")
        return 1
    print(f"agw key ok ({len(urls)} proxy URLs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
