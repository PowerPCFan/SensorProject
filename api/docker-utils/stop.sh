#!/bin/bash

cd "$(dirname "$0")/.." || exit

echo -e "Stopping the API container...\n"
docker compose down
echo -e "\nStopped!"
