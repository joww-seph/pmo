<?php
header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");

$host = "sql302.infinityfree.com"; // Check yours in cpanel → MySQL Databases
$db   = "if0_41381183_pmo_db";   // YOUR full db name
$user = "if0_41381183"; // YOUR full username
$pass = "54copas0";            // YOUR password

$conn = new mysqli($host, $user, $pass, $db);

// Latest reading
$latest = $conn->query("SELECT * FROM sensor_readings ORDER BY id DESC LIMIT 1")->fetch_assoc();

// History - last 50 readings
$histRes = $conn->query("SELECT recorded_at, voltage, current_a, power_w, apparent_power, reactive_power, frequency_hz, power_factor, energy_kwh FROM sensor_readings ORDER BY id DESC LIMIT 50");
$history = [];
while ($row = $histRes->fetch_assoc()) { $history[] = $row; }
$history = array_reverse($history);

echo json_encode(["latest" => $latest, "history" => $history]);
$conn->close();
?>