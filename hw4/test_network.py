import re
import signal
import socket
import subprocess
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path


class Session:
    def __init__(self, work, *options):
        self.work = Path(work)
        self.children = []
        self.files = ExitStack()
        self.log = self.work / "railway.log"
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.address = ("127.0.0.1", probe.getsockname()[1])
        self.server = self.start("server", "server", "--log", str(self.log), *options)
        self.wait_text("server", "Сервер слушает")

    def start(self, program, output, *args):
        file = self.files.enter_context((self.work / f"{output}.out").open("w"))
        process = subprocess.Popen(
            [f"./railway_{program}", self.address[0], str(self.address[1]), *map(str, args)],
            stdout=file, stderr=subprocess.STDOUT,
        )
        self.children.append(process)
        return process

    def text(self, output):
        return (self.work / f"{output}.out").read_text()

    def wait_text(self, output, expected):
        deadline = time.monotonic() + 5
        while expected not in self.text(output):
            if time.monotonic() >= deadline or self.server.poll() is not None:
                raise AssertionError((expected, self.text(output), self.text("server")))
            time.sleep(0.01)

    def close(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
        self.files.close()


def check_events(text, total, capacity, gap, group, travel):
    permits = set()
    entered = {}
    exited = set()
    active = {}
    direction = None
    previous_entry = -gap
    group_size = busiest = 0
    for line in text.splitlines():
        match = re.match(r"time=(\d+) event=(\w+)\s+train=(\d+) dir=(\S+)\s+occupancy=(\d+)", line)
        if match is None:
            continue
        minute, event, train, way, occupied = match.groups()
        minute, train, occupied = int(minute), int(train), int(occupied)
        if event == "DIRECTION":
            assert not active
            direction, group_size = way, 0
        elif event == "PERMIT":
            permits.add(train)
        elif event == "REVOKE":
            permits.discard(train)
        elif event == "ENTER":
            assert train in permits and train not in entered
            assert direction == way and minute - previous_entry >= gap
            permits.remove(train)
            entered[train] = previous_entry = minute
            active[train] = way
            group_size += 1
            assert group_size <= group
        elif event == "EXIT":
            assert train not in exited and minute - entered[train] >= travel
            active.pop(train)
            exited.add(train)
        assert len(active) == occupied <= capacity
        assert len(set(active.values())) <= 1
        busiest = max(busiest, occupied)
    assert len(entered) == len(exited) == total
    return busiest


def simple(s):
    observer = s.start("observer", "observer")
    s.wait_text("observer", "SNAPSHOT")
    trains = [s.start("train", f"train{i}", i) for i in range(1, 5)]
    assert s.server.wait(timeout=10) == 0
    assert all(client.wait(timeout=5) == 0 for client in [observer, *trains])
    assert check_events(s.log.read_text(), 4, 1, 2, 1, 5) == 1
    assert "SUMMARY reason=completed finished=4/4" in s.text("observer")


def reconnect(s):
    observer1 = s.start("observer", "observer1")
    observer2 = s.start("observer", "observer2")
    s.wait_text("observer1", "SNAPSHOT")
    s.wait_text("observer2", "SNAPSHOT")
    observer1.send_signal(signal.SIGINT)
    assert observer1.wait(timeout=5) == 4

    # Split the command between writes and disconnect before acknowledging GO.
    with socket.create_connection(s.address, timeout=5) as connection:
        connection.sendall(b"TRA")
        time.sleep(0.01)
        connection.sendall(b"IN 1\n")
        with connection.makefile("rb") as reader:
            for line in reader:
                if line == b"GO\n":
                    break
            else:
                raise AssertionError("GO expected")

    s.wait_text("observer2", "event=REVOKE")
    observer3 = s.start("observer", "observer3")
    s.wait_text("observer3", "state=WAITING connected=0")
    train1 = s.start("train", "train1", 1)
    s.wait_text("train1", "event=ENTER")
    train1.kill()
    train1.wait(timeout=5)
    train2 = s.start("train", "train2", 2)
    s.wait_text("train2", "event=ENTER")
    train1 = s.start("train", "train1again", 1)
    train3 = s.start("train", "train3", 3)
    assert s.server.wait(timeout=10) == 0
    assert all(client.wait(timeout=5) == 0 for client in [observer2, observer3, train1, train2, train3])
    assert check_events(s.log.read_text(), 3, 2, 2, 3, 60) == 2
    assert "SUMMARY reason=completed finished=3/3" in s.text("observer3")


def stop_server(s, interrupt):
    observer = s.start("observer", "observer")
    train = s.start("train", "train", 1)
    invalid = s.start("train", "invalid", 2)
    assert invalid.wait(timeout=5) == 1
    assert "ERROR" in s.text("invalid")
    s.wait_text("train", "REGISTERED")
    s.wait_text("observer", "SNAPSHOT")
    if interrupt:
        s.server.send_signal(signal.SIGTERM)
    assert s.server.wait(timeout=5) == (4 if interrupt else 2)
    assert observer.wait(timeout=5) == train.wait(timeout=5) == 0
    assert "Сервер завершил работу" in s.text("train")
    assert "reason=" + ("interrupted" if interrupt else "time_limit") in s.text("observer")


def run(name, function, *options):
    with tempfile.TemporaryDirectory() as work:
        session = Session(work, *options)
        try:
            function(session)
        finally:
            session.close()
    print(f"OK: {name}")


if __name__ == "__main__":
    run("simple, FIFO", simple, "--left", 2, "--right", 2, "--capacity", 1,
        "--max-group", 1, "--strategy", "fifo", "--delay-ms", 20)
    run("group, two observers, reconnect, fragmented command", reconnect,
        "--left", 2, "--right", 1, "--arrival-max", 0, "--travel", 60,
        "--delay-ms", 20, "--strategy", "fifo")
    common = ("--left", 1, "--right", 0, "--arrival-min", 100, "--arrival-max", 100,
              "--delay-ms", 20)
    run("server interruption", lambda s: stop_server(s, True), *common)
    run("time limit", lambda s: stop_server(s, False), *common, "--until", 75)
