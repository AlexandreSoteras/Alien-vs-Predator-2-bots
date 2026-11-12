"""Query an AvP2 / LithTech server with GameSpy-style status packets.

Usage: py -I query.py [host] [port ...]
Prints the first response received for each port, decoded as key/value pairs when possible.
"""
import socket
import sys

QUERIES = [b"\\status\\", b"\\basic\\\\info\\\\rules\\\\players\\", b"\\info\\"]


def query(host, port, timeout=1.5):
    for q in QUERIES:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        try:
            s.sendto(q, (host, port))
            chunks = []
            while True:
                try:
                    data, _ = s.recvfrom(65535)
                    chunks.append(data)
                    s.settimeout(0.3)
                except socket.timeout:
                    break
            if chunks:
                return q, b"".join(chunks)
        except OSError:
            pass
        finally:
            s.close()
    return None, None


def parse(raw):
    parts = raw.decode("latin-1").split("\\")[1:]
    return dict(zip(parts[0::2], parts[1::2]))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    ports = [int(p) for p in sys.argv[2:]] or [27888, 27889, 27890]
    for port in ports:
        q, raw = query(host, port)
        if raw is None:
            print(f"{port}: no response")
            continue
        print(f"{port}: reply to {q!r} ({len(raw)} bytes)")
        try:
            for k, v in parse(raw).items():
                print(f"  {k} = {v}")
        except Exception:
            print("  raw:", raw[:500])


if __name__ == "__main__":
    main()
