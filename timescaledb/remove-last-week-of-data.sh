#!/bin/bash

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

    if [[ "$TABLE_NAME" != "indoor" && "$TABLE_NAME" != "outdoor" ]]; then
        echo "Only 'indoor' or 'outdoor' allowed"
        exit 1
    fi

    command -v pg_dump >/dev/null 2>&1 || {
        echo "pg_dump not found. Install PostgreSQL client tools."
        exit 1
    }

    command -v psql >/dev/null 2>&1 || {
        echo "psql not found. Install PostgreSQL client tools."
        exit 1
    }

    BACKUP_FILE="timescaledb_backup_$(date +"%Y%m%d%H%M%S").dump"

    PGPASSWORD="$POSTGRES_PASSWORD" pg_dump \
        -h "localhost" \
        -p "$POSTGRES_PORT" \
        -U "$POSTGRES_USER" \
        -d "$POSTGRES_DB" \
        -F c -b -v \
        -f "$BACKUP_FILE" || {
            echo "Backup failed. Aborting delete."
            exit 1
        }

    echo "Backup created: $BACKUP_FILE"

    # PGPASSWORD="$POSTGRES_PASSWORD" psql \
    #     -h "localhost" \
    #     -p "$POSTGRES_PORT" \
    #     -U "$POSTGRES_USER" \
    #     -d "$POSTGRES_DB" \
    #     -c "SELECT drop_chunks('$TABLE_NAME', INTERVAL '7 days');"

    PGPASSWORD="$POSTGRES_PASSWORD" psql \
        -h "localhost" \
        -p "$POSTGRES_PORT" \
        -U "$POSTGRES_USER" \
        -d "$POSTGRES_DB" \
        -c "DELETE FROM $TABLE_NAME WHERE time > NOW() - INTERVAL '7 days';"

    echo "Cleanup completed for table: $TABLE_NAME"
)