#!/usr/bin/env python3
"""Black-box integration checks for the parent/child ticket application."""

import os
import pathlib
import re
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PATH_TO_TICKET_SYSTEM", file=sys.stderr)
        return 2

    executable = pathlib.Path(sys.argv[1]).resolve()
    commands = [
        "/help",
        "/new Alice|Cannot log in|1",
        "/new Bob|Payment issue|2",
        "/new Cara|Urgent outage|3",
        "/new Dan|Account locked|1",
        "/new Eve|Queue overflow case|1",
        "/stats",
        "/process",
        "/queue",
        "/process",
        "/process",
        "/list",
        "/stats",
        "/quit",
    ]

    with tempfile.TemporaryDirectory(prefix="ticket-system-test-") as temp_dir:
        result = subprocess.run(
            [str(executable)],
            input="\n".join(commands) + "\n",
            text=True,
            capture_output=True,
            cwd=temp_dir,
            timeout=10,
            check=False,
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            print(output, file=sys.stderr)
            raise AssertionError(f"application exited with status {result.returncode}")

        expected = [
            "Commands: /new CUSTOMER",
            "Created and queued ticket #1 for Alice.",
            "Created and queued ticket #5; queue is full",
            "total=5 queued=4/4",
            "status: Resolved",
            "Moved 1 waiting ticket(s) to the queue",
            "Eve",
            "status: Escalated",
        ]
        for text in expected:
            if text not in output:
                print(output, file=sys.stderr)
                raise AssertionError(f"expected output not found: {text!r}")

        log_path = pathlib.Path(temp_dir) / "ticket_log.txt"
        if not log_path.is_file():
            raise AssertionError("audit log was not created")
        log = log_path.read_text(encoding="utf-8")
        if "ticket created and queued" not in log or "ticket processed" not in log:
            raise AssertionError("audit log is missing creation or processing events")
        if not re.search(r"\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\]", log):
            raise AssertionError("audit log timestamp format was not found")
        if os.name == "posix" and log_path.stat().st_mode & 0o777 != 0o600:
            raise AssertionError("audit log permissions are not owner-only (0600)")

    print("Process-level integration tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
