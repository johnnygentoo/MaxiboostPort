# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's version and the projects it credits (the About page).

gzport is a local project: there are no releases to check, so the launcher does not look for
updates. Its base projects are linked from the About page.
"""

NAME = "gzport"
VERSION = "0.1"
# The projects gzport is built from.
BASE_URL = "https://github.com/ayozetr/bloodborne_linux"  # bloodborne_linux (Linux fork of bbport)
UPSTREAM_URL = "https://github.com/deadinside28/bloodborne_pc"  # bbport
SHADPS4_URL = "https://github.com/shadps4-emu/shadPS4"
