#!/bin/bash

cd "$(dirname "$0")/.." || exit

echo "Restarting the API container... (note that this doesn't apply any changes since last build)"
docker compose restart
echo "Restarted!"
