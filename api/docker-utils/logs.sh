#!/bin/bash

cd "$(dirname "$0")/.." || exit
echo "Streaming API logs (Ctrl+C to exit)..."
# docker compose logs -f
docker logs -f api  # i prefer this view
