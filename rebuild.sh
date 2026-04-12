#!/bin/bash

cd "$(dirname "$0")"

echo "Stopping and Removing All Containers... "

# echo "Stopping Grafana..."
# cd grafana
# docker compose down
# cd ..

echo "Stopping API..."
cd api
docker compose down
cd ..

echo "Stopping TimescaleDB..."
cd timescaledb
docker compose down
cd ..

echo -e "\nRebuilding and Starting All Containers... "

echo "Starting TimescaleDB..."
cd timescaledb
docker compose up -d --build
cd ..

echo "Starting API..."
cd api
docker compose up -d --build
cd ..

# echo "Starting Grafana..."
# cd grafana
# docker compose up -d --build
# cd ..

echo -e "\nDone"
