import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("unreal_runner", Path(__file__).with_name("run.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class ReportTests(unittest.TestCase):
    def test_sdk_root_is_this_repository(self):
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(runner.SDK, root)
        self.assertEqual(runner.HERE, root / "ci")

    def test_expected_results(self):
        good = {"failed":0,"notRun":0,"inProcess":0,"tests":[{"fullTestPath":"Haiven.Plugin.Loads","state":"Success","entries":[],"errors":0}]}
        reports = [(good, True)]
        reports.append(({"tests":[]}, False))
        for state in ("Fail", "Skipped", "NotRun", "InProcess"):
            report = copy.deepcopy(good); report["tests"][0]["state"] = state
            reports.append((report, False))
        for counter in ("failed", "notRun", "inProcess"):
            report = copy.deepcopy(good); report[counter] = 1
            reports.append((report, False))
        report = copy.deepcopy(good); report["tests"][0]["fullTestPath"] = "Other.Test"
        reports.append((report, False))
        report = copy.deepcopy(good); report["tests"] *= 2
        reports.append((report, False))
        report = copy.deepcopy(good); report["tests"][0]["entries"] = [{"event":{"type":"Error"}}]
        reports.append((report, False))
        report = copy.deepcopy(good); report["tests"][0]["errors"] = 1
        reports.append((report, False))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "index.json"
            for report, valid in reports:
                with self.subTest(report=report):
                    path.write_text(json.dumps(report))
                    if valid: runner.verify_report(path, {"Haiven.Plugin.Loads"})
                    else:
                        with self.assertRaises(RuntimeError): runner.verify_report(path, {"Haiven.Plugin.Loads"})

if __name__ == "__main__": unittest.main()
