#!/bin/sh
# Transactional ELF packing. Make supplies CC and PYTHON explicitly.
set -eu
exec "${PYTHON:-python3}" "$(dirname "$0")/pack.py" "$@"
