
import shutil
import subprocess
import tempfile
from pathlib import Path

from common import PackageMetadata, stage_linux_payload

def build_package(
    repo_root: Path, dist_dir: Path, meta: PackageMetadata
) -> tuple[Path, str]:
    dist_dir.mkdir(parents=True, exist_ok=True)
    rpmbuild = shutil.which("rpmbuild")
    if not rpmbuild:
        raise RuntimeError(
            "rpmbuild not found. Install it with: sudo dnf install rpm-build"
        )

    with tempfile.TemporaryDirectory(prefix="wsap-rpm-") as tmp:
        rpm_top = Path(tmp)
        for d in ["BUILD", "RPMS", "SOURCES", "SPECS", "SRPMS", "BUILDROOT"]:
            (rpm_top / d).mkdir()

        buildroot = (
            rpm_top
            / "BUILDROOT"
            / f"{meta.name}-{meta.version}-{meta.release}.{meta.arch}"
        )
        stage_linux_payload(repo_root, buildroot / "usr")

        spec_path = rpm_top / "SPECS" / "wsap_term.spec"
        spec_content = f"""Name:           {meta.name}
Version:        {meta.version}
Release:        {meta.release}%{{?dist}}
Summary:        {meta.summary}
License:        {meta.license}
URL:            {meta.url}
AutoReqProv:    yes

%description
{meta.description}

%files
/usr/bin/wsap_term
%license /usr/share/licenses/wsap_term/LICENSE
%ghost /usr/bin/libfastpty.so
/usr/bin/wsap-ssh
/usr/share/applications/wsap_term.desktop
/usr/share/icons/hicolor/scalable/apps/wsap-term.svg
/usr/share/wsap_term/
/usr/share/terminfo/

%changelog
* Sat Sep 19 2026 {meta.maintainer} - {meta.version}-{meta.release}
- Initial release
"""
        spec_path.write_text(spec_content, encoding="utf-8")

        subprocess.check_call(
            [rpmbuild, "--define", f"_topdir {rpm_top}", "-bb", str(spec_path)],
            stdout=subprocess.DEVNULL,
        )

        rpms = list((rpm_top / "RPMS").rglob("*.rpm"))
        if not rpms:
            raise RuntimeError("rpmbuild completed but no .rpm was generated.")

        built_rpm = rpms[0]
        output_path = dist_dir / built_rpm.name
        shutil.copy2(built_rpm, output_path)

    return output_path, f"sudo dnf install {output_path}"
