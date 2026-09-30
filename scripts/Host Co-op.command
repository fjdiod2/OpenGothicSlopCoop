#!/bin/sh
exec python3 "$(dirname "$0")/coop-launch.py" host "$@"
