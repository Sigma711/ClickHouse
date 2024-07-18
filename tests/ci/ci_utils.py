import os
import re
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator, List, Union, Optional, Sequence

import requests


class Envs:
    GITHUB_REPOSITORY = os.getenv("GITHUB_REPOSITORY", "ClickHouse/ClickHouse")


class WithIter(type):
    def __iter__(cls):
        return (v for k, v in cls.__dict__.items() if not k.startswith("_"))


@contextmanager
def cd(path: Union[Path, str]) -> Iterator[None]:
    oldpwd = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(oldpwd)


def is_hex(s):
    try:
        int(s, 16)
        return True
    except ValueError:
        return False


def normalize_string(string: str) -> str:
    res = string.lower()
    for r in ((" ", "_"), ("(", "_"), (")", "_"), (",", "_"), ("/", "_"), ("-", "_")):
        res = res.replace(*r)
    return res


class GHActions:
    @staticmethod
    def print_in_group(group_name: str, lines: Union[Any, List[Any]]) -> None:
        lines = list(lines)
        print(f"::group::{group_name}")
        for line in lines:
            print(line)
        print("::endgroup::")

    @staticmethod
    def get_commit_status_by_name(
        token: str, commit_sha: str, status_name: Union[str, Sequence]
    ) -> Optional[str]:
        assert len(token) == 40
        assert len(commit_sha) == 40
        assert is_hex(commit_sha)
        assert not is_hex(token)
        url = f"https://api.github.com/repos/{Envs.GITHUB_REPOSITORY}/commits/{commit_sha}/statuses?per_page={200}"
        headers = {
            "Authorization": f"token {token}",
            "Accept": "application/vnd.github.v3+json",
        }
        response = requests.get(url, headers=headers, timeout=5)

        if isinstance(status_name, str):
            status_name = (status_name,)
        if response.status_code == 200:
            assert "next" not in response.links, "Response truncated"
            statuses = response.json()
            for status in statuses:
                if status["context"] in status_name:
                    return status["state"]
        return None

    @staticmethod
    def check_wf_completed(token: str, commit_sha: str) -> bool:
        headers = {
            "Authorization": f"token {token}",
            "Accept": "application/vnd.github.v3+json",
        }
        url = f"https://api.github.com/repos/{Envs.GITHUB_REPOSITORY}/commits/{commit_sha}/check-runs?per_page={100}"

        for i in range(3):
            try:
                response = requests.get(url, headers=headers, timeout=5)
                response.raise_for_status()
                # assert "next" not in response.links, "Response truncated"

                data = response.json()
                assert data["check_runs"], "?"

                for check in data["check_runs"]:
                    if check["status"] != "completed":
                        print(
                            f"   Check workflow status: Check not completed [{check['name']}]"
                        )
                        return False
                else:
                    return True
            except Exception as e:
                print(f"ERROR: exception {e}")
                time.sleep(1)

        return False


class Shell:
    @classmethod
    def run_strict(cls, command):
        res = subprocess.run(
            command,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True,
        )
        return res.stdout.strip()

    @classmethod
    def run(cls, command, check=False):
        print(f"Run command [{command}]")
        res = ""
        result = subprocess.run(
            command,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if result.returncode == 0:
            res = result.stdout
        else:
            print(
                f"ERROR: stdout {result.stdout.strip()}, stderr {result.stderr.strip()}"
            )
            if check:
                assert result.returncode == 0
        return res.strip()

    @classmethod
    def check(cls, command):
        result = subprocess.run(
            command,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        return result.returncode == 0


class Utils:
    @staticmethod
    def get_failed_tests_number(description: str) -> Optional[int]:
        description = description.lower()

        pattern = r"fail:\s*(\d+)\s*(?=,|$)"
        match = re.search(pattern, description)
        if match:
            return int(match.group(1))
        return None

    @staticmethod
    def is_killed_with_oom():
        if Shell.check(
            "sudo dmesg -T | grep -q -e 'Out of memory: Killed process' -e 'oom_reaper: reaped process' -e 'oom-kill:constraint=CONSTRAINT_NONE'"
        ):
            return True
        return False

    @staticmethod
    def clear_dmesg():
        Shell.run("sudo dmesg --clear ||:")
