#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Simple module for inspecting GITEA users, pulls and issues.

import json
import urllib.error
import urllib.parse
import urllib.request

BASE_API_URL = "https://projects.blender.org/api/v1"


def url_json_get(url):
    try:
        # Make the HTTP request and store the response in a 'response' object
        response = urllib.request.urlopen(url)
    except urllib.error.URLError as ex:
        print(url)
        print("Error making HTTP request:", ex)
        return None

    # Convert the response content to a JSON object containing the user information
    return json.loads(response.read())


def url_json_get_all_pages(url, limit=50, verbose=False):
    assert limit <= 50, "50 is the maximum limit of items per page"
    result = []
    page = 1
    while True:
        if verbose:
            print(f"Requesting page {page}", end="\r", flush=True)

        if page == 1:
            # XXX: In some cases, a bug prevents using the `page` and `limit` parameters if the page is 1
            result_page = url_json_get(url)
        else:
            result_page = url_json_get(f"{url}&page={page}&limit={limit}")

        if not result_page:
            break

        result.extend(result_page)

        if len(result_page) < limit:
            break

        page += 1

    return result


def gitea_user_get(username):
    """
    Get the user data as JSON from the user name. https://docs.gitea.com/api/next/#tag/user/operation/userGet
    """

    url = f"{BASE_API_URL}/users/{username}"
    return url_json_get(url)


def gitea_json_issue_get(issue_fullname):
    """
    Get issue/pull JSON data.
    :param issue_fullname: string in the format "{owner}/{repo}/issues/{number}"
    """
    url = f"{BASE_API_URL}/repos/{issue_fullname}"
    return url_json_get(url)


def gitea_json_activities_get(username, date):
    """
    List a user's activity feeds.
    :param username: username of user.
    :param date: the date of the activities to be found.
    """
    activity_url = f"{BASE_API_URL}/users/{username}/activities/feeds?only-performed-by=true&date={date}"
    return url_json_get_all_pages(activity_url)


def gitea_json_issues_search(
        type=None,
        since=None,
        before=None,
        state='all',
        labels=None,
        created=False,
        reviewed=False,
        access_token=None,
        verbose=True):
    """
    Search for issues across the repositories that the user has access to.
    :param type: filter by type (issues / pulls) if set.
    :param since: Only show notifications updated after the given time. This is a timestamp in RFC 3339 format.
    :param before: Only show notifications updated before the given time. This is a timestamp in RFC 3339 format.
    :param state: whether issue is open or closed.
    :param labels: comma separated list of labels. Fetch only issues that have any of this labels. Non existent labels are discarded.
    :param created: filter (issues / pulls) created by you, default is false.
    :param reviewed: filter pulls reviewed by you, default is false.
    :param access_token: token generated by the GITEA API.
    :return: List of issues or pulls.
    """

    query_params = {k: v for k, v in locals().items() if v and k not in {"verbose"}}
    for k, v in query_params.items():
        if v is True:
            query_params[k] = "true"
        elif v is False:
            query_params[k] = "false"

    if verbose:
        print("# Searching for {} #".format(
            query_params["type"] if "type" in query_params else "issues and pulls"))

        print("Query params:", {
              k: v for k, v in query_params.items() if k not in ("type", "access_token")})

    base_url = f"{BASE_API_URL}/repos/issues/search"
    encoded_query_params = urllib.parse.urlencode(query_params)
    issues_url = f"{base_url}?{encoded_query_params}"

    issues = url_json_get_all_pages(issues_url, verbose=verbose)

    if verbose:
        print(f"Total: {len(issues)}         ", end="\n\n", flush=True)

    return issues


def gitea_json_issue_events_filter(
        issue_fullname,
        date_start=None,
        date_end=None,
        username=None,
        labels=None,
        event_type=set()):
    """
    Filter all comments and events on the issue list.
    :param issue_fullname: string in the format "{owner}/{repo}/issues/{number}"
    :param date_start: if provided, only comments updated since the specified time are returned.
    :param date_end: if provided, only comments updated before the provided time are returned.
    :param labels: list of labels. Fetch only events that have any of this labels.
    :param event_type: list of types of events in {"close", "commit_ref"...}.
    :return: List of comments or events.
    """
    issue_events_url = f"{BASE_API_URL}/repos/{issue_fullname}/timeline"
    if date_start or date_end:
        query_params = {}
        if date_start:
            query_params["since"] = f"{date_start.isoformat()}Z"
        if date_end:
            query_params["before"] = f"{date_end.isoformat()}Z"

        encoded_query_params = urllib.parse.urlencode(query_params)
        issue_events_url = f"{issue_events_url}?{encoded_query_params}"

    result = []
    for event in url_json_get_all_pages(issue_events_url):
        if not event:
            continue

        if not event["user"] or event["user"]["username"] != username:
            continue

        if labels and event["type"] == "label" and event["label"]["name"] in labels:
            pass
        elif event["type"] in event_type:
            pass
        else:
            continue

        result.append(event)

    return result


# WORKAROUND: This function doesn't involve GITEA, and the obtained username may not match the username used in GITEA.
# However, it provides an option to fetch the configured username from the local Git,
# in case the user does not explicitly supply the username.
def git_username_detect():
    import os
    import subprocess

    # Get the repository directory
    repo_dir = os.path.abspath(os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))

    # Attempt to get the configured username from the local Git
    try:
        result = subprocess.run(["git", "config", "user.username"], stdout=subprocess.PIPE, cwd=repo_dir)
        result.check_returncode()  # Check if the command was executed successfully
        username = result.stdout.decode().rstrip()
        return username
    except subprocess.CalledProcessError as ex:
        # Handle errors if the git config command fails
        print(f"Error fetching Git username: {ex}")
        return None
