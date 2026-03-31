<?php
header("Content-Type: application/json");
header("Access-Control-Allow-Origin: *");

$host = "sql302.infinityfree.com"; 
$db   = "if0_41381183_pmo_db";   
$user = "if0_41381183"; 
$pass = "54copas0";            

$conn = new mysqli($host, $user, $pass, $db);

$method = $_SERVER['REQUEST_METHOD'];

if ($method === 'POST') {
    
    $d = json_decode(file_get_contents("php://input"), true);
    $cmd = intval($d['command']);
    $dev = $conn->real_escape_string($d['device_id'] ?? 'PMO-ESP32-001');
    $conn->query("INSERT INTO relay_commands (device_id, command) VALUES ('$dev', $cmd)");
    echo json_encode(["status" => "queued"]);
} else {
    
    $dev = $conn->real_escape_string($_GET['device_id'] ?? 'PMO-ESP32-001');
    $res = $conn->query("SELECT id, command FROM relay_commands WHERE device_id='$dev' AND executed=0 ORDER BY id DESC LIMIT 1");
    $row = $res->fetch_assoc();
    if ($row) {
        $conn->query("UPDATE relay_commands SET executed=1 WHERE id={$row['id']}");
        echo json_encode(["command" => intval($row['command']), "pending" => true]);
    } else {
        echo json_encode(["pending" => false]);
    }
}
$conn->close();
?>