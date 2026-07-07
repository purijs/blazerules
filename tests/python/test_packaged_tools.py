import socket
import subprocess
import time
import unittest
import urllib.request

import blazerules


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class PackagedToolsTest(unittest.TestCase):
    def test_tool_versions_match_python_module(self):
        expected = blazerules.__version__
        self.assertEqual(
            subprocess.check_output(["blazerules", "--version"], text=True).strip(),
            expected,
        )
        self.assertEqual(
            subprocess.check_output(["blazerules_agent", "--version"], text=True).strip(),
            expected,
        )
        self.assertEqual(
            subprocess.check_output(["blazerules_dashboard", "--version"], text=True).strip(),
            expected,
        )

    def test_dashboard_serves_embedded_assets(self):
        port = free_port()
        proc = subprocess.Popen(
            ["blazerules_dashboard", "--host", "127.0.0.1", "--port", str(port), "--poll-ms", "5000"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            base = f"http://127.0.0.1:{port}"
            last_error = None
            for _ in range(50):
                try:
                    with urllib.request.urlopen(base + "/api/health", timeout=0.25) as response:
                        if response.status == 200:
                            break
                except Exception as exc:
                    last_error = exc
                    time.sleep(0.05)
            else:
                self.fail(f"dashboard did not start: {last_error}")

            with urllib.request.urlopen(base + "/", timeout=2.0) as response:
                html = response.read().decode("utf-8")
            self.assertIn("/assets/styles.css", html)
            self.assertIn("/assets/app.js", html)

            with urllib.request.urlopen(base + "/assets/styles.css", timeout=2.0) as response:
                css = response.read().decode("utf-8")
            self.assertEqual(response.status, 200)
            self.assertIn(".app", css)

            with urllib.request.urlopen(base + "/assets/app.js", timeout=2.0) as response:
                js = response.read().decode("utf-8")
            self.assertEqual(response.status, 200)
            self.assertIn("api/summary", js)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


if __name__ == "__main__":
    unittest.main()
