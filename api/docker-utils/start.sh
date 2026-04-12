#!/bin/bash

cd "$(dirname "$0")/.." || exit

echo -e "Starting the API container in the background...\n"
docker compose up -d
echo -e "\nDone!"
