#!/bin/bash
# Remove last week of data from TimescaleDB (safe version)

set -euo pipefail

(
    cd "$(dirname "$0")"

    set -a
    source .env
    set +a

    read -p "Enter the table name [default: indoor]: " TABLE_NAME

    if [ -z "${TABLE_NAME:-}" ]; then
        TABLE_NAME="indoor"
    fi

    if [[ ! "$TABLE_NAME" =~ ^[a-zA-Z0-9_]+$ ]]; then
        echo "Invalid table name: $TABLE_NAME"
        exit 1
    fi

    ONE_WEEK_AGO=$(date -d "7 days ago" +"%Y-%m-%d %H:%M:%S")

    command -v pg_dump >/dev/null 2>&1 || {
        echo "pg_dump not found. Install PostgreSQL client tools."
        exit 1
    }

    command -v psql >/dev/null 2>&1 || {
        echo "psql not found. Install PostgreSQL client tools."
        exit 1
    }

    BACKUP_FILE="timescaledb_backup_$(date +"%Y%m%d%H%M%S").sql"

    pg_dump \
        -h "$TIMESCALEDB_HOST" \
        -p "$TIMESCALEDB_PORT" \
        -U "$TIMESCALEDB_USER" \
        -d "$TIMESCALEDB_DB" \
        -F c -b -v \
        -f "$BACKUP_FILE" || {
            echo "Backup failed. Aborting delete."
            exit 1
        }

    echo "Backup created: $BACKUP_FILE"

    psql \
        -h "$TIMESCALEDB_HOST" \
        -p "$TIMESCALEDB_PORT" \
        -U "$TIMESCALEDB_USER" \
        -d "$TIMESCALEDB_DB" \
        <<SQL
BEGIN;

DELETE FROM "$TABLE_NAME"
WHERE timestamp < '$ONE_WEEK_AGO';

DELETE FROM "${TABLE_NAME}_condensed"
WHERE timestamp < '$ONE_WEEK_AGO';

COMMIT;
SQL

    echo "Cleanup completed for table: $TABLE_NAME"
)