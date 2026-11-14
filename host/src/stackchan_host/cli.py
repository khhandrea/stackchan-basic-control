"""Command-line tool: ``stackchan ping``, ``stackchan call <method> [params]``, ``stackchan listen``, ``stackchan capture``, ``stackchan serve``."""

from __future__ import annotations

import argparse
import json
import os
import queue
import sys
import time

from .client import DEFAULT_PORT, StackChanClient, RpcError
from .image import rgb565be_to_png


def _print_json(value: object) -> None:
    print(json.dumps(value, ensure_ascii=False))


def cmd_ping(client: StackChanClient, args: argparse.Namespace) -> int:
    for _ in range(args.count):
        start = time.perf_counter()
        client.call("sys.ping")
        print(f"pong {1000 * (time.perf_counter() - start):.1f} ms")
    return 0


def cmd_call(client: StackChanClient, args: argparse.Namespace) -> int:
    params = json.loads(args.params) if args.params else None
    _print_json(client.call(args.method, params, timeout=args.timeout))
    return 0


def cmd_listen(client: StackChanClient, args: argparse.Namespace) -> int:
    deadline = None if args.seconds is None else time.monotonic() + args.seconds
    while deadline is None or time.monotonic() < deadline:
        try:
            message = client.notifications.get(timeout=0.2)
        except queue.Empty:
            continue
        _print_json(message)
        sys.stdout.flush()
    return 0


def cmd_capture(client: StackChanClient, args: argparse.Namespace) -> int:
    frame = client.call("display.capture", timeout=10)
    with open(args.output, "wb") as f:
        f.write(rgb565be_to_png(frame["data"], frame["width"], frame["height"]))
    print(args.output)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="stackchan", description=__doc__)
    parser.add_argument("--port", default=os.environ.get("STACKCHAN_PORT", DEFAULT_PORT))
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ping", help="round-trip sys.ping")
    p.add_argument("-c", "--count", type=int, default=3)
    p.set_defaults(func=cmd_ping)

    p = sub.add_parser("call", help="call a method and print its result")
    p.add_argument("method")
    p.add_argument("params", nargs="?", help='JSON object, e.g. \'{"yaw": 30}\'')
    p.add_argument("--timeout", type=float, default=2.0)
    p.set_defaults(func=cmd_call)

    p = sub.add_parser("listen", help="print notifications (events, streams, logs, raw lines)")
    p.add_argument("-s", "--seconds", type=float, help="stop after this many seconds")
    p.set_defaults(func=cmd_listen)

    p = sub.add_parser("capture", help="save the current screen as PNG (display.capture)")
    p.add_argument("output")
    p.set_defaults(func=cmd_capture)

    p = sub.add_parser("serve", help="web dashboard to watch and control the device")
    p.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all interfaces)")
    p.add_argument("--http-port", type=int, default=8080)
    p.set_defaults(func=None)

    args = parser.parse_args()
    if args.command == "serve":
        from .server import serve

        try:
            serve(args.port, args.host, args.http_port)
        except KeyboardInterrupt:
            pass
        return 0
    try:
        with StackChanClient(args.port) as client:
            return args.func(client, args)
    except RpcError as exc:
        print(f"error {exc}", file=sys.stderr)
        return 1
    except TimeoutError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
