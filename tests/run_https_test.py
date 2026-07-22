import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) not in {4, 5}:
        print(
            "usage: run_https_test.py COMPILER SOURCE OUTPUT [rsa|ecdsa]",
            file=sys.stderr,
        )
        return 2

    profile = sys.argv[4] if len(sys.argv) == 5 else "rsa"
    compiler, source, output = sys.argv[1:4]
    compiled = subprocess.run(
        [compiler, source, "-o", output], capture_output=True, text=True
    )
    if compiled.returncode != 0:
        sys.stderr.write(compiled.stdout)
        sys.stderr.write(compiled.stderr)
        return compiled.returncode or 1

    server_path = Path(__file__).resolve().with_name("https_server.py")
    server = subprocess.Popen(
        [sys.executable, str(server_path), profile],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ready = server.stdout.readline().strip()
        if ready != "ready":
            _, error = server.communicate(timeout=5)
            print(f"TLS test server did not start: {ready}\n{error}", file=sys.stderr)
            return 1

        client = subprocess.run([output], capture_output=True, text=True, timeout=30)
        _, server_error = server.communicate(timeout=5)
        if client.returncode != 42:
            print(
                f"Concept HTTPS client exited {client.returncode}:\n"
                f"{client.stdout}{client.stderr}",
                file=sys.stderr,
            )
            return 1
        if "hello from Concept TLS" not in client.stdout:
            print(f"unexpected HTTPS response:\n{client.stdout}", file=sys.stderr)
            return 1
        if server.returncode != 0:
            print(f"TLS test server failed:\n{server_error}", file=sys.stderr)
            return 1
        return 0
    finally:
        if server.poll() is None:
            server.kill()
            server.wait()


if __name__ == "__main__":
    raise SystemExit(main())
