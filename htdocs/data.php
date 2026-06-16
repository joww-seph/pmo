<?php
header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");

require_once __DIR__ . '/config.php';

$latest = $conn->query("SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 1")->fetch_assoc();

$histRes = $conn->query("SELECT recorded_at, voltage, current_a, power_w, apparent_power, reactive_power, frequency_hz, power_factor, energy_kwh FROM sensor_readings ORDER BY id DESC LIMIT 50");
$history = [];
while ($row = $histRes->fetch_assoc()) { $history[] = $row; }
$history = array_reverse($history);

echo json_encode(["latest" => $latest, "history" => $history]);
$conn->close();
