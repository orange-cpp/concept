import socket
import ssl
import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parent
    profile = sys.argv[1] if len(sys.argv) > 1 else "rsa"
    if profile not in {"rsa", "ecdsa"}:
        print(f"unknown HTTPS test profile: {profile}", file=sys.stderr)
        return 2
    port = 9444 if profile == "ecdsa" else 9443
    stem = "https-ecdsa" if profile == "ecdsa" else "https"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(
        root / "fixtures" / f"{stem}-cert.pem",
        root / "fixtures" / f"{stem}-key.pem",
    )

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen(1)
        print("ready", flush=True)
        connection, _ = listener.accept()
        with connection:
            with context.wrap_socket(connection, server_side=True) as tls:
                request = bytearray()
                while b"\r\n\r\n" not in request:
                    block = tls.recv(4096)
                    if not block:
                        return 2
                    request.extend(block)
                body = b"hello from Concept TLS\n"
                response = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: text/plain\r\n"
                    + f"Content-Length: {len(body)}\r\n".encode()
                    + b"Connection: close\r\n\r\n"
                    + body
                )
                tls.sendall(response)
                tls.unwrap()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"https test server: {error}", file=sys.stderr, flush=True)
        raise
