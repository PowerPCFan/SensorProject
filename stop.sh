#!/bin/bash

echo "Stopping Grafana..."
cd "$(dirname "$0")/grafana"
docker compose down
cd ..

echo "Stopping API..."
cd api
docker compose down
cd ..

echo "Stopping TimescaleDB..."
cd timescaledb
docker compose down
cd ..

echo "All services stopped successfully!"