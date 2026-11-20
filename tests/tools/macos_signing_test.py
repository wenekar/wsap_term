#!/usr/bin/env python3
"""Check signing identity across macOS app updates."""

import os
import plistlib
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/install"))

from macos_signing import LOCAL_IDENTITY, sign_bundle


@unittest.skipUnless(sys.platform == "darwin", "macOS signing tools are required")
class MacOSSigningTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="wsap-signing-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binaries = []
        for version in (1, 2):
            binary = Path(cls.build.name) / f"program-{version}"
            subprocess.run(
                ["/usr/bin/cc", "-x", "c", "-o", str(binary), "-"],
                input=f"int main(void) {{ return {version}; }}\n",
                text=True,
                check=True,
            )
            cls.binaries.append(binary)
        cls.library = Path(cls.build.name) / "libtest.dylib"
        subprocess.run(
            ["/usr/bin/cc", "-dynamiclib", "-x", "c", "-o", str(cls.library), "-"],
            input="int test(void) { return 1; }\n",
            text=True,
            check=True,
        )

    def setUp(self):
        env = patch.dict(os.environ, {"MACOS_SIGN_IDENTITY": ""})
        env.start()
        self.addCleanup(env.stop)
        work = tempfile.TemporaryDirectory(prefix="wsap-signing-test-")
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.keychain = self.make_keychain("local")
        self.bundle = self.root / "wsap_term.app"
        contents = self.bundle / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        (contents / "Frameworks").mkdir()
        (contents / "Resources").mkdir()
        shutil.copy2(self.library, contents / "Frameworks/libtest.dylib")
        shutil.copy2(self.library, contents / "MacOS/libfastpty.dylib")
        self.install_version(1)

    def make_keychain(self, name):
        keychain = self.root / f"{name}.keychain-db"
        subprocess.run(
            ["security", "create-keychain", "-p", secrets.token_hex(16), str(keychain)],
            check=True,
            capture_output=True,
        )
        self.addCleanup(subprocess.run, ["security", "delete-keychain", str(keychain)],
                        check=False, capture_output=True)
        return keychain

    def install_version(self, version):
        contents = self.bundle / "Contents"
        shutil.copy2(self.binaries[version - 1], contents / "MacOS/wsap_term")
        (contents / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "org.wsap.wsap-term",
            "CFBundleExecutable": "wsap_term",
            "CFBundlePackageType": "APPL",
            "CFBundleVersion": str(version),
        }))
        (contents / "Resources/version").write_text(str(version), encoding="utf-8")

    def requirement(self):
        result = subprocess.run(
            ["codesign", "-d", "-r-", str(self.bundle)],
            text=True, capture_output=True, check=True,
        )
        output = result.stdout + result.stderr
        return re.search(r"^(?:# )?designated => (.+)$", output, re.MULTILINE).group(1)

    def cdhash(self):
        result = subprocess.run(
            ["codesign", "-d", "--verbose=4", str(self.bundle)],
            text=True, capture_output=True, check=True,
        )
        return re.search(r"^CDHash=(.+)$", result.stderr, re.MULTILINE).group(1)

    def matches(self, requirement):
        return subprocess.run(
            ["codesign", "--verify", "--strict", "-R=" + requirement, str(self.bundle)],
            capture_output=True,
        ).returncode == 0

    def test_local_update_keeps_identity_and_rejects_other_signers(self):
        sign_bundle(self.bundle, local=True, keychain=self.keychain)
        requirement, cdhash = self.requirement(), self.cdhash()
        self.assertIn("certificate leaf", requirement)
        self.assertNotIn("cdhash", requirement)

        self.install_version(2)
        sign_bundle(self.bundle, local=True, keychain=self.keychain)
        self.assertEqual(requirement, self.requirement())
        self.assertNotEqual(cdhash, self.cdhash())
        self.assertTrue(self.matches(requirement))

        other = self.make_keychain("other")
        sign_bundle(self.bundle, local=True, keychain=other)
        self.assertFalse(self.matches(requirement))

    def test_explicit_identity_uses_the_same_certificate(self):
        sign_bundle(self.bundle, local=True, keychain=self.keychain)
        requirement = self.requirement()
        self.install_version(2)
        with patch.dict(os.environ, {"MACOS_SIGN_IDENTITY": LOCAL_IDENTITY}):
            sign_bundle(self.bundle, keychain=self.keychain)
        self.assertTrue(self.matches(requirement))

    def test_package_default_stays_adhoc(self):
        sign_bundle(self.bundle, keychain=self.keychain)
        self.assertIn("cdhash", self.requirement())

    def test_explicit_adhoc_does_not_create_a_local_identity(self):
        sign_bundle(self.bundle, local=True, identity="-", keychain=self.keychain)
        self.assertIn("cdhash", self.requirement())
        identities = subprocess.check_output(
            ["security", "find-identity", "-p", "codesigning", str(self.keychain)],
            text=True,
        )
        self.assertNotIn(LOCAL_IDENTITY, identities)

    def test_missing_identity_fails_without_adhoc_fallback(self):
        with self.assertRaises(subprocess.CalledProcessError):
            sign_bundle(self.bundle, identity="no-such-wsap-identity", keychain=self.keychain)

    def test_missing_private_key_does_not_replace_the_certificate(self):
        sign_bundle(self.bundle, local=True, keychain=self.keychain)
        certificate = self.root / "certificate.pem"
        certificate.write_bytes(subprocess.check_output(
            ["security", "find-certificate", "-c", LOCAL_IDENTITY, "-p", str(self.keychain)]
        ))
        incomplete = self.make_keychain("incomplete")
        subprocess.run(
            ["security", "import", str(certificate), "-k", str(incomplete)],
            check=True, capture_output=True,
        )
        with self.assertRaisesRegex(RuntimeError, "private key"):
            sign_bundle(self.bundle, local=True, keychain=incomplete)


if __name__ == "__main__":
    unittest.main()
