#!/usr/bin/env python3
"""Reject native-crypto loader references in an Android translator before it is installed.

DT_NEEDED alone misses .NET's statically linked OpenSSL shim: it dlopens libssl/libcrypto.
Scan the complete published bytes, including loader strings and any retained symbol names.
"""
from pathlib import Path
import sys

FORBIDDEN = (b'libssl', b'libcrypto', b'System.Security.Cryptography.Native', b'CryptoNative_')


def check(path):
    data = Path(path).read_bytes()
    found = [token.decode('ascii') for token in FORBIDDEN if token in data]
    if found:
        raise ValueError('Android translator contains native crypto references: ' + ', '.join(found))


def main(argv):
    if len(argv) != 1:
        print('usage: check_android_crypto.py <published Android totk_xlate>', file=sys.stderr)
        return 2
    try:
        check(argv[0])
    except (OSError, ValueError) as error:
        print(f'Android translator crypto check: FAIL: {error}', file=sys.stderr)
        return 1
    print('Android translator crypto check: PASS (no libssl/libcrypto/crypto-native references)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
