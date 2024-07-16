import argparse
import dataclasses
import json
import logging
import os
import sys
from typing import List

from get_robot_token import get_best_robot_token
from github_helper import GitHub
from ci_utils import Shell
from env_helper import GITHUB_REPOSITORY
from report import SUCCESS
from ci_buddy import CIBuddy


def parse_args():
    parser = argparse.ArgumentParser(
        "Checks if enough days elapsed since the last release on each release "
        "branches and do a release in case for green builds."
    )
    parser.add_argument("--token", help="GitHub token, if not set, used from smm")
    parser.add_argument(
        "--post-status",
        action="store_true",
        help="Post release branch statuses",
    )
    parser.add_argument(
        "--prepare",
        action="store_true",
        help="Prepare autorelease info",
    )
    return parser.parse_args(), parser


MAX_NUMBER_OF_COMMITS_TO_CONSIDER_FOR_RELEASE = 5
AUTORELEASE_INFO_FILE = "/tmp/autorelease_info.json"


@dataclasses.dataclass
class ReleaseParams:
    ready: bool
    ci_status: str
    num_patches: int
    release_branch: str
    commit_sha: str

    def to_dict(self):
        return dataclasses.asdict(self)


@dataclasses.dataclass
class AutoReleaseInfo:
    releases: List[ReleaseParams]

    def add_release(self, release_params: ReleaseParams):
        self.releases.append(release_params)

    def dump(self):
        print(f"Dump release info into [{AUTORELEASE_INFO_FILE}]")
        with open(AUTORELEASE_INFO_FILE, "w", encoding="utf-8") as f:
            print(json.dumps(dataclasses.asdict(self), indent=2), file=f)

    @staticmethod
    def from_file() -> "AutoReleaseInfo":
        with open(AUTORELEASE_INFO_FILE, "r", encoding="utf-8") as json_file:
            res = json.load(json_file)
        releases = [ReleaseParams(**release) for release in res["releases"]]
        return AutoReleaseInfo(releases=releases)


def _prepare(token):
    assert len(token) > 10
    os.environ["GH_TOKEN"] = token
    (Shell.run("gh auth status", check=True))
    gh = GitHub(token)
    prs = gh.get_release_pulls(GITHUB_REPOSITORY)
    branch_names = [pr.head.ref for pr in prs]
    print(f"Found release branches [{branch_names}]")
    repo = gh.get_repo(GITHUB_REPOSITORY)
    autoRelease_info = AutoReleaseInfo(releases=[])
    for pr in prs:
        print(f"Checking PR [{pr.head.ref}]")

        refs = list(repo.get_git_matching_refs(f"tags/v{pr.head.ref}"))
        refs.sort(key=lambda ref: ref.ref)

        latest_release_tag_ref = refs[-1]
        latest_release_tag = repo.get_git_tag(latest_release_tag_ref.object.sha)
        commit_num = int(
            Shell.run(
                f"git rev-list --count {latest_release_tag.tag}..origin/{pr.head.ref}",
                check=True,
            )
        )
        print(
            f"Previous release is [{latest_release_tag}] was [{commit_num}] commits before, date [{latest_release_tag.tagger.date}]"
        )
        commit_reverse_index = 0
        commit_found = False
        commit_checked = False
        last_ci_status = ""
        best_ci_status = ""
        last_commit_sha = ""
        best_commit_sha = ""
        while (
            commit_reverse_index < commit_num - 1
            and commit_reverse_index < MAX_NUMBER_OF_COMMITS_TO_CONSIDER_FOR_RELEASE
        ):
            commit_checked = True
            best_commit_sha = Shell.run(
                f"git rev-list --max-count=1 --skip={commit_reverse_index} origin/{pr.head.ref}",
                check=True,
            )
            print(
                f"Check if commit [{best_commit_sha}] [{pr.head.ref}~{commit_reverse_index}] is ready for release"
            )
            if not last_commit_sha:
                last_commit_sha = best_commit_sha
            commit_reverse_index += 1

            cmd = f"gh api -H 'Accept: application/vnd.github.v3+json' /repos/{GITHUB_REPOSITORY}/commits/{best_commit_sha}/status"
            ci_status_json = Shell.run(cmd, check=True)
            best_ci_status = json.loads(ci_status_json)["state"]
            if not last_ci_status:
                last_ci_status = best_ci_status
            if best_ci_status == SUCCESS:
                commit_found = True
            break
        if commit_found:
            print(
                f"Add release ready info for commit [{best_commit_sha}] and release branch [{pr.head.ref}]"
            )
            autoRelease_info.add_release(
                ReleaseParams(
                    release_branch=pr.head.ref,
                    commit_sha=best_commit_sha,
                    ready=True,
                    ci_status=best_ci_status,
                    num_patches=commit_num,
                )
            )
        else:
            print(f"WARNING: No good commits found for release branch [{pr.head.ref}]")
            autoRelease_info.add_release(
                ReleaseParams(
                    release_branch=pr.head.ref,
                    commit_sha=last_commit_sha,
                    ready=False,
                    ci_status=last_ci_status,
                    num_patches=commit_num,
                )
            )
            if commit_checked:
                print(
                    f"ERROR: CI is failed. check CI status for branch [{pr.head.ref}]"
                )
    autoRelease_info.dump()


def main():
    args, parser = parse_args()

    if args.post_status:
        info = AutoReleaseInfo.from_file()
        for release_info in info.releases:
            CIBuddy(dry_run=False).post_info(
                title=f"Auto Release Status for {release_info.release_branch}",
                body=release_info.to_dict(),
            )
    elif args.prepare:
        _prepare(token=args.token or get_best_robot_token())
    else:
        parser.print_help()
        sys.exit(2)


if __name__ == "__main__":
    main()
