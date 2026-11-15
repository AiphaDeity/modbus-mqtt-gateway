"""制作V2加密包或Boot预留区信任记录；加密包使用持久nonce账本防止重复。"""

import argparse
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import secrets
import sqlite3
import struct
import zlib

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


# 与设备信任记录保持一致，拒绝长度错误及全00/FF的设备AES密钥。
def validate_aes_key(aes_key):
    if len(aes_key) != 32 or aes_key in (bytes(32), b"\xff" * 32):
        raise ValueError("设备AES密钥须为32 B，且不能全为00或FF")


# 按Boot/App共用的128 B布局制作记录，不连接设备或写入保护区。
def make_trust_record(public_key, aes_key):
    validate_aes_key(aes_key)
    if not isinstance(public_key, ec.EllipticCurvePublicKey) or public_key.curve.name != "secp256r1":
        raise ValueError("验签公钥必须为ECDSA P-256")
    record = bytearray(128)
    record[:8] = b"GWTRUST\x00"
    struct.pack_into("<III", record, 8, 128, 1, 1)
    record[20:85] = public_key.public_bytes(serialization.Encoding.X962,
                                           serialization.PublicFormat.UncompressedPoint)
    record[85:117] = aes_key
    struct.pack_into("<I", record, 124, zlib.crc32(record[:124]))
    return bytes(record)


@contextmanager
def open_ledger(path, initialize=False):
    """正常制作使用mode=rw，丢失账本不能自动创建一个新的空账本。"""
    path = path.resolve()
    if initialize:
        database = sqlite3.connect(path)
    else:
        database = sqlite3.connect(path.as_uri() + "?mode=rw", uri=True)
    try:
        with database:
            yield database
    finally:
        database.close()


# 为首次使用的 AES 密钥登记随机前缀和计数器，拒绝覆盖已有记录。
def initialize_ledger(path, aes_key):
    validate_aes_key(aes_key)
    fingerprint = hashlib.sha256(aes_key).digest()
    with open_ledger(path, initialize=True) as database:
        database.execute("PRAGMA synchronous=FULL")
        database.execute("CREATE TABLE IF NOT EXISTS nonce_keys "
                         "(fingerprint BLOB PRIMARY KEY, prefix BLOB NOT NULL, next_counter INTEGER NOT NULL)")
        database.execute("BEGIN IMMEDIATE")
        if database.execute("SELECT 1 FROM nonce_keys WHERE fingerprint=?", (fingerprint,)).fetchone():
            raise ValueError("该密钥已有nonce记录，禁止重新初始化或归零")
        database.execute("INSERT INTO nonce_keys VALUES (?, ?, 0)",
                         (fingerprint, secrets.token_bytes(4)))
        database.commit()


# 先持久递增密钥计数器，再返回 12 B nonce，失败不回收已消耗编号。
def reserve_nonce(path, aes_key):
    validate_aes_key(aes_key)
    fingerprint = hashlib.sha256(aes_key).digest()
    with open_ledger(path) as database:
        database.execute("PRAGMA synchronous=FULL")
        database.execute("BEGIN IMMEDIATE")
        row = database.execute("SELECT prefix, next_counter FROM nonce_keys WHERE fingerprint=?",
                                (fingerprint,)).fetchone()
        if row is None:
            raise ValueError("该AES密钥尚未登记；仅首次使用可显式初始化账本")
        prefix, counter = row
        if len(prefix) != 4 or not 0 <= counter < 0x7FFFFFFFFFFFFFFF:
            raise ValueError("nonce账本损坏或计数耗尽")
        database.execute("UPDATE nonce_keys SET next_counter=? WHERE fingerprint=?", (counter + 1, fingerprint))
        database.commit()  # 先持久消耗，即使后续签名/输出失败也不回收nonce。
        return prefix + struct.pack(">Q", counter)


def make_package(firmware, signing_key, aes_key, nonce, *, firmware_version, persistence_version=2):
    """只返回加密包，不写出原始bin、私钥、AES密钥或解密明文。"""
    if persistence_version != 2 or not 1 <= firmware_version <= 0xFFFFFFFF:
        raise ValueError("持久兼容版本或固件版本非法")
    if not 8 <= len(firmware) <= 448 * 1024:
        raise ValueError("App必须包含向量，且不超过448 KiB")
    stack, entry = struct.unpack_from("<II", firmware)
    if (stack <= 0x20000040 or stack > 0x20020000 or stack & 7
            or not entry & 1 or not 0x08010008 <= (entry & ~1) < 0x08010000 + len(firmware)):
        raise ValueError("App向量或链接地址不合法；必须为0x08010000偏移App")
    if not isinstance(signing_key, ec.EllipticCurvePrivateKey) or signing_key.curve.name != "secp256r1":
        raise ValueError("签名私钥必须为ECDSA P-256")
    validate_aes_key(aes_key)
    if len(nonce) != 12:
        raise ValueError("nonce须12 B")
    manifest = bytearray(128)
    manifest[:8] = b"GWUPD001"
    struct.pack_into("<HH", manifest, 8, 2, 128)
    manifest[12:28] = b"F407-TEST".ljust(16, b"\x00")
    struct.pack_into("<IIIII", manifest, 28, firmware_version, 0x08010000, len(firmware), len(firmware), 1)
    manifest[48:60] = nonce
    manifest[60:92] = hashlib.sha256(firmware).digest()
    manifest[92:108] = secrets.token_bytes(16)
    struct.pack_into("<IHH", manifest, 108, 1, persistence_version, 1)
    manifest = bytes(manifest)
    encrypted = AESGCM(aes_key).encrypt(nonce, firmware, manifest)
    signature = signing_key.sign(manifest + encrypted, ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(signature)
    package = manifest + r.to_bytes(32, "big") + s.to_bytes(32, "big") + encrypted
    # 输出前真实验签和解密自检；检查不产生明文文件或密钥转储。
    signing_key.public_key().verify(signature, manifest + encrypted, ec.ECDSA(hashes.SHA256()))
    if AESGCM(aes_key).decrypt(nonce, encrypted, manifest) != firmware:
        raise ValueError("制作后解密自检失败")
    return package


# 校验输入和密钥路径，显式初始化账本或制作并保存加密升级包。
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aes-key", type=Path, required=True, help="受控的32 B设备AES密钥文件")
    parser.add_argument("--nonce-ledger", type=Path, help="此AES密钥持续使用并备份的SQLite账本")
    parser.add_argument("--trust-record", type=Path, help="仅生成含设备AES密钥的128 B Boot信任记录；目标必须位于Git外")
    parser.add_argument("--public-key", type=Path, help="生成信任记录使用的P-256 PEM验签公钥")
    parser.add_argument("--initialize-ledger", action="store_true", help="仅密钥首次使用时登记；已有记录禁止重置")
    parser.add_argument("--input", type=Path, help="受控的偏移App bin")
    parser.add_argument("--signing-key", type=Path, help="受控的P-256 PEM私钥")
    parser.add_argument("--signing-password-env", help="读取该环境变量中的PEM密码，不将密码放进命令行")
    parser.add_argument("--output", type=Path, help="输出加密包；目标必须不存在")
    parser.add_argument("--persistence-version", type=int, choices=(2,), default=2)
    parser.add_argument("--firmware-version", type=int)
    parser.add_argument("--iwdg-handoff", action="store_true", help="确认实际App已实现看门狗接管")
    args = parser.parse_args()
    if args.trust_record:
        if not args.public_key or any((args.initialize_ledger, args.nonce_ledger, args.input,
                args.signing_key, args.signing_password_env, args.output, args.firmware_version, args.iwdg_handoff)):
            parser.error("信任记录只接受--aes-key、--public-key和--trust-record")
        if any((parent / ".git").exists() for parent in args.trust_record.resolve().parents):
            parser.error("信任记录包含设备密钥，禁止写入任何Git工作目录")
    elif args.public_key:
        parser.error("--public-key仅用于--trust-record")
    elif args.initialize_ledger:
        if not args.nonce_ledger:
            parser.error("初始化须指定--nonce-ledger")
        if any((args.input, args.signing_key, args.signing_password_env, args.output,
                args.firmware_version, args.iwdg_handoff)):
            parser.error("初始化账本不能同时制作包")
    elif not all((args.nonce_ledger, args.input, args.signing_key, args.output, args.persistence_version,
                  args.firmware_version, args.iwdg_handoff)):
        parser.error("制作包须明确输入/签名私钥/输出/固件版本和--iwdg-handoff")
    paths = [args.aes_key, args.nonce_ledger, args.input, args.signing_key, args.output,
             args.public_key, args.trust_record]
    resolved = [path.resolve() for path in paths if path is not None]
    if len(resolved) != len(set(resolved)):
        parser.error("密钥、账本、输入和输出路径必须不同")
    aes_key = args.aes_key.read_bytes()
    validate_aes_key(aes_key)
    if args.trust_record:
        public_key = serialization.load_pem_public_key(args.public_key.read_bytes())
        record = make_trust_record(public_key, aes_key)
        with args.trust_record.open("xb") as target:
            target.write(record)
            target.flush()
            os.fsync(target.fileno())
        print("已生成128 B受控信任记录；包含设备密钥，不随固件发布。未写入设备。")
        return
    if args.initialize_ledger:
        initialize_ledger(args.nonce_ledger, aes_key)
        print("nonce账本首次登记完成；不得删除或回滚后继续使用同一AES密钥")
        return
    if args.output.exists():
        raise ValueError("输出已存在，禁止覆盖")
    firmware = args.input.read_bytes()
    password = None
    if args.signing_password_env:
        value = os.environ.get(args.signing_password_env)
        if value is None:
            raise ValueError("指定的签名密码环境变量不存在")
        password = value.encode("utf-8")
    signing_key = serialization.load_pem_private_key(args.signing_key.read_bytes(), password=password)
    nonce = reserve_nonce(args.nonce_ledger, aes_key)
    package = make_package(firmware, signing_key, aes_key, nonce,
                           persistence_version=args.persistence_version, firmware_version=args.firmware_version)
    with args.output.open("xb") as target:
        target.write(package)
        target.flush()
        os.fsync(target.fileno())
    print(f"已生成加密包：{len(package)} B；持久兼容号{args.persistence_version}，制作后自检通过")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, TypeError, sqlite3.Error) as error:
        raise SystemExit(f"FAILED: {error}") from error
