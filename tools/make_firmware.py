#!/usr/bin/env python3
"""Turn a Pulsedart release into firmware files for YOUR mouse. No compiler needed.

A release contains the firmware with marker bytes where the PixArt sensor firmware (SROM)
and the HyperX LED tables belong: that data cannot be published. This script takes it from
your own flash dump and writes ready-to-flash files:

    python tools/make_firmware.py --release release/v0.4.2 --dump dump/flash.bin --out out

Output (flash with the scripts in pi/, see docs/flashing.md):
    out/fw.hex               firmware behind the stock boot code   -> pi/flash_mouse.sh
    out/mcuboot.hex          MCUboot with your public key  \\
    out/app.signed.hex       firmware signed with your key  > -> pi/flash_mcuboot.sh
    out/app.signed.bin       the same image for USB updates   -> pi/dfu_update.sh

The MCUboot files need a signing key. --key names it (default fw/keys/pulsedart-ec-p256.pem);
it is created on the first run. Keep it private and keep a backup: later updates must be
signed with the same key (run this script again on every new release).
The MCUboot part needs the "cryptography" package (pip install cryptography).
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_blobs import BLOBS, placeholder, stock_blobs  # noqa: E402

STOCKBOOT_ADDR = 0x50000   # our image behind the stock boot stub
MCUBOOT_ADDR = 0x00000
SLOT0_ADDR = 0x10000
IMAGE_MAGIC = 0x96F3B83D
TLV_INFO_MAGIC = 0x6907
TLV_KEYHASH, TLV_SHA256, TLV_ECDSA_SIG = 0x01, 0x10, 0x22
# DER SubjectPublicKeyInfo header of a P-256 key; 65 bytes (04 || X || Y) follow
P256_SPKI = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")
SPKI_LEN = 91


def replace_once(buf, old, new, what):
    i = buf.find(old)
    if i < 0:
        sys.exit(f"{what}: not found (is this a release build with markers?)")
    if buf.find(old, i + 1) >= 0:
        sys.exit(f"{what}: found more than once, refusing to guess")
    buf[i:i + len(old)] = new
    return i


def put_blobs(image, blobs):
    """Replace the marker bytes of every blob with the data from the dump."""
    for name, _, n, _ in BLOBS:
        at = replace_once(image, placeholder(name, n), blobs[name], f"marker of {name}")
        print(f"  {name}: {n} bytes at image offset 0x{at:05x}")


def to_hex(data, addr):
    """Intel HEX text, 16 bytes per record, extended linear address records."""
    out = []

    def rec(typ, off, payload):
        body = bytes([len(payload), off >> 8, off & 0xFF, typ]) + payload
        out.append(":" + body.hex().upper() + f"{(-sum(body)) & 0xFF:02X}")

    upper = None
    for i in range(0, len(data), 16):
        a = addr + i
        if a >> 16 != upper:
            upper = a >> 16
            rec(4, 0, struct.pack(">H", upper))
        rec(0, a & 0xFFFF, data[i:i + 16])
    rec(1, 0, b"")
    return "\n".join(out) + "\n"


def load_key(path):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    if path.exists():
        key = serialization.load_pem_private_key(path.read_bytes(), None)
        print(f"signing key: {path}")
    else:
        key = ec.generate_private_key(ec.SECP256R1())
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                           serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
        print(f"signing key CREATED: {path}  (private: never publish it, keep a backup)")
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit(f"{path}: not an ECDSA P-256 key")
    pub = key.public_key().public_bytes(serialization.Encoding.DER,
                                        serialization.PublicFormat.SubjectPublicKeyInfo)
    return key, pub


def split_image(signed):
    """(header + image, version) of an MCUboot image; the TLV area is dropped."""
    magic, load, hdr_size, prot, img_size, _flags = struct.unpack_from("<IIHHII", signed, 0)
    if magic != IMAGE_MAGIC:
        sys.exit("app.signed.bin: not an MCUboot image")
    if prot:
        sys.exit("app.signed.bin: protected TLVs are not supported")
    if load != SLOT0_ADDR:
        sys.exit(f"app.signed.bin: load address 0x{load:x}, expected 0x{SLOT0_ADDR:x}")
    ver = struct.unpack_from("<BBHI", signed, 20)
    return bytearray(signed[:hdr_size + img_size]), ver


def sign_image(body, key, pub):
    """body = header + image. Returns body + TLVs (SHA-256, key hash, ECDSA signature)."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec, utils

    digest = hashlib.sha256(body).digest()
    sig = key.sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    key.public_key().verify(sig, digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    tlvs = b"".join(struct.pack("<HH", t, len(v)) + v for t, v in (
        (TLV_SHA256, digest), (TLV_KEYHASH, hashlib.sha256(pub).digest()), (TLV_ECDSA_SIG, sig)))
    return bytes(body) + struct.pack("<HH", TLV_INFO_MAGIC, 4 + len(tlvs)) + tlvs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--release", required=True, type=Path, help="release directory")
    ap.add_argument("--dump", required=True, type=Path, help="your full 1 MB flash dump")
    ap.add_argument("--out", required=True, type=Path, help="output directory")
    ap.add_argument("--key", type=Path,
                    default=Path(__file__).resolve().parent.parent / "fw/keys/pulsedart-ec-p256.pem",
                    help="MCUboot signing key (created if missing)")
    ap.add_argument("--no-mcuboot", action="store_true", help="only write fw.hex")
    a = ap.parse_args()

    blobs = stock_blobs(a.dump.read_bytes())
    a.out.mkdir(parents=True, exist_ok=True)

    print("stock-boot variant:")
    img = bytearray((a.release / "pulsedart-stockboot.bin").read_bytes())
    put_blobs(img, blobs)
    (a.out / "fw.hex").write_text(to_hex(bytes(img), STOCKBOOT_ADDR), newline="\n")
    print(f"  -> {a.out / 'fw.hex'}  (0x{STOCKBOOT_ADDR:05x}-0x{STOCKBOOT_ADDR + len(img):05x})")
    if a.no_mcuboot:
        return

    print("MCUboot variant:")
    key, pub = load_key(a.key)
    boot = bytearray((a.release / "pulsedart-mcuboot.bin").read_bytes())
    at = boot.find(P256_SPKI)
    if at < 0 or boot.find(P256_SPKI, at + 1) >= 0:
        sys.exit("pulsedart-mcuboot.bin: public key not found exactly once")
    boot[at:at + SPKI_LEN] = pub
    print(f"  your public key at MCUboot offset 0x{at:05x}")
    (a.out / "mcuboot.hex").write_text(to_hex(bytes(boot), MCUBOOT_ADDR), newline="\n")

    body, ver = split_image((a.release / "pulsedart-app.signed.bin").read_bytes())
    put_blobs(body, blobs)
    signed = sign_image(body, key, pub)
    (a.out / "app.signed.bin").write_bytes(signed)
    (a.out / "app.signed.hex").write_text(to_hex(signed, SLOT0_ADDR), newline="\n")
    print(f"  image version {ver[0]}.{ver[1]}.{ver[2]}, {len(signed)} bytes, signed")
    print(f"  -> {a.out / 'mcuboot.hex'}, {a.out / 'app.signed.hex'}, {a.out / 'app.signed.bin'}")


if __name__ == "__main__":
    main()
