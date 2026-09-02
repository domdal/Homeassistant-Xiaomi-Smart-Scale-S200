"""
scale_live_weight.py - live BLE listener + decrypter for the Xiaomi Smart
Scale S200. Combines scale_logger_minimal.py (passive BLE scan) with
decode_mibeacon.py (MiBeacon header parsing + AES-CCM decrypt) into one
script that prints the actual weight in real time as you step on the scale.

pip install bleak pycryptodome

Usage:
    python scale_live_weight.py
    (Ctrl+C to stop)
"""

import asyncio
from datetime import datetime

from bleak import BleakScanner
from Crypto.Cipher import AES

from secrets import ADDRESS, BINDKEY_HEX

BINDKEY = bytes.fromhex(BINDKEY_HEX)
# Fallback MAC bytes as they appear inside the MiBeacon payload, i.e.
# reversed vs. the human-readable "aa:bb:..." ADDRESS above.
EXPECTED_MAC_BYTES = bytes(reversed(bytes.fromhex(ADDRESS.replace(":", ""))))

FE95_UUID = "0000fe95-0000-1000-8000-00805f9b34fb"

last_hex = None


def parse_frctrl(frctrl: int) -> dict:
    return {
        "version": (frctrl >> 12) & 0x0F,
        "auth_mode": (frctrl >> 10) & 0x03,
        "solicited": (frctrl >> 9) & 0x01,
        "registered": (frctrl >> 8) & 0x01,
        "mesh": (frctrl >> 7) & 0x01,
        "object_include": (frctrl >> 6) & 0x01,
        "capability_include": (frctrl >> 5) & 0x01,
        "mac_include": (frctrl >> 4) & 0x01,
        "is_encrypted": (frctrl >> 3) & 0x01,
    }


def decrypt_object(blob: bytes, mac_bytes: bytes, product_id_bytes: bytes, frame_cnt_byte: bytes):
    if len(blob) < 7:
        return None
    ext_cnt = blob[-7:-4]
    mic = blob[-4:]
    ciphertext = blob[:-7]
    nonce = mac_bytes + product_id_bytes + frame_cnt_byte + ext_cnt
    cipher = AES.new(BINDKEY, AES.MODE_CCM, nonce=nonce, mac_len=4)
    cipher.update(b"\x11")
    try:
        return cipher.decrypt_and_verify(ciphertext, mic)
    except ValueError:
        return None


def parse_tlv_objects(plaintext: bytes):
    i = 0
    objects = []
    while i + 3 <= len(plaintext):
        obj_id = int.from_bytes(plaintext[i:i + 2], "little")
        obj_len = plaintext[i + 2]
        i += 3
        obj_val = plaintext[i:i + obj_len]
        i += obj_len
        objects.append((obj_id, obj_val))
    return objects


def handle_packet(ts: str, data: bytes):
    if len(data) < 5:
        return

    frctrl = int.from_bytes(data[0:2], "little")
    product_id = data[2:4]
    frame_cnt = data[4:5]
    flags = parse_frctrl(frctrl)

    idx = 5
    mac_bytes = None
    if flags["mac_include"]:
        mac_bytes = data[idx:idx + 6]
        idx += 6
    if flags["capability_include"]:
        idx += 1

    if not flags["object_include"]:
        print(f"[{ts}] idle beacon (no measurement) - {data.hex()}")
        return

    if mac_bytes is None:
        mac_bytes = EXPECTED_MAC_BYTES

    remainder = data[idx:]
    if not flags["is_encrypted"]:
        print(f"[{ts}] unencrypted object - {remainder.hex()}")
        return

    plaintext = decrypt_object(remainder, mac_bytes, product_id, frame_cnt)
    if plaintext is None:
        print(f"[{ts}] decryption FAILED for {data.hex()}")
        return

    for obj_id, obj_val in parse_tlv_objects(plaintext):
        if obj_id == 0x4e16 and len(obj_val) >= 3:
            weight_kg = int.from_bytes(obj_val[1:3], "little") / 100.0
            print(f"[{ts}] WEIGHT = {weight_kg:.2f} kg   (raw obj={obj_val.hex()})")
        else:
            print(f"[{ts}] obj_id=0x{obj_id:04x} value={obj_val.hex()}")


def callback(_device, adv):
    # Not filtering on device.address: macOS/CoreBluetooth exposes a random
    # OS-generated UUID there instead of the real BLE MAC (Windows/WinRT does
    # give the real MAC), so that filter silently drops everything on macOS.
    # Filter on the MAC bytes embedded in the MiBeacon payload instead, which
    # are the real on-air MAC on every platform.
    global last_hex
    for uuid, data in (adv.service_data or {}).items():
        if uuid != FE95_UUID:
            continue
        if len(data) >= 11:
            flags = parse_frctrl(int.from_bytes(data[0:2], "little"))
            if flags["mac_include"] and data[5:11] != EXPECTED_MAC_BYTES:
                continue
        h = data.hex()
        if h == last_hex:
            continue  # skip pure repeats of the exact same advertisement
        last_hex = h
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        handle_packet(ts, data)


async def main():
    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    print(f"Listening for {ADDRESS} ... step on the scale. Ctrl+C to stop.")
    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
