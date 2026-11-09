"""Sign macOS apps with a stable identity for local installs."""

import os
import re
import secrets
import shutil
import subprocess
import tempfile
from pathlib import Path

LOCAL_IDENTITY = "wsap_term local development"


def _tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"{name} not found; cannot sign the macOS app")
    return path


def _local_identity(keychain: Path, codesign: str) -> str:
    security = _tool("security")
    output = subprocess.check_output(
        [security, "find-identity", "-p", "codesigning", str(keychain)], text=True
    )
    # Include untrusted identities. A local certificate needs no trust override.
    identities = set(re.findall(
        rf'^\s*\d+\) ([0-9A-Fa-f]{{40}}) "{re.escape(LOCAL_IDENTITY)}"',
        output,
        re.MULTILINE,
    ))
    if len(identities) == 1:
        return identities.pop()
    if identities:
        raise RuntimeError(
            "More than one local signing identity exists. "
            "Set MACOS_SIGN_IDENTITY to the certificate fingerprint."
        )

    certificate = subprocess.run(
        [security, "find-certificate", "-c", LOCAL_IDENTITY, str(keychain)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if certificate.returncode == 0:
        raise RuntimeError(
            "The local signing certificate has no usable private key. "
            "Restore the key from your Keychain backup."
        )
    if certificate.returncode != 44:  # errSecItemNotFound
        raise RuntimeError(f"Cannot read the signing keychain: {certificate.stderr.strip()}")

    openssl = _tool("openssl")
    config = Path(__file__).resolve().parents[2] / "assets/macos/local-signing.cnf"
    with tempfile.TemporaryDirectory(prefix="wsap-signing-") as tmp:
        work = Path(tmp)
        key, cert, archive = (work / name for name in ("key.pem", "cert.pem", "identity.p12"))
        subprocess.run(
            [
                openssl, "req", "-new", "-x509", "-newkey", "rsa:3072",
                "-nodes", "-sha256", "-days", "3650", "-config", str(config),
                "-keyout", str(key), "-out", str(cert),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        # These algorithms work with both LibreSSL and OpenSSL 3.
        password = secrets.token_hex(32)
        subprocess.run(
            [
                openssl, "pkcs12", "-export", "-inkey", str(key), "-in", str(cert),
                "-out", str(archive), "-passout", "env:WSAP_SIGNING_PASSWORD",
                "-keypbe", "PBE-SHA1-3DES", "-certpbe", "PBE-SHA1-3DES",
                "-macalg", "sha1",
            ],
            env=dict(os.environ, WSAP_SIGNING_PASSWORD=password),
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        # Only codesign gets key access. Do not change certificate trust settings.
        subprocess.check_call(
            [security, "import", str(archive), "-k", str(keychain), "-P", password, "-T", codesign],
            stdout=subprocess.DEVNULL,
        )
        fingerprint = subprocess.check_output(
            [openssl, "x509", "-in", str(cert), "-noout", "-fingerprint", "-sha1"],
            text=True,
        ).strip().split("=", 1)[1].replace(":", "")
    print(f"Created local signing identity in {keychain}")
    print("macOS can ask for folder access once after this signing change.")
    return fingerprint


def sign_bundle(
    bundle: Path,
    *,
    local: bool = False,
    identity: str | None = None,
    keychain: Path | None = None,
) -> None:
    """Sign nested libraries first. Verify the complete app after signing."""
    codesign = _tool("codesign")
    if identity is None:
        identity = os.environ.get("MACOS_SIGN_IDENTITY", "")
    automatic = not identity and local
    if automatic:
        if keychain is None:
            if os.geteuid() == 0:
                raise RuntimeError(
                    "Run a local macOS install without sudo, or set MACOS_SIGN_IDENTITY."
                )
            keychain = Path(subprocess.check_output(
                [_tool("security"), "default-keychain", "-d", "user"], text=True
            ).strip().strip('"'))
        identity = _local_identity(keychain, codesign)
    elif not identity:
        identity = "-"

    command = [codesign, "--force", "--sign", identity]
    if keychain is not None:
        command += ["--keychain", str(keychain)]
    if automatic or identity == "-":
        command += ["--timestamp=none"]
    contents = bundle / "Contents"
    libraries = sorted((contents / "Frameworks").glob("*.dylib"))
    libraries += sorted((contents / "MacOS").glob("*.dylib"))
    for target in [*libraries, bundle]:
        subprocess.check_call([*command, str(target)], stdout=subprocess.DEVNULL)
    subprocess.check_call([codesign, "--verify", "--deep", "--strict", str(bundle)])
