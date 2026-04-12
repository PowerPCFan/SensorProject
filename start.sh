#!/bin/bash

echo "Starting TimescaleDB..."
cd "$(dirname "$0")/timescaledb"
docker compose up -d
cd ..

echo "Starting API..."
cd api
docker compose up -d
cd ..

# echo "Starting Grafana..."
# cd grafana
# docker compose up -d
# cd ..

echo "All services started successfully!"