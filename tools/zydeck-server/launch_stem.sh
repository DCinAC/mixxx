#!/usr/bin/env bash
# Now part of launch.sh (both servers in one): this is ./launch.sh <start|stop|status|logs> stems
exec "$(dirname "$0")/launch.sh" "${1:-start}" stems
