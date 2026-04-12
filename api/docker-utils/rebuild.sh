#!/bin/bash

cd "$(dirname "$0")/.." || exit

echo -e "Building and restarting the API container...\n"
docker compose up -d --build --force-recreate
echo -e "\nDone!"
