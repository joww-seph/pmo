<?php
header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");

$host = "sql302.infinityfree.com";
$db   = "if0_41381183_pmo_db";
$user = "if0_41381183";
$pass = "54copas0";

$conn = new mysqli($host, $user, $pass, $db);
if ($conn->connect_error) {
    http_response_code(500);
    echo json_encode(["error" => "DB connect failed"]);
    exit;
}

// Force Philippine Time (UTC+8) for this session
$conn->query("SET time_zone = '+08:00'");

$d = json_decode(file_get_contents("php://input"), true);
if (!$d) { $d = $_POST; }

$stmt = $conn->prepare("INSERT INTO sensor_readings 
  (device_id, wifi_ssid, voltage, current_a, power_w, energy_kwh, 
   frequency_hz, power_factor, apparent_power, reactive_power, 
   load_type, cost_per_hour, cost_per_month, wasted_power, 
   safety_margin, co2_kg, relay_state, trip_reason, recorded_at)
  VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,NOW())");

$stmt->bind_param("ssddddddddsdddddis",
  $d['device_id'], $d['wifi_ssid'],
  $d['voltage'], $d['current_a'], $d['power_w'], $d['energy_kwh'],
  $d['frequency_hz'], $d['power_factor'], $d['apparent_power'], $d['reactive_power'],
  $d['load_type'], $d['cost_per_hour'], $d['cost_per_month'],
  $d['wasted_power'], $d['safety_margin'], $d['co2_kg'],
  $d['relay_state'], $d['trip_reason']
);

$stmt->execute();
echo json_encode(["status" => "ok", "id" => $conn->insert_id]);
$conn->close();
?>