import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

spec = importlib.util.spec_from_file_location("haiven_release", Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class VersionTests(unittest.TestCase):
    def test_tag_must_match_descriptor(self):
        with mock.patch.object(release, "descriptor_version", return_value="1.2.3"):
            self.assertEqual(release.release_version("v1.2.3"), "1.2.3")
            self.assertEqual(release.release_version(None), "1.2.3")
            for tag in ("v1.2.4", "1.2.3", "v1.2", "v1.2.3-rc.1", "vx.y.z", "v1.2.3/../x"):
                with self.subTest(tag=tag), self.assertRaises(RuntimeError):
                    release.release_version(tag)

    def test_prerelease_tag(self):
        with mock.patch.object(release, "descriptor_version", return_value="1.2.3-rc.1"):
            self.assertEqual(release.release_version("v1.2.3-rc.1"), "1.2.3-rc.1")

    def test_descriptor_must_be_semver(self):
        with mock.patch.object(release, "descriptor_version", return_value="1.0"):
            with self.assertRaises(RuntimeError):
                release.release_version(None)

    def test_checked_rejects_path_like_versions(self):
        for version in ("../1.2.3", "1.2.3/x", ""):
            with self.subTest(version=version), self.assertRaises(RuntimeError):
                release.checked(version)


class MatrixTests(unittest.TestCase):
    def test_matrix_mirrors_engines_json(self):
        engines = json.loads((release.HERE / "engines.json").read_text())["engines"]
        include = release.matrix()["include"]
        self.assertEqual([row["label"] for row in include], [engine["runnerLabel"] for engine in engines])
        self.assertEqual([row["ue"] for row in include], [engine["association"] for engine in engines])
        self.assertTrue(all(row["platform"] == "Linux" for row in include))


class SourceZipTests(unittest.TestCase):
    def test_source_zip_excludes_build_products(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "Haiven"
            for relative in ("Haiven.uplugin", "Source/Haiven/Haiven.Build.cs", "Binaries/Linux/lib.so",
                             "Intermediate/Build/x.o", "Source/Haiven/Intermediate/y.o", "Saved/z.log"):
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                (root / relative).write_text(relative)
            first = release.write_zip(root, Path(directory) / "out/a.zip")
            second = release.write_zip(root, Path(directory) / "out/b.zip")
            with zipfile.ZipFile(first) as bundle:
                self.assertEqual(bundle.namelist(), ["Haiven/Haiven.uplugin", "Haiven/Source/Haiven/Haiven.Build.cs"])
            self.assertEqual(first.read_bytes(), second.read_bytes(), "zip output must be deterministic")

    def test_repository_plugin_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = release.source("0.0.0", directory)
            self.assertEqual(archive.name, "haiven-unreal-plugin-source-0.0.0.zip")
            with zipfile.ZipFile(archive) as bundle:
                names = bundle.namelist()
            self.assertIn("Haiven/Haiven.uplugin", names)
            self.assertTrue(all(name.startswith("Haiven/") for name in names))
            self.assertFalse(any(part in release.EXCLUDED_DIRS for name in names for part in name.split("/")))

    def test_packaged_descriptor_is_pinned_to_its_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            descriptor = Path(directory) / "Haiven.uplugin"
            descriptor.write_text(json.dumps({"VersionName": "1.2.3"}))
            self.assertEqual(release.stamp_engine_version(descriptor, "5.6"), "5.6.0")
            self.assertEqual(json.loads(descriptor.read_text())["EngineVersion"], "5.6.0")
            release.stamp_engine_version(descriptor, "5.6")
            with self.assertRaises(RuntimeError):
                release.stamp_engine_version(descriptor, "5.8")

    def test_binary_refuses_unsupported_platform(self):
        with tempfile.TemporaryDirectory() as directory, self.assertRaises(RuntimeError):
            release.binary("5.8.2", "0.0.0", directory, directory, platform="Win64")


if __name__ == "__main__":
    unittest.main()
