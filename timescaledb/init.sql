-- Enables the TimescaleDB extension
CREATE EXTENSION IF NOT EXISTS timescaledb;

-- Create the indoor and outdoor tables (normal PgSQL tables) if they don't exist
CREATE TABLE IF NOT EXISTS outdoor (
    time TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    temperature DOUBLE PRECISION,
    humidity DOUBLE PRECISION,
    pressure DOUBLE PRECISION,
    light_level DOUBLE PRECISION,
    esp32_temperature DOUBLE PRECISION,
    wifi_strength DOUBLE PRECISION
);

CREATE TABLE IF NOT EXISTS indoor (
    time TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    temperature DOUBLE PRECISION,
    humidity DOUBLE PRECISION,
    pressure DOUBLE PRECISION,
    gas DOUBLE PRECISION,
    pm1_0 DOUBLE PRECISION,
    pm2_5 DOUBLE PRECISION,
    pm10_0 DOUBLE PRECISION,
    pm_bin_03_05 DOUBLE PRECISION,
    pm_bin_05_10 DOUBLE PRECISION,
    pm_bin_10_25 DOUBLE PRECISION,
    pm_bin_25_50 DOUBLE PRECISION,
    pm_bin_50_100 DOUBLE PRECISION,
    pm_bin_100_plus DOUBLE PRECISION,
    vocs DOUBLE PRECISION,
    carbon_monoxide DOUBLE PRECISION,
    carbon_dioxide DOUBLE PRECISION,
    wifi_strength DOUBLE PRECISION,
    aqi INTEGER
);

-- Converts the PGSQL tables into TimescaleDB hypertables
SELECT create_hypertable('outdoor', 'time', if_not_exists => TRUE);
SELECT create_hypertable('indoor', 'time', if_not_exists => TRUE);


-- The following has been disabled as it was making things too confusing so I just don't want to bother anymore

-- -- Sets a policy to drop data older than 1 year, resulting in less storage usage
-- -- 1 year is quite generous IMO but I don't want to lose too much raw data
-- SELECT add_retention_policy('indoor', INTERVAL '1 year');
-- SELECT add_retention_policy('outdoor', INTERVAL '1 year');

-- -- Creates a continuous aggregate ("condensed table") for the "indoor" hypertable
-- CREATE MATERIALIZED VIEW IF NOT EXISTS indoor_condensed
-- WITH (timescaledb.continuous) AS
-- SELECT
--     -- Group the data into 1-hour intervals (buckets)
--     time_bucket('1 hour', time) AS bucket,

--     -- Calculate the average for all metrics
--     avg(temperature) AS temperature,
--     avg(humidity) AS humidity,
--     avg(pressure) AS pressure,
--     avg(gas) AS gas,

--     avg(pm1_0) AS pm1_0,
--     avg(pm2_5) AS pm2_5,
--     avg(pm10_0) AS pm10_0,
--     avg(pm_bin_03_05) AS pm_bin_03_05,
--     avg(pm_bin_05_10) AS pm_bin_05_10,
--     avg(pm_bin_10_25) AS pm_bin_10_25,
--     avg(pm_bin_25_50) AS pm_bin_25_50,
--     avg(pm_bin_50_100) AS pm_bin_50_100,
--     avg(pm_bin_100_plus) AS pm_bin_100_plus,

--     avg(vocs) AS vocs,
--     avg(carbon_monoxide) AS carbon_monoxide,
--     avg(carbon_dioxide) AS carbon_dioxide,

--     avg(wifi_strength) AS wifi_strength

-- FROM indoor
-- GROUP BY bucket;

-- -- Set the continuous aggregate to auto-update in the background
-- -- 'start_offset' = how far back the engine should look for old data to update the view
-- -- 'end_offset' = how recent the data must be before the engine adds it to the view
-- -- 'schedule_interval' = how frequently the update process will run
-- SELECT add_continuous_aggregate_policy('indoor_condensed',
--     start_offset => INTERVAL '3 months',
--     end_offset => INTERVAL '1 hour',
--     schedule_interval => INTERVAL '1 hour'
-- );


-- -- Creates a continuous aggregate ("condensed table") for the "outdoor" hypertable
-- CREATE MATERIALIZED VIEW IF NOT EXISTS outdoor_condensed
-- WITH (timescaledb.continuous) AS
-- SELECT
--     -- Group the data into 1-hour intervals (buckets)
--     time_bucket('1 hour', time) AS bucket,

--     -- Calculate the average for all metrics
--     avg(temperature) AS temperature,
--     avg(humidity) AS humidity,
--     avg(pressure) AS pressure,
--     avg(light_level) AS light_level,
--     avg(esp32_temperature) AS esp32_temperature,
--     avg(wifi_strength) AS wifi_strength

-- FROM outdoor
-- GROUP BY bucket;

-- -- See the previous comments for the indoor condensed aggregate policy
-- SELECT add_continuous_aggregate_policy('outdoor_condensed',
--     start_offset => INTERVAL '3 months',
--     end_offset => INTERVAL '1 hour',
--     schedule_interval => INTERVAL '1 hour'
-- );