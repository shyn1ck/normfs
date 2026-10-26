import socket, struct, sys, time, threading
ADDR = ("127.0.0.1", 8888)
PING = bytes([0x12, 0x04, 0x08, 0x01, 0x10, 0x02])

def frame(p):
    return struct.pack("<Q", len(p)) + p

def recv_frame(s, timeout=5):
    s.settimeout(timeout)
    h = b""
    while len(h) < 8:
        c = s.recv(8 - len(h))
        if not c:
            return None
        h += c
    n = struct.unpack("<Q", h)[0]
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            return None
        b += c
    return b

def ping(timeout=3):
    try:
        s = socket.create_connection(ADDR, timeout=timeout)
        s.sendall(frame(b"") + frame(PING))
        r = recv_frame(s, timeout)
        s.close()
        return bool(r and r[0] == 0x12)
    except OSError:
        return False

def closed_after(s, limit):
    s.settimeout(limit)
    t = time.time()
    try:
        return s.recv(1) == b"", time.time() - t
    except socket.timeout:
        return False, time.time() - t
    except ConnectionResetError:
        return True, time.time() - t

mode = sys.argv[1]
if mode == "header":
    s = socket.create_connection(ADDR)
    s.sendall(struct.pack("<Q", 4 * 1024**3) + b"x" * 1000)
    time.sleep(1)
    print("  ping with a 4 GiB header pending:", ping())
elif mode == "emfile":
    held = []
    for _ in range(200):
        try:
            held.append(socket.create_connection(ADDR, timeout=1))
        except OSError:
            break
    time.sleep(1)
    for s in held:
        s.close()
    time.sleep(1)
    print("  ping after the connections close:", ping())
elif mode == "e2e":
    ok = ping()
    print("  ping:", ok)
    s = socket.create_connection(ADDR)
    s.sendall(struct.pack("<Q", 5 * 1024**3 + 1))
    c, dt = closed_after(s, 3)
    print(f"  over-limit header closes: {c} {dt:.2f} s"); ok &= c
    s = socket.create_connection(ADDR)
    c, dt = closed_after(s, 15)
    print(f"  idle connection closes: {c} {dt:.1f} s"); ok &= c and 9 < dt < 12
    s = socket.create_connection(ADDR)
    s.sendall(struct.pack("<Q", 300000) + b"x" * 70000)
    t = time.time()
    s.settimeout(0.2)
    closed = False
    while time.time() - t < 20 and not closed:
        try:
            closed = s.recv(1) == b""
        except socket.timeout:
            pass
        except ConnectionResetError:
            closed = True
        try:
            s.sendall(b"x" * 1000)
        except OSError:
            closed = True
        time.sleep(0.8)
    dt = time.time() - t
    print(f"  slow payload past the first buffer closes: {closed} {dt:.1f} s"); ok &= closed and dt < 12
    stop = []
    def flood():
        f = socket.create_connection(ADDR)
        z = frame(b"") * 4096
        while not stop:
            try:
                f.sendall(z)
            except OSError:
                return
    ts = [threading.Thread(target=flood, daemon=True) for _ in range(20)]
    [t.start() for t in ts]
    time.sleep(1)
    worst = 0
    for _ in range(20):
        t0 = time.time(); ok &= ping(); worst = max(worst, time.time() - t0)
    stop.append(1)
    print(f"  ping under a keepalive flood from 20 connections, worst: {worst*1000:.1f} ms")
    print("  ALL OK" if ok else "  SOMETHING FAILED")
